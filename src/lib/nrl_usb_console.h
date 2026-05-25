#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// USB-Serial-JTAG console wrapper. Replaces Arduino's Serial.read/write/...
// on the AT-command USB-CDC console (the host sees the same VID/PID as the
// arduino-esp32 ARDUINO_USB_MODE=1 build).
//
// Idempotent install — safe to call multiple times.
bool NRL_USB_Console_Init(void);

// Number of bytes available to read without blocking. Returns 0 if the driver
// is not installed.
size_t NRL_USB_Console_Available(void);

// Non-blocking read. Returns the number of bytes actually copied into buffer
// (may be 0 if nothing is buffered).
size_t NRL_USB_Console_Read(uint8_t *buffer, size_t buffer_size);

// Blocking write of `size` bytes. Returns the number actually sent.
size_t NRL_USB_Console_Write(const uint8_t *data, size_t size);

// Best-effort drain of the TX buffer before a planned reboot.
void NRL_USB_Console_Flush(void);

#ifdef __cplusplus
}
#endif
