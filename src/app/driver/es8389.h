#ifndef DRIVER_ES8389_H
#define DRIVER_ES8389_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

bool ES8389_Init(void);
bool ES8389_IsReady(void);
bool ES8389_SetReceiveMode(void);
bool ES8389_SetOutputVolume(uint8_t value);
bool ES8389_SetInputGain(uint8_t value);

// ---- Hi-fi playback path (docs/architecture.md 3.2) -----------------------
// Acquire stops the 16 kHz voice passthrough and reopens the codec at the
// media's native format (44.1k-192k, 16/24-bit, 1-2 ch); Write streams PCM
// straight to I2S with no resampling; Release restores the voice path.
// Voice/music arbitration (who calls Release when a call comes in) lives in
// the service layer, not here.
bool ES8389_HifiAcquire(uint32_t sample_rate_hz, uint8_t bits_per_sample, uint8_t channels);
bool ES8389_HifiWrite(const void *pcm, size_t bytes);
bool ES8389_HifiRelease(void);
bool ES8389_HifiActive(void);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_ES8389_H
