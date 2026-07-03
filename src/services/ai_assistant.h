#ifndef SRC_SERVICES_AI_ASSISTANT_H
#define SRC_SERVICES_AI_ASSISTANT_H

// xiaozhi AI voice assistant client (docs/architecture.md 功能6, open-source
// xiaozhi-esp32 protocol v1 over WebSocket):
//   - hello handshake announcing Opus 16 kHz mono / 60 ms frames
//   - listen start/stop JSON control + binary Opus mic frames (shared
//     media/opus_voice codec, 60 ms instance)
//   - server streams STT/LLM/TTS JSON states + binary Opus TTS frames,
//     decoded and played through the audio router (AUDIO_SRC_AI -> speaker)
// Server URL + access token persist in NVS (works with the public xiaozhi
// service or a self-hosted xiaozhi-server). S31-only (stubs elsewhere).

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Restore persisted config; connects in the background once WiFi is up
// (when enabled). Call once at startup.
void AI_Init(void);

// Configure server URL (ws:// or wss://) and access token; persists and
// reconnects. Empty token is allowed (self-hosted servers without auth).
bool AI_Configure(const char *url, const char *token);

// Runtime on/off (persisted). Enabling connects, disabling closes the link.
bool AI_SetEnabled(bool enabled);
bool AI_IsEnabled(void);

// True while the WebSocket session is up (hello exchanged).
bool AI_IsConnected(void);

// Start/stop a listening turn (push-to-talk): mic audio streams to the
// server while listening; the reply plays through the speaker.
bool AI_StartListen(void);
void AI_StopListen(void);
bool AI_IsListening(void);

// Human-readable status line for the AT console / UI.
void AI_Describe(char *out, size_t out_size);

// Current configuration (for the web form). Buffers may be NULL to skip.
void AI_GetConfig(char *url, size_t url_size, char *token, size_t token_size);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_AI_ASSISTANT_H
