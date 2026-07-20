#include "lib/ctcss_decoder.h"

#include <math.h>
#include <string.h>

namespace {

constexpr float kSampleRate = 16000.0f;
constexpr float kPi = 3.14159265358979323846f;

// EIA standard tones, including the commonly supported split tones.
constexpr float kTones[] = {
    67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f,
    94.8f, 97.4f, 100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f,
    127.3f, 131.8f, 136.5f, 141.3f, 146.2f, 151.4f, 156.7f, 159.8f, 162.2f,
    165.5f, 167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f, 186.2f, 189.9f,
    192.8f, 196.6f, 199.5f, 203.5f, 206.5f, 210.7f, 218.1f, 225.7f, 229.1f,
    233.6f, 241.8f, 250.3f, 254.1f,
};

static_assert(sizeof(kTones) / sizeof(kTones[0]) == 50u, "CTCSS tone table mismatch");

} // namespace

CtcssDecoder::CtcssDecoder()
    : coefficients_{}, q1_{}, q2_{}, energy_(0.0), sample_count_(0u),
      candidate_(-1), active_(-1), stable_windows_(0u), silent_windows_(0u)
{
    for (size_t i = 0u; i < kToneCount; ++i) {
        coefficients_[i] = 2.0f * cosf(2.0f * kPi * kTones[i] / kSampleRate);
    }
}
void CtcssDecoder::reset()
{
    memset(q1_, 0, sizeof(q1_));
    memset(q2_, 0, sizeof(q2_));
    energy_ = 0.0;
    sample_count_ = 0u;
    candidate_ = -1;
    active_ = -1;
    stable_windows_ = 0u;
    silent_windows_ = 0u;
}

void CtcssDecoder::feed(const int16_t *samples, const size_t count,
                        const CtcssDecodedCallback callback, void *context)
{
    if (samples == nullptr) return;
    for (size_t n = 0u; n < count; ++n) {
        const float sample = static_cast<float>(samples[n]);
        energy_ += static_cast<double>(sample) * static_cast<double>(sample);
        for (size_t i = 0u; i < kToneCount; ++i) {
            const float q0 = sample + coefficients_[i] * q1_[i] - q2_[i];
            q2_[i] = q1_[i];
            q1_[i] = q0;
        }
        if (++sample_count_ >= kWindowSamples) evaluate(callback, context);
    }
}

void CtcssDecoder::evaluate(const CtcssDecodedCallback callback, void *context)
{
    int best_index = -1;
    double best_power = 0.0;
    double second_power = 0.0;
    for (size_t i = 0u; i < kToneCount; ++i) {
        const double q1 = q1_[i];
        const double q2 = q2_[i];
        const double power = q1 * q1 + q2 * q2 - coefficients_[i] * q1 * q2;
        if (power > best_power) {
            second_power = best_power;
            best_power = power;
            best_index = static_cast<int>(i);
        } else if (power > second_power) {
            second_power = power;
        }
    }

    // For a window containing one pure tone, normalized_power approaches 1.
    // CTCSS is normally mixed well below voice, so accept 3% narrow-band
    // energy but require strong separation from every neighboring PL tone.
    const double normalized_power = energy_ > 0.0
        ? (2.0 * best_power) / (static_cast<double>(sample_count_) * energy_) : 0.0;
    const double rms = sample_count_ > 0u ? sqrt(energy_ / sample_count_) : 0.0;
    const bool valid = best_index >= 0 && rms >= 40.0 && normalized_power >= 0.03 &&
                       best_power >= second_power * 2.0;

    if (valid) {
        silent_windows_ = 0u;
        if (candidate_ == best_index) {
            if (stable_windows_ < UINT8_MAX) ++stable_windows_;
        } else {
            candidate_ = best_index;
            stable_windows_ = 1u;
        }
        if (stable_windows_ >= 2u && active_ != best_index) {
            active_ = best_index;
            if (callback != nullptr) callback(kTones[best_index], context);
        }
    } else {
        candidate_ = -1;
        stable_windows_ = 0u;
        if (silent_windows_ < UINT8_MAX) ++silent_windows_;
        if (silent_windows_ >= 2u) active_ = -1;
    }

    memset(q1_, 0, sizeof(q1_));
    memset(q2_, 0, sizeof(q2_));
    energy_ = 0.0;
    sample_count_ = 0u;
}
