#ifndef DRIVER_BOARD_PINS_S31_KORVO_H
#define DRIVER_BOARD_PINS_S31_KORVO_H

// GPIO0 is shared I2C SDA on this board, not a free BOOT key.
#define NRL_PIN_BOOT_BUTTON     -1

// UART1 SCI and UART2 GPS use otherwise-idle DVP camera signals.
#define NRL_PIN_SCI_RX          46
#define NRL_PIN_SCI_TX          47
#define NRL_HAS_SCI_SERIAL      1
#define NRL_PIN_GPS_RX          48
#define NRL_PIN_GPS_TX          49

// ADC button ladder. SET is mapped to NRL PTT.
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

// Addressable RGB LED on GPIO37.
#define NRL_PIN_LED_PTT         -1
#define NRL_PIN_LED_AUDIO       -1
#define NRL_PIN_LED_NET         -1

// Audio: official BSP uses ES8389 with PA enable on GPIO7.
#define NRL_PIN_PA_EN           7
#define NRL_HAS_ES7210          0
#define NRL_AUDIO_CODEC_ES8311  0
#define NRL_AUDIO_CODEC_ES8389  1

// TF card and USB host.
#define NRL_HAS_SDCARD          1
#define NRL_HAS_USB_HOST        1

// I2C bus.
#define NRL_PIN_I2C_SCL         1
#define NRL_PIN_I2C_SDA         0

// ES8389 I2S bus.
#define NRL_PIN_I2S_MCLK        2
#define NRL_PIN_I2S_BCLK        3
#define NRL_PIN_I2S_DOUT        5
#define NRL_PIN_I2S_LRCLK       4
#define NRL_PIN_I2S_DIN         6

// 800x480 RGB565 LCD panel.
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

#endif // DRIVER_BOARD_PINS_S31_KORVO_H
