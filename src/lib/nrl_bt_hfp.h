#ifndef SRC_LIB_NRL_BT_HFP_H
#define SRC_LIB_NRL_BT_HFP_H

// Bluetooth hands-free (HFP Audio Gateway) link for routing the project's voice
// through a standard Bluetooth headset. The ESP32 acts as the Audio Gateway; the
// headset's microphone feeds the network uplink and the network downlink plays
// out of the headset's speaker. HFP narrowband (CVSD, 8 kHz mono PCM16) matches
// the project's 8 kHz G.711 pipeline directly, so no resampling is needed.
//
// Only S31 builds this for real (Bluedroid host, see sdkconfig.defaults.esp32s31
// + CONFIG_BT_HFP_AG_ENABLE). On every other board these are no-op stubs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-time init of internal state. Does NOT bring the radio up. Safe on any board.
void NRL_BtHfp_Init(void);

// Runtime on/off. Enabling brings up the Bluedroid Classic-BT + HFP AG stack,
// makes the device discoverable/connectable and (auto-)connects a headset.
// Disabling tears the stack down so the radio is free for Wi-Fi-only voice.
void NRL_BtHfp_SetEnabled(bool enabled);
bool NRL_BtHfp_IsEnabled(void);

// Periodic housekeeping (keeps the SCO link up, applies the headset PTT button).
// Call from the main loop.
void NRL_BtHfp_Poll(void);

// Start a Bluetooth inquiry for nearby devices. Results accumulate and can be
// read with the GetDevice* calls below; the headset has no screen, so the user
// picks one from the NRL device's list. No-op unless enabled.
void NRL_BtHfp_StartScan(void);
bool NRL_BtHfp_IsScanning(void);

// Discovered-device list (from the most recent scan).
size_t NRL_BtHfp_GetDeviceCount(void);
// Fills `out` with the i-th device's display name (or its address if unnamed).
// Returns false if i is out of range.
bool NRL_BtHfp_GetDeviceName(size_t index, char *out, size_t out_size);

// Connect (pair if needed) to the i-th discovered device and keep its voice
// channel up. The chosen device becomes the auto-reconnect target.
void NRL_BtHfp_ConnectIndex(size_t index);

// Saved (previously paired) headsets. Persisted to NVS; auto-reconnected on
// boot (most-recent first). The user can connect or delete them.
size_t NRL_BtHfp_GetSavedCount(void);
bool NRL_BtHfp_GetSavedName(size_t index, char *out, size_t out_size);
void NRL_BtHfp_ConnectSaved(size_t index);
void NRL_BtHfp_RemoveSaved(size_t index);  // forget: removes the bond too

// True once a headset service-level connection exists.
bool NRL_BtHfp_IsConnected(void);

// True while the SCO/voice link is up -- when true, voice must route to/from the
// headset instead of the onboard codec.
bool NRL_BtHfp_IsAudioActive(void);

// Connected peer name (empty when not connected). Returns bytes written (excl. NUL).
size_t NRL_BtHfp_GetPeerName(char *out, size_t out_size);

// Queue downlink (network->headset) PCM16 mono 8 kHz samples for the headset
// speaker. Called by the audio bridge while IsAudioActive(). Excess is dropped.
void NRL_BtHfp_PushPlayback(const int16_t *samples, size_t sample_count);

#ifdef __cplusplus
}
#endif

#endif // SRC_LIB_NRL_BT_HFP_H
