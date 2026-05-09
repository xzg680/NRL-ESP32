#include "nrl_at_commands.h"

#include "driver/es8311.h"
#include "driver/external_radio.h"
#include "driver/sci_serial.h"

#include <Arduino.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct AtCommand {
    char command[32];
    char value[128];
};

static bool stringEqualsIgnoreCase(const char *lhs, const char *rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }

    while (*lhs != '\0' && *rhs != '\0') {
        if (toupper(static_cast<unsigned char>(*lhs)) != toupper(static_cast<unsigned char>(*rhs))) {
            return false;
        }
        ++lhs;
        ++rhs;
    }

    return *lhs == '\0' && *rhs == '\0';
}

static void trimText(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t start = 0u;
    while (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n') {
        ++start;
    }
    if (start > 0u) {
        memmove(text, text + start, strlen(text + start) + 1u);
    }

    size_t len = strlen(text);
    while (len > 0u) {
        const char ch = text[len - 1u];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        text[--len] = '\0';
    }
}

static bool appendReplyBytes(uint8_t *payload,
                             const size_t capacity,
                             size_t *used,
                             const void *src,
                             const size_t src_size)
{
    if (payload == nullptr || used == nullptr || src == nullptr || *used + src_size > capacity) {
        return false;
    }

    memcpy(payload + *used, src, src_size);
    *used += src_size;
    return true;
}

static bool appendReplyLine(uint8_t *payload,
                            const size_t capacity,
                            size_t *used,
                            const char *line)
{
    static const char kCrLf[] = "\r\n";
    return appendReplyBytes(payload, capacity, used, line, strlen(line)) &&
           appendReplyBytes(payload, capacity, used, kCrLf, 2u);
}

static bool appendKeyValueLine(uint8_t *payload,
                               const size_t capacity,
                               size_t *used,
                               const char *key,
                               const char *value)
{
    char line[160];
    snprintf(line, sizeof(line), "AT+%s=%s", key, value);
    return appendReplyLine(payload, capacity, used, line);
}

static bool appendUnsignedLine(uint8_t *payload,
                               const size_t capacity,
                               size_t *used,
                               const char *key,
                               const unsigned value)
{
    char line[96];
    snprintf(line, sizeof(line), "AT+%s=%u", key, value);
    return appendReplyLine(payload, capacity, used, line);
}

static void formatSciConfig(const SciSerialConfig &sci, char *buffer, const size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u) {
        return;
    }
    snprintf(buffer,
             buffer_size,
             "%lu,%u,%c,%u",
             static_cast<unsigned long>(sci.baud),
             static_cast<unsigned>(sci.data_bits),
             sci.parity,
             static_cast<unsigned>(sci.stop_bits));
}

static bool appendSupportedAtList(uint8_t *payload,
                                  const size_t capacity,
                                  size_t *used)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    char sci_config[32] = "9600,8,N,1";
    if (config != nullptr) {
        formatSciConfig(config->sci, sci_config, sizeof(sci_config));
    }
    return appendReplyLine(payload, capacity, used, "AT+SCI=?") &&
           appendKeyValueLine(payload, capacity, used, "SCI", sci_config) &&
           appendReplyLine(payload, capacity, used, "AT+SCI=<baud>,<5..8>,<N|E|O>,<1|2>") &&
           appendReplyLine(payload, capacity, used, "AT+CH=?") &&
           appendReplyLine(payload, capacity, used, "AT+CH=0..7") &&
           appendReplyLine(payload, capacity, used, "AT+D_IP=?") &&
           appendReplyLine(payload, capacity, used, "AT+D_IP=<host_or_ip>") &&
           appendReplyLine(payload, capacity, used, "AT+D_PORT=?") &&
           appendReplyLine(payload, capacity, used, "AT+D_PORT=<1..65535>") &&
           appendReplyLine(payload, capacity, used, "AT+CALL=?") &&
           appendReplyLine(payload, capacity, used, "AT+CALL=<value>") &&
           appendReplyLine(payload, capacity, used, "AT+SSID=?") &&
           appendReplyLine(payload, capacity, used, "AT+SSID=<0..255>") &&
           appendReplyLine(payload, capacity, used, "AT+MIC_GAIN=?") &&
           appendReplyLine(payload, capacity, used, "AT+MIC_GAIN=<0..255>") &&
           appendReplyLine(payload, capacity, used, "AT+VOLUME=?") &&
           appendReplyLine(payload, capacity, used, "AT+VOLUME=<0..255>") &&
           appendReplyLine(payload, capacity, used, "AT+HP_DRIVE=?") &&
           appendReplyLine(payload, capacity, used, "AT+HP_DRIVE=<ON|OFF>") &&
           appendReplyLine(payload, capacity, used, "AT+REBOOT=1");
}

static bool decodeAtCommand(const uint8_t *payload, const size_t payload_size, AtCommand *out_cmd)
{
    if (payload == nullptr || out_cmd == nullptr || payload_size < 3u || payload[0] != 0x01u) {
        return false;
    }

    char buffer[160];
    size_t copy_size = payload_size - 1u;
    if (copy_size >= sizeof(buffer)) {
        copy_size = sizeof(buffer) - 1u;
    }
    memcpy(buffer, payload + 1u, copy_size);
    buffer[copy_size] = '\0';

    char *separator = strchr(buffer, '=');
    if (separator == nullptr) {
        return false;
    }

    *separator = '\0';
    strncpy(out_cmd->command, buffer, sizeof(out_cmd->command) - 1u);
    out_cmd->command[sizeof(out_cmd->command) - 1u] = '\0';
    strncpy(out_cmd->value, separator + 1, sizeof(out_cmd->value) - 1u);
    out_cmd->value[sizeof(out_cmd->value) - 1u] = '\0';

    trimText(out_cmd->command);
    trimText(out_cmd->value);
    if (strncmp(out_cmd->command, "AT+", 3u) == 0) {
        memmove(out_cmd->command, out_cmd->command + 3u, strlen(out_cmd->command + 3u) + 1u);
    }
    return out_cmd->command[0] != '\0';
}

static bool parseUnsignedValue(const char *text, unsigned long *out_value)
{
    if (text == nullptr || out_value == nullptr || text[0] == '\0') {
        return false;
    }

    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || (end != nullptr && *end != '\0')) {
        return false;
    }

    *out_value = value;
    return true;
}

static bool parseSciConfigValue(const char *text,
                                uint32_t *out_baud,
                                uint8_t *out_data_bits,
                                char *out_parity,
                                uint8_t *out_stop_bits)
{
    if (text == nullptr || out_baud == nullptr || out_data_bits == nullptr ||
        out_parity == nullptr || out_stop_bits == nullptr) {
        return false;
    }

    char buffer[80];
    strncpy(buffer, text, sizeof(buffer) - 1u);
    buffer[sizeof(buffer) - 1u] = '\0';

    char *parts[4] = {};
    char *cursor = buffer;
    for (size_t i = 0; i < 4u; ++i) {
        parts[i] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == nullptr) {
            if (i != 3u) {
                return false;
            }
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }
    if (strchr(parts[3], ',') != nullptr) {
        return false;
    }

    for (size_t i = 0; i < 4u; ++i) {
        trimText(parts[i]);
        if (parts[i][0] == '\0') {
            return false;
        }
    }

    unsigned long baud = 0u;
    unsigned long data_bits = 0u;
    unsigned long stop_bits = 0u;
    if (!parseUnsignedValue(parts[0], &baud) ||
        !parseUnsignedValue(parts[1], &data_bits) ||
        !parseUnsignedValue(parts[3], &stop_bits)) {
        return false;
    }

    const char parity = static_cast<char>(toupper(static_cast<unsigned char>(parts[2][0])));
    if (parts[2][1] != '\0' ||
        baud < 300u || baud > 921600u ||
        data_bits < 5u || data_bits > 8u ||
        (parity != 'N' && parity != 'E' && parity != 'O') ||
        (stop_bits != 1u && stop_bits != 2u)) {
        return false;
    }

    *out_baud = static_cast<uint32_t>(baud);
    *out_data_bits = static_cast<uint8_t>(data_bits);
    *out_parity = parity;
    *out_stop_bits = static_cast<uint8_t>(stop_bits);
    return true;
}

static bool startAtReply(NrlAtCommandResult *result)
{
    static const uint8_t kPrefix = 0x02u;
    static const char kBanner[] = "NRL3188-ESP32 V1.0\r\n";

    result->payload_size = 0u;
    return appendReplyBytes(result->payload, sizeof(result->payload), &result->payload_size, &kPrefix, 1u) &&
           appendReplyBytes(result->payload, sizeof(result->payload), &result->payload_size, kBanner, sizeof(kBanner) - 1u);
}

static bool appendAllConfigLines(NrlAtCommandResult *result)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    char sci_config[32];
    formatSciConfig(config->sci, sci_config, sizeof(sci_config));
    return appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SCI", sci_config) &&
           appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "CH", config->channel) &&
           appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "D_IP", config->server_host) &&
           appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "D_PORT", config->server_port) &&
           appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "CALL", config->callsign) &&
           appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "SSID", config->callsign_ssid) &&
           appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "MIC_GAIN", config->mic_volume) &&
           appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOLUME", config->line_out_volume) &&
           appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "HP_DRIVE", config->hp_drive_enabled ? "ON" : "OFF");
}

} // namespace

void NRL_AT_HandlePayload(const uint8_t *payload,
                          const size_t payload_size,
                          NrlAtCommandResult *result)
{
    if (result == nullptr) {
        return;
    }
    memset(result, 0, sizeof(*result));

    AtCommand command = {};
    if (!startAtReply(result)) {
        return;
    }

    result->should_reply = true;
    if (!decodeAtCommand(payload, payload_size, &command)) {
        if (!appendSupportedAtList(result->payload, sizeof(result->payload), &result->payload_size) ||
            !appendAllConfigLines(result)) {
            result->should_reply = false;
        }
        return;
    }

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const bool is_query = stringEqualsIgnoreCase(command.value, "?");

    if (stringEqualsIgnoreCase(command.command, "REBOOT")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "REBOOT", "1");
            return;
        }
        if (command.value[0] != '\0' && !stringEqualsIgnoreCase(command.value, "1") &&
            !stringEqualsIgnoreCase(command.value, "ON")) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "REBOOT");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "REBOOT", "OK");
        result->reboot = true;
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "CH")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "CH", config->channel);
            return;
        }

        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value > 7u ||
            !EXTERNAL_RADIO_SetChannel(static_cast<uint8_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "CH");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "CH", EXTERNAL_RADIO_GetChannel());
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "D_IP")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "D_IP", config->server_host);
            return;
        }
        if (!EXTERNAL_RADIO_SetServerHost(command.value, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "D_IP");
            return;
        }
        result->restart_udp = true;
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "D_IP", EXTERNAL_RADIO_GetConfig()->server_host);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "D_PORT")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "D_PORT", config->server_port);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value == 0u || value > 65535u ||
            !EXTERNAL_RADIO_SetServerPort(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "D_PORT");
            return;
        }
        result->restart_udp = true;
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "D_PORT", EXTERNAL_RADIO_GetConfig()->server_port);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "CALL")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "CALL", config->callsign);
            return;
        }
        if (!EXTERNAL_RADIO_SetCallsign(command.value, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "CALL");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "CALL", EXTERNAL_RADIO_GetConfig()->callsign);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "SSID")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "SSID", config->callsign_ssid);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value > 255u ||
            !EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "SSID");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "SSID", EXTERNAL_RADIO_GetConfig()->callsign_ssid);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "MIC_GAIN")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "MIC_GAIN", config->mic_volume);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value > 255u ||
            !EXTERNAL_RADIO_SetMicVolume(static_cast<uint8_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "MIC_GAIN");
            return;
        }
        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        ES8311_ApplyAudioConfig(updated->mic_volume, updated->line_out_volume, updated->hp_drive_enabled);
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "MIC_GAIN", updated->mic_volume);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "VOLUME")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOLUME", config->line_out_volume);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value > 255u ||
            !EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "VOLUME");
            return;
        }
        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        ES8311_ApplyAudioConfig(updated->mic_volume, updated->line_out_volume, updated->hp_drive_enabled);
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOLUME", updated->line_out_volume);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "HP_DRIVE")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "HP_DRIVE", config->hp_drive_enabled ? "ON" : "OFF");
            return;
        }
        bool enabled = false;
        if (stringEqualsIgnoreCase(command.value, "ON") || stringEqualsIgnoreCase(command.value, "1")) {
            enabled = true;
        } else if (stringEqualsIgnoreCase(command.value, "OFF") || stringEqualsIgnoreCase(command.value, "0")) {
            enabled = false;
        } else {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "HP_DRIVE");
            return;
        }
        if (!EXTERNAL_RADIO_SetHpDriveEnabled(enabled, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "HP_DRIVE");
            return;
        }
        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        ES8311_ApplyAudioConfig(updated->mic_volume, updated->line_out_volume, updated->hp_drive_enabled);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "HP_DRIVE", updated->hp_drive_enabled ? "ON" : "OFF");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "SCI")) {
        char sci_config[32];
        if (is_query) {
            formatSciConfig(config->sci, sci_config, sizeof(sci_config));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SCI", sci_config);
            return;
        }

        uint32_t baud = 0u;
        uint8_t data_bits = 0u;
        char parity = '\0';
        uint8_t stop_bits = 0u;
        if (!parseSciConfigValue(command.value, &baud, &data_bits, &parity, &stop_bits) ||
            !EXTERNAL_RADIO_SetSciConfig(baud, data_bits, parity, stop_bits, true) ||
            !SCI_SERIAL_ApplyConfig(baud, data_bits, parity, stop_bits)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "SCI");
            return;
        }

        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        formatSciConfig(updated->sci, sci_config, sizeof(sci_config));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SCI", sci_config);
        return;
    }

    Serial.printf("[NRL] unknown AT command: %s=%s, returning command list\n",
                  command.command,
                  command.value);
    if (!appendSupportedAtList(result->payload, sizeof(result->payload), &result->payload_size) ||
        !appendAllConfigLines(result)) {
        result->should_reply = false;
    }
}
