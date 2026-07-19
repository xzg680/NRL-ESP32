#include "sci_serial.h"

#include "board_pins.h"
#include "external_radio.h"
#include "serial_port_config.h"

#include <driver/uart.h>
#include <esp_log.h>

#include <string.h>

static const char *TAG = "SCI";

namespace {

// SCI is always UART1. UART0 remains reserved for the system log/AT console;
// UART2 is the independent GPS/NMEA port. RX/TX pins come from the persisted
// serial-port config, whose board defaults live in board_pins.h.
constexpr uart_port_t kSciPort = UART_NUM_1;
constexpr size_t kSciRxBufBytes = 512u;

bool s_sci_ready = false;
bool s_sci_driver_installed = false;
uint32_t s_sci_baud = 0u;
uint8_t s_sci_data_bits = 0u;
char s_sci_parity = '\0';
uint8_t s_sci_stop_bits = 0u;

static void stopDriver()
{
    if (s_sci_driver_installed) uart_driver_delete(kSciPort);
    s_sci_driver_installed = false;
    s_sci_ready = false;
}

static bool buildUartConfig(uint32_t baud, uint8_t data_bits, char parity,
                            uint8_t stop_bits, uart_config_t &out)
{
    memset(&out, 0, sizeof(out));
    out.baud_rate = static_cast<int>(baud);
    out.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    out.source_clk = UART_SCLK_DEFAULT;

    switch (data_bits) {
        case 5: out.data_bits = UART_DATA_5_BITS; break;
        case 6: out.data_bits = UART_DATA_6_BITS; break;
        case 7: out.data_bits = UART_DATA_7_BITS; break;
        case 8: out.data_bits = UART_DATA_8_BITS; break;
        default: return false;
    }

    switch (parity) {
        case 'N': out.parity = UART_PARITY_DISABLE; break;
        case 'E': out.parity = UART_PARITY_EVEN; break;
        case 'O': out.parity = UART_PARITY_ODD; break;
        default: return false;
    }

    switch (stop_bits) {
        case 1: out.stop_bits = UART_STOP_BITS_1; break;
        case 2: out.stop_bits = UART_STOP_BITS_2; break;
        default: return false;
    }

    return true;
}

} // namespace

extern "C" bool SCI_SERIAL_Init(void)
{
#if defined(NRL_HAS_SCI_SERIAL) && !NRL_HAS_SCI_SERIAL
    return true;
#endif

    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    if (!serial.uart1_enabled) {
        stopDriver();
        return true;
    }
    if (s_sci_ready) {
        return true;
    }

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (config == nullptr) {
        return false;
    }
    return SCI_SERIAL_ApplyConfig(config->sci.baud,
                                  config->sci.data_bits,
                                  config->sci.parity,
                                  config->sci.stop_bits);
}

extern "C" bool SCI_SERIAL_ApplyConfig(const uint32_t baud,
                                       const uint8_t data_bits,
                                       const char parity,
                                       const uint8_t stop_bits)
{
#if defined(NRL_HAS_SCI_SERIAL) && !NRL_HAS_SCI_SERIAL
    (void)baud;
    (void)data_bits;
    (void)parity;
    (void)stop_bits;
    return true;
#endif

    if (baud == 0u || data_bits < 5u || data_bits > 8u ||
        (parity != 'N' && parity != 'E' && parity != 'O') ||
        (stop_bits != 1u && stop_bits != 2u)) {
        return false;
    }

    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    if (!serial.uart1_enabled) {
        stopDriver();
        return true;
    }

    if (s_sci_ready &&
        s_sci_baud == baud &&
        s_sci_data_bits == data_bits &&
        s_sci_parity == parity &&
        s_sci_stop_bits == stop_bits) {
        return true;
    }

    uart_config_t cfg{};
    if (!buildUartConfig(baud, data_bits, parity, stop_bits, cfg)) {
        return false;
    }

    if (s_sci_driver_installed) {
        // Reinstalling lets us change parity/stop-bits/data-bits cleanly;
        // uart_param_config alone is sufficient for baud-only changes but a
        // full reinstall keeps the code path uniform.
        stopDriver();
    }

    if (uart_driver_install(kSciPort, kSciRxBufBytes, /*tx_buf=*/0,
                            /*queue_size=*/0, /*event_queue=*/nullptr,
                            /*intr_alloc_flags=*/0) != ESP_OK) {
        return false;
    }
    s_sci_driver_installed = true;

    if (uart_param_config(kSciPort, &cfg) != ESP_OK) {
        uart_driver_delete(kSciPort);
        s_sci_driver_installed = false;
        return false;
    }

    if (uart_set_pin(kSciPort, serial.uart1_tx_pin, serial.uart1_rx_pin,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete(kSciPort);
        s_sci_driver_installed = false;
        return false;
    }

    s_sci_baud = baud;
    s_sci_data_bits = data_bits;
    s_sci_parity = parity;
    s_sci_stop_bits = stop_bits;
    s_sci_ready = true;
    ESP_LOGI(TAG, "ready: %lu,%u,%c,%u rx=%d tx=%d",
             static_cast<unsigned long>(baud),
             static_cast<unsigned>(data_bits),
             parity,
             static_cast<unsigned>(stop_bits),
             serial.uart1_rx_pin,
             serial.uart1_tx_pin);
    return true;
}

extern "C" bool SCI_SERIAL_ReloadPins(void)
{
#if defined(NRL_HAS_SCI_SERIAL) && !NRL_HAS_SCI_SERIAL
    return true;
#endif
    stopDriver();
    return SCI_SERIAL_Init();
}

extern "C" int SCI_SERIAL_Available(void)
{
#if defined(NRL_HAS_SCI_SERIAL) && !NRL_HAS_SCI_SERIAL
    return 0;
#endif

    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    if (!serial.uart1_enabled || !SCI_SERIAL_Init()) {
        return 0;
    }
    size_t buffered = 0;
    if (uart_get_buffered_data_len(kSciPort, &buffered) != ESP_OK) {
        return 0;
    }
    return static_cast<int>(buffered);
}

extern "C" size_t SCI_SERIAL_Read(uint8_t *buffer, const size_t buffer_size)
{
#if defined(NRL_HAS_SCI_SERIAL) && !NRL_HAS_SCI_SERIAL
    (void)buffer;
    (void)buffer_size;
    return 0u;
#endif

    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    if (!serial.uart1_enabled || buffer == nullptr || buffer_size == 0u ||
        !SCI_SERIAL_Init()) {
        return 0u;
    }

    // Non-blocking read (timeout=0) matches the original Serial.setTimeout(0)
    // behavior: return whatever is available right now.
    const int got = uart_read_bytes(kSciPort, buffer, buffer_size, 0);
    return (got > 0) ? static_cast<size_t>(got) : 0u;
}

extern "C" size_t SCI_SERIAL_Write(const uint8_t *data, const size_t data_size)
{
#if defined(NRL_HAS_SCI_SERIAL) && !NRL_HAS_SCI_SERIAL
    (void)data;
    (void)data_size;
    return 0u;
#endif

    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    if (!serial.uart1_enabled || data == nullptr || data_size == 0u ||
        !SCI_SERIAL_Init()) {
        return 0u;
    }
    const int wrote = uart_write_bytes(kSciPort, reinterpret_cast<const char *>(data), data_size);
    return (wrote > 0) ? static_cast<size_t>(wrote) : 0u;
}
