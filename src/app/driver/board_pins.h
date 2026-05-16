#ifndef DRIVER_BOARD_PINS_H
#define DRIVER_BOARD_PINS_H

// Centralized ESP32-S3 pin mapping for the 3188 NRL boards.
// Keep board-facing pins here so future board/radio variants only need
// one mapping update instead of editing multiple drivers.

// ============================================================
// Board variant switch
//   NRL_BOARD_GEZIPAI  -- 格子派 (default)
//   NRL_BOARD_BH4TDV   -- BH4TDV 3188 NRL
// Set NRL_BOARD below, or override from platformio.ini with:
//   build_flags = -DNRL_BOARD=NRL_BOARD_BH4TDV
// ============================================================
#define NRL_BOARD_GEZIPAI   0
#define NRL_BOARD_BH4TDV    1

#ifndef NRL_BOARD
#define NRL_BOARD           NRL_BOARD_GEZIPAI
#endif

// ---- Common pins (same on both boards) ----

// SCI radio-control serial
#define NRL_PIN_SCI_RX          4    // Temporary SCI RX;
#define NRL_PIN_SCI_TX          5    // Temporary SCI TX;

// Bootloader UI
#define NRL_PIN_BOOT_BUTTON     0

// ---- Board-specific pins (selected by NRL_BOARD) ----
#if NRL_BOARD == NRL_BOARD_BH4TDV

// Radio control / status
#define NRL_PIN_PTT_OUT         8
#define NRL_PIN_SQL1            17
#define NRL_PIN_SQL2            18
#define NRL_PIN_STATUS_IO1      1    // HBLED for heartbeat + NET status
#define NRL_PIN_STATUS_IO2      2    // SQL LED for squelch status
#define NRL_PIN_STATUS_PTT_LED  42   // RED LED for PTT status

// Channel selection bits (up to 8 channels) -- BH4TDV only; 格子派 has
// no channel-select hardware (and 40/39/38 are its I2S/I2C pins).
#define NRL_PIN_CHANNEL_BIT0    40
#define NRL_PIN_CHANNEL_BIT1    39
#define NRL_PIN_CHANNEL_BIT2    38

// Power amplifier (PA) enable pin -- BH4TDV has no PA enable pin
// (-1 disables it; es8311.cpp guards with `if (kPinPaEn >= 0)`)
#define NRL_PIN_PA_EN           -1

// I2C bus -- BH4TDV 3188 NRL
#define NRL_PIN_I2C_SCL         14
#define NRL_PIN_I2C_SDA         21

// ES8311 I2S bus pins -- BH4TDV 3188 NRL
#define NRL_PIN_I2S_MCLK        9
#define NRL_PIN_I2S_BCLK        10
#define NRL_PIN_I2S_DOUT        13
#define NRL_PIN_I2S_LRCLK       12
#define NRL_PIN_I2S_DIN         11

#elif NRL_BOARD == NRL_BOARD_GEZIPAI

// User controls -- 3 push buttons (press-to-GND, read with INPUT_PULLUP)
#define NRL_PIN_BTN_VOL_UP      1    // IO1  -- volume up
#define NRL_PIN_BTN_VOL_DOWN    2    // IO2  -- volume down
#define NRL_PIN_BTN_PTT         18   // IO18 -- physical PTT

// User indicators -- 3 LEDs (active-low: drive the pin LOW to light)
#define NRL_PIN_LED_PTT         41   // IO41 yellow -- PTT held / mic capture
#define NRL_PIN_LED_AUDIO       42   // IO42 green  -- inbound network audio
#define NRL_PIN_LED_NET         17   // IO17 white  -- network/server status

// Power amplifier (PA) enable pin -- 格子派
#define NRL_PIN_PA_EN           46

// I2C bus -- 格子派
#define NRL_PIN_I2C_SCL         47
#define NRL_PIN_I2C_SDA         38

// ES8311 I2S bus pins -- 格子派
#define NRL_PIN_I2S_MCLK        40
#define NRL_PIN_I2S_BCLK        39
#define NRL_PIN_I2S_DOUT        48
#define NRL_PIN_I2S_LRCLK       45
#define NRL_PIN_I2S_DIN         21

#else
#error "Unknown NRL_BOARD: set it to NRL_BOARD_GEZIPAI or NRL_BOARD_BH4TDV"
#endif // NRL_BOARD

#endif // DRIVER_BOARD_PINS_H
