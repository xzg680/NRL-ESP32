#include "touch_bi4umd.h"

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_BI4UMD

#include "i2c1.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr uint8_t kRegTouchCount = 0x02;
constexpr uint8_t kMaxTouchCount = 5;
const char *TAG = "BI4UMD_TOUCH";
bool s_ready = false;

bool readRegisters(const uint8_t reg, uint8_t *data, const uint8_t size)
{
    if (data == nullptr || size == 0) {
        return false;
    }

    bool ok = true;
    I2C_Start();
    if (I2C_Write((NRL_TOUCH_I2C_ADDR << 1) | I2C_WRITE) < 0 ||
        I2C_Write(reg) < 0) {
        ok = false;
        goto stop;
    }
    I2C_Start();
    if (I2C_Write((NRL_TOUCH_I2C_ADDR << 1) | I2C_READ) < 0) {
        ok = false;
        goto stop;
    }
    I2C_ReadBuffer(data, size);

stop:
    I2C_Stop();
    return ok;
}

} // namespace

bool BI4UMD_Touch_Init(void)
{
    gpio_config_t input = {};
    input.pin_bit_mask = 1ULL << NRL_PIN_TOUCH_INT;
    input.mode = GPIO_MODE_INPUT;
    input.pull_up_en = GPIO_PULLUP_ENABLE;
    input.pull_down_en = GPIO_PULLDOWN_DISABLE;
    input.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&input);

    gpio_config_t reset = {};
    reset.pin_bit_mask = 1ULL << NRL_PIN_TOUCH_RST;
    reset.mode = GPIO_MODE_OUTPUT;
    gpio_config(&reset);
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_TOUCH_RST), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_TOUCH_RST), 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    I2C_Init();
    uint8_t count = 0;
    if (!readRegisters(kRegTouchCount, &count, 1)) {
        ESP_LOGW(TAG, "controller 0x%02X not found", NRL_TOUCH_I2C_ADDR);
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "FT5x06/FT6x36 touch ready (SCL=%d SDA=%d INT=%d RST=%d)",
             NRL_PIN_I2C_SCL, NRL_PIN_I2C_SDA,
             NRL_PIN_TOUCH_INT, NRL_PIN_TOUCH_RST);
    return true;
}

bool BI4UMD_Touch_Read(uint16_t *x, uint16_t *y)
{
    if (!s_ready || x == nullptr || y == nullptr) {
        return false;
    }

    // Reading while INT is high is still needed once to observe release, but
    // the controller's touch-count register makes that read unambiguous.
    uint8_t point[5] = {};
    if (!readRegisters(kRegTouchCount, point, sizeof(point))) {
        return false;
    }
    const uint8_t count = point[0] & 0x0F;
    if (count == 0 || count > kMaxTouchCount) {
        return false;
    }

    *x = static_cast<uint16_t>(((point[1] & 0x0F) << 8) | point[2]);
    *y = static_cast<uint16_t>(((point[3] & 0x0F) << 8) | point[4]);
    return *x < NRL_DISPLAY_WIDTH && *y < NRL_DISPLAY_HEIGHT;
}

#else

bool BI4UMD_Touch_Init(void) { return false; }
bool BI4UMD_Touch_Read(uint16_t *, uint16_t *) { return false; }

#endif
