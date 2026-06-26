#include "s31_i2c.h"

#if NRL_BOARD == NRL_BOARD_S31_KORVO

#include <esp_err.h>
#include <esp_log.h>

static const char *TAG = "S31_I2C";

namespace {

constexpr int kI2cPort = I2C_NUM_0;
i2c_master_bus_handle_t s_i2c_bus = nullptr;

} // namespace

bool S31_I2C_GetBus(i2c_master_bus_handle_t *out_bus)
{
    if (out_bus == nullptr) {
        return false;
    }
    if (s_i2c_bus != nullptr) {
        *out_bus = s_i2c_bus;
        return true;
    }

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = static_cast<i2c_port_num_t>(kI2cPort);
    bus_config.sda_io_num = static_cast<gpio_num_t>(NRL_PIN_I2C_SDA);
    bus_config.scl_io_num = static_cast<gpio_num_t>(NRL_PIN_I2C_SCL);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    const esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "bus ready: port=%d sda=%d scl=%d",
             kI2cPort, NRL_PIN_I2C_SDA, NRL_PIN_I2C_SCL);
    *out_bus = s_i2c_bus;
    return true;
}

#endif
