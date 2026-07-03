#ifndef SRC_SERVICES_CONFIG_NOTIFY_H
#define SRC_SERVICES_CONFIG_NOTIFY_H

// Global configuration-generation counter. Every code path that persists a
// setting (web portal, AT console, BLE provisioning, touch UI) bumps it; the
// LCD UI polls the generation and rebuilds the visible form page when the
// config changed underneath it, so web/AT edits show up on screen without
// re-entering the page.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void CONFIG_NOTIFY_Bump(void);
uint32_t CONFIG_NOTIFY_Generation(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_CONFIG_NOTIFY_H
