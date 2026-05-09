#include "sci_serial.h"

#include "board_pins.h"
#include "external_radio.h"

#include <Arduino.h>

namespace {

HardwareSerial s_sci_serial(1);
bool s_sci_ready = false;
uint32_t s_sci_baud = 0u;
uint8_t s_sci_data_bits = 0u;
char s_sci_parity = '\0';
uint8_t s_sci_stop_bits = 0u;

static uint32_t serialConfig(const uint8_t data_bits, const char parity, const uint8_t stop_bits)
{
    switch (data_bits) {
        case 5:
            if (parity == 'E') {
                return (stop_bits == 2u) ? SERIAL_5E2 : SERIAL_5E1;
            }
            if (parity == 'O') {
                return (stop_bits == 2u) ? SERIAL_5O2 : SERIAL_5O1;
            }
            return (stop_bits == 2u) ? SERIAL_5N2 : SERIAL_5N1;
        case 6:
            if (parity == 'E') {
                return (stop_bits == 2u) ? SERIAL_6E2 : SERIAL_6E1;
            }
            if (parity == 'O') {
                return (stop_bits == 2u) ? SERIAL_6O2 : SERIAL_6O1;
            }
            return (stop_bits == 2u) ? SERIAL_6N2 : SERIAL_6N1;
        case 7:
            if (parity == 'E') {
                return (stop_bits == 2u) ? SERIAL_7E2 : SERIAL_7E1;
            }
            if (parity == 'O') {
                return (stop_bits == 2u) ? SERIAL_7O2 : SERIAL_7O1;
            }
            return (stop_bits == 2u) ? SERIAL_7N2 : SERIAL_7N1;
        case 8:
        default:
            if (parity == 'E') {
                return (stop_bits == 2u) ? SERIAL_8E2 : SERIAL_8E1;
            }
            if (parity == 'O') {
                return (stop_bits == 2u) ? SERIAL_8O2 : SERIAL_8O1;
            }
            return (stop_bits == 2u) ? SERIAL_8N2 : SERIAL_8N1;
    }
}

} // namespace

extern "C" bool SCI_SERIAL_Init(void)
{
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
    if (baud == 0u || data_bits < 5u || data_bits > 8u ||
        (parity != 'N' && parity != 'E' && parity != 'O') ||
        (stop_bits != 1u && stop_bits != 2u)) {
        return false;
    }

    if (s_sci_ready &&
        s_sci_baud == baud &&
        s_sci_data_bits == data_bits &&
        s_sci_parity == parity &&
        s_sci_stop_bits == stop_bits) {
        return true;
    }

    if (s_sci_ready) {
        s_sci_serial.flush();
        s_sci_serial.end();
        s_sci_ready = false;
    }

    s_sci_serial.setRxBufferSize(512);
    s_sci_serial.begin(baud, serialConfig(data_bits, parity, stop_bits), NRL_PIN_SCI_RX, NRL_PIN_SCI_TX);
    s_sci_serial.setTimeout(0);
    s_sci_baud = baud;
    s_sci_data_bits = data_bits;
    s_sci_parity = parity;
    s_sci_stop_bits = stop_bits;
    s_sci_ready = true;
    Serial.printf("[SCI] ready: %lu,%u,%c,%u rx=%d tx=%d\n",
                  static_cast<unsigned long>(baud),
                  static_cast<unsigned>(data_bits),
                  parity,
                  static_cast<unsigned>(stop_bits),
                  NRL_PIN_SCI_RX,
                  NRL_PIN_SCI_TX);
    return true;
}

extern "C" int SCI_SERIAL_Available(void)
{
    if (!SCI_SERIAL_Init()) {
        return 0;
    }
    return s_sci_serial.available();
}

extern "C" size_t SCI_SERIAL_Read(uint8_t *buffer, const size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u || !SCI_SERIAL_Init()) {
        return 0u;
    }

    size_t read = 0u;
    while (read < buffer_size && s_sci_serial.available() > 0) {
        const int value = s_sci_serial.read();
        if (value < 0) {
            break;
        }
        buffer[read++] = static_cast<uint8_t>(value);
    }
    return read;
}

extern "C" size_t SCI_SERIAL_Write(const uint8_t *data, const size_t data_size)
{
    if (data == nullptr || data_size == 0u || !SCI_SERIAL_Init()) {
        return 0u;
    }
    return s_sci_serial.write(data, data_size);
}
