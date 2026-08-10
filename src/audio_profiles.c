#include "audio_profiles.h"

// Input alignment/gain values:
//   1 shift: discard the Philips-I2S boundary bit, unity gain.
//   2 shifts: the same alignment plus the proven 2x gain used by legacy ports.
#define AUDIO_ALIGNMENT_UNITY_GAIN 1u
#define AUDIO_ALIGNMENT_2X_GAIN    2u

// Adding a profile:
//   1. Add a rate/aspect rule below if the source needs a different filter.
//   2. Add a video-specific override after the broad rules if it needs a
//      different input gain. Later overrides intentionally take priority.
//   3. Use the named filter values rather than adding raw Q16 coefficients.
//
// Keep the Default profile unchanged so sources without a special requirement
// retain the established 2x input gain and full audio bandwidth.
audio_profile_t audio_profile_select(uint32_t sample_rate,
                                     uint32_t active_width,
                                     uint32_t active_height) {
    // Default: legacy input gain with no low-pass filtering. This also covers
    // all sources at 36 kHz and above.
    audio_profile_t profile = {
        .name = "Default",
        .alignment_shift = AUDIO_ALIGNMENT_2X_GAIN,
        .filter = AUDIO_FILTER_BYPASS,
    };

    // Filter rules are selected first. They intentionally reproduce the broad
    // fallbacks that have already worked across the tested emulator collection.
    if (sample_rate < 24000) {
        // Doom: physical ~22 kHz, native ~11 kHz.
        profile.name = "Low-rate / Doom";
        profile.filter = AUDIO_FILTER_1200_HZ;
    } else if (sample_rate < 36000) {
        // Game Boy and Lynx both use a physical rate around 32 kHz. Lynx is
        // identified by its known heights or a wide active-video aspect ratio.
        if (active_height == 102 || active_height == 204 ||
            active_width * 10 >= active_height * 13) {
            profile.name = "Lynx";
            profile.filter = AUDIO_FILTER_BYPASS;
        } else {
            profile.name = "Game Boy";
            profile.filter = AUDIO_FILTER_2000_HZ;
        }
    }

    // Video-specific overrides come after the broad rate rules. Celeste keeps
    // the low-rate 1200 Hz filter selected above, but its near-full-scale PCM
    // must not receive the legacy 2x gain.
    if (active_width == 320 && active_height == 182) {
        profile.name = "Celeste";
        profile.alignment_shift = AUDIO_ALIGNMENT_UNITY_GAIN;
    }

    return profile;
}

uint32_t audio_filter_alpha(audio_filter_t filter) {
    switch (filter) {
        case AUDIO_FILTER_1200_HZ:
            return 17563u;
        case AUDIO_FILTER_2000_HZ:
            return 29261u;
        case AUDIO_FILTER_BYPASS:
        default:
            return 65536u;
    }
}

const char *audio_filter_description(audio_filter_t filter) {
    switch (filter) {
        case AUDIO_FILTER_1200_HZ:
            return "ACTIVE (Cutoff ~1200Hz)";
        case AUDIO_FILTER_2000_HZ:
            return "ACTIVE (Cutoff ~2000Hz)";
        case AUDIO_FILTER_BYPASS:
        default:
            return "BYPASS";
    }
}
