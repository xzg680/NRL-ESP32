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
#include <Arduino.h>

static constexpr uint32_t I2C_DELAY_US = 5;

static inline void I2C_SDA_Low(void)
{
    pinMode(I2C_PIN_SDA, OUTPUT);
    digitalWrite(I2C_PIN_SDA, LOW);
}

static inline void I2C_SDA_Release(void)
{
    // 释放 SDA（开漏高）。使用内部上拉作为兜底（若外部上拉不存在/偏弱）�?
    pinMode(I2C_PIN_SDA, INPUT_PULLUP);
}

static inline void I2C_SCL_Low(void)
{
    pinMode(I2C_PIN_SCL, OUTPUT);
    digitalWrite(I2C_PIN_SCL, LOW);
}

static inline void I2C_SCL_Release(void)
{
    // 释放 SCL（开漏高）。EEPROM 通常不拉伸时钟，但开漏实现更稳�?
    pinMode(I2C_PIN_SCL, INPUT_PULLUP);
}

static inline void I2C_RestoreSharedPinsForKeyboard(void)
{
    // KEY4/KEY5 �?I2C 复用时，键盘矩阵需要列引脚为输出�?
    // I2C 传输结束后把它们恢复为输出高电平，避免按键被“留在输入上拉模式”�?
    pinMode(I2C_PIN_SDA, OUTPUT);
    digitalWrite(I2C_PIN_SDA, HIGH);
    pinMode(I2C_PIN_SCL, OUTPUT);
    digitalWrite(I2C_PIN_SCL, HIGH);
}

// I2C 初始�?
void I2C_Init(void) {
    // Optional I2C bus enable pin.
#ifdef I2C_PIN_EN
    pinMode(I2C_PIN_EN, OUTPUT);
    digitalWrite(I2C_PIN_EN, LOW);
#endif

    // 空闲状态：SCL/SDA 都为高（开漏释放）
    I2C_SDA_Release();
    I2C_SCL_Release();
}

// I2C 开始信�?
// SDA 从高变低，�?SCL 保持�?
void I2C_Start(void) {
    I2C_SDA_Release();
    I2C_SCL_Release();
    delayMicroseconds(I2C_DELAY_US);
    I2C_SDA_Low();
    delayMicroseconds(I2C_DELAY_US);
    I2C_SCL_Low();
    delayMicroseconds(I2C_DELAY_US);
}

// I2C 停止信号
// SDA 从低变高，�?SCL 为高
void I2C_Stop(void) {
    I2C_SDA_Low();
    delayMicroseconds(I2C_DELAY_US);
    I2C_SCL_Release();
    delayMicroseconds(I2C_DELAY_US);
    I2C_SDA_Release();
    delayMicroseconds(I2C_DELAY_US);

    // 兼容键盘扫描（共享引脚）
    I2C_RestoreSharedPinsForKeyboard();
}

// I2C 读取一个字�?
// bFinal: 最后一个字节时�?true（发�?NAK），否则�?false（发�?ACK�?
uint8_t I2C_Read(bool bFinal) {
    uint8_t Data = 0;

    // 释放 SDA 由从机驱�?
    I2C_SDA_Release();

    for (uint8_t i = 0; i < 8; i++) {
        I2C_SCL_Release();
        delayMicroseconds(I2C_DELAY_US);
        Data = (uint8_t)((Data << 1) | (digitalRead(I2C_PIN_SDA) ? 1U : 0U));
        I2C_SCL_Low();
        delayMicroseconds(I2C_DELAY_US);
    }

    // ACK/NAK
    if (bFinal) {
        I2C_SDA_Release(); // NAK
    } else {
        I2C_SDA_Low();     // ACK
    }
    delayMicroseconds(I2C_DELAY_US);
    I2C_SCL_Release();
    delayMicroseconds(I2C_DELAY_US);
    I2C_SCL_Low();
    delayMicroseconds(I2C_DELAY_US);
    I2C_SDA_Release();

    return Data;
}

// I2C 写入一个字�?
// 返回�? 0 表示收到 ACK�?1 表示未收�?ACK
int I2C_Write(uint8_t Data) {
    // 发�?8 位数据（MSB first�?
    for (uint8_t i = 0; i < 8; i++) {
        if (Data & 0x80) {
            I2C_SDA_Release();
        } else {
            I2C_SDA_Low();
        }
        Data <<= 1;
        delayMicroseconds(I2C_DELAY_US);
        I2C_SCL_Release();
        delayMicroseconds(I2C_DELAY_US);
        I2C_SCL_Low();
        delayMicroseconds(I2C_DELAY_US);
    }

    // ACK：释�?SDA，由从机拉低
    I2C_SDA_Release();
    delayMicroseconds(I2C_DELAY_US);
    I2C_SCL_Release();
    delayMicroseconds(I2C_DELAY_US);
    const int ret = digitalRead(I2C_PIN_SDA) ? -1 : 0;
    I2C_SCL_Low();
    delayMicroseconds(I2C_DELAY_US);

    return ret;
}

// I2C 读取数据缓冲�?
int I2C_ReadBuffer(void *pBuffer, uint8_t Size) {
    uint8_t *pData = (uint8_t *)pBuffer;
    uint8_t i;
    
    if (Size == 1) {
        *pData = I2C_Read(true);
        return 1;
    }
    
    for (i = 0; i < Size - 1; i++) {
        delayMicroseconds(1);
        pData[i] = I2C_Read(false);
    }
    
    delayMicroseconds(1);
    pData[i++] = I2C_Read(true);
    
    return Size;
}

// I2C 写入数据缓冲�?
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