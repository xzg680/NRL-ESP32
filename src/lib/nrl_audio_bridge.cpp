#include "nrl_audio_bridge.h"

#include "nrl_at_commands.h"
#include "nrl_audio_config.h"
#include "nrl_g711.h"
#include "nrl_net_compat.h"
#include "nrl_usb_console.h"
#include "nrl_wifi.h"
#include "wifi_config_portal.h"

#include "driver/audio_passthrough.h"
#include "driver/es8311.h"
#include "driver/external_radio.h"
#include "driver/sci_serial.h"
#include "driver/status_io.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "NRL";

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
// Minimum / maximum G.711 A-law payload bytes per outbound voice packet. The
// runtime value comes from ExternalRadioConfig::voice_payload_bytes and is
// clamped into this range; the accumulator and stack packet buffer below are
// sized at the max so the threshold can be changed at runtime without realloc.
constexpr size_t kG711PayloadMinBytes = 160u;
constexpr size_t kG711PayloadMaxBytes = 500u;
constexpr size_t kG711PayloadDefaultBytes = kG711PayloadMinBytes;
constexpr size_t kG711RxPayloadMaxBytes = 500u;
constexpr size_t kSciPayloadMaxBytes = 256u;
constexpr size_t kNrlMaxPayloadSize = 1024u;
constexpr size_t kNrlMaxPacketSize = kNrlHeaderSize + kNrlMaxPayloadSize;
constexpr int kDownlinkPcmGain = 1;
constexpr uint32_t kSciPollIntervalMs = 20u;
constexpr uint32_t kSciFlushIntervalMs = 20u;

int s_udp_fd = -1;
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
uint32_t s_server_ip = 0u;
uint32_t s_last_peer_ip = 0u;
uint16_t s_last_peer_port = 0u;
uint8_t s_cpu_id[4] = {};
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
    s_last_remote_identity_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
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
                            const uint32_t dest_ip,
                            const uint16_t dest_port)
{
    if (packet == nullptr || packet_size == 0u || !s_udp_started || s_udp_fd < 0 ||
        !nrlWifiStaConnected() || dest_port == 0u || dest_ip == 0u) {
        return false;
    }
    if (s_udp_mutex == nullptr) {
        return false;
    }
    // Wait up to 5 ms for the mutex instead of returning false immediately.
    // The bridge task's recvfrom() holds this mutex for microseconds at a
    // time, but a non-blocking try-take meant occasional voice packets were
    // silently dropped under contention -- showing up as occasional missing
    // packets on the wire.
    if (xSemaphoreTake(s_udp_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(dest_port);
    dest.sin_addr.s_addr = dest_ip;
    const ssize_t sent = sendto(s_udp_fd, packet, packet_size, 0,
                                reinterpret_cast<struct sockaddr *>(&dest),
                                sizeof(dest));
    const bool ok = (sent == static_cast<ssize_t>(packet_size));
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
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (!sent && (now - s_last_sci_log_ms) >= 1000u) {
        s_last_sci_log_ms = now;
        ESP_LOGI(TAG,"[SCI] uplink send failed bytes=%u\n",
                      static_cast<unsigned>(payload_count));
    }
}

static void pollSciUplink(void)
{
    int available = SCI_SERIAL_Available();
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
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
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (written != payload_size && (now - s_last_sci_log_ms) >= 1000u) {
        s_last_sci_log_ms = now;
        ESP_LOGI(TAG,"[SCI] downlink short write bytes=%u written=%u\n",
                      static_cast<unsigned>(payload_size),
                      static_cast<unsigned>(written));
    }
}

// The fixed NRL_AUDIO_RX_PACKET_TIMEOUT_MS (default 120 ms) was tuned for
// ~20 ms G.711 frames. Voice senders configured for larger payloads (up to
// 500 bytes = 62.5 ms) exceed it after just one or two packets of jitter,
// which would call stopDownlinkPlayback() and flush the DAC queue mid-talk.
// Scale to 4× the last observed frame duration so a few packets of jitter
// don't tear the playback.
static uint32_t currentRxPacketTimeoutMs(void)
{
    const uint32_t base_ms = static_cast<uint32_t>(NRL_AUDIO_RX_PACKET_TIMEOUT_MS);
    if (s_last_voice_payload_size == 0u) {
        return base_ms;
    }
    // G.711 8 kHz A-law: 8 bytes per millisecond of audio.
    const uint32_t frame_ms = static_cast<uint32_t>(s_last_voice_payload_size) / 8u;
    const uint32_t scaled = frame_ms * 4u;
    return (scaled > base_ms) ? scaled : base_ms;
}

static size_t currentVoicePayloadBytes(void)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const size_t configured = (config != nullptr)
                                  ? static_cast<size_t>(config->voice_payload_bytes)
                                  : kG711PayloadDefaultBytes;
    if (configured < kG711PayloadMinBytes) {
        return kG711PayloadDefaultBytes;
    }
    if (configured > kG711PayloadMaxBytes) {
        return kG711PayloadMaxBytes;
    }
    return configured;
}

static void sendVoiceFrame(const int16_t *pcm8k, const size_t sample_count)
{
    if (pcm8k == nullptr || sample_count == 0u) {
        return;
    }
    if (!STATUS_IO_IsSqlActive()) {
        return;
    }

    static uint8_t alaw_accumulator[kG711PayloadMaxBytes];
    static size_t alaw_accumulator_count = 0u;

    const size_t target_bytes = currentVoicePayloadBytes();
    // If the configured size shrunk while we already had more accumulated,
    // drop the stale tail so we start clean at the new threshold.
    if (alaw_accumulator_count > target_bytes) {
        alaw_accumulator_count = 0u;
    }

    const size_t used_samples = (sample_count < kAudioCodecFrameSamples) ? sample_count : kAudioCodecFrameSamples;
    for (size_t i = 0; i < used_samples; ++i) {
        if (alaw_accumulator_count < target_bytes) {
            alaw_accumulator[alaw_accumulator_count++] = NRL_G711_EncodeALaw(pcm8k[i]);
        }

        if (alaw_accumulator_count >= target_bytes) {
            uint8_t packet[kNrlHeaderSize + kG711PayloadMaxBytes];
            const size_t packet_size = buildNrlPacket(kNrlTypeVoice,
                                                      alaw_accumulator,
                                                      target_bytes,
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
                            AUDIO_Mode_t mode,
                            void *)
{
    if (mode != AUDIO_MODE_RECEIVE) {
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
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
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

    if (!nrlWifiStaConnected()) {
        const WifiConnResult result = wifiEnsureConnected(wifi_ssid, wifi_password, 15000u);
        if (result != WIFI_CONN_OK && result != WIFI_ALREADY_ON_SSID) {
            if (s_wifi_connect_failures < 0xFFu) {
                ++s_wifi_connect_failures;
            }
            s_next_wifi_retry_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) + kWifiRetryBackoffMs;
            if (s_wifi_connect_failures >= kWifiFallbackFailureThreshold) {
                WifiConfigPortal_EnterFallbackMode();
                ESP_LOGI(TAG,"[NRL] wifi connect failed %u times, enter config AP and retry after %lu ms\n",
                              static_cast<unsigned>(s_wifi_connect_failures),
                              static_cast<unsigned long>(kWifiRetryBackoffMs));
            } else {
                ESP_LOGI(TAG,"[NRL] wifi connect failed (%u/%u), retry after %lu ms\n",
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
        struct in_addr literal = {};
        if (inet_aton(server_host, &literal) != 0) {
            s_server_ip = literal.s_addr;
        } else {
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            struct addrinfo *res = nullptr;
            if (getaddrinfo(server_host, nullptr, &hints, &res) != 0 || res == nullptr) {
                ESP_LOGE(TAG, "resolve host failed: %s", server_host);
                if (res != nullptr) {
                    freeaddrinfo(res);
                }
                return false;
            }
            const struct sockaddr_in *sa = reinterpret_cast<const struct sockaddr_in *>(res->ai_addr);
            s_server_ip = sa->sin_addr.s_addr;
            freeaddrinfo(res);
        }

        if (s_udp_fd >= 0) {
            close(s_udp_fd);
            s_udp_fd = -1;
        }
        s_udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_udp_fd < 0) {
            ESP_LOGE(TAG, "udp socket() failed");
            return false;
        }
        struct sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_port = htons(local_port);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s_udp_fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) < 0) {
            ESP_LOGE(TAG, "udp bind(%u) failed", static_cast<unsigned>(local_port));
            close(s_udp_fd);
            s_udp_fd = -1;
            return false;
        }
        const int flags = fcntl(s_udp_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(s_udp_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ESP_LOGE(TAG, "udp set O_NONBLOCK failed");
            close(s_udp_fd);
            s_udp_fd = -1;
            return false;
        }
        // Mark every packet on this socket with DSCP EF (0xB8). The WiFi
        // driver maps that to WMM access category AC_VO, which (a) bypasses
        // AMPDU TX aggregation -- aggregation is what causes the observed
        // 2-packet bursts followed by long gaps under WIFI_AMPDU_TX_ENABLED
        // -- and (b) gets high MAC TX priority. Result: each sendto goes
        // on-air immediately instead of being batched into the next TX
        // opportunity with other queued packets.
        const int tos = 0xB8;
        if (setsockopt(s_udp_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0) {
            ESP_LOGW(TAG, "udp set IP_TOS=0xB8 failed (continuing anyway)");
        }
        s_udp_started = true;
        char ip_buf[16] = {};
        nrlIpToString(s_server_ip, ip_buf, sizeof(ip_buf));
        ESP_LOGI(TAG, "udp local=%u remote=%s(%s):%u",
                 static_cast<unsigned>(local_port),
                 server_host,
                 ip_buf,
                 static_cast<unsigned>(server_port));
    }

    return true;
}

static void startDownlinkPlayback(void)
{
    if (s_downlink_playback_active) {
        return;
    }

    AUDIO_ClearOutputQueue();
    STATUS_IO_SetPttActive(true);

    if (!ES8311_SetReceiveMode()) {
        ESP_LOGI(TAG,"[NRL] failed to keep ES8311 in receive/speaker mode");
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
        ESP_LOGI(TAG,"[NRL] voice start: from=%s-%u\n",
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
                              const uint32_t peer_ip,
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
    char ip_buf[16] = {};
    nrlIpToString(peer_ip, ip_buf, sizeof(ip_buf));
    ESP_LOGI(TAG, "rx packet: from=%s-%u ip=%s:%u type=%u packet=%u payload=%u",
             callsign,
             static_cast<unsigned>(s_remote_ssid),
             ip_buf,
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

    AUDIO_ClearOutputQueue();

    if (!ES8311_SetReceiveMode()) {
        ESP_LOGI(TAG,"[NRL] failed to keep ES8311 in receive/speaker mode");
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

    s_last_rx_packet_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    logIncomingVoicePacket(payload_size);

    size_t offset = 0u;
    while (offset < payload_size) {
        const size_t chunk = ((payload_size - offset) < kG711RxPayloadMaxBytes)
                                 ? (payload_size - offset)
                                 : kG711RxPayloadMaxBytes;
        int16_t peak = 0;
        for (size_t i = 0; i < chunk; ++i) {
            s_downlink_pcm_buffer[i] = applyPcmGain(NRL_G711_DecodeALaw(payload[offset + i]), kDownlinkPcmGain);
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
            AUDIO_QueueOutputSamples(s_downlink_pcm_buffer, chunk);
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
        if (s_udp_fd >= 0) {
            close(s_udp_fd);
            s_udp_fd = -1;
        }
        s_udp_started = false;
        s_server_ip = 0u;
    }
    if (restart_wifi) {
        esp_wifi_disconnect();
        // Clear the retry backoff and failure counter so the bridge task
        // tries the new credentials on its next loop iteration instead of
        // waiting out the 30s backoff from the previous bad SSID. Without
        // this, submitting a new SSID via the config portal silently waits
        // up to 30s before the device leaves AP fallback for STA.
        s_next_wifi_retry_ms = 0u;
        s_wifi_connect_failures = 0u;
    }
}

static void scheduleReboot(void)
{
    s_reboot_pending = true;
    s_reboot_at_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) + 1000u;
    ESP_LOGI(TAG,"[NRL] reboot scheduled by AT command");
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

    ESP_LOGI(TAG,"[AT] serial command: %s\n", line);

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
        NRL_USB_Console_Write(at_result.payload + 1u, at_result.payload_size - 1u);
    }

    if (at_result.restart_wifi || at_result.restart_udp) {
        ESP_LOGI(TAG,"[AT] applying config via serial: restart_wifi=%d restart_udp=%d\n",
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
    uint8_t buf[32];
    while (true) {
        const size_t got = NRL_USB_Console_Read(buf, sizeof(buf));
        if (got == 0u) {
            break;
        }
        for (size_t i = 0; i < got; ++i) {
            const char ch = static_cast<char>(buf[i]);
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
                ESP_LOGW(TAG, "AT serial command too long, discarded");
                continue;
            }
            s_serial_at_line[s_serial_at_len++] = ch;
        }
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
        if (s_udp_fd >= 0 && s_udp_mutex != nullptr &&
            xSemaphoreTake(s_udp_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            struct sockaddr_in from = {};
            socklen_t fromlen = sizeof(from);
            const ssize_t got = recvfrom(s_udp_fd,
                                         s_rx_packet_buffer,
                                         sizeof(s_rx_packet_buffer),
                                         MSG_DONTWAIT,
                                         reinterpret_cast<struct sockaddr *>(&from),
                                         &fromlen);
            if (got > 0) {
                s_last_peer_ip = from.sin_addr.s_addr;
                s_last_peer_port = ntohs(from.sin_port);
                bytes_read = static_cast<int>(got);
                have_packet = true;
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
                    ESP_LOGI(TAG,"[NRL] at packet: from=%s-%u payload=%u data=%s\n",
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
                                char ip_buf[16] = {};
                                nrlIpToString(s_last_peer_ip, ip_buf, sizeof(ip_buf));
                                ESP_LOGE(TAG, "at reply send failed to %s:%u",
                                         ip_buf,
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
                char ip_buf[16] = {};
                nrlIpToString(s_last_peer_ip, ip_buf, sizeof(ip_buf));
                ESP_LOGW(TAG, "rx parse failed: ip=%s:%u bytes=%u hdr=%02X %02X %02X %02X len=%02X%02X type=%02X",
                         ip_buf,
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

        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (s_downlink_playback_active &&
            (now - s_last_rx_packet_ms) > currentRxPacketTimeoutMs()) {
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
            ESP_LOGI(TAG, "reboot now");
            NRL_USB_Console_Flush();
            esp_restart();
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
    NRL_G711_Init();

    if (s_udp_mutex == nullptr) {
        s_udp_mutex = xSemaphoreCreateMutex();
        if (s_udp_mutex == nullptr) {
            ESP_LOGI(TAG,"[NRL] failed to create udp mutex");
            return false;
        }
    }
    AUDIO_SetFrameHook(es8311FrameHook, nullptr);

    // Pin to core 0 so RX recvfrom() / heartbeat / AT replies share the WiFi
    // and lwIP cores; voice TX from the audio task on core 1 still hits the
    // same socket but lwIP's internal locking handles that.
    if (xTaskCreatePinnedToCore(bridgeTask, "nrl_audio_bridge", 8192, nullptr,
                                2, &s_bridge_task, 0) != pdPASS) {
        ESP_LOGI(TAG,"[NRL] failed to create bridge task");
        return false;
    }

    s_bridge_initialized = true;
    ESP_LOGI(TAG,"[NRL] bridge task started");
    return true;
}

bool NRLAudioBridge_GetRemoteIdentity(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u) {
        return false;
    }

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
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
