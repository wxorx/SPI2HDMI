/**
 * pico_hdmi Pin Configuration
 *
 * Default HSTX output pin mapping for the pico_hdmi library.
 */

#ifndef HSTX_PINS_H
#define HSTX_PINS_H

// =============================================================================
// DVI/HSTX Output Pins (GPIO 12-19)
// =============================================================================
// These are the default pins for the RP2350's HSTX peripheral. They define
// the contiguous HSTX-capable GPIO block used for pad setup only; the TMDS
// lane -> GPIO pair crossbar mapping (including the Olimex RP2040-PICO-PC
// red/green routing) is configured in src/video_output.c.
#define PIN_HSTX_CLK 12 // Clock pair base (12=CLK-, 13=CLK+)
#define PIN_HSTX_D0 14  // Data 0 pair base (14=D0-, 15=D0+)
#define PIN_HSTX_D1 16  // First data pair base (16/17; Olimex board routes TMDS2/red here)
#define PIN_HSTX_D2 18  // Last data pair base (18/19; Olimex board routes TMDS1/green here)

#endif // HSTX_PINS_H
