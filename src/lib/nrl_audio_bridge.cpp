#include "nrl_audio_bridge.h"

#include "nrl_at_commands.h"
#include "nrl_audio_config.h"
#include "wifi.h"
#include "wifi_config_portal.h"

#include "driver/es8311.h"
#include "driver/external_radio.h"
#include "driver/sci_serial.h"
#include "driver/status_io.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint8_t kNrlHeaderSize = 48u;
constexpr uint8_t kNrlTypeVoice = 1u;
constexpr uint8_t kNrlTypeHeartbeat = 2u;
constexpr uint8_t kNrlTypeServerHeartbeat = 3u;
constexpr uint8_t kNrlTypeServerVoice = 9u;
constexpr uint8_t kNrlTypeAtCommand = 11u;
constexpr uint8_t kNrlTypeSciTransparent = 12u;
constexpr size_t kAudioCodecFrameSamples = 80u;
constexpr size_t kAudioCapture16kFrameSamples = kAudioCodecFrameSamples * 2u;
constexpr size_t kG711PayloadBytes = 160u;
constexpr size_t kG711RxPayloadMaxBytes = 500u;
constexpr size_t kSciPayloadMaxBytes = 256u;
constexpr size_t kNrlMaxPayloadSize = 1024u;
constexpr size_t kNrlMaxPacketSize = kNrlHeaderSize + kNrlMaxPayloadSize;
constexpr int kDownlinkPcmGain = 1;
constexpr uint32_t kSciPollIntervalMs = 20u;
constexpr uint32_t kSciFlushIntervalMs = 20u;

WiFiUDP s_udp;
SemaphoreHandle_t s_udp_mutex = nullptr;
TaskHandle_t s_bridge_task = nullptr;
bool s_bridge_initialized = false;
bool s_udp_started = false;
bool s_downlink_playback_active = false;
bool s_voice_stream_logged = false;
bool s_voice_decode_logged = false;
bool s_reboot_pending = false;
int16_t s_voice_decode_peak_max = 0;
uint32_t s_voice_decode_sample_total = 0u;
uint32_t s_voice_decode_chunks = 0u;
uint16_t s_packet_counter = 0u;
uint32_t s_last_rx_packet_ms = 0u;
uint32_t s_last_heartbeat_ms = 0u;
uint32_t s_last_remote_identity_ms = 0u;
uint32_t s_reboot_at_ms = 0u;
IPAddress s_server_ip;
IPAddress s_last_peer_ip;
uint16_t s_last_peer_port = 0u;
uint8_t s_cpu_id[4] = {};
bool s_alaw_tables_ready = false;
int16_t s_alaw_decode_table[256];
uint8_t s_alaw_encode_mag_table[4096];
char s_remote_callsign[7] = {};
uint8_t s_remote_ssid = 0u;
// Identity of the last *voice* caller specifically. s_remote_callsign above
// tracks every packet (heartbeat, AT, SCI...), so it must not be used to
// decide "who is talking" or "is a voice stream active".
char s_voice_callsign[7] = {};
uint8_t s_voice_ssid = 0u;
size_t s_last_voice_payload_size = 0u;
uint32_t s_next_wifi_retry_ms = 0u;
uint8_t s_wifi_connect_failures = 0u;
uint8_t s_rx_packet_buffer[kNrlMaxPacketSize];
uint8_t s_at_reply_packet[kNrlMaxPacketSize];
uint8_t s_sci_tx_packet[kNrlHeaderSize + kSciPayloadMaxBytes];
uint8_t s_sci_payload_buffer[kSciPayloadMaxBytes];
int16_t s_downlink_pcm_buffer[kG711RxPayloadMaxBytes];
size_t s_sci_payload_count = 0u;
uint32_t s_last_sci_rx_ms = 0u;
uint32_t s_last_sci_log_ms = 0u;
uint32_t s_last_sci_poll_ms = 0u;

constexpr uint32_t kWifiRetryBackoffMs = 30000u;
constexpr uint8_t kWifiFallbackFailureThreshold = 3u;
constexpr size_t kAtLogTextMax = 96u;

static void initCpuId(void)
{
    memset(s_cpu_id, 0, sizeof(s_cpu_id));
}

static void formatAtLogText(const uint8_t *payload,
                            const size_t payload_size,
                            char *out,
                            const size_t out_size)
{
    if (out == nullptr || out_size == 0u) {
        return;
    }
    out[0] = '\0';
    if (payload == nullptr || payload_size <= 1u) {
        return;
    }

    const size_t text_size = payload_size - 1u;
    const size_t copy_size = (text_size < (out_size - 1u)) ? text_size : (out_size - 1u);
    for (size_t i = 0; i < copy_size; ++i) {
        const uint8_t ch = payload[i + 1u];
        out[i] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : '.';
    }
    out[copy_size] = '\0';
}

static void updateRemoteIdentity(const uint8_t *packet, const size_t packet_size)
{
    if (packet == nullptr || packet_size < 31u) {
        return;
    }

    memcpy(s_remote_callsign, packet + 24u, 6u);
    s_remote_callsign[6] = '\0';
    for (int i = 5; i >= 0; --i) {
        if (s_remote_callsign[i] == '\0' || s_remote_callsign[i] == '\r' || s_remote_callsign[i] == ' ') {
            s_remote_callsign[i] = '\0';
        } else {
            break;
        }
    }
    s_remote_ssid = packet[30];
    s_last_remote_identity_ms = millis();
}

static uint8_t linearToALawSlow(const int16_t pcm)
{
    uint8_t sign = 0u;
    int16_t ix = 0;

    if (pcm < 0) {
        sign = 0x80u;
        ix = static_cast<int16_t>((~pcm) >> 4);
    } else {
        ix = static_cast<int16_t>(pcm >> 4);
    }

    if (ix > 15) {
        uint8_t iexp = 1u;
        while (ix > 31) {
            ix = static_cast<int16_t>(ix >> 1);
            ++iexp;
        }
        ix = static_cast<int16_t>(ix - 16);
        ix = static_cast<int16_t>(ix + static_cast<int16_t>(iexp << 4));
    }

    if (sign == 0u) {
        ix = static_cast<int16_t>(ix | 0x80);
    }

    return static_cast<uint8_t>(ix) ^ 0x55u;
}

static int16_t aLawToLinearSlow(const uint8_t alaw)
{
    const uint8_t code = static_cast<uint8_t>(alaw ^ 0x55u);
    const int16_t iexp = static_cast<int16_t>((code & 0x70u) >> 4);
    int16_t mant = static_cast<int16_t>(code & 0x0Fu);

    if (iexp > 0) {
        mant = static_cast<int16_t>(mant + 16);
    }
    mant = static_cast<int16_t>((mant << 4) + 0x08);

    if (iexp > 1) {
        mant = static_cast<int16_t>(mant << static_cast<uint8_t>(iexp - 1));
    }

    return ((code & 0x80u) != 0u) ? mant : static_cast<int16_t>(-mant);
}

static void initALawTables(void)
{
    if (s_alaw_tables_ready) {
        return;
    }

    for (size_t i = 0; i < 256u; ++i) {
        s_alaw_decode_table[i] = aLawToLinearSlow(static_cast<uint8_t>(i));
    }

    for (size_t i = 0; i < 4096u; ++i) {
        s_alaw_encode_mag_table[i] = linearToALawSlow(static_cast<int16_t>(i));
    }

    s_alaw_tables_ready = true;
}

static uint8_t linearToALaw(const int16_t pcm)
{
    if (!s_alaw_tables_ready) {
        initALawTables();
    }

    if (pcm >= 0) {
        const uint16_t mag = (pcm > 0x0FFF) ? 0x0FFFu : static_cast<uint16_t>(pcm);
        return s_alaw_encode_mag_table[mag];
    }

    int32_t mag32 = -(static_cast<int32_t>(pcm)) - 1;
    if (mag32 < 0) {
        mag32 = 0;
    } else if (mag32 > 0x0FFF) {
        mag32 = 0x0FFF;
    }
    return static_cast<uint8_t>(s_alaw_encode_mag_table[static_cast<uint16_t>(mag32)] ^ 0x80u);
}

static int16_t aLawToLinear(const uint8_t alaw)
{
    if (!s_alaw_tables_ready) {
        initALawTables();
    }
    return s_alaw_decode_table[alaw];
}

static int16_t applyPcmGain(const int16_t sample, const int gain)
{
    int32_t value = static_cast<int32_t>(sample) * gain;
    if (value > INT16_MAX) {
        value = INT16_MAX;
    } else if (value < INT16_MIN) {
        value = INT16_MIN;
    }
    return static_cast<int16_t>(value);
}

static size_t buildNrlPacket(const uint8_t packet_type,
                             const uint8_t *payload,
                             const size_t payload_size,
                             uint8_t *out_packet,
                             const size_t out_capacity)
{
    const size_t total_size = kNrlHeaderSize + payload_size;
    if (out_packet == nullptr || total_size > out_capacity) {
        return 0;
    }

    memset(out_packet, 0, total_size);
    memcpy(out_packet, "NRL2", 4u);
    out_packet[4] = static_cast<uint8_t>((total_size >> 8) & 0xFFu);
    out_packet[5] = static_cast<uint8_t>(total_size & 0xFFu);
    memcpy(out_packet + 6u, s_cpu_id, sizeof(s_cpu_id));
    out_packet[20] = packet_type;
    out_packet[21] = 1u;
    ++s_packet_counter;
    out_packet[22] = static_cast<uint8_t>((s_packet_counter >> 8) & 0xFFu);
    out_packet[23] = static_cast<uint8_t>(s_packet_counter & 0xFFu);

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const char *callsign = (config != nullptr) ? config->callsign : NRL_AUDIO_CALLSIGN;
    const uint8_t callsign_ssid = (config != nullptr) ? config->callsign_ssid : static_cast<uint8_t>(NRL_AUDIO_CALLSIGN_SSID);
    const uint8_t device_mode = (config != nullptr) ? config->device_mode : static_cast<uint8_t>(NRL_AUDIO_DEVICE_MODE);
    strncpy(reinterpret_cast<char *>(out_packet + 24u), callsign, 6u);
    out_packet[30] = callsign_ssid;
    out_packet[31] = device_mode;

    if (payload_size > 0u) {
        memcpy(out_packet + kNrlHeaderSize, payload, payload_size);
    }

    return total_size;
}

static bool parseNrlPacket(const uint8_t *packet,
                           const size_t packet_size,
                           uint8_t *out_type,
                           const uint8_t **out_payload,
                           size_t *out_payload_size)
{
    if (packet == nullptr || packet_size < kNrlHeaderSize) {
        return false;
    }
    if (memcmp(packet, "NRL2", 4u) != 0) {
        return false;
    }

    uint16_t total_size = static_cast<uint16_t>((packet[4] << 8) | packet[5]);
    if (total_size > packet_size || total_size < kNrlHeaderSize) {
        return false;
    }

    // Some NRL senders keep the header length fixed at 48 even when a payload
    // is present. In that case, trust the UDP datagram length so voice/AT
    // payloads are not discarded.
    if (total_size == kNrlHeaderSize && packet_size > kNrlHeaderSize) {
        total_size = static_cast<uint16_t>(packet_size);
    }

    if (out_type != nullptr) {
        *out_type = packet[20];
    }
    if (out_payload != nullptr) {
        *out_payload = packet + kNrlHeaderSize;
    }
    if (out_payload_size != nullptr) {
        *out_payload_size = total_size - kNrlHeaderSize;
    }
    return true;
}

static bool udpSendPacketTo(const uint8_t *packet,
                            const size_t packet_size,
                            const IPAddress &dest_ip,
                            const uint16_t dest_port)
{
    if (packet == nullptr || packet_size == 0u || !s_udp_started || WiFi.status() != WL_CONNECTED || dest_port == 0u) {
        return false;
    }

    if (s_udp_mutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(s_udp_mutex, 0) != pdTRUE) {
        return false;
    }

    const bool ok = s_udp.beginPacket(dest_ip, dest_port) == 1 &&
                    s_udp.write(packet, packet_size) == packet_size &&
                    s_udp.endPacket() == 1;
    xSemaphoreGive(s_udp_mutex);
    return ok;
}

static bool udpSendPacket(const uint8_t *packet, const size_t packet_size)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const uint16_t server_port = (config != nullptr) ? config->server_port : static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    return udpSendPacketTo(packet, packet_size, s_server_ip, server_port);
}

static void flushSciPayload(void)
{
    if (s_sci_payload_count == 0u) {
        return;
    }

    const size_t packet_size = buildNrlPacket(kNrlTypeSciTransparent,
                                              s_sci_payload_buffer,
                                              s_sci_payload_count,
                                              s_sci_tx_packet,
                                              sizeof(s_sci_tx_packet));
    const size_t payload_count = s_sci_payload_count;
    s_sci_payload_count = 0u;
    if (packet_size == 0u) {
        return;
    }

    const bool sent = udpSendPacket(s_sci_tx_packet, packet_size);
    const uint32_t now = millis();
    if (!sent && (now - s_last_sci_log_ms) >= 1000u) {
        s_last_sci_log_ms = now;
        Serial.printf("[SCI] uplink send failed bytes=%u\n",
                      static_cast<unsigned>(payload_count));
    }
}

static void pollSciUplink(void)
{
    int available = SCI_SERIAL_Available();
    const uint32_t now = millis();
    while (available > 0) {
        const size_t space = sizeof(s_sci_payload_buffer) - s_sci_payload_count;
        if (space == 0u) {
            flushSciPayload();
            continue;
        }

        const size_t read = SCI_SERIAL_Read(s_sci_payload_buffer + s_sci_payload_count, space);
        if (read == 0u) {
            break;
        }
        s_sci_payload_count += read;
        s_last_sci_rx_ms = now;
        if (s_sci_payload_count == sizeof(s_sci_payload_buffer)) {
            flushSciPayload();
        }
        available = SCI_SERIAL_Available();
    }

    if (s_sci_payload_count > 0u &&
        (now - s_last_sci_rx_ms) >= kSciFlushIntervalMs) {
        flushSciPayload();
    }
}

static void handleIncomingSciPayload(const uint8_t *payload, const size_t payload_size)
{
    if (payload == nullptr || payload_size == 0u || payload_size > kNrlMaxPayloadSize) {
        return;
    }

    const size_t written = SCI_SERIAL_Write(payload, payload_size);
    const uint32_t now = millis();
    if (written != payload_size && (now - s_last_sci_log_ms) >= 1000u) {
        s_last_sci_log_ms = now;
        Serial.printf("[SCI] downlink short write bytes=%u written=%u\n",
                      static_cast<unsigned>(payload_size),
                      static_cast<unsigned>(written));
    }
}

static void sendVoiceFrame(const int16_t *pcm8k, const size_t sample_count)
{
    if (pcm8k == nullptr || sample_count == 0u) {
        return;
    }
    if (!STATUS_IO_IsSqlActive()) {
        return;
    }

    static uint8_t alaw_accumulator[kG711PayloadBytes];
    static size_t alaw_accumulator_count = 0u;

    const size_t used_samples = (sample_count < kAudioCodecFrameSamples) ? sample_count : kAudioCodecFrameSamples;
    for (size_t i = 0; i < used_samples; ++i) {
        if (alaw_accumulator_count < kG711PayloadBytes) {
            alaw_accumulator[alaw_accumulator_count++] = linearToALaw(pcm8k[i]);
        }

        if (alaw_accumulator_count == kG711PayloadBytes) {
            uint8_t packet[kNrlHeaderSize + kG711PayloadBytes];
            const size_t packet_size = buildNrlPacket(kNrlTypeVoice,
                                                      alaw_accumulator,
                                                      kG711PayloadBytes,
                                                      packet,
                                                      sizeof(packet));
            if (packet_size > 0u) {
                udpSendPacket(packet, packet_size);
            }
            alaw_accumulator_count = 0u;
        }
    }
}

static size_t downsample16kTo8kForG711(const int16_t *pcm16k,
                                       const size_t sample_count,
                                       int16_t *pcm8k,
                                       const size_t pcm8k_capacity)
{
    if (pcm16k == nullptr || pcm8k == nullptr || pcm8k_capacity == 0u) {
        return 0u;
    }
    const size_t pairs = sample_count / 2u;
    const size_t out_count = (pairs < pcm8k_capacity) ? pairs : pcm8k_capacity;
    for (size_t i = 0; i < out_count; ++i) {
        const int32_t a = pcm16k[i * 2u];
        const int32_t b = pcm16k[i * 2u + 1u];
        pcm8k[i] = static_cast<int16_t>((a + b) / 2);
    }
    return out_count;
}

static void es8311FrameHook(const int16_t *samples,
                            const size_t sample_count,
                            ES8311_AudioMode_t mode,
                            void *)
{
    if (mode != ES8311_AUDIO_MODE_RECEIVE) {
        return;
    }
    if (sample_count == kAudioCapture16kFrameSamples) {
        int16_t pcm8k[kAudioCodecFrameSamples];
        const size_t out_count = downsample16kTo8kForG711(samples, sample_count, pcm8k, kAudioCodecFrameSamples);
        sendVoiceFrame(pcm8k, out_count);
        return;
    }
    sendVoiceFrame(samples, sample_count);
}

static bool ensureWifiAndUdp(void)
{
    const uint32_t now = millis();
    if (s_next_wifi_retry_ms != 0u &&
        static_cast<int32_t>(now - s_next_wifi_retry_ms) < 0) {
        return false;
    }

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const char *wifi_ssid = (config != nullptr) ? config->wifi_ssid : NRL_AUDIO_WIFI_SSID;
    const char *wifi_password = (config != nullptr) ? config->wifi_password : NRL_AUDIO_WIFI_PASSWORD;
    const char *server_host = (config != nullptr) ? config->server_host : NRL_AUDIO_SERVER_HOST;
    const uint16_t server_port = (config != nullptr) ? config->server_port : static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    const uint16_t local_port = (config != nullptr) ? config->local_port : server_port;

    if (WiFi.status() != WL_CONNECTED) {
        const WifiConnResult result = wifiEnsureConnected(wifi_ssid, wifi_password, 15000u);
        if (result != WIFI_CONN_OK && result != WIFI_ALREADY_ON_SSID) {
            if (s_wifi_connect_failures < 0xFFu) {
                ++s_wifi_connect_failures;
            }
            s_next_wifi_retry_ms = millis() + kWifiRetryBackoffMs;
            if (s_wifi_connect_failures >= kWifiFallbackFailureThreshold) {
                WifiConfigPortal_EnterFallbackMode();
                Serial.printf("[NRL] wifi connect failed %u times, enter config AP and retry after %lu ms\n",
                              static_cast<unsigned>(s_wifi_connect_failures),
                              static_cast<unsigned long>(kWifiRetryBackoffMs));
            } else {
                Serial.printf("[NRL] wifi connect failed (%u/%u), retry after %lu ms\n",
                              static_cast<unsigned>(s_wifi_connect_failures),
                              static_cast<unsigned>(kWifiFallbackFailureThreshold),
                              static_cast<unsigned long>(kWifiRetryBackoffMs));
            }
            return false;
        }
        s_next_wifi_retry_ms = 0u;
        s_wifi_connect_failures = 0u;
    }

    if (!s_udp_started) {
        if (!s_server_ip.fromString(server_host)) {
            if (!WiFi.hostByName(server_host, s_server_ip)) {
                Serial.printf("[NRL] resolve host failed: %s\n", server_host);
                return false;
            }
        }
        s_udp_started = s_udp.begin(local_port) == 1;
        if (!s_udp_started) {
            Serial.println("[NRL] udp begin failed");
            return false;
        }
        Serial.printf("[NRL] udp local=%u remote=%s(%s):%u\n",
                      static_cast<unsigned>(local_port),
                      server_host,
                      s_server_ip.toString().c_str(),
                      static_cast<unsigned>(server_port));
    }

    return true;
}

static void startDownlinkPlayback(void)
{
    if (s_downlink_playback_active) {
        return;
    }

    ES8311_ClearOutputQueue();
    STATUS_IO_SetPttActive(true);

    if (!ES8311_SetReceiveMode()) {
        Serial.println("[NRL] failed to keep ES8311 in receive/speaker mode");
        STATUS_IO_SetPttActive(false);
        return;
    }

    s_downlink_playback_active = true;
}

static void logIncomingVoicePacket(const size_t payload_size)
{
    if (!s_voice_stream_logged) {
        s_voice_stream_logged = true;
        s_last_voice_payload_size = payload_size;
        const char *callsign = (s_remote_callsign[0] != '\0') ? s_remote_callsign : "UNKNOWN";
        Serial.printf("[NRL] voice start: from=%s-%u\n",
                      callsign,
                      static_cast<unsigned>(s_remote_ssid));
        return;
    }

    if (payload_size != s_last_voice_payload_size) {
        s_last_voice_payload_size = payload_size;
    }
}

static void logReceivedPacket(const uint8_t packet_type,
                              const size_t payload_size,
                              const size_t packet_size,
                              const IPAddress &peer_ip,
                              const uint16_t peer_port)
{
    if (packet_type == kNrlTypeHeartbeat ||
        packet_type == kNrlTypeServerHeartbeat ||
        packet_type == kNrlTypeVoice ||
        packet_type == kNrlTypeServerVoice ||
        packet_type == kNrlTypeSciTransparent) {
        return;
    }

    const char *callsign = (s_remote_callsign[0] != '\0') ? s_remote_callsign : "UNKNOWN";
    Serial.printf("[NRL] rx packet: from=%s-%u ip=%s:%u type=%u packet=%u payload=%u\n",
                  callsign,
                  static_cast<unsigned>(s_remote_ssid),
                  peer_ip.toString().c_str(),
                  static_cast<unsigned>(peer_port),
                  static_cast<unsigned>(packet_type),
                  static_cast<unsigned>(packet_size),
                  static_cast<unsigned>(payload_size));
}

static void stopDownlinkPlayback(void)
{
    if (!s_downlink_playback_active) {
        return;
    }

    ES8311_ClearOutputQueue();

    if (!ES8311_SetReceiveMode()) {
        Serial.println("[NRL] failed to keep ES8311 in receive/speaker mode");
    }

    s_downlink_playback_active = false;
    s_voice_stream_logged = false;
    s_voice_decode_logged = false;
    s_voice_decode_peak_max = 0;
    s_voice_decode_sample_total = 0u;
    s_voice_decode_chunks = 0u;
    s_last_voice_payload_size = 0u;
    STATUS_IO_SetPttActive(false);
}

static void handleIncomingVoicePayload(const uint8_t *payload, const size_t payload_size)
{
    // The NRL packet type alone identifies a voice packet; the G.711 payload
    // length is variable, so it must not be range-checked here. The decode
    // loop below chunks whatever length arrived.
    if (payload == nullptr) {
        return;
    }

    // A real voice packet: latch its sender as the current voice caller.
    // updateRemoteIdentity() already parsed this packet's header into
    // s_remote_callsign / s_remote_ssid just before this call.
    memcpy(s_voice_callsign, s_remote_callsign, sizeof(s_voice_callsign));
    s_voice_ssid = s_remote_ssid;

    startDownlinkPlayback();
    if (!s_downlink_playback_active) {
        return;
    }

    s_last_rx_packet_ms = millis();
    logIncomingVoicePacket(payload_size);

    size_t offset = 0u;
    while (offset < payload_size) {
        const size_t chunk = ((payload_size - offset) < kG711RxPayloadMaxBytes)
                                 ? (payload_size - offset)
                                 : kG711RxPayloadMaxBytes;
        int16_t peak = 0;
        for (size_t i = 0; i < chunk; ++i) {
            s_downlink_pcm_buffer[i] = applyPcmGain(aLawToLinear(payload[offset + i]), kDownlinkPcmGain);
            int16_t value = s_downlink_pcm_buffer[i];
            if (value < 0) {
                value = static_cast<int16_t>(-value);
            }
            if (value > peak) {
                peak = value;
            }
        }
        if (!s_voice_decode_logged) {
            s_voice_decode_logged = true;
        }
        if (peak > s_voice_decode_peak_max) {
            s_voice_decode_peak_max = peak;
        }
        s_voice_decode_sample_total += static_cast<uint32_t>(chunk);
        ++s_voice_decode_chunks;
        if (chunk > 0u) {
            ES8311_QueueOutputSamples(s_downlink_pcm_buffer, chunk);
        }
        offset += chunk;
    }
}

static void sendHeartbeat(void)
{
    uint8_t packet[kNrlHeaderSize];
    const size_t packet_size = buildNrlPacket(kNrlTypeHeartbeat, nullptr, 0u, packet, sizeof(packet));
    if (packet_size > 0u) {
        udpSendPacket(packet, packet_size);
    }
}

static void restartTransport(const bool restart_wifi, const bool restart_udp)
{
    if (restart_udp) {
        s_udp.stop();
        s_udp_started = false;
        s_server_ip = IPAddress();
    }
    if (restart_wifi) {
        WiFi.disconnect();
    }
}

static void scheduleReboot(void)
{
    s_reboot_pending = true;
    s_reboot_at_ms = millis() + 1000u;
    Serial.println("[NRL] reboot scheduled by AT command");
}

//=============== Serial AT command console (USB debug serial) ================

constexpr size_t kSerialAtLineMax = 168u;
char s_serial_at_line[kSerialAtLineMax];
size_t s_serial_at_len = 0u;
uint8_t s_serial_at_payload[kSerialAtLineMax + 1u];

// Execute one AT command line typed on the USB debug serial and print the
// reply back to the same serial port.
static void processSerialAtLine(char *line)
{
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    size_t len = strlen(line);
    while (len > 0u && (line[len - 1u] == ' ' || line[len - 1u] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0u) {
        return;
    }

    Serial.printf("[AT] serial command: %s\n", line);

    // Wrap the text line into the binary payload the AT handler expects:
    // byte 0 is the 0x01 request marker, followed by the "AT+KEY=VALUE" text.
    if (len > sizeof(s_serial_at_payload) - 1u) {
        len = sizeof(s_serial_at_payload) - 1u;
    }
    s_serial_at_payload[0] = 0x01u;
    memcpy(s_serial_at_payload + 1u, line, len);

    NrlAtCommandResult at_result = {};
    NRL_AT_HandlePayload(s_serial_at_payload, len + 1u, NRL_AT_SOURCE_SERIAL, &at_result);

    if (at_result.should_reply && at_result.payload_size > 1u) {
        // Skip the 0x02 reply marker; the remainder is CRLF-delimited text.
        Serial.write(at_result.payload + 1u, at_result.payload_size - 1u);
    }

    if (at_result.restart_wifi || at_result.restart_udp) {
        Serial.printf("[AT] applying config via serial: restart_wifi=%d restart_udp=%d\n",
                      at_result.restart_wifi ? 1 : 0,
                      at_result.restart_udp ? 1 : 0);
        restartTransport(at_result.restart_wifi, at_result.restart_udp);
    }
    if (at_result.reboot) {
        scheduleReboot();
    }
}

static void pollSerialAtConsole(void)
{
    while (Serial.available() > 0) {
        const int ch = Serial.read();
        if (ch < 0) {
            break;
        }
        if (ch == '\n' || ch == '\r') {
            if (s_serial_at_len > 0u) {
                s_serial_at_line[s_serial_at_len] = '\0';
                processSerialAtLine(s_serial_at_line);
                s_serial_at_len = 0u;
            }
            continue;
        }
        if (s_serial_at_len + 1u >= sizeof(s_serial_at_line)) {
            s_serial_at_len = 0u;
            Serial.println("[AT] serial command too long, discarded");
            continue;
        }
        s_serial_at_line[s_serial_at_len++] = static_cast<char>(ch);
    }
}

static void bridgeTask(void *)
{
    while (true) {
        if (!ensureWifiAndUdp()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool have_packet = false;
        int bytes_read = 0;
        if (s_udp_mutex != nullptr && xSemaphoreTake(s_udp_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            const int packet_size = s_udp.parsePacket();
            if (packet_size > 0) {
                s_last_peer_ip = s_udp.remoteIP();
                s_last_peer_port = s_udp.remotePort();
                const int clamped = (packet_size < static_cast<int>(sizeof(s_rx_packet_buffer)))
                                        ? packet_size
                                        : static_cast<int>(sizeof(s_rx_packet_buffer));
                bytes_read = s_udp.read(s_rx_packet_buffer, clamped);
                have_packet = bytes_read > 0;
            }
            xSemaphoreGive(s_udp_mutex);
        }

        if (have_packet) {
            uint8_t packet_type = 0u;
            const uint8_t *payload = nullptr;
            size_t payload_size = 0u;
            if (parseNrlPacket(s_rx_packet_buffer,
                               static_cast<size_t>(bytes_read),
                               &packet_type,
                               &payload,
                               &payload_size)) {
                STATUS_IO_NotifyHeartbeatReceived();
                updateRemoteIdentity(s_rx_packet_buffer, static_cast<size_t>(bytes_read));
                logReceivedPacket(packet_type,
                                  payload_size,
                                  static_cast<size_t>(bytes_read),
                                  s_last_peer_ip,
                                  s_last_peer_port);
                if (packet_type == kNrlTypeVoice || packet_type == kNrlTypeServerVoice) {
                    handleIncomingVoicePayload(payload, payload_size);
                } else if (packet_type == kNrlTypeSciTransparent) {
                    handleIncomingSciPayload(payload, payload_size);
                } else if (packet_type == kNrlTypeAtCommand) {
                    char at_log_text[kAtLogTextMax] = {};
                    formatAtLogText(payload, payload_size, at_log_text, sizeof(at_log_text));
                    Serial.printf("[NRL] at packet: from=%s-%u payload=%u data=%s\n",
                                  (s_remote_callsign[0] != '\0') ? s_remote_callsign : "UNKNOWN",
                                  static_cast<unsigned>(s_remote_ssid),
                                  static_cast<unsigned>(payload_size),
                                  at_log_text);
                    NrlAtCommandResult at_result = {};
                    NRL_AT_HandlePayload(payload, payload_size, NRL_AT_SOURCE_REMOTE, &at_result);
                    if (at_result.should_reply && at_result.payload_size > 0u) {
                        const size_t reply_packet_size = buildNrlPacket(kNrlTypeAtCommand,
                                                                        at_result.payload,
                                                                        at_result.payload_size,
                                                                        s_at_reply_packet,
                                                                        sizeof(s_at_reply_packet));
                        if (reply_packet_size > 0u) {
                            const bool sent = udpSendPacketTo(s_at_reply_packet,
                                                              reply_packet_size,
                                                              s_last_peer_ip,
                                                              s_last_peer_port);
                            if (!sent) {
                                Serial.printf("[NRL] at reply send failed to %s:%u\n",
                                              s_last_peer_ip.toString().c_str(),
                                              static_cast<unsigned>(s_last_peer_port));
                            }
                        }
                    }
                    if (at_result.restart_wifi || at_result.restart_udp) {
                        restartTransport(at_result.restart_wifi, at_result.restart_udp);
                    }
                    if (at_result.reboot) {
                        scheduleReboot();
                    }
                }
            } else {
                Serial.printf("[NRL] rx parse failed: ip=%s:%u bytes=%u hdr=%02X %02X %02X %02X len=%02X%02X type=%02X\n",
                              s_last_peer_ip.toString().c_str(),
                              static_cast<unsigned>(s_last_peer_port),
                              static_cast<unsigned>(bytes_read),
                              static_cast<unsigned>(s_rx_packet_buffer[0]),
                              static_cast<unsigned>(s_rx_packet_buffer[1]),
                              static_cast<unsigned>(s_rx_packet_buffer[2]),
                              static_cast<unsigned>(s_rx_packet_buffer[3]),
                              static_cast<unsigned>(s_rx_packet_buffer[4]),
                              static_cast<unsigned>(s_rx_packet_buffer[5]),
                              static_cast<unsigned>((bytes_read > 20) ? s_rx_packet_buffer[20] : 0u));
            }
        }

        const uint32_t now = millis();
        if (s_downlink_playback_active &&
            (now - s_last_rx_packet_ms) > static_cast<uint32_t>(NRL_AUDIO_RX_PACKET_TIMEOUT_MS)) {
            stopDownlinkPlayback();
        }
        if ((now - s_last_heartbeat_ms) > static_cast<uint32_t>(NRL_AUDIO_HEARTBEAT_INTERVAL_MS)) {
            sendHeartbeat();
            s_last_heartbeat_ms = now;
        }
        if ((now - s_last_sci_poll_ms) >= kSciPollIntervalMs) {
            s_last_sci_poll_ms = now;
            pollSciUplink();
        }
        if (s_reboot_pending && static_cast<int32_t>(now - s_reboot_at_ms) >= 0) {
            Serial.println("[NRL] reboot now");
            Serial.flush();
            ESP.restart();
        }

        STATUS_IO_Poll();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

} // namespace

bool NRLAudioBridge_Init(void)
{
    if (s_bridge_initialized) {
        return true;
    }

    EXTERNAL_RADIO_Init();
    SCI_SERIAL_Init();
    initCpuId();
    initALawTables();

    if (s_udp_mutex == nullptr) {
        s_udp_mutex = xSemaphoreCreateMutex();
        if (s_udp_mutex == nullptr) {
            Serial.println("[NRL] failed to create udp mutex");
            return false;
        }
    }

    ES8311_SetFrameHook(es8311FrameHook, nullptr);

    if (xTaskCreate(bridgeTask, "nrl_audio_bridge", 8192, nullptr, 2, &s_bridge_task) != pdPASS) {
        Serial.println("[NRL] failed to create bridge task");
        return false;
    }

    s_bridge_initialized = true;
    Serial.println("[NRL] bridge task started");
    return true;
}

bool NRLAudioBridge_GetRemoteIdentity(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u) {
        return false;
    }

    const uint32_t now = millis();
    if (s_last_remote_identity_ms == 0u ||
        (now - s_last_remote_identity_ms) > static_cast<uint32_t>(NRL_AUDIO_RX_PACKET_TIMEOUT_MS + 2000u) ||
        s_remote_callsign[0] == '\0') {
        buffer[0] = '\0';
        return false;
    }

    snprintf(buffer, buffer_size, "%s-%u", s_remote_callsign, static_cast<unsigned>(s_remote_ssid));
    return true;
}

bool NRLAudioBridge_GetRemoteCaller(char *callsign, size_t callsign_size, unsigned *ssid)
{
    if (callsign != nullptr && callsign_size > 0u) {
        strncpy(callsign, s_voice_callsign, callsign_size - 1u);
        callsign[callsign_size - 1u] = '\0';
    }
    if (ssid != nullptr) {
        *ssid = static_cast<unsigned>(s_voice_ssid);
    }

    // "Receiving" means a voice stream is actually playing right now -- not
    // merely that some packet arrived recently. Heartbeat / AT / SCI packets
    // never start downlink playback, so they no longer count as reception.
    return s_downlink_playback_active;
}

void NRLAudioBridge_ApplyConfig(bool restart_wifi, bool restart_udp)
{
    if (!restart_wifi && !restart_udp) {
        return;
    }

    restartTransport(restart_wifi, restart_udp);
}

void NRLAudioBridge_PollSerialConsole(void)
{
    pollSerialAtConsole();
}
