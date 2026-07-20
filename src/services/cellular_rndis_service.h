#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool CELLULAR_RNDIS_Init(void);
bool CELLULAR_RNDIS_IsStarted(void);
bool CELLULAR_RNDIS_HasIp(void);
uint32_t CELLULAR_RNDIS_Ip(void);

#ifdef __cplusplus
}
#endif
