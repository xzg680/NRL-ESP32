#ifndef SRC_LIB_WIFI_CONFIG_PORTAL_H
#define SRC_LIB_WIFI_CONFIG_PORTAL_H

#include <stddef.h>

bool WifiConfigPortal_Init(void);
void WifiConfigPortal_Poll(void);
void WifiConfigPortal_EnterFallbackMode(void);
// Returns the exact SSID used by the configuration access point.
void WifiConfigPortal_GetApSsid(char *out, size_t out_size);

#endif // SRC_LIB_WIFI_CONFIG_PORTAL_H
