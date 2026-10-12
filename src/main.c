
#include <stdio.h>
#include <stdint.h>
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
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"

#include "audio_profiles.h"

#include "spi_capture.pio.h"
#include "i2s_audio.pio.h"

// -----------------------------------------------------------------------------
// Olimex RP2040-PICO-PC Pin Map
//
// This build targets the Olimex RP2040-PICO-PC carrier board with an RP2350
// Pico 2 module plugged in. The board's onboard HDMI connector is hardwired
// to GPIO 12-19 with 270-ohm series termination and the same TMDS polarity
// convention as the reference breakout (even GPIO = minus, odd GPIO = plus),
// but it routes red (TMDS2) to GPIO 16/17 and green (TMDS1) to GPIO 18/19;
// the HSTX crossbar mapping in video_output.c is swapped accordingly.
//
// Host signals are fed in through the board's connectors:
//   SPI video (UEXT):  MOSI = GP4 (UEXT pin 7), DC = GP5 (UEXT pin 10),
//                      SCLK = GP6 (UEXT pin 9), CS = GP7 (UEXT pin 8)
//   I2S audio (UEXT):  DAT  = GP8 (UEXT pin 6), BCK = GP9 (UEXT pin 5),
//                      WS   = GP20 (UEXT pin 3)
//   Aspect button:     GP3 (I2C/QWST connector SCL pin) to GND
//
// Leave the microSD slot empty: GP4-7 are shared with it.
// -----------------------------------------------------------------------------

// Brightness & Gamma Correction Configuration
#define ENABLE_BRIGHTNESS_CORRECTION 1
#define BRIGHTNESS_GAMMA             0.9f   // Exponent: < 1.0 brightens shadows, > 1.0 darkens
#define BRIGHTNESS_GAIN              1.0f    // Multiplier: scales overall brightness
#define BRIGHTNESS_OFFSET            2       // Flat offset: lifts absolute black level (0-31 scale)
#define COLOR_SATURATION             256     // Saturation: 256 = default (1.0), < 256 = muted, > 256 = boosted

// Core Voltage Regulator Configuration (Default: VREG_VOLTAGE_1_20 for 1.20V max stability)
#define VREG_VOLTAGE_LEVEL           VREG_VOLTAGE_1_20

// Audio Volume Gain Configuration (Percentage: 100 = 100%/default boosted baseline, > 100 = additional boost)
#define AUDIO_VOLUME_PERCENT         100

#if defined(COLOR_SATURATION) && (COLOR_SATURATION != 256)
static uint16_t correction_lut[65536];
#elif ENABLE_BRIGHTNESS_CORRECTION
// Brightness/gamma correction is separable by channel. Combining red and
// green keeps the hot pixel path to two lookups while avoiding a 128 KiB LUT.
static uint16_t correction_rg_lut[2048];
static uint16_t correction_b_lut[32];
#endif

#define FB_WIDTH  320
#define FB_HEIGHT 240
#define FB_SIZE   (FB_WIDTH * FB_HEIGHT)

// Aspect Ratio Toggle Button GPIO pin (Olimex board: I2C/QWST connector SCL pin)
#define ASPECTBUTTON 3
#define ASPECT_DEBOUNCE_US 30000u

// CMake's SPI_TO_HDMI_DIAGNOSTICS option controls both this code and USB stdio.
#ifndef ENABLE_DIAGNOSTICS
#define ENABLE_DIAGNOSTICS 0
#endif

#if ENABLE_DIAGNOSTICS
static volatile uint32_t diag_bytes_received = 0;
static volatile uint32_t diag_frame_count = 0;
static uint64_t diag_active_transfer_time = 0; // Cumulative microseconds spent in active transfer
static uint32_t diag_active_bytes = 0;          // Cumulative bytes received during active transfer
static uint64_t burst_start_time = 0;
static uint64_t burst_last_data_time = 0;
static uint32_t burst_bytes = 0;
static bool in_burst = false;
static uint32_t diag_max_dma_backlog = 0;
static uint32_t diag_invalid_windows = 0;
static uint32_t diag_window_changes = 0;
static volatile uint32_t diag_audio_frames = 0;
static volatile uint32_t diag_audio_queue_throttles = 0;
static volatile uint32_t diag_raw_audio_rate = 0;
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

static void init_correction_tables(void) {
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

#if defined(COLOR_SATURATION) && (COLOR_SATURATION != 256)
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
#elif ENABLE_BRIGHTNESS_CORRECTION
    // With default saturation, channel correction is independent. The upper
    // 11 input bits contain red and green; blue occupies the lower 5 bits.
    for (int i = 0; i < 2048; i++) {
        uint16_t r = (i >> 6) & 0x1F;
        uint16_t g = i & 0x3F;
        correction_rg_lut[i] = (uint16_t)((temp_lut_r[r] << 11) | (temp_lut_g[g] << 5));
    }
    for (int i = 0; i < 32; i++) {
        correction_b_lut[i] = temp_lut_b[i];
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
#if defined(COLOR_SATURATION) && (COLOR_SATURATION != 256)
            temp_pixel = correction_lut[temp_pixel];
#elif ENABLE_BRIGHTNESS_CORRECTION
            temp_pixel = correction_rg_lut[temp_pixel >> 5] | correction_b_lut[temp_pixel & 0x1F];
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
#if ENABLE_DIAGNOSTICS
        if (pixel_byte_idx == 4 && (start_x > end_x || end_x >= FB_WIDTH)) {
            diag_invalid_windows++;
        }
#endif
    } else if (last_cmd == 0x2B) {
        if (pixel_byte_idx == 0) start_y = (data << 8);
        else if (pixel_byte_idx == 1) start_y |= data;
        else if (pixel_byte_idx == 2) end_y = (data << 8);
        else if (pixel_byte_idx == 3) end_y |= data;
        pixel_byte_idx++; 
#if ENABLE_DIAGNOSTICS
        if (pixel_byte_idx == 4) {
            diag_window_changes++;
            if (start_y > end_y || end_y >= FB_HEIGHT) {
                diag_invalid_windows++;
            }
        }
#endif
    }
}

static void __scratch_x("") scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst) {
    (void)v_scanline;
    uint32_t fb_y = active_line >> 1;
    if (fb_y >= FB_HEIGHT) { memset(dst, 0, 640 * sizeof(uint16_t)); return; }

    // Each source row is emitted twice for 240-to-480-line doubling. The line
    // buffer still contains the even line, so the odd partner needs no work.
    if (active_line & 1u) return;

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
static uint32_t phase_inc = (uint32_t)(((uint64_t)32000u << 16) / 48000u);

static inline int16_t clamp_i16(int32_t value) {
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

void audio_task() {
    uint32_t now = time_us_32();

    // Debounce the aspect-ratio button and toggle only on a stable press.
    static bool raw_btn_last = true;
    static bool stable_btn = true;
    static uint32_t raw_btn_changed_at = 0;
    bool btn = gpio_get(ASPECTBUTTON);
    if (btn != raw_btn_last) {
        raw_btn_last = btn;
        raw_btn_changed_at = now;
    } else if (btn != stable_btn && (uint32_t)(now - raw_btn_changed_at) >= ASPECT_DEBOUNCE_US) {
        stable_btn = btn;
        if (!stable_btn) is_4_3 = !is_4_3;
    }

    // Fast Rate Measurement using hardware timer (Independent of video framerate!)
    if (last_time_us == 0) last_time_us = now; // Initialize on first run
    
    if (now - last_time_us >= 250000) { // Update every 250ms
        uint32_t elapsed = now - last_time_us;
        uint32_t samples_since = i2s_sample_counter - last_i2s_sample_count;

        // Keep the last valid rate while I2S is idle. Setting the rate to zero
        // would both underflow the queue correction and produce a zero SRC step.
        if (samples_since != 0) {
            uint32_t new_rate = (uint32_t)(((uint64_t)samples_since * 1000000ULL) / elapsed);
#if ENABLE_DIAGNOSTICS
            diag_raw_audio_rate = new_rate;
#endif

            // Snap instantly and reset filters if the rate changed significantly (more than 10%).
            // Otherwise, filter the rate slowly to eliminate timer measurement noise.
            uint32_t old_rate = current_i2s_rate;
            if (abs((int)new_rate - (int)old_rate) > (int)old_rate / 10) {
                current_i2s_rate = new_rate;
                // Clear all filter states to prevent transient pops on emulator swap
                dc_prev_in_l = 0; dc_y_accum_l = 0;
                dc_prev_in_r = 0; dc_y_accum_r = 0;
                lpf_y1_l = 0; lpf_y2_l = 0; lpf_y3_l = 0; lpf_y4_l = 0;
                lpf_y1_r = 0; lpf_y2_r = 0; lpf_y3_r = 0; lpf_y4_r = 0;
            } else {
                current_i2s_rate = (current_i2s_rate * 31 + new_rate) / 32;
            }

            if (current_i2s_rate < 8000) current_i2s_rate = 8000;
            if (current_i2s_rate > 192000) current_i2s_rate = 192000;

            // Closed-loop feedback: fine-tune the rate based on HDMI queue level to prevent drift.
            uint32_t level = hstx_di_queue_get_level();
            if (level > 140 && current_i2s_rate < 192000) {
                current_i2s_rate += 1; // Queue too full: increase rate slightly to generate fewer samples
            } else if (level < 110 && current_i2s_rate > 8000) {
                current_i2s_rate -= 1; // Queue too empty: decrease rate slightly to generate more samples
            }

            // The input-rate estimate changes only here, so calculate the SRC
            // step once per update instead of once per captured audio sample.
            phase_inc = (uint32_t)(((uint64_t)current_i2s_rate << 16) / 48000u);
        }
        
        last_i2s_sample_count = i2s_sample_counter;
        last_time_us = now;
    }

    // Button and rate housekeeping must continue while audio is idle, but the
    // filter selection and sample pipeline have no work until a word arrives.
    if (pio_sm_is_rx_fifo_empty(pio_aud, sm_aud)) {
        return;
    }

    uint32_t active_w = end_x - start_x + 1;
    uint32_t active_h = end_y - start_y + 1;

    audio_profile_t audio_profile =
        audio_profile_select(current_i2s_rate, active_w, active_h);
    uint32_t lpf_alpha = audio_filter_alpha(audio_profile.filter);

    // Drain the FIFO (max 8 words) to keep latency minimal without starving video
    int processed = 0;
    while (!pio_sm_is_rx_fifo_empty(pio_aud, sm_aud) && processed < 8) {
        processed++;

        // PIO pushes a single 32-bit word: Left in top 16, Right in bottom 16
        uint32_t val = pio_sm_get(pio_aud, sm_aud);
        i2s_sample_counter++;
#if ENABLE_DIAGNOSTICS
        diag_audio_frames++;
#endif

        uint16_t captured_l = (uint16_t)(val >> 16);
        uint16_t captured_r = (uint16_t)val;

        audio_sample_t s;
        int32_t raw_l = (int16_t)(captured_l << audio_profile.alignment_shift);
        int32_t raw_r = (int16_t)(captured_r << audio_profile.alignment_shift);

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

        s.left  = clamp_i16(filtered_l);
        s.right = clamp_i16(filtered_r);

        // --- Overflow-Safe Linear Interpolation SRC (Upsampling to 48000 Hz) ---
        while (phase_accum < 0x10000) {
            if (hstx_di_queue_get_level() >= 150) {
                // Buffer full: drop sample safely to prevent Core 1 lockout
#if ENABLE_DIAGNOSTICS
                diag_audio_queue_throttles++;
#endif
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
            out_l = (int32_t)(((int64_t)out_l * AUDIO_VOLUME_PERCENT) / 100);
            out_r = (int32_t)(((int64_t)out_r * AUDIO_VOLUME_PERCENT) / 100);
#endif

            packet_samples[packet_idx].left  = clamp_i16(out_l);
            packet_samples[packet_idx].right = clamp_i16(out_r);
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

#define CORE1_READY_MAGIC 0x48444d49u

void core1_main() {
    video_output_init(640, 480);
    video_output_set_scanline_callback(scanline_callback);
    pico_hdmi_set_audio_sample_rate(48000);
    video_output_set_background_task(audio_task);
    multicore_fifo_push_blocking(CORE1_READY_MAGIC);
    video_output_core1_run();
}

#define SPI_BUF_SIZE 4096
static uint32_t spi_raw_buf[SPI_BUF_SIZE] __attribute__((aligned(16384)));

int main() {
    vreg_set_voltage(VREG_VOLTAGE_LEVEL);
    set_sys_clock_khz(252000, true);
    clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, 252000000, 2);
    
    stdio_init_all(); init_correction_tables();
    
    // Give DMA maximum priority
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    gpio_init(ASPECTBUTTON); gpio_set_dir(ASPECTBUTTON, GPIO_IN); gpio_pull_up(ASPECTBUTTON);
    
    // Noise Rejection
    gpio_init(8); gpio_init(9); gpio_init(20);
    gpio_disable_pulls(8); gpio_disable_pulls(9); gpio_disable_pulls(20);
    gpio_set_input_hysteresis_enabled(8, true);
    gpio_set_input_hysteresis_enabled(9, true);
    gpio_set_input_hysteresis_enabled(20, true);
    
    gpio_pull_up(7); 
    for (int i = 0; i < FB_SIZE; i++) framebuffer[i] = 0x0000;

    // Audio PIO
    pio_aud = pio1;
    uint offset_aud = pio_add_program(pio_aud, &i2s_capture_program);
    sm_aud = pio_claim_unused_sm(pio_aud, true);
    pio_sm_config c_aud = i2s_capture_program_get_default_config(offset_aud);
    sm_config_set_in_pins(&c_aud, 8); 
    pio_gpio_init(pio_aud, 8); pio_gpio_init(pio_aud, 9); pio_gpio_init(pio_aud, 20);
    pio_sm_set_pindirs_with_mask64(pio_aud, sm_aud, 0, (1ull << 8) | (1ull << 9) | (1ull << 20));
    sm_config_set_in_shift(&c_aud, false, false, 32); 
    sm_config_set_fifo_join(&c_aud, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio_aud, sm_aud, offset_aud, &c_aud);
    pio_sm_set_enabled(pio_aud, sm_aud, true);
    
    multicore_launch_core1(core1_main);
    if (multicore_fifo_pop_blocking() != CORE1_READY_MAGIC) {
        panic("Unexpected core 1 startup response");
    }

    // Video PIO
    PIO pio_vid = pio0;
    uint offset_vid = pio_add_program(pio_vid, &spi_st7789_program);
    uint sm_vid = pio_claim_unused_sm(pio_vid, true);
    pio_sm_config c_vid = spi_st7789_program_get_default_config(offset_vid);
    sm_config_set_in_pins(&c_vid, 4); 
    pio_gpio_init(pio_vid, 4); pio_gpio_init(pio_vid, 5); pio_gpio_init(pio_vid, 6); pio_gpio_init(pio_vid, 7);
    pio_sm_set_consecutive_pindirs(pio_vid, sm_vid, 4, 4, false);
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

    while (!gpio_get(7)) tight_loop_contents();
    pio_sm_set_enabled(pio_vid, sm_vid, true);

    uint32_t last_idx = 0;
    uint64_t now_us = time_us_64();
    uint64_t last_active_time = now_us;
    uint64_t last_data_time = now_us;
    bool has_signal = true;

#if ENABLE_DIAGNOSTICS
    const uint64_t diag_event_period_us = 250000ULL;
    const uint64_t diag_health_period_us = 10000000ULL;
    const uint64_t diag_window_stable_us = 500000ULL;
    const uint64_t diag_audio_warning_stable_us = 1000000ULL;
    uint64_t diag_last_event_time = now_us;
    uint64_t diag_health_start_time = now_us;
    uint64_t diag_window_candidate_since = now_us;
    uint64_t diag_bad_audio_rate_since = 0;
    bool diag_boot_reported = false;
    bool diag_seen_video_data = false;
    bool diag_video_active = false;
    bool diag_audio_active = false;
    bool diag_last_aspect = is_4_3;
    bool diag_warned_video_rx_stall = false;
    bool diag_warned_audio_rx_stall = false;
    bool diag_warned_invalid_window = false;
    bool diag_warned_audio_throttle = false;
    bool diag_warned_audio_rate = false;
    bool diag_warned_spi_clock = false;
    uint16_t diag_window_candidate_x0 = UINT16_MAX;
    uint16_t diag_window_candidate_y0 = UINT16_MAX;
    uint16_t diag_window_candidate_x1 = UINT16_MAX;
    uint16_t diag_window_candidate_y1 = UINT16_MAX;
    uint16_t diag_reported_x0 = UINT16_MAX;
    uint16_t diag_reported_y0 = UINT16_MAX;
    uint16_t diag_reported_x1 = UINT16_MAX;
    uint16_t diag_reported_y1 = UINT16_MAX;
    uint32_t diag_last_audio_frames = 0;
    uint32_t diag_last_reported_rate = 0;
    uint32_t diag_last_health_throttles = 0;
    uint32_t diag_video_rx_stalls = 0;
    uint32_t diag_audio_rx_stalls = 0;
    audio_filter_t diag_last_filter = AUDIO_FILTER_BYPASS;
    uint32_t diag_last_alignment_shift = 0;
    const char *diag_last_profile_name = NULL;
#endif

    while (1) {
        now_us = time_us_64();
        uint32_t curr_idx = (dma_hw->ch[dma_chan].write_addr - (uint32_t)spi_raw_buf) / 4;
        if (last_idx != curr_idx) {
#if ENABLE_DIAGNOSTICS
            uint32_t count = (curr_idx - last_idx + SPI_BUF_SIZE) % SPI_BUF_SIZE;
            if (count > diag_max_dma_backlog) diag_max_dma_backlog = count;
            diag_seen_video_data = true;
            if (!in_burst) {
                burst_start_time = now_us;
                in_burst = true;
                burst_bytes = 0;
            }
            burst_bytes += count;
            burst_last_data_time = now_us;
#endif
            last_active_time = now_us;
            last_data_time = now_us;
            if (!has_signal) {
                has_signal = true;
                memset(framebuffer, 0, sizeof(framebuffer));
            }
            while (last_idx != curr_idx) {
                uint32_t val = spi_raw_buf[last_idx];
                process_spi_byte(val & 0xFF, !((val >> 8) & 1));
                last_idx = (last_idx + 1) % SPI_BUF_SIZE;
            }
        } else if (gpio_get(7) && (now_us - last_active_time > 100000ULL)) {
            pio_sm_restart(pio_vid, sm_vid); pio_sm_exec(pio_vid, sm_vid, pio_encode_jmp(offset_vid));
            last_idx = (dma_hw->ch[dma_chan].write_addr - (uint32_t)spi_raw_buf) / 4;
            last_cmd = 0; pixel_byte_idx = 0; last_active_time = now_us;
        }

        // Check for signal loss (10 seconds of no SPI data)
        if (has_signal && (now_us - last_data_time > 10000000ULL)) {
            has_signal = false;
            memset(framebuffer, 0, sizeof(framebuffer));
            draw_string_2x("NO SIGNAL", 88, 112, 0xF800, 0x0000); // Red text on black background
        }

#if ENABLE_DIAGNOSTICS
        if (in_burst && (now_us - burst_last_data_time > 1000ULL)) {
            uint64_t dt = burst_last_data_time - burst_start_time;
            if (dt > 50) {
                diag_active_transfer_time += dt;
                diag_active_bytes += burst_bytes;
            }
            diag_bytes_received += burst_bytes;
            in_burst = false;
        }

        // USB output is permitted only in a genuine input gap. If a source has
        // continuous SPI traffic, defer diagnostics rather than risk the DMA
        // ring wrapping while core 0 is formatting or transmitting text.
        bool diag_spi_idle = now_us - last_active_time >= 2000ULL;

        if (diag_spi_idle && now_us - diag_last_event_time >= diag_event_period_us) {
            diag_last_event_time = now_us;

            uint32_t active_w = (end_x >= start_x) ? (end_x - start_x + 1) : 0;
            uint32_t active_h = (end_y >= start_y) ? (end_y - start_y + 1) : 0;
            bool video_active_now = diag_seen_video_data && has_signal;
            uint32_t audio_frames_now = diag_audio_frames;
            bool audio_active_now = audio_frames_now != diag_last_audio_frames;
            diag_last_audio_frames = audio_frames_now;

            if (!diag_boot_reported) {
                printf("BOOT diagnostics=event-driven heartbeat=10s\n");
                if (!video_active_now) printf("VIDEO waiting-for-signal\n");
                diag_boot_reported = true;
            }

            if (video_active_now != diag_video_active) {
                if (video_active_now) {
                    printf("VIDEO acquired\n");
                    diag_window_candidate_x0 = start_x;
                    diag_window_candidate_y0 = start_y;
                    diag_window_candidate_x1 = end_x;
                    diag_window_candidate_y1 = end_y;
                    diag_window_candidate_since = now_us;
                    diag_reported_x0 = UINT16_MAX;
                    diag_reported_y0 = UINT16_MAX;
                    diag_reported_x1 = UINT16_MAX;
                    diag_reported_y1 = UINT16_MAX;
                } else {
                    printf("VIDEO lost\n");
                }
                diag_video_active = video_active_now;
            }

            if (video_active_now) {
                bool candidate_changed =
                    start_x != diag_window_candidate_x0 ||
                    start_y != diag_window_candidate_y0 ||
                    end_x != diag_window_candidate_x1 ||
                    end_y != diag_window_candidate_y1;
                if (candidate_changed) {
                    diag_window_candidate_x0 = start_x;
                    diag_window_candidate_y0 = start_y;
                    diag_window_candidate_x1 = end_x;
                    diag_window_candidate_y1 = end_y;
                    diag_window_candidate_since = now_us;
                }

                bool candidate_unreported =
                    diag_window_candidate_x0 != diag_reported_x0 ||
                    diag_window_candidate_y0 != diag_reported_y0 ||
                    diag_window_candidate_x1 != diag_reported_x1 ||
                    diag_window_candidate_y1 != diag_reported_y1;
                if (candidate_unreported &&
                    now_us - diag_window_candidate_since >= diag_window_stable_us) {
                    uint32_t stable_w = diag_window_candidate_x1 >= diag_window_candidate_x0
                        ? diag_window_candidate_x1 - diag_window_candidate_x0 + 1u : 0u;
                    uint32_t stable_h = diag_window_candidate_y1 >= diag_window_candidate_y0
                        ? diag_window_candidate_y1 - diag_window_candidate_y0 + 1u : 0u;
                    printf("VIDEO stable-window=%lu,%lu-%lu,%lu size=%lux%lu\n",
                           (unsigned long)diag_window_candidate_x0,
                           (unsigned long)diag_window_candidate_y0,
                           (unsigned long)diag_window_candidate_x1,
                           (unsigned long)diag_window_candidate_y1,
                           (unsigned long)stable_w, (unsigned long)stable_h);
                    diag_reported_x0 = diag_window_candidate_x0;
                    diag_reported_y0 = diag_window_candidate_y0;
                    diag_reported_x1 = diag_window_candidate_x1;
                    diag_reported_y1 = diag_window_candidate_y1;
                }
            }

            if (is_4_3 != diag_last_aspect) {
                printf("VIDEO output-aspect=%s\n", is_4_3 ? "4:3" : "16:9");
                diag_last_aspect = is_4_3;
            }

            // Base diagnostic profile reporting on the last stable window. This
            // suppresses launcher/loading-window noise without changing the
            // live profile used by the audio processing path.
            uint32_t profile_w = active_w;
            uint32_t profile_h = active_h;
            if (diag_reported_x0 != UINT16_MAX) {
                profile_w = diag_reported_x1 >= diag_reported_x0
                    ? diag_reported_x1 - diag_reported_x0 + 1u : 0u;
                profile_h = diag_reported_y1 >= diag_reported_y0
                    ? diag_reported_y1 - diag_reported_y0 + 1u : 0u;
            }
            audio_profile_t diag_audio_profile =
                audio_profile_select(current_i2s_rate, profile_w, profile_h);

            uint32_t rate_delta = current_i2s_rate > diag_last_reported_rate
                ? current_i2s_rate - diag_last_reported_rate
                : diag_last_reported_rate - current_i2s_rate;
            uint32_t rate_threshold = diag_last_reported_rate / 20u;
            if (rate_threshold < 250u) rate_threshold = 250u;
            bool profile_changed = diag_last_profile_name == NULL ||
                strcmp(diag_last_profile_name, diag_audio_profile.name) != 0 ||
                diag_last_filter != diag_audio_profile.filter ||
                diag_last_alignment_shift != diag_audio_profile.alignment_shift;

            if (audio_active_now && (!diag_audio_active || profile_changed ||
                                     rate_delta >= rate_threshold)) {
                uint32_t gain = 1u << (diag_audio_profile.alignment_shift - 1u);
                printf("AUDIO active rate=%lu profile=%s gain=%lux lpf=%s\n",
                       (unsigned long)current_i2s_rate, diag_audio_profile.name,
                       (unsigned long)gain,
                       audio_filter_description(diag_audio_profile.filter));
                diag_last_reported_rate = current_i2s_rate;
                diag_last_profile_name = diag_audio_profile.name;
                diag_last_filter = diag_audio_profile.filter;
                diag_last_alignment_shift = diag_audio_profile.alignment_shift;
            } else if (!audio_active_now && diag_audio_active) {
                printf("AUDIO idle last-rate=%lu\n", (unsigned long)current_i2s_rate);
            }
            diag_audio_active = audio_active_now;

            uint32_t video_rx_stall_mask =
                1u << (PIO_FDEBUG_RXSTALL_LSB + sm_vid);
            if ((pio_vid->fdebug & video_rx_stall_mask) != 0) {
                pio_vid->fdebug = video_rx_stall_mask;
                diag_video_rx_stalls++;
                if (!diag_warned_video_rx_stall) {
                    printf("WARN video-pio-rx-stall\n");
                    diag_warned_video_rx_stall = true;
                }
            }

            uint32_t audio_rx_stall_mask =
                1u << (PIO_FDEBUG_RXSTALL_LSB + sm_aud);
            if ((pio_aud->fdebug & audio_rx_stall_mask) != 0) {
                pio_aud->fdebug = audio_rx_stall_mask;
                diag_audio_rx_stalls++;
                if (!diag_warned_audio_rx_stall) {
                    printf("WARN audio-pio-rx-stall\n");
                    diag_warned_audio_rx_stall = true;
                }
            }

            if (diag_invalid_windows != 0 && !diag_warned_invalid_window) {
                printf("WARN video-window-outside-framebuffer count=%lu\n",
                       (unsigned long)diag_invalid_windows);
                diag_warned_invalid_window = true;
            }
            if (diag_audio_queue_throttles != 0 && !diag_warned_audio_throttle) {
                printf("WARN audio-hdmi-queue-throttled count=%lu\n",
                       (unsigned long)diag_audio_queue_throttles);
                diag_warned_audio_throttle = true;
            }
            if (audio_active_now && diag_raw_audio_rate != 0 &&
                (diag_raw_audio_rate < 8000u || diag_raw_audio_rate > 192000u)) {
                if (diag_bad_audio_rate_since == 0) {
                    diag_bad_audio_rate_since = now_us;
                } else if (!diag_warned_audio_rate &&
                           now_us - diag_bad_audio_rate_since >=
                               diag_audio_warning_stable_us) {
                    printf("WARN audio-rate-out-of-range raw=%luHz\n",
                           (unsigned long)diag_raw_audio_rate);
                    diag_warned_audio_rate = true;
                }
            } else {
                diag_bad_audio_rate_since = 0;
                diag_warned_audio_rate = false;
            }
        }

        if (diag_spi_idle && now_us - diag_health_start_time >= diag_health_period_us) {
            uint64_t elapsed_us = now_us - diag_health_start_time;
            uint32_t spi_clock_tenths = 0;
            if (diag_active_transfer_time != 0) {
                spi_clock_tenths = (uint32_t)
                    (((uint64_t)diag_active_bytes * 90ULL) / diag_active_transfer_time);
            }

            uint32_t payload_hundredths = (uint32_t)
                (((uint64_t)diag_bytes_received * 100000000ULL) /
                 (elapsed_us * 1048576ULL));
            uint32_t write_rate_tenths = (uint32_t)
                (((uint64_t)diag_frame_count * 10000000ULL) / elapsed_us);
            uint32_t throttles_now = diag_audio_queue_throttles;
            uint32_t throttle_delta = throttles_now - diag_last_health_throttles;
            diag_last_health_throttles = throttles_now;

            printf("HEALTH video=%s spi=%lu.%luMHz payload=%lu.%02luMiB/s "
                   "writes=%lu.%lu/s windows=%lu backlog=%lu audio=%s rate=%luHz "
                   "queue=%lu throttles=%lu vstall=%lu astall=%lu badwin=%lu\n",
                   diag_video_active ? "active" : "idle",
                   (unsigned long)(spi_clock_tenths / 10u),
                   (unsigned long)(spi_clock_tenths % 10u),
                   (unsigned long)(payload_hundredths / 100u),
                   (unsigned long)(payload_hundredths % 100u),
                   (unsigned long)(write_rate_tenths / 10u),
                   (unsigned long)(write_rate_tenths % 10u),
                   (unsigned long)diag_window_changes,
                   (unsigned long)diag_max_dma_backlog,
                   diag_audio_active ? "active" : "idle",
                   (unsigned long)current_i2s_rate,
                   (unsigned long)hstx_di_queue_get_level(),
                   (unsigned long)throttle_delta,
                   (unsigned long)diag_video_rx_stalls,
                   (unsigned long)diag_audio_rx_stalls,
                   (unsigned long)diag_invalid_windows);

            if (spi_clock_tenths > 450u && !diag_warned_spi_clock) {
                printf("WARN spi-clock-above-45MHz measured=%lu.%luMHz\n",
                       (unsigned long)(spi_clock_tenths / 10u),
                       (unsigned long)(spi_clock_tenths % 10u));
                diag_warned_spi_clock = true;
            } else if (spi_clock_tenths <= 450u) {
                diag_warned_spi_clock = false;
            }

            diag_bytes_received = 0;
            diag_frame_count = 0;
            diag_active_bytes = 0;
            diag_active_transfer_time = 0;
            diag_max_dma_backlog = 0;
            diag_window_changes = 0;
            diag_health_start_time = now_us;
        }
#endif
    }
}
