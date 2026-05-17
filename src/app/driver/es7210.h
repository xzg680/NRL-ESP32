#ifndef DRIVER_ES7210_H
#define DRIVER_ES7210_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ES7210 is a dedicated multi-channel microphone ADC used on the 格子派 board.
// On that board the audio path is split across two chips:
//   ES8311 -> DAC only  (speaker / TO_MIC output)
//   ES7210 -> ADC only  (microphone capture, drives I2S DIN / GPIO21)
// The ES8311 driver owns the shared I2S bus; ES7210 only needs I2C register
// configuration. Call ES7210_Init() AFTER ES8311_Init() so that MCLK/BCLK/LRCK
// are already running when the ES7210 comes out of reset.

// Configure the ES7210 over I2C. Returns true on success.
bool ES7210_Init(void);

// True once ES7210_Init() succeeded.
bool ES7210_IsReady(void);

// Set the microphone capture gain. `volume` is the 0-255 value shared with
// the web/AT "mic volume" control; it is mapped onto the ES7210 analog PGA
// gain steps. Safe to call before ES7210_Init() -- the value is latched and
// applied during init. On boards without an ES7210 this is a no-op.
void ES7210_SetMicVolume(uint8_t volume);

// Dump the ES7210 control registers to Serial for debugging.
void ES7210_DumpRegisters(void);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_ES7210_H
