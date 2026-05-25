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
// The hook is called for raw captured receive-mode PCM16 frames at 16 kHz.
// AFE/AEC processed frames are already downsampled to 8 kHz for G.711.
void ES8311_SetFrameHook(ES8311_FrameHook_t hook, void *user_data);

// Queue PCM16 mono samples (8 kHz) for DAC playback.
size_t ES8311_QueueOutputSamples(const int16_t *samples, size_t sample_count);
void ES8311_ClearOutputQueue(void);

// AEC reference source: 0 = delayed network playback, 1 = second I2S input
// channel. Takes effect immediately while the resident AFE keeps running.
void ES8311_SetAecReferenceSource(uint8_t source);
bool ES8311_ApplyAudioConfig(uint8_t mic_volume,
                             uint8_t line_out_volume,
                             bool hp_drive_enabled,
                             bool drc_enabled,
                             uint8_t drc_winsize,
                             uint8_t drc_maxlevel,
                             uint8_t drc_minlevel,
                             uint8_t dac_ramprate,
                             bool dac_eq_bypass,
                             uint32_t daceq_b0,
                             uint32_t daceq_b1,
                             uint32_t daceq_a1,
                             bool adc_dmic_enabled,
                             bool adc_linsel,
                             uint8_t adc_pga_gain,
                             uint8_t adc_ramprate,
                             bool adc_dmic_sense,
                             bool adc_sync,
                             bool adc_inv,
                             bool adc_ramclr,
                             uint8_t adc_scale,
                             bool alc_enabled,
                             bool adc_automute_enabled,
                             uint8_t alc_winsize,
                             uint8_t alc_maxlevel,
                             uint8_t alc_minlevel,
                             uint8_t adc_automute_winsize,
                             uint8_t adc_automute_noise_gate,
                             uint8_t adc_automute_volume,
                             uint8_t adc_hpfs1,
                             bool adc_eq_bypass,
                             bool adc_hpf,
                             uint8_t adc_hpfs2,
                             uint32_t adceq_b0,
                             uint32_t adceq_a1,
                             uint32_t adceq_a2,
                             uint32_t adceq_b1,
                             uint32_t adceq_b2);

int ES8311_GetSampleRate(void);
size_t ES8311_GetFrameSamples(void);

// Software high-pass filter on captured mic audio. When enabled, a 4th-order
// IIR HPF (~200 Hz cutoff at 16 kHz sample rate) is applied to every frame
// before it reaches AEC / the frame hook, removing DC offset and low-frequency
// rumble. Stateful: toggling resets the filter memory to avoid a transient
// on the first frame.
void ES8311_SetMicHpfEnabled(bool enabled);
bool ES8311_GetMicHpfEnabled(void);

// Convenience test tone output to verify speaker chain.
bool ES8311_PlayTestTone(uint32_t durationMs);

// Record from mic for durationMs and then play back the captured audio once.
bool ES8311_RecordMicAndPlayback(uint32_t durationMs);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_ES8311_H
