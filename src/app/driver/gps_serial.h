#ifndef DRIVER_GPS_SERIAL_H
#define DRIVER_GPS_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool GPS_SERIAL_Init(void);
bool GPS_SERIAL_ReloadConfig(void);
size_t GPS_SERIAL_Read(uint8_t *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_GPS_SERIAL_H
