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
#define NRL_BOARD_S31_KORVO 2

#ifndef NRL_BOARD
#define NRL_BOARD           NRL_BOARD_GEZIPAI
#endif

// ---- Common pins (same on both boards) ----

// Bootloader UI
#define NRL_PIN_BOOT_BUTTON     0

// SCI radio-control serial pins are board-specific: see each [NRL_BOARD] block.
// 格子派 routes its LCD over GPIO 4-7/15/16, so SCI cannot stay on GPIO 4/5
// there.

// ---- Board-specific pins (selected by NRL_BOARD) ----
#if NRL_BOARD == NRL_BOARD_BH4TDV

// SCI radio-control serial
#define NRL_PIN_SCI_RX          4
#define NRL_PIN_SCI_TX          5
#define NRL_HAS_SCI_SERIAL      1

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

// Microphone codec: BH4TDV captures mic audio through the ES8311's own
// ADC, so it has no separate ES7210.
#define NRL_HAS_ES7210          0
#define NRL_AUDIO_CODEC_ES8311  1
#define NRL_AUDIO_CODEC_ES8389  0

// BH4TDV has no on-board LCD.
#define NRL_HAS_DISPLAY         0
#define NRL_HAS_BATTERY_ADC     0

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

// SCI radio-control serial -- 格子派 exposes the NRL transparent-passthrough
// link on its U0 header, which is wired to the chip's default U0 pins
// (GPIO 44 = U0RXD, GPIO 43 = U0TXD). The UART peripheral itself is UART1
// (routed through the GPIO matrix in sci_serial.cpp, identical to BH4TDV);
// UART0 is left free for other use. IDF console output is on USB-Serial-JTAG
// (sdkconfig: CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y) so neither UART carries
// log traffic that would collide with the radio data.
#define NRL_PIN_SCI_RX          44
#define NRL_PIN_SCI_TX          43
#define NRL_HAS_SCI_SERIAL      1

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

// Microphone codec: 格子派 has a dedicated ES7210 4-channel mic ADC whose
// SDOUT1 drives I2S DIN (GPIO21). The ES8311 is used as DAC only, so the
// ES7210 must be configured over I2C for the microphone to work.
#define NRL_HAS_ES7210          1
#define NRL_AUDIO_CODEC_ES8311  1
#define NRL_AUDIO_CODEC_ES8389  0

// I2C bus -- 格子派
#define NRL_PIN_I2C_SCL         47
#define NRL_PIN_I2C_SDA         38

// ES8311 I2S bus pins -- 格子派
#define NRL_PIN_I2S_MCLK        40
#define NRL_PIN_I2S_BCLK        39
#define NRL_PIN_I2S_DOUT        48
#define NRL_PIN_I2S_LRCLK       45
#define NRL_PIN_I2S_DIN         21

// ST7789 240x240 SPI LCD -- same panel/wiring as the 小智 (xiaozhi) 格子派
// board. Driven by src/app/driver/display.cpp.
#define NRL_PIN_DISPLAY_SCLK    7
#define NRL_PIN_DISPLAY_MOSI    6
#define NRL_PIN_DISPLAY_DC      16
#define NRL_PIN_DISPLAY_CS      15
#define NRL_PIN_DISPLAY_RST     5
#define NRL_PIN_DISPLAY_BL      4    // backlight (active-high)
#define NRL_DISPLAY_BUS_ST7789  1
#define NRL_DISPLAY_BUS_RGB     0
#define NRL_DISPLAY_WIDTH       240
#define NRL_DISPLAY_HEIGHT      240

// Battery voltage sense -- ADC1 channel 2 (GPIO3), behind a 1:2 divider.
#define NRL_PIN_BATTERY_ADC     3
#define NRL_BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define NRL_HAS_BATTERY_ADC     1
#define NRL_HAS_DISPLAY         1

#elif NRL_BOARD == NRL_BOARD_S31_KORVO

// ESP32-S31-Korvo-1 official BSP pin map:
// https://github.com/espressif/esp-bsp/tree/master/bsp/esp32_s31_korvo_1

// SCI radio-control serial. These are provisional free GPIOs from the board
// IO map; verify against the final carrier/header schematic before bring-up.
#define NRL_PIN_SCI_RX          26
#define NRL_PIN_SCI_TX          27
#define NRL_HAS_SCI_SERIAL      0

// User controls are ADC buttons on ADC1 channel 0. SET is mapped to NRL PTT.
#define NRL_HAS_ADC_BUTTONS     1
#define NRL_PIN_ADC_BUTTONS     42
#define NRL_ADC_BUTTON_CHANNEL  ADC_CHANNEL_0
#define NRL_ADC_BTN_VOL_UP_LOW  160
#define NRL_ADC_BTN_VOL_UP_HIGH 600
#define NRL_ADC_BTN_VOL_DN_LOW  600
#define NRL_ADC_BTN_VOL_DN_HIGH 1080
#define NRL_ADC_BTN_MODE_LOW    1080
#define NRL_ADC_BTN_MODE_HIGH   1605
#define NRL_ADC_BTN_SET_LOW     1605
#define NRL_ADC_BTN_SET_HIGH    1935
#define NRL_PIN_BTN_VOL_UP      1001
#define NRL_PIN_BTN_VOL_DOWN    1002
#define NRL_PIN_BTN_PTT         1003

// ESP32-S31-Korvo-1 has an addressable RGB LED on GPIO37, not three simple
// active-low GPIO LEDs. Keep normal LED outputs disabled until a strip driver
// is added.
#define NRL_PIN_LED_PTT         -1
#define NRL_PIN_LED_AUDIO       -1
#define NRL_PIN_LED_NET         -1

// Audio: official BSP uses ES8389 with PA enable on GPIO7.
#define NRL_PIN_PA_EN           7
#define NRL_HAS_ES7210          0
#define NRL_AUDIO_CODEC_ES8311  0
#define NRL_AUDIO_CODEC_ES8389  1

// I2C bus
#define NRL_PIN_I2C_SCL         1
#define NRL_PIN_I2C_SDA         0

// ES8389 I2S bus
#define NRL_PIN_I2S_MCLK        2
#define NRL_PIN_I2S_BCLK        3
#define NRL_PIN_I2S_DOUT        5
#define NRL_PIN_I2S_LRCLK       4
#define NRL_PIN_I2S_DIN         6

// 800x480 RGB565 LCD panel
#define NRL_HAS_DISPLAY         1
#define NRL_HAS_BATTERY_ADC     0
#define NRL_DISPLAY_BUS_ST7789  0
#define NRL_DISPLAY_BUS_RGB     1
#define NRL_DISPLAY_WIDTH       800
#define NRL_DISPLAY_HEIGHT      480
#define NRL_PIN_LCD_VSYNC       45
#define NRL_PIN_LCD_HSYNC       44
#define NRL_PIN_LCD_DE          43
#define NRL_PIN_LCD_PCLK        40
#define NRL_PIN_LCD_DISP        -1
#define NRL_PIN_LCD_RST         -1
#define NRL_PIN_DISPLAY_BL      -1
#define NRL_PIN_LCD_DATA0       8
#define NRL_PIN_LCD_DATA1       9
#define NRL_PIN_LCD_DATA2       10
#define NRL_PIN_LCD_DATA3       11
#define NRL_PIN_LCD_DATA4       12
#define NRL_PIN_LCD_DATA5       13
#define NRL_PIN_LCD_DATA6       14
#define NRL_PIN_LCD_DATA7       15
#define NRL_PIN_LCD_DATA8       16
#define NRL_PIN_LCD_DATA9       17
#define NRL_PIN_LCD_DATA10      18
#define NRL_PIN_LCD_DATA11      19
#define NRL_PIN_LCD_DATA12      33
#define NRL_PIN_LCD_DATA13      34
#define NRL_PIN_LCD_DATA14      35
#define NRL_PIN_LCD_DATA15      36

#else
#error "Unknown NRL_BOARD: set it to NRL_BOARD_GEZIPAI, NRL_BOARD_BH4TDV, or NRL_BOARD_S31_KORVO"
#endif // NRL_BOARD

#endif // DRIVER_BOARD_PINS_H
