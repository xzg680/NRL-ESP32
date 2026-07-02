#include "audio/voice_resampler.h"

#include <math.h>
#include <string.h>

namespace {
constexpr uint32_t kVoiceRateHz = 8000u;

static inline int16_t mono_frame(const int16_t *in, const size_t frame, const uint8_t channels)
{
    if (channels == 2u) {
        return static_cast<int16_t>((static_cast<int32_t>(in[frame * 2u]) +
                                     static_cast<int32_t>(in[frame * 2u + 1u])) / 2);
    }
    return in[frame];
}
} // namespace

extern "C" int VOICE_RESAMPLER_Init(VoiceResampler *rs, const uint32_t in_rate_hz, const uint8_t channels)
{
    if (rs == nullptr || in_rate_hz < kVoiceRateHz || in_rate_hz > 192000u ||
        (channels != 1u && channels != 2u)) {
        return 0;
    }
    memset(rs, 0, sizeof(*rs));
    rs->in_rate_hz = in_rate_hz;
    rs->channels = channels;
    return 1;
}

extern "C" size_t VOICE_RESAMPLER_Process(VoiceResampler *rs,
                                          const int16_t *in, const size_t in_frames,
                                          int16_t *out, const size_t out_capacity)
{
    if (rs == nullptr || in == nullptr || out == nullptr || in_frames == 0u ||
        rs->in_rate_hz == 0u) {
        return 0;
    }

    const float step = static_cast<float>(rs->in_rate_hz) / static_cast<float>(kVoiceRateHz);
    size_t produced = 0;

    // The virtual input places the previous chunk's final sample at index -1,
    // so interpolation across chunk boundaries stays continuous. `position`
    // is always >= -1 after the rebase below (floorf, NOT integer cast:
    // truncation would round -0.3 to 0 and misindex).
    float pos = rs->position;
    while (produced < out_capacity) {
        const float fbase = floorf(pos);
        const long base = static_cast<long>(fbase);
        if (base + 1 >= static_cast<long>(in_frames)) {
            break; // s1 lives in the next chunk
        }
        const float frac = pos - fbase; // [0, 1)

        const int16_t s0 = (base < 0)
                               ? (rs->has_carry ? rs->carry_sample : mono_frame(in, 0, rs->channels))
                               : mono_frame(in, static_cast<size_t>(base), rs->channels);
        const size_t next = (base < 0) ? 0u : static_cast<size_t>(base + 1);
        const int16_t s1 = mono_frame(in, next, rs->channels);

        out[produced++] = static_cast<int16_t>(
            static_cast<float>(s0) + (static_cast<float>(s1) - static_cast<float>(s0)) * frac);
        pos += step;
    }

    // Rebase the position for the next chunk and carry the last input sample.
    rs->position = pos - static_cast<float>(in_frames);
    rs->carry_sample = mono_frame(in, in_frames - 1u, rs->channels);
    rs->has_carry = 1;
    return produced;
}
