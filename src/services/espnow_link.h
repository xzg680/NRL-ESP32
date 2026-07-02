#ifndef SRC_SERVICES_ESPNOW_LINK_H
#define SRC_SERVICES_ESPNOW_LINK_H

// ESP-NOW voice link (docs/architecture.md 功能7): off-grid short-range
// intercom between NRL-ESP32 devices, no AP or server needed. Voice frames
// are G.711 A-law over broadcast ESP-NOW (20 ms / 160 bytes per packet,
// same codec as the NRL uplink), tagged with the local callsign.
//
// Routing (audio router): mic -> AUDIO_SINK_ESPNOW when enabled (subject to
// the same squelch gating as the NRL uplink; NRL simulcast keeps working),
// received frames -> AUDIO_SRC_ESPNOW -> speaker.
//
// ESP-NOW shares the WiFi radio; it works alongside the STA connection on
// the same channel. Enable state persists in NVS.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Restore the persisted enable state (call after WiFi init).
void ESPNOW_LINK_Init(void);

// Enable/disable the link at runtime; persists. Enabling requires WiFi
// to be started (returns false otherwise).
bool ESPNOW_LINK_SetEnabled(bool enabled);

bool ESPNOW_LINK_IsEnabled(void);

// Callsign-SSID of the most recent peer heard (empty when none yet).
void ESPNOW_LINK_GetLastPeer(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_ESPNOW_LINK_H
