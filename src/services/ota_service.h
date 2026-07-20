#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Remote OTA is deliberately independent of ExternalRadioConfig so extending
// the radio configuration structure never invalidates existing EEPROM data.
constexpr size_t NRL_OTA_URL_MAX = 192u;
constexpr size_t NRL_OTA_VERSION_MAX = 64u;
constexpr size_t NRL_OTA_RELEASE_MAX = 8u;

struct NrlOtaRelease {
    char version[NRL_OTA_VERSION_MAX];
    char url[NRL_OTA_URL_MAX];
    char notes[160];
};

struct NrlOtaStatus {
    char server_url[NRL_OTA_URL_MAX];
    char last_error[128];
    char latest_version[NRL_OTA_VERSION_MAX];
    bool configured;
    bool checking;
    bool updating;
    uint32_t update_bytes;
    uint32_t update_size;
    uint8_t update_percent;
    uint32_t last_check_ms;
    size_t release_count;
    NrlOtaRelease releases[NRL_OTA_RELEASE_MAX];
};

bool OtaService_Init();
bool OtaService_SetConfig(const char *server_url, const char *device_token);
// Update only the server URL while preserving the optional device token.
// This is used by device-local UIs that intentionally do not expose tokens.
bool OtaService_SetServerUrl(const char *server_url);
void OtaService_GetStatus(NrlOtaStatus *out);
bool OtaService_CheckNow();
// Check the release manifest and, when a newer release exists, install it.
// Used by the non-touch Gezipai's VOL+ + VOL- chord and AT+OTA=LATEST.
bool OtaService_CheckAndUpdateLatest();
bool OtaService_UpdateVersion(const char *version);
