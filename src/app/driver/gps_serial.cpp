#include "gps_serial.h"

#include "serial_port_config.h"

#include <driver/uart.h>
#include <esp_log.h>

#include <string.h>

namespace {

constexpr const char *TAG = "GPS";
constexpr uart_port_t kGpsPort = UART_NUM_2;
constexpr size_t kRxBufferBytes = 1024u;
bool s_ready = false;
bool s_driver_installed = false;

bool buildConfig(const SerialPortConfig &serial, uart_config_t *out)
{
    if (out == nullptr) return false;
    memset(out, 0, sizeof(*out));
    out->baud_rate = static_cast<int>(serial.uart2_baud);
    out->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    out->source_clk = UART_SCLK_DEFAULT;
    switch (serial.uart2_data_bits) {
        case 5: out->data_bits = UART_DATA_5_BITS; break;
        case 6: out->data_bits = UART_DATA_6_BITS; break;
        case 7: out->data_bits = UART_DATA_7_BITS; break;
        case 8: out->data_bits = UART_DATA_8_BITS; break;
        default: return false;
    }
    switch (serial.uart2_parity) {
        case 'N': out->parity = UART_PARITY_DISABLE; break;
        case 'E': out->parity = UART_PARITY_EVEN; break;
        case 'O': out->parity = UART_PARITY_ODD; break;
        default: return false;
    }
    switch (serial.uart2_stop_bits) {
        case 1: out->stop_bits = UART_STOP_BITS_1; break;
        case 2: out->stop_bits = UART_STOP_BITS_2; break;
        default: return false;
    }
    return true;
}

void stopDriver()
{
    if (s_driver_installed) uart_driver_delete(kGpsPort);
    s_driver_installed = false;
    s_ready = false;
}

} // namespace

extern "C" bool GPS_SERIAL_Init(void)
{
    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    if (!serial.uart2_enabled) {
        stopDriver();
        return true;
    }
    if (s_ready) return true;
    uart_config_t cfg{};
    if (!buildConfig(serial, &cfg)) return false;

    if (uart_driver_install(kGpsPort, kRxBufferBytes, 0, 0, nullptr, 0) != ESP_OK) {
        ESP_LOGE(TAG, "UART2 driver install failed");
        return false;
    }
    s_driver_installed = true;
    if (uart_param_config(kGpsPort, &cfg) != ESP_OK ||
        uart_set_pin(kGpsPort, serial.uart2_tx_pin, serial.uart2_rx_pin,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        stopDriver();
        ESP_LOGE(TAG, "UART2 config failed");
        return false;
    }
    s_ready = true;
    ESP_LOGI(TAG, "UART2 ready: %lu,%u,%c,%u rx=%d tx=%d",
             static_cast<unsigned long>(serial.uart2_baud),
             static_cast<unsigned>(serial.uart2_data_bits), serial.uart2_parity,
             static_cast<unsigned>(serial.uart2_stop_bits),
             serial.uart2_rx_pin, serial.uart2_tx_pin);
    return true;
}

extern "C" void GPS_SERIAL_Stop(void)
{
    stopDriver();
}

extern "C" bool GPS_SERIAL_ReloadConfig(void)
{
    stopDriver();
    return GPS_SERIAL_Init();
}

extern "C" size_t GPS_SERIAL_Read(uint8_t *buffer, const size_t buffer_size)
{
    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    if (!serial.uart2_enabled || buffer == nullptr || buffer_size == 0u ||
        !GPS_SERIAL_Init()) return 0u;
    const int got = uart_read_bytes(kGpsPort, buffer, buffer_size, 0);
    return got > 0 ? static_cast<size_t>(got) : 0u;
}
