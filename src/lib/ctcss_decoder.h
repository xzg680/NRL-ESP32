#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*CtcssDecodedCallback)(float frequency_hz, void *context);

// Streaming detector for the 50 standard CTCSS/PL tones (67.0-254.1 Hz).
// Input is mono PCM16 at 16 kHz. It keeps only Goertzel state -- no audio
// frame buffer -- and requires the same narrow-band tone in two consecutive
// 400 ms windows before reporting it.
class CtcssDecoder {
public:
    CtcssDecoder();

    void reset();
    void feed(const int16_t *samples, size_t count,
              CtcssDecodedCallback callback, void *context);

private:
    static constexpr size_t kToneCount = 50u;
    static constexpr size_t kWindowSamples = 6400u; // 400 ms at 16 kHz

    float coefficients_[kToneCount];
    float q1_[kToneCount];
    float q2_[kToneCount];
    double energy_;
    size_t sample_count_;
    int candidate_;
    int active_;
    uint8_t stable_windows_;
    uint8_t silent_windows_;

    void evaluate(CtcssDecodedCallback callback, void *context);
};
