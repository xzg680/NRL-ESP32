#include "external_radio.h"

#include "board_pins.h"
#include "eeprom.h"
#include "es8311.h"
#include "../../lib/nrl_audio_config.h"

#ifndef ENABLE_OPENCV
#include <Arduino.h>
#else
#include "../opencv/Arduino.hpp"
#endif

#include <ctype.h>
#include <stdint.h>
#include <string.h>

namespace {

constexpr uint32_t kConfigAddress = 0x2C00U;
constexpr uint32_t kConfigMagic = 0x58455655U;
constexpr uint8_t kConfigVersion = 1U;
constexpr uint8_t kMinChannel = 0U;
constexpr uint8_t kMaxChannel = 7U;
constexpr uint8_t kDefaultMicVolume = 0xE0U;
constexpr uint8_t kDefaultLineOutVolume = 0xFFU;
constexpr uint8_t kPersistedHpDriveOff = 1U;
constexpr uint8_t kPersistedHpDriveOn = 2U;
constexpr uint32_t kDefaultSciBaud = 9600U;
constexpr uint8_t kDefaultSciDataBits = 8U;
constexpr char kDefaultSciParity = 'N';
constexpr uint8_t kDefaultSciStopBits = 1U;

struct PersistedExternalRadioConfig {
    uint32_t magic;
    uint8_t version;
    uint8_t channel;
    uint16_t server_port;
    uint16_t local_port;
    uint8_t callsign_ssid;
    uint8_t device_mode;
    uint8_t mic_volume;
    uint8_t line_out_volume;
    char wifi_ssid[33];
    char wifi_password[65];
    char server_host[65];
    char callsign[7];
    uint8_t reserved[8];
} __attribute__((packed));

static_assert((sizeof(PersistedExternalRadioConfig) % 8U) == 0U, "config must stay 8-byte aligned");

ExternalRadioConfig s_config = {};
bool s_loaded = false;
uint8_t s_last_channel_encoded = 0xFFu;

static void copyBounded(char *dst, const size_t dst_size, const char *src)
{
    if (dst == nullptr || dst_size == 0U) {
        return;
    }

    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}

static void trimTrailingWhitespace(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t len = strlen(text);
    while (len > 0U) {
        const char ch = text[len - 1U];
        if (ch != '\r' && ch != '\n' && ch != ' ' && ch != '\t') {
            break;
        }
        text[--len] = '\0';
    }
}

static bool isPrintableAscii(const char ch)
{
    const unsigned char value = static_cast<unsigned char>(ch);
    return value >= 32U && value <= 126U;
}

static void sanitizeString(char *text)
{
    if (text == nullptr) {
        return;
    }

    trimTrailingWhitespace(text);
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (!isPrintableAscii(text[i])) {
            text[i] = '_';
        }
    }
}

static void sanitizeCallsign(char *callsign)
{
    if (callsign == nullptr) {
        return;
    }

    sanitizeString(callsign);

    char compact[7] = {};
    size_t out = 0U;
    for (size_t i = 0; callsign[i] != '\0' && out < 6U; ++i) {
        const unsigned char ch = static_cast<unsigned char>(callsign[i]);
        if (isspace(ch)) {
            continue;
        }
        compact[out++] = static_cast<char>(toupper(ch));
    }
    compact[out] = '\0';
    copyBounded(callsign, 7U, compact);

    if (callsign[0] == '\0') {
        copyBounded(callsign, 7U, NRL_AUDIO_CALLSIGN);
    }
}

static uint8_t clampChannel(const uint8_t channel)
{
    if (channel < kMinChannel) {
        return kMinChannel;
    }
    if (channel > kMaxChannel) {
        return kMaxChannel;
    }
    return channel;
}

static char normalizeParity(const char parity)
{
    const char upper = static_cast<char>(toupper(static_cast<unsigned char>(parity)));
    if (upper == 'N' || upper == 'E' || upper == 'O') {
        return upper;
    }
    return '\0';
}

static bool isValidSciConfig(const uint32_t baud,
                             const uint8_t data_bits,
                             const char parity,
                             const uint8_t stop_bits)
{
    return baud >= 300U &&
           baud <= 921600U &&
           data_bits >= 5U &&
           data_bits <= 8U &&
           normalizeParity(parity) != '\0' &&
           (stop_bits == 1U || stop_bits == 2U);
}

static void applyDefaultSciConfig(void)
{
    s_config.sci.baud = kDefaultSciBaud;
    s_config.sci.data_bits = kDefaultSciDataBits;
    s_config.sci.parity = kDefaultSciParity;
    s_config.sci.stop_bits = kDefaultSciStopBits;
}

static void applyChannelOutputs(void)
{
    const uint8_t encoded = clampChannel(s_config.channel);
    digitalWrite(NRL_PIN_CHANNEL_BIT0, (encoded & 0x01U) ? HIGH : LOW);
    digitalWrite(NRL_PIN_CHANNEL_BIT1, (encoded & 0x02U) ? HIGH : LOW);
    digitalWrite(NRL_PIN_CHANNEL_BIT2, (encoded & 0x04U) ? HIGH : LOW);

    if (encoded != s_last_channel_encoded) {
        Serial.printf("[IO] channel=%u bits ch0=%u ch1=%u ch2=%u\n",
                      static_cast<unsigned>(s_config.channel),
                      static_cast<unsigned>((encoded & 0x01U) ? 1u : 0u),
                      static_cast<unsigned>((encoded & 0x02U) ? 1u : 0u),
                      static_cast<unsigned>((encoded & 0x04U) ? 1u : 0u));
        s_last_channel_encoded = encoded;
    }
}

static void applyDefaults(void)
{
    memset(&s_config, 0, sizeof(s_config));
    s_config.channel = 0U;
    s_config.server_port = static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    s_config.local_port = s_config.server_port;
    s_config.callsign_ssid = static_cast<uint8_t>(NRL_AUDIO_CALLSIGN_SSID);
    s_config.device_mode = static_cast<uint8_t>(NRL_AUDIO_DEVICE_MODE);
    s_config.mic_volume = kDefaultMicVolume;
    s_config.line_out_volume = kDefaultLineOutVolume;
    s_config.hp_drive_enabled = false;
    applyDefaultSciConfig();
    s_config.wifi_ssid[0] = '\0';
    s_config.wifi_password[0] = '\0';
    copyBounded(s_config.server_host, sizeof(s_config.server_host), NRL_AUDIO_SERVER_HOST);
    copyBounded(s_config.callsign, sizeof(s_config.callsign), NRL_AUDIO_CALLSIGN);
    sanitizeCallsign(s_config.callsign);
}

static void applyNetworkDefaults(void)
{
    s_config.wifi_ssid[0] = '\0';
    s_config.wifi_password[0] = '\0';
    copyBounded(s_config.server_host, sizeof(s_config.server_host), NRL_AUDIO_SERVER_HOST);
    s_config.server_port = static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    s_config.local_port = s_config.server_port;
}

static void normalizeConfig(void)
{
    s_config.channel = clampChannel(s_config.channel);
    sanitizeString(s_config.wifi_ssid);
    sanitizeString(s_config.wifi_password);
    sanitizeString(s_config.server_host);
    sanitizeCallsign(s_config.callsign);

    if (s_config.server_port == 0U) {
        s_config.server_port = static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    }
    s_config.local_port = s_config.server_port;
    s_config.sci.parity = normalizeParity(s_config.sci.parity);
    if (!isValidSciConfig(s_config.sci.baud,
                          s_config.sci.data_bits,
                          s_config.sci.parity,
                          s_config.sci.stop_bits)) {
        applyDefaultSciConfig();
    }
}

static bool loadPersistedConfig(void)
{
    PersistedExternalRadioConfig persisted = {};
    for (uint8_t offset = 0U; offset < sizeof(persisted); offset += 8U) {
        EEPROM_ReadBuffer(kConfigAddress + offset, reinterpret_cast<uint8_t *>(&persisted) + offset, 8U);
    }

    if (persisted.magic != kConfigMagic || persisted.version != kConfigVersion) {
        return false;
    }

    s_config.channel = persisted.channel;
    s_config.server_port = persisted.server_port;
    s_config.local_port = persisted.local_port;
    s_config.callsign_ssid = persisted.callsign_ssid;
    s_config.device_mode = persisted.device_mode;
    s_config.mic_volume = persisted.mic_volume;
    s_config.line_out_volume = persisted.line_out_volume;
    s_config.hp_drive_enabled = persisted.reserved[0] == kPersistedHpDriveOn;
    s_config.sci.data_bits = persisted.reserved[1];
    s_config.sci.parity = static_cast<char>(persisted.reserved[2]);
    s_config.sci.stop_bits = persisted.reserved[3];
    s_config.sci.baud = static_cast<uint32_t>(persisted.reserved[4]) |
                        (static_cast<uint32_t>(persisted.reserved[5]) << 8) |
                        (static_cast<uint32_t>(persisted.reserved[6]) << 16) |
                        (static_cast<uint32_t>(persisted.reserved[7]) << 24);
    copyBounded(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), persisted.wifi_ssid);
    copyBounded(s_config.wifi_password, sizeof(s_config.wifi_password), persisted.wifi_password);
    copyBounded(s_config.server_host, sizeof(s_config.server_host), persisted.server_host);
    copyBounded(s_config.callsign, sizeof(s_config.callsign), persisted.callsign);
    normalizeConfig();
    return true;
}

static bool savePersistedConfig(void)
{
    PersistedExternalRadioConfig persisted = {};
    persisted.magic = kConfigMagic;
    persisted.version = kConfigVersion;
    persisted.channel = s_config.channel;
    persisted.server_port = s_config.server_port;
    persisted.local_port = s_config.local_port;
    persisted.callsign_ssid = s_config.callsign_ssid;
    persisted.device_mode = s_config.device_mode;
    persisted.mic_volume = s_config.mic_volume;
    persisted.line_out_volume = s_config.line_out_volume;
    persisted.reserved[0] = s_config.hp_drive_enabled ? kPersistedHpDriveOn : kPersistedHpDriveOff;
    persisted.reserved[1] = s_config.sci.data_bits;
    persisted.reserved[2] = static_cast<uint8_t>(s_config.sci.parity);
    persisted.reserved[3] = s_config.sci.stop_bits;
    persisted.reserved[4] = static_cast<uint8_t>(s_config.sci.baud & 0xFFU);
    persisted.reserved[5] = static_cast<uint8_t>((s_config.sci.baud >> 8) & 0xFFU);
    persisted.reserved[6] = static_cast<uint8_t>((s_config.sci.baud >> 16) & 0xFFU);
    persisted.reserved[7] = static_cast<uint8_t>((s_config.sci.baud >> 24) & 0xFFU);
    copyBounded(persisted.wifi_ssid, sizeof(persisted.wifi_ssid), s_config.wifi_ssid);
    copyBounded(persisted.wifi_password, sizeof(persisted.wifi_password), s_config.wifi_password);
    copyBounded(persisted.server_host, sizeof(persisted.server_host), s_config.server_host);
    copyBounded(persisted.callsign, sizeof(persisted.callsign), s_config.callsign);

    for (uint8_t offset = 0U; offset < sizeof(persisted); offset += 8U) {
        EEPROM_WriteBuffer(kConfigAddress + offset, reinterpret_cast<const uint8_t *>(&persisted) + offset, 8U);
    }
    return true;
}

static bool updateStringField(char *field, const size_t field_size, const char *value, const bool persist)
{
    if (field == nullptr || field_size == 0U || value == nullptr) {
        return false;
    }

    copyBounded(field, field_size, value);
    sanitizeString(field);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

} // namespace

extern "C" void EXTERNAL_RADIO_Init(void)
{
    if (s_loaded) {
        return;
    }

    pinMode(NRL_PIN_CHANNEL_BIT0, OUTPUT);
    pinMode(NRL_PIN_CHANNEL_BIT1, OUTPUT);
    pinMode(NRL_PIN_CHANNEL_BIT2, OUTPUT);

    applyDefaults();
    if (!loadPersistedConfig()) {
        normalizeConfig();
        savePersistedConfig();
    }

    applyChannelOutputs();
    s_loaded = true;
}

extern "C" uint8_t EXTERNAL_RADIO_GetChannel(void)
{
    EXTERNAL_RADIO_Init();
    return s_config.channel;
}

extern "C" bool EXTERNAL_RADIO_SetChannel(const uint8_t channel, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (channel > kMaxChannel) {
        return false;
    }
    s_config.channel = channel;
    applyChannelOutputs();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

extern "C" bool EXTERNAL_RADIO_SaveConfig(void)
{
    EXTERNAL_RADIO_Init();
    normalizeConfig();
    return savePersistedConfig();
}

extern "C" bool EXTERNAL_RADIO_ResetNetworkConfig(void)
{
    EXTERNAL_RADIO_Init();
    applyNetworkDefaults();
    normalizeConfig();
    return savePersistedConfig();
}

const ExternalRadioConfig *EXTERNAL_RADIO_GetConfig(void)
{
    EXTERNAL_RADIO_Init();
    return &s_config;
}

bool EXTERNAL_RADIO_SetWifiSsid(const char *value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (!updateStringField(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), value, false) ||
        s_config.wifi_ssid[0] == '\0') {
        return false;
    }
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetWifiPassword(const char *value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    return updateStringField(s_config.wifi_password, sizeof(s_config.wifi_password), value, persist);
}

bool EXTERNAL_RADIO_SetServerHost(const char *value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (!updateStringField(s_config.server_host, sizeof(s_config.server_host), value, false) ||
        s_config.server_host[0] == '\0') {
        return false;
    }
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetServerPort(const uint16_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value == 0U) {
        return false;
    }
    s_config.server_port = value;
    s_config.local_port = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetLocalPort(const uint16_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value == 0U) {
        return false;
    }
    s_config.server_port = value;
    s_config.local_port = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetCallsign(const char *value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value == nullptr) {
        return false;
    }

    copyBounded(s_config.callsign, sizeof(s_config.callsign), value);
    sanitizeCallsign(s_config.callsign);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetCallsignSsid(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.callsign_ssid = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetDeviceMode(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.device_mode = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetMicVolume(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.mic_volume = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetLineOutVolume(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.line_out_volume = value;
    ES8311_ApplyAudioConfig(s_config.mic_volume, s_config.line_out_volume, s_config.hp_drive_enabled);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetHpDriveEnabled(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.hp_drive_enabled = enabled;
    ES8311_ApplyAudioConfig(s_config.mic_volume, s_config.line_out_volume, s_config.hp_drive_enabled);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetSciConfig(const uint32_t baud,
                                 const uint8_t data_bits,
                                 const char parity,
                                 const uint8_t stop_bits,
                                 const bool persist)
{
    EXTERNAL_RADIO_Init();
    const char normalized_parity = normalizeParity(parity);
    if (!isValidSciConfig(baud, data_bits, normalized_parity, stop_bits)) {
        return false;
    }

    s_config.sci.baud = baud;
    s_config.sci.data_bits = data_bits;
    s_config.sci.parity = normalized_parity;
    s_config.sci.stop_bits = stop_bits;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}
