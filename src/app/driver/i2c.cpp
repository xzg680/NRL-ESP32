/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include "i2c1.h"
#include <driver/gpio.h>
#include <esp_rom_sys.h>

static constexpr uint32_t I2C_DELAY_US = 5;

static inline void I2C_SDA_Low(void)
{
    gpio_set_direction((gpio_num_t)I2C_PIN_SDA, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)I2C_PIN_SDA, 0);
}

static inline void I2C_SDA_Release(void)
{
    // Release SDA as open-drain high. The internal pull-up enabled in
    // I2C_Init is a fallback if the external pull-up is missing or weak.
    gpio_set_direction((gpio_num_t)I2C_PIN_SDA, GPIO_MODE_INPUT);
}

static inline void I2C_SCL_Low(void)
{
    gpio_set_direction((gpio_num_t)I2C_PIN_SCL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)I2C_PIN_SCL, 0);
}

static inline void I2C_SCL_Release(void)
{
    // Release SCL as open-drain high. EEPROM usually does not stretch the
    // clock, but open-drain behavior is more robust on the shared bus.
    gpio_set_direction((gpio_num_t)I2C_PIN_SCL, GPIO_MODE_INPUT);
}

static inline void I2C_RestoreSharedPinsForKeyboard(void)
{
    // KEY4/KEY5 share the I2C pins. Restore them as output-high after each
    // transfer so the keyboard matrix is not left in input-pullup mode.
    gpio_set_direction((gpio_num_t)I2C_PIN_SDA, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)I2C_PIN_SDA, 1);
    gpio_set_direction((gpio_num_t)I2C_PIN_SCL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)I2C_PIN_SCL, 1);
}

// I2C init.
void I2C_Init(void) {
    // Optional I2C bus enable pin.
#ifdef I2C_PIN_EN
    gpio_reset_pin((gpio_num_t)I2C_PIN_EN);
    gpio_set_direction((gpio_num_t)I2C_PIN_EN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)I2C_PIN_EN, 0);
#endif

    // Configure internal pull-ups once so the input-mode "release" leaves the
    // line at a high level even without an external pull-up.
    gpio_reset_pin((gpio_num_t)I2C_PIN_SDA);
    gpio_set_pull_mode((gpio_num_t)I2C_PIN_SDA, GPIO_PULLUP_ONLY);
    gpio_reset_pin((gpio_num_t)I2C_PIN_SCL);
    gpio_set_pull_mode((gpio_num_t)I2C_PIN_SCL, GPIO_PULLUP_ONLY);

    // Idle state: SCL/SDA are both high (open-drain released).
    I2C_SDA_Release();
    I2C_SCL_Release();
}

// I2C start condition.
// SDA goes high-to-low while SCL is high.
void I2C_Start(void) {
    I2C_SDA_Release();
    I2C_SCL_Release();
    esp_rom_delay_us(I2C_DELAY_US);
    I2C_SDA_Low();
    esp_rom_delay_us(I2C_DELAY_US);
    I2C_SCL_Low();
    esp_rom_delay_us(I2C_DELAY_US);
}

// I2C stop condition.
// SDA goes low-to-high while SCL is high.
void I2C_Stop(void) {
    I2C_SDA_Low();
    esp_rom_delay_us(I2C_DELAY_US);
    I2C_SCL_Release();
    esp_rom_delay_us(I2C_DELAY_US);
    I2C_SDA_Release();
    esp_rom_delay_us(I2C_DELAY_US);

    // Keep keyboard scanning happy on the shared pins.
    I2C_RestoreSharedPinsForKeyboard();
}

// Read one I2C byte.
// bFinal: true for the last byte (send NAK), false to send ACK.
uint8_t I2C_Read(bool bFinal) {
    uint8_t Data = 0;

    // Release SDA so the slave can drive it.
    I2C_SDA_Release();

    for (uint8_t i = 0; i < 8; i++) {
        I2C_SCL_Release();
        esp_rom_delay_us(I2C_DELAY_US);
        Data = (uint8_t)((Data << 1) | (gpio_get_level((gpio_num_t)I2C_PIN_SDA) ? 1U : 0U));
        I2C_SCL_Low();
        esp_rom_delay_us(I2C_DELAY_US);
    }

    // ACK/NAK
    if (bFinal) {
        I2C_SDA_Release(); // NAK
    } else {
        I2C_SDA_Low();     // ACK
    }
    esp_rom_delay_us(I2C_DELAY_US);
    I2C_SCL_Release();
    esp_rom_delay_us(I2C_DELAY_US);
    I2C_SCL_Low();
    esp_rom_delay_us(I2C_DELAY_US);
    I2C_SDA_Release();

    return Data;
}

// Write one I2C byte.
// Returns 0 when ACK is received, -1 when no ACK is received.
int I2C_Write(uint8_t Data) {
    // Send 8 data bits, MSB first.
    for (uint8_t i = 0; i < 8; i++) {
        if (Data & 0x80) {
            I2C_SDA_Release();
        } else {
            I2C_SDA_Low();
        }
        Data <<= 1;
        esp_rom_delay_us(I2C_DELAY_US);
        I2C_SCL_Release();
        esp_rom_delay_us(I2C_DELAY_US);
        I2C_SCL_Low();
        esp_rom_delay_us(I2C_DELAY_US);
    }

    // ACK: release SDA and let the slave pull it low.
    I2C_SDA_Release();
    esp_rom_delay_us(I2C_DELAY_US);
    I2C_SCL_Release();
    esp_rom_delay_us(I2C_DELAY_US);
    const int ret = gpio_get_level((gpio_num_t)I2C_PIN_SDA) ? -1 : 0;
    I2C_SCL_Low();
    esp_rom_delay_us(I2C_DELAY_US);

    return ret;
}

// Read an I2C data buffer.
int I2C_ReadBuffer(void *pBuffer, uint8_t Size) {
    uint8_t *pData = (uint8_t *)pBuffer;
    uint8_t i;

    if (Size == 1) {
        *pData = I2C_Read(true);
        return 1;
    }

    for (i = 0; i < Size - 1; i++) {
        esp_rom_delay_us(1);
        pData[i] = I2C_Read(false);
    }

    esp_rom_delay_us(1);
    pData[i++] = I2C_Read(true);

    return Size;
}

// Write an I2C data buffer.
int I2C_WriteBuffer(const void *pBuffer, uint8_t Size) {
    const uint8_t *pData = (const uint8_t *)pBuffer;
    uint8_t i;

    for (i = 0; i < Size; i++) {
        if (I2C_Write(*pData++) < 0) {
            return -1;
        }
    }

    return 0;
}
