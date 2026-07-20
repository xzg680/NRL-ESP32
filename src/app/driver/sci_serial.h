#ifndef DRIVER_SCI_SERIAL_H
#define DRIVER_SCI_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool SCI_SERIAL_Init(void);
void SCI_SERIAL_Stop(void);
bool SCI_SERIAL_ApplyConfig(uint32_t baud, uint8_t data_bits, char parity, uint8_t stop_bits);
bool SCI_SERIAL_ReloadPins(void);
int SCI_SERIAL_Available(void);
size_t SCI_SERIAL_Read(uint8_t *buffer, size_t buffer_size);
size_t SCI_SERIAL_Write(const uint8_t *data, size_t data_size);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_SCI_SERIAL_H
