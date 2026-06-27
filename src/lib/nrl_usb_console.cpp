#include "nrl_usb_console.h"

#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>

static const char *TAG = "USB";

namespace {

constexpr size_t kRxBufferBytes = 1024;
constexpr size_t kTxBufferBytes = 1024;

bool s_installed = false;

// One-byte "peek" buffer. The IDF usb_serial_jtag driver exposes a "read with
// timeout" API but no peek count. To support the Arduino-style
// available()/read() pattern used by pollSerialAtConsole(), Available()
// stashes a byte if one is buffered; Read() drains the stash first.
uint8_t s_peek_byte = 0;
bool s_peek_valid = false;

} // namespace

extern "C" bool NRL_USB_Console_Init(void)
{
    if (s_installed) {
        return true;
    }
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.rx_buffer_size = kRxBufferBytes;
    cfg.tx_buffer_size = kTxBufferBytes;
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %d", err);
        return false;
    }
    // Route stdout/stderr (and therefore ESP_LOG) through the just-installed
    // driver. With CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG the console otherwise keeps
    // writing straight to the USB-JTAG FIFO, which collides with this driver once
    // it is installed and silences all further log output. Switching the VFS to
    // the driver lets logging and the AT-command reads share the port.
    usb_serial_jtag_vfs_use_driver();
    s_installed = true;
    ESP_LOGI(TAG, "USB-Serial-JTAG console ready");
    return true;
}

extern "C" size_t NRL_USB_Console_Available(void)
{
    if (!s_installed) {
        return 0u;
    }
    if (s_peek_valid) {
        return 1u;
    }
    const int got = usb_serial_jtag_read_bytes(&s_peek_byte, 1, 0);
    if (got > 0) {
        s_peek_valid = true;
        return 1u;
    }
    return 0u;
}

extern "C" size_t NRL_USB_Console_Read(uint8_t *buffer, const size_t buffer_size)
{
    if (!s_installed || buffer == nullptr || buffer_size == 0u) {
        return 0u;
    }
    size_t off = 0u;
    if (s_peek_valid) {
        buffer[off++] = s_peek_byte;
        s_peek_valid = false;
        if (off >= buffer_size) {
            return off;
        }
    }
    const int got = usb_serial_jtag_read_bytes(buffer + off, buffer_size - off, 0);
    if (got > 0) {
        off += static_cast<size_t>(got);
    }
    return off;
}

extern "C" size_t NRL_USB_Console_Write(const uint8_t *data, const size_t size)
{
    if (!s_installed || data == nullptr || size == 0u) {
        return 0u;
    }
    const int wrote = usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(100));
    return (wrote > 0) ? static_cast<size_t>(wrote) : 0u;
}

extern "C" void NRL_USB_Console_Flush(void)
{
    if (!s_installed) {
        return;
    }
    // The IDF driver doesn't expose a tx-drain primitive; a short delay is the
    // conventional best-effort wait.
    vTaskDelay(pdMS_TO_TICKS(50));
}
