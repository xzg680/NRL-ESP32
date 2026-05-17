#ifndef APP_AEC_AEC_PROCESSOR_H
#define APP_AEC_AEC_PROCESSOR_H

// Gezipai-only acoustic echo cancellation built on the Espressif esp-sr AFE.
//
// Pipeline (the NRL audio path is 8 kHz, the esp-sr AFE is fixed at 16 kHz):
//   ES7210 capture 8kHz [mic, reference]
//     -> upsample 8k->16k
//     -> AFE feed (2-channel interleaved: mic + reference)
//     -> AFE fetch (echo-cancelled mono 16k)
//     -> downsample 16k->8k
//     -> output callback (clean 8k mic audio for the NRL uplink)

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Receives echo-cancelled 8 kHz mono PCM in fixed 80-sample frames.
typedef void (*AEC_OutputCallback)(const int16_t *clean_8k,
                                   size_t sample_count,
                                   void *user_data);

// Create the AFE instance, resamplers and the fetch task. Call once, after
// PSRAM is up. Returns false if esp-sr could not allocate the AFE.
bool AEC_Init(void);

// True once AEC_Init() has succeeded.
bool AEC_IsReady(void);

// Register the sink for echo-cancelled audio (e.g. the NRL uplink).
void AEC_SetOutputCallback(AEC_OutputCallback callback, void *user_data);

// Submit one captured 8 kHz frame: mic and reference are separate mono
// buffers of `sample_count` samples each (the ES8311 I2S frame size, 80).
// Safe to call from the I2S capture task. Non-blocking.
void AEC_SubmitCapture(const int16_t *mic_8k,
                       const int16_t *ref_8k,
                       size_t sample_count);

#ifdef __cplusplus
}
#endif

#endif // APP_AEC_AEC_PROCESSOR_H
