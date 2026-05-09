#ifndef DRIVER_STATUS_IO_H
#define DRIVER_STATUS_IO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void STATUS_IO_Init(void);
void STATUS_IO_Poll(void);
void STATUS_IO_SetPttActive(bool active);
void STATUS_IO_NotifyHeartbeatReceived(void);
bool STATUS_IO_IsSqlActive(void);
bool STATUS_IO_IsPttActive(void);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_STATUS_IO_H
