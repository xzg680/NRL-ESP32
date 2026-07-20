#ifndef DRIVER_BOARD_PINS_GEZIPAI_4G_H
#define DRIVER_BOARD_PINS_GEZIPAI_4G_H

#define NRL_PIN_BOOT_BUTTON     0

// UART1 is reserved for the onboard ML307R modem. The normal SCI transparent
// serial feature is disabled on this board.
#define NRL_PIN_SCI_RX          44
#define NRL_PIN_SCI_TX          43
#define NRL_HAS_SCI_SERIAL      0

// UART2/GPS is disabled by default in serial_port_config.cpp. These are safe
// fallback defaults that avoid the 4G board's LCD backlight and modem pins.
#define NRL_PIN_GPS_RX          10
#define NRL_PIN_GPS_TX          11

// 4G-board buttons.
#define NRL_HAS_USER_BUTTONS    1
#define NRL_PIN_BTN_VOL_UP      8
#define NRL_PIN_BTN_VOL_DOWN    18
#define NRL_PIN_BTN_PTT         2

// Active-low status LEDs. IO42 is ML307 UART RX, so no AUDIO LED on 4G.
#define NRL_PIN_LED_PTT         41
#define NRL_PIN_LED_AUDIO       -1
#define NRL_PIN_LED_NET         13

// ML307R modem.
#define NRL_HAS_ML307_UART_4G   1
#define NRL_ML307_UART_NUM      1
#define NRL_PIN_ML307_UART_RX   42
#define NRL_PIN_ML307_UART_TX   1
#define NRL_ML307_UART_BAUD     921600
#define NRL_PIN_ML307_POWER     17
#define NRL_PIN_ML307_POWER_ON_LEVEL 1

#define NRL_PIN_PA_EN           46
#define NRL_HAS_ES7210          1
#define NRL_AUDIO_CODEC_ES8311  1
#define NRL_AUDIO_CODEC_ES8389  0
#define NRL_HAS_SDCARD          0
#define NRL_HAS_USB_HOST        0

// I2C bus.
#define NRL_PIN_I2C_SCL         44
#define NRL_PIN_I2C_SDA         45

// ES8311/ES7210 I2S bus.
#define NRL_PIN_I2S_MCLK        40
#define NRL_PIN_I2S_BCLK        39
#define NRL_PIN_I2S_DOUT        48
#define NRL_PIN_I2S_LRCLK       45
#define NRL_PIN_I2S_DIN         21

// ST7789 240x240 SPI LCD from the 4G screen schematic.
#define NRL_PIN_DISPLAY_SCLK    4
#define NRL_PIN_DISPLAY_MOSI    5
#define NRL_PIN_DISPLAY_DC      6
#define NRL_PIN_DISPLAY_CS      7
#define NRL_PIN_DISPLAY_RST     15
#define NRL_PIN_DISPLAY_BL      9
#define NRL_DISPLAY_BUS_ST7789  1
#define NRL_DISPLAY_BUS_RGB     0
#define NRL_DISPLAY_WIDTH       240
#define NRL_DISPLAY_HEIGHT      240

// Battery voltage sense -- ADC1 channel 2 (GPIO3), behind a 1:2 divider.
#define NRL_PIN_BATTERY_ADC     3
#define NRL_BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define NRL_HAS_BATTERY_ADC     1
#define NRL_HAS_DISPLAY         1

#endif // DRIVER_BOARD_PINS_GEZIPAI_4G_H
