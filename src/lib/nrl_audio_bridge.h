#ifndef SRC_LIB_NRL_AUDIO_BRIDGE_H
#define SRC_LIB_NRL_AUDIO_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool NRLAudioBridge_Init(void);
bool NRLAudioBridge_GetRemoteIdentity(char *buffer, size_t buffer_size);
void NRLAudioBridge_ApplyConfig(bool restart_wifi, bool restart_udp);

// Poll the USB debug serial for AT commands typed by the user. Lines are
// terminated by CR/LF; e.g. "AT" lists commands, "AT+WIFI_SSID=MyNet" sets it.
void NRLAudioBridge_PollSerialConsole(void);

#ifdef __cplusplus
}
#endif

#endif
