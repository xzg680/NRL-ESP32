#include "nrl_at_commands.h"

#include "nrl_audio_bridge.h"
#include "nrl_net_compat.h"
#include "nrl_version.h"

#include "driver/board_pins.h"
#include "driver/display.h"
#include "driver/es8311.h"
#include "driver/es8389.h"
#include "driver/external_radio.h"
#include "services/espnow_link.h"
#include "services/music_player.h"
#include "services/music_playlist.h"
#include "services/nanny.h"
#include "services/storage_service.h"
#include "driver/sci_serial.h"

#include <esp_log.h>
#include <lwip/sockets.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "NRL";

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

static const char *ipText(const uint32_t value, char *buffer, const size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u) {
        return "";
    }
    nrlIpToString(value, buffer, buffer_size);
    return buffer;
}

static uint32_t currentWifiIpValue(const uint32_t configured_value,
                                   const uint32_t dhcp_value,
                                   const bool use_dhcp_value)
{
    if (use_dhcp_value) {
        return dhcp_value;
    }
    return configured_value;
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

// Format a secret (e.g. WiFi password) for AT replies: never echo plaintext,
// only report its length.
static void formatMaskedSecret(const char *text, char *buffer, const size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u) {
        return;
    }
    const size_t len = (text != nullptr) ? strlen(text) : 0u;
    if (len == 0u) {
        snprintf(buffer, buffer_size, "(empty)");
    } else {
        snprintf(buffer, buffer_size, "****** (%u chars)", static_cast<unsigned>(len));
    }
}

static bool appendSupportedAtList(uint8_t *payload,
                                  const size_t capacity,
                                  size_t *used)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    char sci_config[32] = "9600,8,N,1";
    char wifi_pass[32] = "(empty)";
    char hp_drive[4] = "OFF";
    char wifi_ip[16] = "0.0.0.0";
    char wifi_mask[16] = "0.0.0.0";
    char wifi_gw[16] = "0.0.0.0";
    char wifi_dns[16] = "0.0.0.0";
    if (config != nullptr) {
        formatSciConfig(config->sci, sci_config, sizeof(sci_config));
        formatMaskedSecret(config->wifi_password, wifi_pass, sizeof(wifi_pass));
        snprintf(hp_drive, sizeof(hp_drive), "%s", config->hp_drive_enabled ? "ON" : "OFF");
        const bool show_dhcp_values = config->wifi_dhcp_enabled && nrlWifiStaConnected();
        ipText(currentWifiIpValue(config->wifi_ip, nrlWifiStaIp(), show_dhcp_values), wifi_ip, sizeof(wifi_ip));
        ipText(currentWifiIpValue(config->wifi_netmask, nrlWifiStaNetmask(), show_dhcp_values), wifi_mask, sizeof(wifi_mask));
        ipText(currentWifiIpValue(config->wifi_gateway, nrlWifiStaGateway(), show_dhcp_values), wifi_gw, sizeof(wifi_gw));
        ipText(currentWifiIpValue(config->wifi_dns, nrlWifiStaDns(), show_dhcp_values), wifi_dns, sizeof(wifi_dns));
    }
    return appendKeyValueLine(payload, capacity, used, "WIFI_SSID", (config != nullptr) ? config->wifi_ssid : "") &&
           appendKeyValueLine(payload, capacity, used, "WIFI_PASS", wifi_pass) &&
           appendUnsignedLine(payload, capacity, used, "WIFI_DHCP", (config != nullptr && config->wifi_dhcp_enabled) ? 1u : 0u) &&
           appendKeyValueLine(payload, capacity, used, "WIFI_IP", wifi_ip) &&
           appendKeyValueLine(payload, capacity, used, "WIFI_MASK", wifi_mask) &&
           appendKeyValueLine(payload, capacity, used, "WIFI_GW", wifi_gw) &&
           appendKeyValueLine(payload, capacity, used, "WIFI_DNS", wifi_dns) &&
           appendKeyValueLine(payload, capacity, used, "SCI", sci_config) &&
           appendUnsignedLine(payload, capacity, used, "CH", (config != nullptr) ? config->channel : 0u) &&
           appendKeyValueLine(payload, capacity, used, "D_IP", (config != nullptr) ? config->server_host : "") &&
           appendUnsignedLine(payload, capacity, used, "D_PORT", (config != nullptr) ? config->server_port : 0u) &&
           appendUnsignedLine(payload, capacity, used, "L_PORT", (config != nullptr) ? config->local_port : 0u) &&
           appendKeyValueLine(payload, capacity, used, "CALL", (config != nullptr) ? config->callsign : "") &&
           appendUnsignedLine(payload, capacity, used, "SSID", (config != nullptr) ? config->callsign_ssid : 0u) &&
           appendUnsignedLine(payload, capacity, used, "PTT_TIMEOUT", (config != nullptr) ? config->ptt_timeout_s : 0u) &&
           appendUnsignedLine(payload, capacity, used, "VOICE_BYTES", (config != nullptr) ? config->voice_payload_bytes : 0u) &&
           appendUnsignedLine(payload, capacity, used, "TAIL_SUPPRESS", (config != nullptr) ? config->tail_suppress_ms : 0u) &&
#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY
           appendUnsignedLine(payload, capacity, used, "BATT", static_cast<unsigned>(Display_GetBatteryCalibratedMv())) &&
           appendUnsignedLine(payload, capacity, used, "BATT_RAW", static_cast<unsigned>(Display_GetBatteryRawMv())) &&
           appendUnsignedLine(payload, capacity, used, "BATT_CAL", (config != nullptr) ? config->battery_cal_milli : 0u) &&
#endif
           appendUnsignedLine(payload, capacity, used, "MIC_GAIN", (config != nullptr) ? config->mic_volume : 0u) &&
           appendUnsignedLine(payload, capacity, used, "VOLUME", (config != nullptr) ? config->line_out_volume : 0u) &&
           appendKeyValueLine(payload, capacity, used, "HP_DRIVE", hp_drive) &&
#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
           appendKeyValueLine(payload, capacity, used, "AEC",
                              (config != nullptr && config->aec_enabled) ? "ON" : "OFF") &&
#endif
           appendKeyValueLine(payload, capacity, used, "REBOOT", "1");
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

static bool parseBoolValue(const char *text, bool *out_value)
{
    if (text == nullptr || out_value == nullptr) {
        return false;
    }
    if (stringEqualsIgnoreCase(text, "1") ||
        stringEqualsIgnoreCase(text, "ON") ||
        stringEqualsIgnoreCase(text, "TRUE") ||
        stringEqualsIgnoreCase(text, "ENABLE")) {
        *out_value = true;
        return true;
    }
    if (stringEqualsIgnoreCase(text, "0") ||
        stringEqualsIgnoreCase(text, "OFF") ||
        stringEqualsIgnoreCase(text, "FALSE") ||
        stringEqualsIgnoreCase(text, "DISABLE")) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool parseIpValue(const char *text, uint32_t *out_value)
{
    if (text == nullptr || out_value == nullptr) {
        return false;
    }
    struct in_addr addr = {};
    if (inet_aton(text, &addr) == 0) {
        return false;
    }
    *out_value = addr.s_addr;
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
    static const char kBanner[] = NRL_FIRMWARE_BANNER "\r\n";

    result->payload_size = 0u;
    return appendReplyBytes(result->payload, sizeof(result->payload), &result->payload_size, &kPrefix, 1u) &&
           appendReplyBytes(result->payload, sizeof(result->payload), &result->payload_size, kBanner, sizeof(kBanner) - 1u);
}

static bool applyCurrentAudioConfig(void)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (config == nullptr) {
        return false;
    }
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    return ES8311_ApplyAudioConfig(config->mic_volume,
                                   config->line_out_volume,
                                   config->hp_drive_enabled,
                                   config->drc_enabled,
                                   config->drc_winsize,
                                   config->drc_maxlevel,
                                   config->drc_minlevel,
                                   config->dac_ramprate,
                                   config->dac_eq_bypass,
                                   config->daceq_b0,
                                   config->daceq_b1,
                                   config->daceq_a1,
                                   config->adc_dmic_enabled,
                                   config->adc_linsel,
                                   config->adc_pga_gain,
                                   config->adc_ramprate,
                                   config->adc_dmic_sense,
                                   config->adc_sync,
                                   config->adc_inv,
                                   config->adc_ramclr,
                                   config->adc_scale,
                                   config->alc_enabled,
                                   config->adc_automute_enabled,
                                   config->alc_winsize,
                                   config->alc_maxlevel,
                                   config->alc_minlevel,
                                   config->adc_automute_winsize,
                                   config->adc_automute_noise_gate,
                                   config->adc_automute_volume,
                                   config->adc_hpfs1,
                                   config->adc_eq_bypass,
                                   config->adc_hpf,
                                   config->adc_hpfs2,
                                   config->adceq_b0,
                                   config->adceq_a1,
                                   config->adceq_a2,
                                   config->adceq_b1,
                                   config->adceq_b2);
#elif defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389
    return ES8389_SetOutputVolume(config->line_out_volume) &&
           ES8389_SetInputGain(config->mic_volume);
#else
    return true;
#endif
}

} // namespace

void NRL_AT_HandlePayload(const uint8_t *payload,
                          const size_t payload_size,
                          const NrlAtCommandSource source,
                          NrlAtCommandResult *result)
{
    if (result == nullptr) {
        return;
    }
    *result = NrlAtCommandResult{};

    AtCommand command = {};
    if (!startAtReply(result)) {
        return;
    }

    result->should_reply = true;
    if (!decodeAtCommand(payload, payload_size, &command)) {
        if (!appendSupportedAtList(result->payload, sizeof(result->payload), &result->payload_size)) {
            result->should_reply = false;
        }
        return;
    }

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const bool is_query = stringEqualsIgnoreCase(command.value, "?");
    const bool allow_wifi_config_write = source == NRL_AT_SOURCE_SERIAL;

    if (stringEqualsIgnoreCase(command.command, "WIFI_SSID")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_SSID", config->wifi_ssid);
            return;
        }
        if (!allow_wifi_config_write ||
            !EXTERNAL_RADIO_SetWifiSsid(command.value, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "WIFI_SSID");
            return;
        }
        result->restart_wifi = true;
        result->restart_udp = true;
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_SSID", EXTERNAL_RADIO_GetConfig()->wifi_ssid);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "WIFI_PASS") ||
        stringEqualsIgnoreCase(command.command, "WIFI_PASSWORD")) {
        char masked[32];
        if (is_query) {
            formatMaskedSecret(config->wifi_password, masked, sizeof(masked));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_PASS", masked);
            return;
        }
        if (!allow_wifi_config_write ||
            !EXTERNAL_RADIO_SetWifiPassword(command.value, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "WIFI_PASS");
            return;
        }
        result->restart_wifi = true;
        result->restart_udp = true;
        formatMaskedSecret(EXTERNAL_RADIO_GetConfig()->wifi_password, masked, sizeof(masked));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_PASS", masked);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "WIFI_DHCP")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_DHCP", config->wifi_dhcp_enabled ? 1u : 0u);
            return;
        }
        bool enabled = false;
        if (!allow_wifi_config_write ||
            !parseBoolValue(command.value, &enabled) ||
            !EXTERNAL_RADIO_SetWifiDhcpEnabled(enabled, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "WIFI_DHCP");
            return;
        }
        result->restart_wifi = true;
        result->restart_udp = true;
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_DHCP", EXTERNAL_RADIO_GetConfig()->wifi_dhcp_enabled ? 1u : 0u);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "WIFI_IP") ||
        stringEqualsIgnoreCase(command.command, "WIFI_MASK") ||
        stringEqualsIgnoreCase(command.command, "WIFI_GW") ||
        stringEqualsIgnoreCase(command.command, "WIFI_DNS")) {
        char current[16];
        uint32_t configured = config->wifi_ip;
        uint32_t dhcp = nrlWifiStaIp();
        const char *key = "WIFI_IP";
        if (stringEqualsIgnoreCase(command.command, "WIFI_MASK")) {
            configured = config->wifi_netmask;
            dhcp = nrlWifiStaNetmask();
            key = "WIFI_MASK";
        } else if (stringEqualsIgnoreCase(command.command, "WIFI_GW")) {
            configured = config->wifi_gateway;
            dhcp = nrlWifiStaGateway();
            key = "WIFI_GW";
        } else if (stringEqualsIgnoreCase(command.command, "WIFI_DNS")) {
            configured = config->wifi_dns;
            dhcp = nrlWifiStaDns();
            key = "WIFI_DNS";
        }
        if (is_query) {
            const bool show_dhcp_value = config->wifi_dhcp_enabled && nrlWifiStaConnected();
            appendKeyValueLine(result->payload,
                               sizeof(result->payload),
                               &result->payload_size,
                               key,
                               ipText(currentWifiIpValue(configured, dhcp, show_dhcp_value), current, sizeof(current)));
            return;
        }
        uint32_t value = 0U;
        bool ok = allow_wifi_config_write && parseIpValue(command.value, &value);
        if (ok && strcmp(key, "WIFI_IP") == 0) {
            ok = EXTERNAL_RADIO_SetWifiIp(value, true);
        } else if (ok && strcmp(key, "WIFI_MASK") == 0) {
            ok = EXTERNAL_RADIO_SetWifiNetmask(value, true);
        } else if (ok && strcmp(key, "WIFI_GW") == 0) {
            ok = EXTERNAL_RADIO_SetWifiGateway(value, true);
        } else if (ok && strcmp(key, "WIFI_DNS") == 0) {
            ok = EXTERNAL_RADIO_SetWifiDns(value, true);
        }
        if (!ok) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", key);
            return;
        }
        result->restart_wifi = true;
        result->restart_udp = true;
        appendKeyValueLine(result->payload,
                           sizeof(result->payload),
                           &result->payload_size,
                           key,
                           ipText(value, current, sizeof(current)));
        return;
    }

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

    if (stringEqualsIgnoreCase(command.command, "L_PORT") ||
        stringEqualsIgnoreCase(command.command, "LOCAL_PORT")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "L_PORT", config->local_port);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value == 0u || value > 65535u ||
            !EXTERNAL_RADIO_SetLocalPort(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "L_PORT");
            return;
        }
        result->restart_udp = true;
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "L_PORT", EXTERNAL_RADIO_GetConfig()->local_port);
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

    if (stringEqualsIgnoreCase(command.command, "PTT_TIMEOUT")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "PTT_TIMEOUT", config->ptt_timeout_s);
            return;
        }
        unsigned long value = 0u;
        // 5..3600 s; the explicit bound also stops a >65535 value from
        // wrapping when narrowed to uint16_t inside the setter.
        if (!parseUnsignedValue(command.value, &value) || value < 5u || value > 3600u ||
            !EXTERNAL_RADIO_SetPttTimeout(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "PTT_TIMEOUT");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "PTT_TIMEOUT", EXTERNAL_RADIO_GetConfig()->ptt_timeout_s);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "VOICE_BYTES")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOICE_BYTES", config->voice_payload_bytes);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value < 160u || value > 500u ||
            !EXTERNAL_RADIO_SetVoicePayloadBytes(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "VOICE_BYTES");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOICE_BYTES", EXTERNAL_RADIO_GetConfig()->voice_payload_bytes);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "TAIL_SUPPRESS")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "TAIL_SUPPRESS", config->tail_suppress_ms);
            return;
        }
        unsigned long value = 0u;
        // 0..5000 ms; 0 disables. The explicit bound also stops a >65535 value
        // from wrapping when narrowed to uint16_t inside the setter.
        if (!parseUnsignedValue(command.value, &value) || value > 5000u ||
            !EXTERNAL_RADIO_SetTailSuppressMs(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "TAIL_SUPPRESS");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "TAIL_SUPPRESS", EXTERNAL_RADIO_GetConfig()->tail_suppress_ms);
        return;
    }

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY
    // Read-only: the current calibrated battery voltage in millivolts. Always
    // 0 if the ADC is not initialised (e.g. before Display_Init).
    if (stringEqualsIgnoreCase(command.command, "BATT")) {
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT",
                           static_cast<unsigned>(Display_GetBatteryCalibratedMv()));
        return;
    }

    // Read-only: the raw (pre-calibration) battery voltage in millivolts.
    // Useful for working out a fresh calibration factor by comparing against a
    // multimeter reading.
    if (stringEqualsIgnoreCase(command.command, "BATT_RAW")) {
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT_RAW",
                           static_cast<unsigned>(Display_GetBatteryRawMv()));
        return;
    }

    // Battery calibration scale factor in units of 1/1000 (1000 = no
    // correction). Accepts 500..2000.
    if (stringEqualsIgnoreCase(command.command, "BATT_CAL")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT_CAL", config->battery_cal_milli);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value < 500u || value > 2000u ||
            !EXTERNAL_RADIO_SetBatteryCalibration(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BATT_CAL");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT_CAL", EXTERNAL_RADIO_GetConfig()->battery_cal_milli);
        return;
    }

    // Auto-calibrate from a multimeter reading: AT+BATT_CAL_AUTO=<actual_mv>.
    // Samples the raw voltage now, computes scale = actual_mv * 1000 / raw_mv,
    // clamps to the valid range, and persists it.
    if (stringEqualsIgnoreCase(command.command, "BATT_CAL_AUTO")) {
        unsigned long actual_mv = 0u;
        if (is_query || !parseUnsignedValue(command.value, &actual_mv) ||
            actual_mv < 1000u || actual_mv > 9000u) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BATT_CAL_AUTO");
            return;
        }
        const int raw_mv = Display_GetBatteryRawMv();
        if (raw_mv <= 0) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BATT_CAL_AUTO");
            return;
        }
        const unsigned long scale = (actual_mv * 1000ul + static_cast<unsigned long>(raw_mv) / 2ul) /
                                    static_cast<unsigned long>(raw_mv);
        if (scale < 500ul || scale > 2000ul ||
            !EXTERNAL_RADIO_SetBatteryCalibration(static_cast<uint16_t>(scale), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BATT_CAL_AUTO");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT_CAL", EXTERNAL_RADIO_GetConfig()->battery_cal_milli);
        return;
    }
#endif

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
        applyCurrentAudioConfig();
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
        applyCurrentAudioConfig();
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOLUME", updated->line_out_volume);
        return;
    }

    // Music player bring-up commands: AT+PLAY=/sdcard/music/a.wav, AT+PLAY=?
    // (current track), AT+STOP. UI/playlist control arrives with the player
    // screen; these stay as the serial/remote test path.
    if (stringEqualsIgnoreCase(command.command, "PLAY")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "PLAY",
                               MUSIC_IsPlaying() ? MUSIC_CurrentPath() : "(idle)");
            const MediaTrackInfo *track = MUSIC_GetTrackInfo();
            if (MUSIC_IsPlaying() && track != nullptr && track->title[0] != '\0') {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TITLE", track->title);
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ARTIST", track->artist);
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ALBUM", track->album);
            }
            return;
        }
        if (!MUSIC_PlayFile(command.value)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "PLAY");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "PLAY", command.value);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "STOP")) {
        MUSIC_Stop();
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "STOP", "OK");
        return;
    }

    // Music local output device: AT+OUTPUT=SPK (onboard hi-fi speaker) or
    // AT+OUTPUT=BT (Bluetooth headset via A2DP; falls back to the speaker
    // when the headset is unreachable). Applies from the next track.
    if (stringEqualsIgnoreCase(command.command, "OUTPUT")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "OUTPUT",
                               (MUSIC_GetOutput() == MUSIC_OUTPUT_BT) ? "BT" : "SPK");
            return;
        }
        int output = -1;
        if (stringEqualsIgnoreCase(command.value, "SPK") ||
            stringEqualsIgnoreCase(command.value, "SPEAKER")) {
            output = MUSIC_OUTPUT_SPEAKER;
        } else if (stringEqualsIgnoreCase(command.value, "BT")) {
            output = MUSIC_OUTPUT_BT;
        }
        if (output < 0) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "OUTPUT");
            return;
        }
        MUSIC_SetOutput(output);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "OUTPUT",
                           (output == MUSIC_OUTPUT_BT) ? "BT" : "SPK");
        return;
    }

    // NRL TX voice codec: AT+CODEC=G711 (8k narrowband, packet type 1) or
    // AT+CODEC=OPUS (16k wideband, packet type 8). RX always accepts both.
    if (stringEqualsIgnoreCase(command.command, "CODEC")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "CODEC",
                               (NRLAudioBridge_GetVoiceCodec() == 1u) ? "OPUS" : "G711");
            return;
        }
        uint8_t codec = 0xFFu;
        if (stringEqualsIgnoreCase(command.value, "G711")) {
            codec = 0u;
        } else if (stringEqualsIgnoreCase(command.value, "OPUS")) {
            codec = 1u;
        }
        if (codec > 1u) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "CODEC");
            return;
        }
        NRLAudioBridge_SetVoiceCodec(codec);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "CODEC",
                           (codec == 1u) ? "OPUS" : "G711");
        return;
    }

    // ESP-NOW off-grid voice link: AT+ESPNOW=ON|OFF|? (broadcast intercom
    // between NRL-ESP32 devices on the same WiFi channel, no server).
    if (stringEqualsIgnoreCase(command.command, "ESPNOW")) {
        if (is_query) {
            char peer[16] = {};
            ESPNOW_LINK_GetLastPeer(peer, sizeof(peer));
            char status[48];
            snprintf(status, sizeof(status), "%s%s%s",
                     ESPNOW_LINK_IsEnabled() ? "ON" : "OFF",
                     peer[0] != '\0' ? " last=" : "",
                     peer);
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ESPNOW", status);
            return;
        }
        bool enabled = false;
        if (!parseBoolValue(command.value, &enabled) || !ESPNOW_LINK_SetEnabled(enabled)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "ESPNOW");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ESPNOW",
                           enabled ? "ON" : "OFF");
        return;
    }

    // Nanny beacon: AT+BEACON=/sdcard/beacon/id.wav,30 (path, minutes),
    // AT+BEACON=OFF, AT+BEACON=?.
    if (stringEqualsIgnoreCase(command.command, "BEACON")) {
        char path[128];
        uint32_t interval = 0;
        if (is_query) {
            if (NANNY_GetBeacon(path, sizeof(path), &interval)) {
                char status[160];
                snprintf(status, sizeof(status), "%s,%lu", path, static_cast<unsigned long>(interval));
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "BEACON", status);
            } else {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "BEACON", "OFF");
            }
            return;
        }
        if (stringEqualsIgnoreCase(command.value, "OFF")) {
            NANNY_DisableBeacon();
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "BEACON", "OFF");
            return;
        }
        char parse[160];
        snprintf(parse, sizeof(parse), "%s", command.value);
        char *comma = strrchr(parse, ',');
        unsigned long minutes = 0;
        if (comma == nullptr) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BEACON");
            return;
        }
        *comma++ = '\0';
        if (!parseUnsignedValue(comma, &minutes) ||
            !NANNY_SetBeacon(parse, static_cast<uint32_t>(minutes))) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BEACON");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "BEACON", command.value);
        return;
    }

    // Music playback target (nanny 三档切换): AT+TARGET=LOCAL|NET|BOTH.
    // LOCAL = speaker only; NET = stream to the NRL server (8k G.711, mic
    // muted while streaming); BOTH = fan-out to both. Applies to the next
    // track started.
    if (stringEqualsIgnoreCase(command.command, "TARGET")) {
        static const char *kTargetNames[] = {"LOCAL", "NET", "BOTH"};
        if (is_query) {
            const int t = MUSIC_GetTarget();
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TARGET",
                               (t >= 0 && t <= 2) ? kTargetNames[t] : "?");
            return;
        }
        int target = -1;
        if (stringEqualsIgnoreCase(command.value, "LOCAL")) {
            target = MUSIC_TARGET_LOCAL;
        } else if (stringEqualsIgnoreCase(command.value, "NET")) {
            target = MUSIC_TARGET_NET;
        } else if (stringEqualsIgnoreCase(command.value, "BOTH")) {
            target = MUSIC_TARGET_BOTH;
        }
        if (target < 0) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "TARGET");
            return;
        }
        MUSIC_SetTarget(target);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TARGET",
                           kTargetNames[target]);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "NEXT") ||
        stringEqualsIgnoreCase(command.command, "PREV")) {
        const bool ok = stringEqualsIgnoreCase(command.command, "NEXT") ? PLAYLIST_Next() : PLAYLIST_Prev();
        if (!ok) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "PLAYLIST_EMPTY");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "PLAY", MUSIC_CurrentPath());
        return;
    }

    // CJK font engine on the S31 LCD: AT+FONT=BMP (built-in bitmap subset)
    // or AT+FONT=FT (FreeType vector from /sdcard/fonts/cjk.ttf).
    if (stringEqualsIgnoreCase(command.command, "FONT")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "FONT",
                               (Display_GetCjkFontEngine() == DISPLAY_CJK_FONT_FREETYPE) ? "FT" : "BMP");
            return;
        }
        int engine = -1;
        if (stringEqualsIgnoreCase(command.value, "BMP")) {
            engine = DISPLAY_CJK_FONT_BITMAP;
        } else if (stringEqualsIgnoreCase(command.value, "FT")) {
            engine = DISPLAY_CJK_FONT_FREETYPE;
        }
        if (engine < 0 || !Display_SetCjkFontEngine(engine)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "FONT");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "FONT",
                           (engine == DISPLAY_CJK_FONT_FREETYPE) ? "FT" : "BMP");
        return;
    }

    // SMB network share for music playback (S31):
    //   AT+SMB=server/share[,user[,password]]   configure + mount
    //   AT+SMB=OFF                              unmount + clear config
    //   AT+SMB=?                                status
    if (stringEqualsIgnoreCase(command.command, "SMB")) {
        char status[128];
        if (is_query) {
            STORAGE_SmbDescribe(status, sizeof(status));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SMB", status);
            return;
        }
        if (stringEqualsIgnoreCase(command.value, "OFF")) {
            STORAGE_SmbClear();
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SMB", "OFF");
            return;
        }
        char parse[192];
        snprintf(parse, sizeof(parse), "%s", command.value);
        char *user = strchr(parse, ',');
        char *pass = nullptr;
        if (user != nullptr) {
            *user++ = '\0';
            pass = strchr(user, ',');
            if (pass != nullptr) {
                *pass++ = '\0';
            }
        }
        char *share = strchr(parse, '/');
        if (share != nullptr) {
            *share++ = '\0';
        }
        if (share == nullptr || !STORAGE_SmbConfigure(parse, share, user, pass)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "SMB");
            return;
        }
        STORAGE_SmbDescribe(status, sizeof(status));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SMB", status);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "PLAYLIST")) {
        if (is_query) {
            char count_line[16];
            snprintf(count_line, sizeof(count_line), "%u", static_cast<unsigned>(PLAYLIST_Count()));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TRACKS", count_line);
            return;
        }
        const size_t found = PLAYLIST_Scan();
        char count_line[16];
        snprintf(count_line, sizeof(count_line), "%u", static_cast<unsigned>(found));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TRACKS", count_line);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "HP_DRIVE")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "HP_DRIVE", config->hp_drive_enabled ? "ON" : "OFF");
            return;
        }
        bool enabled = false;
        if (!parseBoolValue(command.value, &enabled)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "HP_DRIVE");
            return;
        }
        if (!EXTERNAL_RADIO_SetHpDriveEnabled(enabled, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "HP_DRIVE");
            return;
        }
        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        applyCurrentAudioConfig();
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "HP_DRIVE", updated->hp_drive_enabled ? "ON" : "OFF");
        return;
    }

#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
    if (stringEqualsIgnoreCase(command.command, "AEC")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "AEC", config->aec_enabled ? "ON" : "OFF");
            return;
        }
        bool enabled = false;
        if (!parseBoolValue(command.value, &enabled)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "AEC");
            return;
        }
        if (!EXTERNAL_RADIO_SetAecEnabled(enabled, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "AEC");
            return;
        }
        // AEC is brought up by the ES8311 driver at boot; the new value takes
        // effect after a restart.
        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "AEC", updated->aec_enabled ? "ON" : "OFF");
        return;
    }
#endif

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

    ESP_LOGI(TAG, "unknown AT command: %s=%s, returning command list",
             command.command,
             command.value);
    if (!appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "UNKNOWN") ||
        !appendSupportedAtList(result->payload, sizeof(result->payload), &result->payload_size)) {
        result->should_reply = false;
    }
}
