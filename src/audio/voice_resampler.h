#ifndef SRC_AUDIO_VOICE_RESAMPLER_H
#define SRC_AUDIO_VOICE_RESAMPLER_H

// Stateful media -> voice-domain resampler (docs/architecture.md 3.2 SRC):
// arbitrary-rate stereo/mono PCM16 (8k-192k) down to 8 kHz mono for the NRL
// G.711 uplink. Stereo is downmixed first, then linearly interpolated; the
// fractional read position and the last input frame carry across calls so
// chunk boundaries stay click-free. Voice-grade quality by design -- the
// network path is 8 kHz telephony anyway.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t in_rate_hz;
    uint8_t channels;      // 1 or 2 (interleaved)
    float position;        // fractional read index into the virtual mono input
    int16_t carry_sample;  // last mono sample of the previous chunk
    int has_carry;
} VoiceResampler;

// Initialise / re-initialise for an input format. Returns 0 on bad args.
int VOICE_RESAMPLER_Init(VoiceResampler *rs, uint32_t in_rate_hz, uint8_t channels);

// Convert one interleaved PCM16 chunk. in_frames counts frames (sample
// groups), not samples. Returns the number of 8 kHz mono samples written to
// out (bounded by out_capacity; size out for in_frames * 8000 / in_rate + 2).
size_t VOICE_RESAMPLER_Process(VoiceResampler *rs,
                               const int16_t *in, size_t in_frames,
                               int16_t *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif // SRC_AUDIO_VOICE_RESAMPLER_H
