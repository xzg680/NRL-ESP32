#ifndef SRC_LIB_NRL_BT_HFP_H
#define SRC_LIB_NRL_BT_HFP_H

// Bluetooth hands-free (HFP Audio Gateway) link for routing the project's voice
// through a standard Bluetooth headset. The ESP32 acts as the Audio Gateway; the
// headset's microphone feeds the network uplink and the network downlink plays
// out of the headset's speaker. HFP narrowband (CVSD, 8 kHz mono PCM16) matches
// the project's 8 kHz G.711 pipeline directly, so no resampling is needed.
//
// Only S31 builds this for real (Bluedroid host, see the S31 board defaults
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
// Non-blocking: records the desired state and returns immediately; a dedicated
// BT task performs the (slow) stack transition, so a UI callback never freezes.
void NRL_BtHfp_SetEnabled(bool enabled);
bool NRL_BtHfp_IsEnabled(void);

// True while a SetEnabled request is still waiting to be applied by the BT task
// (i.e. the stack is mid-transition). Lets the UI show a transient state.
bool NRL_BtHfp_TogglePending(void);

// Periodic housekeeping (keeps the SCO link up, applies the headset PTT button,
// and applies any pending enable/disable request). Call from the main loop.
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

// ---- Headset speaker volume -------------------------------------------------
// Each headset remembers its own speaker volume (HFP gain 0..15, persisted with
// the saved-device list). On connect the saved value is pushed to the headset;
// pressing the headset's own volume keys updates it live. While a headset is
// connected the NRL device's volume buttons should drive these instead of the
// onboard codec.

// Current headset speaker volume as a percentage (0..100), or -1 if no headset
// is connected.
int NRL_BtHfp_GetVolumePercent(void);

// Step the connected headset's speaker volume by one HFP gain unit
// (direction > 0 = louder, < 0 = quieter; ~6.7 % per step). No-op when not
// connected. Applies to the headset and saves the value for that device.
void NRL_BtHfp_AdjustVolume(int direction);

// Set the connected headset's speaker volume to an absolute percentage (0..100,
// snapped to the nearest HFP gain step). No-op when not connected. Applies to
// the headset and saves the value for that device. Used by the on-screen slider.
void NRL_BtHfp_SetVolumePercent(int percent);

// ---- A2DP source: music streaming to the headset ---------------------------
// The IDF6 A2DP source takes app-encoded SBC frames; this module owns the
// SBC encoder (fixed 44.1 kHz joint-stereo endpoint) and a paced TX task.
// The music player feeds 44.1 kHz stereo PCM through NRL_BtA2dp_Write. On
// boards without Bluedroid A2DP all of these are no-op stubs.

// Async: connect the A2DP channel to the current HFP headset peer and start
// media streaming. Returns false when no peer is known / BT is off.
bool NRL_BtA2dp_RequestStart(void);

// Suspend media streaming (connection stays for a quick restart).
void NRL_BtA2dp_RequestStop(void);

// True once the media channel is started and the TX task is running.
bool NRL_BtA2dp_IsStreaming(void);

// Queue 44.1 kHz stereo interleaved PCM (frame = L+R pair). Blocks briefly
// while the ring is full (this paces the producer); returns frames accepted
// (0 when not streaming).
size_t NRL_BtA2dp_Write(const int16_t *stereo, size_t frames);

#ifdef __cplusplus
}
#endif

#endif // SRC_LIB_NRL_BT_HFP_H
