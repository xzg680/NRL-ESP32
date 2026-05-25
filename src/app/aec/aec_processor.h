#ifndef APP_AEC_AEC_PROCESSOR_H
#define APP_AEC_AEC_PROCESSOR_H

// Optional esp-sr AFE processing built on the Espressif AFE.
//
// Pipeline (capture is 16 kHz, the NRL/G.711 network uplink stays 8 kHz):
//   ES8311 capture 16kHz [mic, optional reference]
//     -> AFE feed (mono mic, or 2-channel interleaved mic + reference for AEC)
//     -> AFE fetch (processed mono 16k)
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
bool AEC_Init(bool enable_aec, bool enable_ai_noise);

// True once AEC_Init() has succeeded.
bool AEC_IsReady(void);

// Runtime route switches. The AFE instance can stay resident while the uplink
// is switched between raw passthrough and processed output.
void AEC_SetRuntimeEnabled(bool enable_aec, bool enable_ai_noise);

// True when the active AFE instance was created with a reference channel.
bool AEC_UsesReference(void);

// True when the processed AFE output should be forwarded right now.
bool AEC_IsRuntimeActive(void);

// Register the sink for echo-cancelled audio (e.g. the NRL uplink).
void AEC_SetOutputCallback(AEC_OutputCallback callback, void *user_data);

// Submit one captured 16 kHz frame: mic is mono; ref is required only when
// AEC_UsesReference() is true (the ES8311 I2S frame size is 160).
// Safe to call from the I2S capture task. Non-blocking.
void AEC_SubmitCapture(const int16_t *mic_16k,
                       const int16_t *ref_16k,
                       size_t sample_count);

#ifdef __cplusplus
}
#endif

#endif // APP_AEC_AEC_PROCESSOR_H
