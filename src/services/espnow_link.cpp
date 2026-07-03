#include "services/espnow_link.h"

#include "audio/audio_router.h"
#include "driver/board_pins.h"
#include "driver/external_radio.h"
#include "driver/status_io.h"
#include "lib/nrl_g711.h"
#include "services/config_notify.h"

#include <esp_log.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <stdio.h>
#include <string.h>

static const char *TAG = "ESPNOW";

namespace {

constexpr const char *kNvsNamespace = "espnow";

// Packet layout: magic(4) type(1) callsign(6) ssid(1) payload(G.711 A-law).
constexpr uint8_t kMagic[4] = {'N', 'R', 'L', 'E'};
constexpr uint8_t kTypeVoice = 1;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kVoiceBytes = 160; // 20 ms at 8 kHz
constexpr size_t kPacketBytes = kHeaderBytes + kVoiceBytes;
static_assert(kPacketBytes <= ESP_NOW_MAX_DATA_LEN, "packet exceeds ESP-NOW MTU");

static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static volatile bool s_enabled = false;
static bool s_espnow_inited = false;
static uint8_t s_tx_packet[kPacketBytes];
static size_t s_tx_fill = kHeaderBytes;
static char s_last_peer[16] = {};
static volatile bool s_ptt_held = false;      // dedicated ESP-NOW hold-to-talk
static volatile int64_t s_last_rx_us = 0;     // last voice frame arrival

static void save_enabled(const bool enabled)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "en", enabled ? 1 : 0);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool load_enabled(void)
{
    nvs_handle_t nvs;
    uint8_t en = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u8(nvs, "en", &en);
        nvs_close(nvs);
    }
    return en != 0;
}

// Router sink: mic frames (8 kHz, router-converted) -> G.711 -> broadcast.
// Keying: the S31 touch UI has a dedicated ESP-NOW PTT (the NRL PTT no
// longer simulcasts here); radio-gateway boards without a touch UI keep the
// squelch-relay gate. Runs in the audio task.
static void espnow_sink_write(uint8_t /*source_id*/,
                              const int16_t *samples,
                              size_t sample_count,
                              void *)
{
#if NRL_BOARD == NRL_BOARD_S31_KORVO
    const bool keyed = s_ptt_held;
#else
    const bool keyed = STATUS_IO_IsSqlActive();
#endif
    if (!s_enabled || !keyed) {
        s_tx_fill = kHeaderBytes; // drop partial packet on key release
        return;
    }
    for (size_t i = 0; i < sample_count; ++i) {
        s_tx_packet[s_tx_fill++] = NRL_G711_EncodeALaw(samples[i]);
        if (s_tx_fill >= kPacketBytes) {
            s_tx_fill = kHeaderBytes;
            (void)esp_now_send(kBroadcastMac, s_tx_packet, kPacketBytes);
        }
    }
}

// Receive callback (WiFi task context): decode and hand to the router;
// the speaker sink only queues, so this stays cheap.
static void espnow_recv_cb(const esp_now_recv_info_t * /*info*/,
                           const uint8_t *data, int len)
{
    if (!s_enabled || data == nullptr ||
        len <= static_cast<int>(kHeaderBytes) ||
        memcmp(data, kMagic, sizeof(kMagic)) != 0 || data[4] != kTypeVoice) {
        return;
    }

    char callsign[7];
    memcpy(callsign, data + 5, 6);
    callsign[6] = '\0';
    for (int i = 5; i >= 0 && (callsign[i] == ' ' || callsign[i] == '\0'); --i) {
        callsign[i] = '\0';
    }
    snprintf(s_last_peer, sizeof(s_last_peer), "%s-%u", callsign,
             static_cast<unsigned>(data[11]));
    s_last_rx_us = esp_timer_get_time();

    const size_t payload = static_cast<size_t>(len) - kHeaderBytes;
    int16_t pcm[ESP_NOW_MAX_DATA_LEN];
    const size_t n = (payload < sizeof(pcm) / sizeof(pcm[0])) ? payload : sizeof(pcm) / sizeof(pcm[0]);
    for (size_t i = 0; i < n; ++i) {
        pcm[i] = NRL_G711_DecodeALaw(data[kHeaderBytes + i]);
    }
    AudioRouter_PushFrame(AUDIO_SRC_ESPNOW, 8000u, pcm, n);
}

static void fill_tx_header(void)
{
    memcpy(s_tx_packet, kMagic, sizeof(kMagic));
    s_tx_packet[4] = kTypeVoice;
    memset(s_tx_packet + 5, 0, 7);
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (config != nullptr) {
        strncpy(reinterpret_cast<char *>(s_tx_packet + 5), config->callsign, 6);
        s_tx_packet[11] = config->callsign_ssid;
    }
}

static bool espnow_bring_up(void)
{
    if (s_espnow_inited) {
        return true;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
        ESP_LOGW(TAG, "WiFi not started yet");
        return false;
    }
    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed");
        return false;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcastMac, sizeof(kBroadcastMac));
    peer.channel = 0; // follow the current WiFi channel
    peer.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        ESP_LOGE(TAG, "add broadcast peer failed");
        esp_now_deinit();
        return false;
    }
    if (esp_now_register_recv_cb(espnow_recv_cb) != ESP_OK) {
        ESP_LOGE(TAG, "register recv cb failed");
        esp_now_deinit();
        return false;
    }
    NRL_G711_Init();
    fill_tx_header();
    AudioRouter_RegisterSink(AUDIO_SINK_ESPNOW, 8000u, espnow_sink_write, nullptr);
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_ESPNOW, true);
    AudioRouter_SetRoute(AUDIO_SRC_BT_HFP_MIC, AUDIO_SINK_ESPNOW, true);
    AudioRouter_SetRoute(AUDIO_SRC_ESPNOW, AUDIO_SINK_SPEAKER, true);
    s_espnow_inited = true;
    ESP_LOGI(TAG, "voice link up (broadcast, G.711 20ms frames)");
    return true;
}

} // namespace

namespace {
// One-shot deferred enable: the bridge task brings WiFi up asynchronously
// after boot, so restoring the persisted ON state waits for it here.
static void espnow_deferred_enable_task(void *)
{
    for (int i = 0; i < 60; ++i) { // give WiFi up to 2 minutes
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (espnow_bring_up()) {
            s_enabled = true;
            ESP_LOGI(TAG, "restored persisted ON state");
            break;
        }
    }
    vTaskDelete(nullptr);
}
} // namespace

extern "C" void ESPNOW_LINK_Init(void)
{
    if (load_enabled()) {
        if (espnow_bring_up()) {
            s_enabled = true;
        } else if (xTaskCreatePinnedToCore(espnow_deferred_enable_task, "espnow_en",
                                           3072, nullptr, 2, nullptr, 0) != pdPASS) {
            ESP_LOGW(TAG, "enable deferred task failed; use AT+ESPNOW=ON");
        }
    }
}

extern "C" bool ESPNOW_LINK_SetEnabled(const bool enabled)
{
    if (enabled) {
        if (!espnow_bring_up()) {
            return false;
        }
        fill_tx_header(); // refresh callsign in case config changed
        s_enabled = true;
    } else {
        s_enabled = false;
        s_ptt_held = false;
        s_tx_fill = kHeaderBytes;
    }
    save_enabled(enabled);
    CONFIG_NOTIFY_Bump();
    return true;
}

extern "C" bool ESPNOW_LINK_IsEnabled(void)
{
    return s_enabled;
}

extern "C" void ESPNOW_LINK_SetPtt(const bool held)
{
    s_ptt_held = held && s_enabled;
}

extern "C" bool ESPNOW_LINK_PttActive(void)
{
    return s_ptt_held;
}

extern "C" bool ESPNOW_LINK_IsReceiving(void)
{
    // "Receiving" = a voice frame arrived within the last half second.
    return s_enabled && s_last_rx_us != 0 &&
           (esp_timer_get_time() - s_last_rx_us) < 500000;
}

extern "C" void ESPNOW_LINK_GetLastPeer(char *out, const size_t out_size)
{
    if (out != nullptr && out_size > 0u) {
        snprintf(out, out_size, "%s", s_last_peer);
    }
}
