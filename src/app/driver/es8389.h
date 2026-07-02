#ifndef DRIVER_ES8389_H
#define DRIVER_ES8389_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ES8389_Init(void);
bool ES8389_IsReady(void);
bool ES8389_SetReceiveMode(void);
bool ES8389_SetOutputVolume(uint8_t value);
bool ES8389_SetInputGain(uint8_t value);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_ES8389_H
