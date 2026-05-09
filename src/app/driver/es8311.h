#ifndef DRIVER_ES8311_H
#define DRIVER_ES8311_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ES8311_AUDIO_MODE_RECEIVE = 0,   // Full-duplex bridge: ADC captures MIC, DAC plays NRL downlink.
} ES8311_AudioMode_t;

typedef void (*ES8311_FrameHook_t)(const int16_t *samples,
                                   size_t sample_count,
                                   ES8311_AudioMode_t mode,
                                   void *user_data);

// Initialize the ES8311 codec and start I2S pass-through.
bool ES8311_Init(void);

// True if the codec initialization sequence already succeeded.
bool ES8311_IsReady(void);

// Switch ES8311 external audio route.
bool ES8311_SetAudioMode(ES8311_AudioMode_t mode);
bool ES8311_SetReceiveMode(void);

// Register a callback invoked from the ES8311 passthrough task.
// The hook is called for captured receive-mode PCM16 frames at 8 kHz.
void ES8311_SetFrameHook(ES8311_FrameHook_t hook, void *user_data);

// Queue PCM16 mono samples (8 kHz) for DAC playback.
size_t ES8311_QueueOutputSamples(const int16_t *samples, size_t sample_count);
void ES8311_ClearOutputQueue(void);
bool ES8311_ApplyAudioConfig(uint8_t mic_volume, uint8_t line_out_volume, bool hp_drive_enabled);

int ES8311_GetSampleRate(void);
size_t ES8311_GetFrameSamples(void);
void ES8311_LogStatus(void);
void ES8311_LogIdRegisters(void);

// Convenience test tone output to verify speaker chain.
bool ES8311_PlayTestTone(uint32_t durationMs);

// Record from mic for durationMs and then play back the captured audio once.
bool ES8311_RecordMicAndPlayback(uint32_t durationMs);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_ES8311_H
