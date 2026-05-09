#ifndef DRIVER_BOARD_PINS_H
#define DRIVER_BOARD_PINS_H

// Centralized ESP32-S3 pin mapping for the Moto3188 radio branch.
// Keep radio-facing pins here so future board/radio variants only need
// one mapping update instead of editing multiple drivers.

// LCD / ST7565
#define NRL_PIN_LCD_RST         45
#define NRL_PIN_LCD_CS          7
#define NRL_PIN_LCD_MOSI        4
#define NRL_PIN_LCD_CLK         6
#define NRL_PIN_LCD_A0          5

// Radio control / status
#define NRL_PIN_PTT_OUT         8
#define NRL_PIN_SQL1            17
#define NRL_PIN_SQL2            18
#define NRL_PIN_STATUS_IO1      1    //HBLED for heartbeat + NET status
#define NRL_PIN_STATUS_IO2      2    //SQL LED for squelch status
#define NRL_PIN_STATUS_PTT_LED  42   //RED LED for PTT status

// Channel selection bits (up to 8 channels)
#define NRL_PIN_CHANNEL_BIT0    40   
#define NRL_PIN_CHANNEL_BIT1    39
#define NRL_PIN_CHANNEL_BIT2    38
#define NRL_PIN_SCI_RX          6    // Temporary SCI RX;  
#define NRL_PIN_SCI_TX          7    // Temporary SCI TX;  

// I2C bus
// #define NRL_PIN_I2C_SCL         47
// #define NRL_PIN_I2C_SDA         38

//BH4TDV 3188 NRL
#define NRL_PIN_I2C_SCL         14
#define NRL_PIN_I2C_SDA         21



// Power amplifier (PA) enable pin
#define NRL_PIN_PA_EN           46

// ES8311 I2S bus pins
// #define NRL_PIN_I2S_MCLK        40
// #define NRL_PIN_I2S_BCLK        39
// #define NRL_PIN_I2S_DOUT        48
// #define NRL_PIN_I2S_LRCLK       45
// #define NRL_PIN_I2S_DIN         21

//BH4TDV 3188 NRL
#define NRL_PIN_I2S_MCLK        9
#define NRL_PIN_I2S_BCLK        10
#define NRL_PIN_I2S_DOUT        13
#define NRL_PIN_I2S_LRCLK       12
#define NRL_PIN_I2S_DIN         11

// Bootloader UI 
#define NRL_PIN_BOOT_BUTTON     0

#endif // DRIVER_BOARD_PINS_H
