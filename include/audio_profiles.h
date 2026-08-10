#ifndef AUDIO_PROFILES_H
#define AUDIO_PROFILES_H

#include <stdint.h>

// Readable filter choices used by the audio pipeline. The implementation owns
// the fixed-point coefficients so profiles do not need to contain magic values.
typedef enum {
    AUDIO_FILTER_BYPASS,
    AUDIO_FILTER_1200_HZ,
    AUDIO_FILTER_2000_HZ,
} audio_filter_t;

typedef struct {
    const char *name;
    uint32_t alignment_shift;
    audio_filter_t filter;
} audio_profile_t;

// Selects the existing, hardware-tested profile from the measured I2S rate and
// active video dimensions. Rules are documented in audio_profiles.c.
audio_profile_t audio_profile_select(uint32_t sample_rate,
                                     uint32_t active_width,
                                     uint32_t active_height);

// Returns the Q16 coefficient used by the four-stage 48 kHz low-pass filter.
uint32_t audio_filter_alpha(audio_filter_t filter);

// Human-readable filter status for optional diagnostics.
const char *audio_filter_description(audio_filter_t filter);

#endif
