
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "hardware/structs/bus_ctrl.h"

#include "pico_hdmi/video_output.h"
#include "pico_hdmi/video_output_rt.h"
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"

#include "spi_capture.pio.h"
#include "i2s_audio.pio.h"

// Brightness & Gamma Correction Configuration
#define ENABLE_BRIGHTNESS_CORRECTION 1
#define BRIGHTNESS_GAMMA             1.0f   // Exponent: < 1.0 brightens shadows, > 1.0 darkens
#define BRIGHTNESS_GAIN              1.0f    // Multiplier: scales overall brightness
#define BRIGHTNESS_OFFSET            2       // Flat offset: lifts absolute black level (0-31 scale)
#define COLOR_SATURATION             256     // Saturation: 256 = default (1.0), < 256 = muted, > 256 = boosted

// Core Voltage Regulator Configuration (Default: VREG_VOLTAGE_1_20 for 1.20V max stability)
#define VREG_VOLTAGE_LEVEL           VREG_VOLTAGE_1_20

// Audio Volume Gain Configuration (Percentage: 100 = 100%/default boosted baseline, > 100 = additional boost)
#define AUDIO_VOLUME_PERCENT         100

#if ENABLE_BRIGHTNESS_CORRECTION || (defined(COLOR_SATURATION) && (COLOR_SATURATION != 256))
static uint16_t correction_lut[65536];
#endif

#define FB_WIDTH  320
#define FB_HEIGHT 240
#define FB_SIZE   (FB_WIDTH * FB_HEIGHT)

// Aspect Ratio Toggle Button GPIO pin
#define ASPECTBUTTON 22

// Diagnostics Configuration (Disabled by default)
// Set to 1 to enable diagnostic logging to USB serial COM port.
// Note: If enabled, you must also set pico_enable_stdio_usb(spi_to_hdmi 1) in CMakeLists.txt
#define ENABLE_DIAGNOSTICS           0

#if ENABLE_DIAGNOSTICS
static volatile uint32_t diag_bytes_received = 0;
static volatile uint32_t diag_frame_count = 0;
static uint64_t diag_last_time = 0;
static uint64_t diag_active_transfer_time = 0; // Cumulative microseconds spent in active transfer
static uint32_t diag_active_bytes = 0;          // Cumulative bytes received during active transfer
static uint64_t burst_start_time = 0;
static uint64_t burst_last_data_time = 0;
static uint32_t burst_bytes = 0;
static bool in_burst = false;
#endif

static uint16_t framebuffer[FB_SIZE] __attribute__((aligned(64)));

// Global State
static uint16_t cur_x = 0, cur_y = 0;
static uint16_t start_x = 0, end_x = 319;
static uint16_t start_y = 0, end_y = 239;
static uint8_t last_cmd = 0;
static int pixel_byte_idx = 0;
static uint16_t temp_pixel = 0;
static volatile bool is_4_3 = true;

// SPI Fast Decoding Table
static uint8_t mosi_table[65536];
static bool is_cmd_table[65536];

void init_tables() {
    for (int i = 0; i < 65536; i++) {
        uint8_t m = 0;
        if (i & 0x0001) m |= 0x01; if (i & 0x0004) m |= 0x02;
        if (i & 0x0010) m |= 0x04; if (i & 0x0040) m |= 0x08;
        if (i & 0x0100) m |= 0x10; if (i & 0x0400) m |= 0x20;
        if (i & 0x1000) m |= 0x40; if (i & 0x4000) m |= 0x80;
        mosi_table[i] = m;
        is_cmd_table[i] = !(i & 0x0002);
    }

#if ENABLE_BRIGHTNESS_CORRECTION
    // Initialize temporary Red, Green, Blue LUTs for brightness mapping
    uint8_t temp_lut_r[32];
    uint8_t temp_lut_g[64];
    uint8_t temp_lut_b[32];
    
    // Initialize Red LUT (ranges [BRIGHTNESS_OFFSET, 31])
    for (int i = 0; i < 32; i++) {
        float val = (float)i / 31.0f;
        val = powf(val, BRIGHTNESS_GAMMA) * BRIGHTNESS_GAIN;
        int out = (int)(val * (31.0f - BRIGHTNESS_OFFSET)) + BRIGHTNESS_OFFSET;
        if (out < 0) out = 0;
        if (out > 31) out = 31;
        temp_lut_r[i] = out;
    }
    // Initialize Green LUT (ranges [BRIGHTNESS_OFFSET * 2, 63])
    for (int i = 0; i < 64; i++) {
        float val = (float)i / 63.0f;
        val = powf(val, BRIGHTNESS_GAMMA) * BRIGHTNESS_GAIN;
        int out = (int)(val * (63.0f - (BRIGHTNESS_OFFSET * 2))) + (BRIGHTNESS_OFFSET * 2);
        if (out < 0) out = 0;
        if (out > 63) out = 63;
        temp_lut_g[i] = out;
    }
    // Initialize Blue LUT (ranges [BRIGHTNESS_OFFSET, 31])
    for (int i = 0; i < 32; i++) {
        float val = (float)i / 31.0f;
        val = powf(val, BRIGHTNESS_GAMMA) * BRIGHTNESS_GAIN;
        int out = (int)(val * (31.0f - BRIGHTNESS_OFFSET)) + BRIGHTNESS_OFFSET;
        if (out < 0) out = 0;
        if (out > 31) out = 31;
        temp_lut_b[i] = out;
    }
#endif

#if ENABLE_BRIGHTNESS_CORRECTION || (defined(COLOR_SATURATION) && (COLOR_SATURATION != 256))
    // Populate the 16-bit combined Correction Lookup Table
    for (int i = 0; i < 65536; i++) {
        uint16_t r = (i >> 11) & 0x1F;
        uint16_t g = (i >> 5) & 0x3F;
        uint16_t b = i & 0x1F;

#if ENABLE_BRIGHTNESS_CORRECTION
        r = temp_lut_r[r];
        g = temp_lut_g[g];
        b = temp_lut_b[b];
#endif

#if defined(COLOR_SATURATION) && (COLOR_SATURATION != 256)
        // Scale R and B from 5-bit to 6-bit to match Green (0-63 range)
        int r_6 = r << 1;
        int g_6 = g;
        int b_6 = b << 1;
        
        // Calculate luminance Y in 6-bit range
        int y = (r_6 * 77 + g_6 * 150 + b_6 * 29) >> 8;
        
        const int sat_scale = COLOR_SATURATION;
        
        // Apply saturation: Color_out = Y + scale * (Color_in - Y)
        int r_out = y + ((sat_scale * (r_6 - y)) >> 8);
        int g_out = y + ((sat_scale * (g_6 - y)) >> 8);
        int b_out = y + ((sat_scale * (b_6 - y)) >> 8);
        
        // Clamp to 6-bit range (0 to 63)
        if (r_out < 0) r_out = 0; else if (r_out > 63) r_out = 63;
        if (g_out < 0) g_out = 0; else if (g_out > 63) g_out = 63;
        if (b_out < 0) b_out = 0; else if (b_out > 63) b_out = 63;
        
        // Convert back to 5-6-5 range
        r = r_out >> 1;
        g = g_out;
        b = b_out >> 1;
#endif

        correction_lut[i] = (r << 11) | (g << 5) | b;
    }
#endif
}

// 8x8 font bitmaps for characters: N, O, S, I, G, A, L, Space
static const uint8_t font_8x8[8][8] = {
    // 'N'
    {0x82, 0xC2, 0xA2, 0x92, 0x8A, 0x86, 0x82, 0x00},
    // 'O'
    {0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00},
    // 'S'
    {0x3C, 0x42, 0x40, 0x3C, 0x02, 0x42, 0x3C, 0x00},
    // 'I'
    {0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x3E, 0x00},
    // 'G'
    {0x3C, 0x42, 0x40, 0x4E, 0x42, 0x42, 0x3C, 0x00},
    // 'A'
    {0x18, 0x24, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x00},
    // 'L'
    {0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7E, 0x00},
    // Space
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

void draw_char_2x(char c, int x, int y, uint16_t color, uint16_t bg_color) {
    int idx = 7; // Space by default
    if (c == 'N') idx = 0;
    else if (c == 'O') idx = 1;
    else if (c == 'S') idx = 2;
    else if (c == 'I') idx = 3;
    else if (c == 'G') idx = 4;
    else if (c == 'A') idx = 5;
    else if (c == 'L') idx = 6;
    
    for (int row = 0; row < 8; row++) {
        uint8_t bits = font_8x8[idx][row];
        for (int col = 0; col < 8; col++) {
            uint16_t pixel_color = (bits & (0x80 >> col)) ? color : bg_color;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int px = x + col * 2 + dx;
                    int py = y + row * 2 + dy;
                    if (px >= 0 && px < FB_WIDTH && py >= 0 && py < FB_HEIGHT) {
                        framebuffer[py * FB_WIDTH + px] = pixel_color;
                    }
                }
            }
        }
    }
}

void draw_string_2x(const char *str, int x, int y, uint16_t color, uint16_t bg_color) {
    while (*str) {
        draw_char_2x(*str, x, y, color, bg_color);
        x += 16;
        str++;
    }
}

static inline void __scratch_x("") process_spi_byte(uint8_t data, bool is_cmd) {
    if (is_cmd) {
        last_cmd = data; pixel_byte_idx = 0;
        if (last_cmd == 0x2C) {
            cur_x = start_x; cur_y = start_y;
#if ENABLE_DIAGNOSTICS
            diag_frame_count++;
#endif
        }
        return;
    }
    if (last_cmd == 0x2C) {
        if (pixel_byte_idx == 0) { temp_pixel = (data << 8); pixel_byte_idx = 1; }
        else {
            temp_pixel |= data; pixel_byte_idx = 0;
#if ENABLE_BRIGHTNESS_CORRECTION || (defined(COLOR_SATURATION) && (COLOR_SATURATION != 256))
            temp_pixel = correction_lut[temp_pixel];
#endif
            if (cur_x <= end_x && cur_y <= end_y && cur_x < FB_WIDTH && cur_y < FB_HEIGHT)
                framebuffer[cur_y * FB_WIDTH + cur_x] = temp_pixel;
            cur_x++;
            if (cur_x > end_x || cur_x >= FB_WIDTH) {
                cur_x = start_x; cur_y++;
                if (cur_y > end_y || cur_y >= FB_HEIGHT) cur_y = start_y;
            }
        }
        return;
    }
    if (last_cmd == 0x2A) {
        if (pixel_byte_idx == 0) start_x = (data << 8);
        else if (pixel_byte_idx == 1) start_x |= data;
        else if (pixel_byte_idx == 2) end_x = (data << 8);
        else if (pixel_byte_idx == 3) end_x |= data;
        pixel_byte_idx++; 
    } else if (last_cmd == 0x2B) {
        if (pixel_byte_idx == 0) start_y = (data << 8);
        else if (pixel_byte_idx == 1) start_y |= data;
        else if (pixel_byte_idx == 2) end_y = (data << 8);
        else if (pixel_byte_idx == 3) end_y |= data;
        pixel_byte_idx++; 
    }
}

static void __scratch_x("") scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst) {
    uint32_t fb_y = active_line >> 1;
    if (fb_y >= FB_HEIGHT) { memset(dst, 0, 640 * sizeof(uint32_t)); return; }
    const uint16_t *src = &framebuffer[fb_y * FB_WIDTH];

    if (is_4_3) {
        for (int i = 0; i < 40; i++) *dst++ = 0;
        for (int i = 0; i < FB_WIDTH / 4; i++) {
            uint32_t p0 = src[i*4]; uint32_t p1 = src[i*4+1];
            uint32_t p2 = src[i*4+2]; uint32_t p3 = src[i*4+3];
            *dst++ = (p0 << 16) | p0; *dst++ = (p2 << 16) | p1; *dst++ = (p3 << 16) | p3;
        }
        for (int i = 0; i < 40; i++) *dst++ = 0;
    } else {
        const uint32_t *src32 = (const uint32_t *)src;
        for (int i = 0; i < FB_WIDTH / 2; i++) {
            uint32_t p = *src32++;
            uint32_t p0 = p & 0xFFFF; uint32_t p1 = p >> 16;
            *dst++ = (p0 << 16) | p0; *dst++ = (p1 << 16) | p1;
        }
    }
}

// Audio State
static PIO pio_aud;
static uint sm_aud;
static int g_hdmi_audio_frame_counter = 0;
static audio_sample_t packet_samples[4];
static int packet_idx = 0;

static uint32_t i2s_sample_counter = 0;
static uint32_t last_i2s_sample_count = 0;
static uint32_t current_i2s_rate = 32000;
static uint32_t last_time_us = 0;

// DC Filter State (Removes DC offsets from 8-bit unsigned emulators like Gameboy)
static int64_t dc_prev_in_l = 0;
static int64_t dc_y_accum_l = 0;
static int64_t dc_prev_in_r = 0;
static int64_t dc_y_accum_r = 0;
#define DC_ALPHA 65503 // 0.9995 in Q16

// 4-Stage Cascaded Low-Pass Filter State (24 dB/octave roll-off)
static int64_t lpf_y1_l = 0; // Stage 1 state (Q16)
static int64_t lpf_y2_l = 0; // Stage 2 state (Q16)
static int64_t lpf_y3_l = 0; // Stage 3 state (Q16)
static int64_t lpf_y4_l = 0; // Stage 4 state (Q16)
static int64_t lpf_y1_r = 0; // Stage 1 state (Q16)
static int64_t lpf_y2_r = 0; // Stage 2 state (Q16)
static int64_t lpf_y3_r = 0; // Stage 3 state (Q16)
static int64_t lpf_y4_r = 0; // Stage 4 state (Q16)

// Linear Interpolation SRC State
static audio_sample_t prev_audio_sample = {0, 0};
static uint32_t phase_accum = 0;

void audio_task() {
    // Check Toggle Button
    static bool last_btn = true;
    bool btn = gpio_get(ASPECTBUTTON);
    if (!btn && last_btn) is_4_3 = !is_4_3;
    last_btn = btn;

    // Fast Rate Measurement using hardware timer (Independent of video framerate!)
    uint32_t now = time_us_32();
    if (last_time_us == 0) last_time_us = now; // Initialize on first run
    
    if (now - last_time_us >= 250000) { // Update every 250ms
        uint32_t elapsed = now - last_time_us;
        uint32_t new_rate = (uint32_t)(((uint64_t)(i2s_sample_counter - last_i2s_sample_count) * 1000000ULL) / elapsed);
        
        // Snap instantly and reset filters if the rate changed significantly (more than 10%).
        // Otherwise, filter the rate slowly to eliminate timer measurement noise.
        uint32_t old_rate = current_i2s_rate;
        if (old_rate == 0 || abs((int)new_rate - (int)old_rate) > (int)old_rate / 10) {
            current_i2s_rate = new_rate;
            // Clear all filter states to prevent transient pops on emulator swap
            dc_prev_in_l = 0; dc_y_accum_l = 0;
            dc_prev_in_r = 0; dc_y_accum_r = 0;
            lpf_y1_l = 0; lpf_y2_l = 0; lpf_y3_l = 0; lpf_y4_l = 0;
            lpf_y1_r = 0; lpf_y2_r = 0; lpf_y3_r = 0; lpf_y4_r = 0;
        } else {
            current_i2s_rate = (current_i2s_rate * 31 + new_rate) / 32;
        }
        
        // Closed-loop feedback: fine-tune the rate based on HDMI queue level to prevent drift
        uint32_t level = hstx_di_queue_get_level();
        if (level > 140) {
            current_i2s_rate += 1; // Queue too full: increase rate slightly to generate fewer samples
        } else if (level < 110) {
            current_i2s_rate -= 1; // Queue too empty: decrease rate slightly to generate more samples
        }
        
        if (current_i2s_rate < 8000) current_i2s_rate = 8000;
        if (current_i2s_rate > 192000) current_i2s_rate = 192000;
        
        last_i2s_sample_count = i2s_sample_counter;
        last_time_us = now;
    }

    // Select LPF coefficients (for 48kHz domain) based on physical sample rate and video dimensions
    uint32_t lpf_alpha = 65536; // Default to bypass
    if (current_i2s_rate < 24000) {
        lpf_alpha = 17563; // Doom (physical ~22kHz, native 11kHz) -> cutoff ~1200Hz (4-stage LPF at 48kHz)
    } else if (current_i2s_rate < 36000) {
        // Both Gameboy and Lynx run at physical ~32000 Hz.
        // We distinguish Gameboy from Lynx using active video dimensions:
        // - Gameboy: 160x144 (aspect ratio 1.11).
        // - Lynx: 160x102 (aspect ratio 1.57).
        uint32_t active_w = end_x - start_x + 1;
        uint32_t active_h = end_y - start_y + 1;
        
        // If active height is 102 or 204, or aspect ratio is wide (Lynx), bypass LPF.
        // Otherwise (Gameboy 144 or 240, or narrow aspect ratio), apply LPF.
        if (active_h == 102 || active_h == 204 || (active_w * 10 >= active_h * 13)) {
            lpf_alpha = 65536; // Lynx -> bypass
        } else {
            lpf_alpha = 29261; // Gameboy -> cutoff ~2000Hz (4-stage LPF at 48kHz)
        }
    }

    // Print diagnostics every 2 seconds over USB serial
    /*
    static uint32_t last_print_time = 0;
    if (now - last_print_time >= 2000000) {
        last_print_time = now;
        uint32_t active_w = end_x - start_x + 1;
        uint32_t active_h = end_y - start_y + 1;
        printf("[AUDIO] Phys Rate: %lu Hz, LPF Alpha: %lu, Win: %lux%lu\n", 
               current_i2s_rate, lpf_alpha, active_w, active_h);
    }
    */

    // Drain the FIFO (max 8 words) to keep latency minimal without starving video
    int processed = 0;
    while (!pio_sm_is_rx_fifo_empty(pio_aud, sm_aud) && processed < 8) {
        processed++;

        // PIO pushes a single 32-bit word: Left in top 16, Right in bottom 16
        uint32_t val = pio_sm_get(pio_aud, sm_aud);
        i2s_sample_counter++;
        
        audio_sample_t s;
        // Correct Channel Alignment & Native 2x Boost:
        // Shift left by 2 to discard 1-clock delay bit, restore sign bit, and apply native 2x volume boost to standard HDMI line levels.
        int32_t raw_l = (int16_t)((val >> 16) << 2);
        int32_t raw_r = (int16_t)((val & 0xFFFF) << 2);

        // --- High-Precision Q16 DC Blocker (64-bit Overflow-Free) ---
        int64_t x_diff_l = (int64_t)raw_l - dc_prev_in_l;
        dc_prev_in_l = (int64_t)raw_l;
        int64_t feedback_l = (dc_y_accum_l * DC_ALPHA) >> 16;
        dc_y_accum_l = (x_diff_l * 65536LL) + feedback_l;
        int32_t filtered_l = (int32_t)(dc_y_accum_l >> 16);

        int64_t x_diff_r = (int64_t)raw_r - dc_prev_in_r;
        dc_prev_in_r = (int64_t)raw_r;
        int64_t feedback_r = (dc_y_accum_r * DC_ALPHA) >> 16;
        dc_y_accum_r = (x_diff_r * 65536LL) + feedback_r;
        int32_t filtered_r = (int32_t)(dc_y_accum_r >> 16);

        s.left  = (int16_t)filtered_l;
        s.right = (int16_t)filtered_r;

        // --- Overflow-Safe Linear Interpolation SRC (Upsampling to 48000 Hz) ---
        uint32_t phase_inc = ((uint64_t)current_i2s_rate << 16) / 48000;

        while (phase_accum < 0x10000) {
            if (hstx_di_queue_get_level() >= 150) {
                // Buffer full: drop sample safely to prevent Core 1 lockout
                phase_accum += phase_inc;
                continue; 
            }

            // Use int64_t to prevent the math from wrapping around into massive static!
            int64_t diff_l = (int64_t)s.left - (int64_t)prev_audio_sample.left;
            int64_t diff_r = (int64_t)s.right - (int64_t)prev_audio_sample.right;

            int32_t out_l = prev_audio_sample.left + ((diff_l * (int64_t)phase_accum) >> 16);
            int32_t out_r = prev_audio_sample.right + ((diff_r * (int64_t)phase_accum) >> 16);

            // --- 4-Stage LPF (applied to the upsampled 48kHz stream) ---
            if (lpf_alpha < 65536) {
                // Left
                int64_t x_q16_l = (int64_t)out_l * 65536LL;
                lpf_y1_l += ((x_q16_l - lpf_y1_l) * (int64_t)lpf_alpha) >> 16;
                lpf_y2_l += ((lpf_y1_l - lpf_y2_l) * (int64_t)lpf_alpha) >> 16;
                lpf_y3_l += ((lpf_y2_l - lpf_y3_l) * (int64_t)lpf_alpha) >> 16;
                lpf_y4_l += ((lpf_y3_l - lpf_y4_l) * (int64_t)lpf_alpha) >> 16;
                out_l = (int32_t)(lpf_y4_l >> 16);

                // Right
                int64_t x_q16_r = (int64_t)out_r * 65536LL;
                lpf_y1_r += ((x_q16_r - lpf_y1_r) * (int64_t)lpf_alpha) >> 16;
                lpf_y2_r += ((lpf_y1_r - lpf_y2_r) * (int64_t)lpf_alpha) >> 16;
                lpf_y3_r += ((lpf_y2_r - lpf_y3_r) * (int64_t)lpf_alpha) >> 16;
                lpf_y4_r += ((lpf_y3_r - lpf_y4_r) * (int64_t)lpf_alpha) >> 16;
                out_r = (int32_t)(lpf_y4_r >> 16);
            }

#if defined(AUDIO_VOLUME_PERCENT) && (AUDIO_VOLUME_PERCENT != 100)
            out_l = (out_l * AUDIO_VOLUME_PERCENT) / 100;
            out_r = (out_r * AUDIO_VOLUME_PERCENT) / 100;
            if (out_l > 32767) out_l = 32767;
            else if (out_l < -32768) out_l = -32768;
            if (out_r > 32767) out_r = 32767;
            else if (out_r < -32768) out_r = -32768;
#endif

            packet_samples[packet_idx].left  = (int16_t)out_l;
            packet_samples[packet_idx].right = (int16_t)out_r;
            packet_idx++;
            
            if (packet_idx == 4) {
                hstx_packet_t packet;
                g_hdmi_audio_frame_counter = hstx_packet_set_audio_samples(&packet, packet_samples, 4, g_hdmi_audio_frame_counter);
                hstx_data_island_t island;
                hstx_encode_data_island(&island, &packet, false, true);
                hstx_di_queue_push(&island);
                packet_idx = 0;
            }
            
            phase_accum += phase_inc;
        }
        
        // Input sample fully consumed. Shift phase and save.
        phase_accum -= 0x10000;
        prev_audio_sample = s;
    }
}

void core1_main() {
    video_output_init(640, 480);
    video_output_set_scanline_callback(scanline_callback);
    pico_hdmi_set_audio_sample_rate(48000);
    video_output_set_background_task(audio_task);
    video_output_core1_run();
}

#define SPI_BUF_SIZE 4096
static uint32_t spi_raw_buf[SPI_BUF_SIZE] __attribute__((aligned(16384)));

int main() {
    vreg_set_voltage(VREG_VOLTAGE_LEVEL);
    set_sys_clock_khz(252000, true);
    clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, 252000000, 2);
    
    stdio_init_all(); init_tables();
#if ENABLE_DIAGNOSTICS
    diag_last_time = time_us_64();
#endif
    
    // Give DMA maximum priority
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    gpio_init(ASPECTBUTTON); gpio_set_dir(ASPECTBUTTON, GPIO_IN); gpio_pull_up(ASPECTBUTTON);
    
    // Noise Rejection
    gpio_init(4); gpio_init(5); gpio_init(6);
    gpio_disable_pulls(4); gpio_disable_pulls(5); gpio_disable_pulls(6);
    gpio_set_input_hysteresis_enabled(4, true);
    gpio_set_input_hysteresis_enabled(5, true);
    gpio_set_input_hysteresis_enabled(6, true);
    
    gpio_pull_up(3); 
    for (int i = 0; i < FB_SIZE; i++) framebuffer[i] = 0x0000;

    // Audio PIO
    pio_aud = pio1;
    uint offset_aud = pio_add_program(pio_aud, &i2s_capture_program);
    sm_aud = pio_claim_unused_sm(pio_aud, true);
    pio_sm_config c_aud = i2s_capture_program_get_default_config(offset_aud);
    sm_config_set_in_pins(&c_aud, 4); 
    pio_gpio_init(pio_aud, 4); pio_gpio_init(pio_aud, 5); pio_gpio_init(pio_aud, 6);
    pio_sm_set_consecutive_pindirs(pio_aud, sm_aud, 4, 3, false);
    sm_config_set_in_shift(&c_aud, false, false, 32); 
    sm_config_set_fifo_join(&c_aud, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio_aud, sm_aud, offset_aud, &c_aud);
    pio_sm_set_enabled(pio_aud, sm_aud, true);
    
    multicore_launch_core1(core1_main);
    sleep_ms(100);

    // Video PIO
    PIO pio_vid = pio0;
    uint offset_vid = pio_add_program(pio_vid, &spi_st7789_program);
    uint sm_vid = pio_claim_unused_sm(pio_vid, true);
    pio_sm_config c_vid = spi_st7789_program_get_default_config(offset_vid);
    sm_config_set_in_pins(&c_vid, 0); 
    pio_gpio_init(pio_vid, 0); pio_gpio_init(pio_vid, 1); pio_gpio_init(pio_vid, 2); pio_gpio_init(pio_vid, 3);
    pio_sm_set_consecutive_pindirs(pio_vid, sm_vid, 0, 3, false);
    sm_config_set_in_shift(&c_vid, false, true, 9); 
    sm_config_set_fifo_join(&c_vid, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio_vid, sm_vid, offset_vid, &c_vid);

    // Video DMA
    int dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_ring(&dc, true, 14); 
    channel_config_set_dreq(&dc, pio_get_dreq(pio_vid, sm_vid, false));
    dma_channel_configure(dma_chan, &dc, spi_raw_buf, &pio_vid->rxf[sm_vid], 0xFFFFFFFF, true);

    while (!gpio_get(3)) tight_loop_contents();
    pio_sm_set_enabled(pio_vid, sm_vid, true);

    uint32_t last_idx = 0;
    uint64_t last_active_time = time_us_64();
    uint64_t last_data_time = time_us_64();
    bool has_signal = true;

    while (1) {
        uint32_t curr_idx = (dma_hw->ch[dma_chan].write_addr - (uint32_t)spi_raw_buf) / 4;
        if (last_idx != curr_idx) {
#if ENABLE_DIAGNOSTICS
            uint32_t count = (curr_idx - last_idx + SPI_BUF_SIZE) % SPI_BUF_SIZE;
            if (!in_burst) {
                burst_start_time = time_us_64();
                in_burst = true;
                burst_bytes = 0;
            }
            burst_bytes += count;
            burst_last_data_time = time_us_64();
#endif
            last_active_time = time_us_64();
            last_data_time = time_us_64();
            if (!has_signal) {
                has_signal = true;
                memset(framebuffer, 0, sizeof(framebuffer));
            }
            while (last_idx != curr_idx) {
                uint32_t val = spi_raw_buf[last_idx];
                process_spi_byte(val & 0xFF, !((val >> 8) & 1));
                last_idx = (last_idx + 1) % SPI_BUF_SIZE;
            }
        } else if (gpio_get(3) && (time_us_64() - last_active_time > 100000ULL)) {
            pio_sm_restart(pio_vid, sm_vid); pio_sm_exec(pio_vid, sm_vid, pio_encode_jmp(offset_vid));
            last_idx = (dma_hw->ch[dma_chan].write_addr - (uint32_t)spi_raw_buf) / 4;
            last_cmd = 0; pixel_byte_idx = 0; last_active_time = time_us_64();
        }

        // Check for signal loss (10 seconds of no SPI data)
        if (has_signal && (time_us_64() - last_data_time > 10000000ULL)) {
            has_signal = false;
            memset(framebuffer, 0, sizeof(framebuffer));
            draw_string_2x("NO SIGNAL", 88, 112, 0xF800, 0x0000); // Red text on black background
        }

#if ENABLE_DIAGNOSTICS
        if (in_burst && (time_us_64() - burst_last_data_time > 1000ULL)) {
            uint64_t dt = burst_last_data_time - burst_start_time;
            if (dt > 50) {
                diag_active_transfer_time += dt;
                diag_active_bytes += burst_bytes;
            }
            diag_bytes_received += burst_bytes;
            in_burst = false;
        }

        if (time_us_64() - diag_last_time > 1000000ULL) {
            diag_last_time = time_us_64();
            
            float throughput_mb = (float)diag_bytes_received / (1024.0f * 1024.0f);
            float fps = (float)diag_frame_count;
            
            float clk_mhz = 0.0f;
            if (diag_active_transfer_time > 0) {
                clk_mhz = ((float)diag_active_bytes * 9.0f) / (float)diag_active_transfer_time;
            }
            
            uint32_t active_w = (end_x >= start_x) ? (end_x - start_x + 1) : 0;
            uint32_t active_h = (end_y >= start_y) ? (end_y - start_y + 1) : 0;
            
            char lpf_str[48];
            if (current_i2s_rate < 24000) {
                sprintf(lpf_str, "ACTIVE (Cutoff ~1200Hz)");
            } else if (current_i2s_rate < 36000) {
                if (active_h == 102 || active_h == 204 || (active_w * 10 >= active_h * 13)) {
                    sprintf(lpf_str, "BYPASS (Atari Lynx)");
                } else {
                    sprintf(lpf_str, "ACTIVE (Cutoff ~2000Hz)");
                }
            } else {
                sprintf(lpf_str, "BYPASS");
            }
            
            printf("\n----------------------------------------\n");
            printf("[SPI-to-HDMI Diagnostics Log]\n");
            if (!has_signal) {
                printf("Signal Status      : NO SIGNAL\n");
                printf("Warning: No SPI video activity detected. Check wiring (CS, SCLK, MOSI).\n");
            } else {
                if (diag_bytes_received > 0 && fps == 0.0f) {
                    printf("Signal Status      : UNSTABLE / INVALID\n");
                } else {
                    printf("Signal Status      : ACTIVE\n");
                }
                printf("SPI Clock (Est)    : %.1f MHz\n", clk_mhz);
                printf("SPI Throughput     : %.2f MB/s\n", throughput_mb);
                printf("Parsed Resolution  : %lux%lu\n", active_w, active_h);
                printf("Video Frame Rate   : %.1f FPS\n", fps);
                printf("Aspect Ratio Mode  : %s\n", is_4_3 ? "4:3 (Pillarbox)" : "16:9 (Stretch)");
                printf("Audio Sample Rate  : %lu Hz\n", current_i2s_rate);
                printf("Audio LPF Status   : %s\n", lpf_str);
                
                if (clk_mhz > 45.0f) {
                    printf("Warning: SPI clock (%.1f MHz) exceeds 40MHz limit. May cause corruption.\n", clk_mhz);
                }
                if (diag_bytes_received > 0 && fps == 0.0f) {
                    printf("Warning: SPI throughput detected but no valid frames received. Check signal/protocol.\n");
                }
            }
            printf("----------------------------------------\n");
            
            diag_bytes_received = 0;
            diag_frame_count = 0;
            diag_active_bytes = 0;
            diag_active_transfer_time = 0;
        }
#endif
    }
}
