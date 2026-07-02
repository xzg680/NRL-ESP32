#include "services/nanny.h"

#include "lib/nrl_audio_bridge.h"
#include "services/music_player.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <stdio.h>
#include <string.h>

static const char *TAG = "NANNY";

namespace {

constexpr const char *kNvsNamespace = "nanny";
constexpr uint32_t kTickMs = 2000u;
constexpr uint32_t kMinIntervalMin = 1u;
constexpr uint32_t kMaxIntervalMin = 1440u;

static char s_beacon_path[128] = {};
static volatile uint32_t s_interval_min = 0; // 0 = disarmed
static TaskHandle_t s_task = nullptr;
static int64_t s_last_fire_us = 0;

static bool save_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_str(nvs, "path", s_beacon_path) == ESP_OK &&
                    nvs_set_u32(nvs, "interval", s_interval_min) == ESP_OK &&
                    nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

static void load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_beacon_path);
    uint32_t interval = 0;
    if (nvs_get_str(nvs, "path", s_beacon_path, &len) == ESP_OK &&
        nvs_get_u32(nvs, "interval", &interval) == ESP_OK &&
        s_beacon_path[0] != '\0' &&
        interval >= kMinIntervalMin && interval <= kMaxIntervalMin) {
        s_interval_min = interval;
    } else {
        s_beacon_path[0] = '\0';
    }
    nvs_close(nvs);
}

static void nanny_task(void *)
{
    // First beacon fires one full interval after boot/arming, not instantly.
    s_last_fire_us = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
        const uint32_t interval = s_interval_min;
        if (interval == 0u) {
            continue;
        }
        const int64_t due_us = s_last_fire_us +
                               static_cast<int64_t>(interval) * 60LL * 1000000LL;
        if (esp_timer_get_time() < due_us) {
            continue;
        }
        // Postpone while an NRL voice stream is playing -- don't talk over
        // an active QSO; retry on the next tick until the channel is idle.
        if (NRLAudioBridge_GetRemoteCaller(nullptr, 0, nullptr)) {
            continue;
        }
        s_last_fire_us = esp_timer_get_time();
        ESP_LOGI(TAG, "beacon: %s", s_beacon_path);
        if (!MUSIC_PlayFile(s_beacon_path)) {
            ESP_LOGW(TAG, "beacon playback failed (file missing?)");
        }
        // When the beacon finishes, the playlist auto-advance resumes the
        // normal rotation (music_player track-end callback).
    }
}

static void ensure_task(void)
{
    if (s_task == nullptr &&
        xTaskCreatePinnedToCore(nanny_task, "nanny", 4096, nullptr, 3, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_task = nullptr;
    }
}

} // namespace

extern "C" void NANNY_Init(void)
{
    load_config();
    if (s_interval_min > 0u) {
        ensure_task();
        ESP_LOGI(TAG, "beacon armed: %s every %lu min",
                 s_beacon_path, static_cast<unsigned long>(s_interval_min));
    }
}

extern "C" bool NANNY_SetBeacon(const char *path, const uint32_t interval_min)
{
    if (path == nullptr || path[0] == '\0' || strlen(path) >= sizeof(s_beacon_path) ||
        interval_min < kMinIntervalMin || interval_min > kMaxIntervalMin) {
        return false;
    }
    snprintf(s_beacon_path, sizeof(s_beacon_path), "%s", path);
    s_interval_min = interval_min;
    s_last_fire_us = esp_timer_get_time();
    if (!save_config()) {
        ESP_LOGW(TAG, "config persist failed (beacon still armed)");
    }
    ensure_task();
    return true;
}

extern "C" void NANNY_DisableBeacon(void)
{
    s_interval_min = 0;
    s_beacon_path[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_erase_all(nvs);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

extern "C" bool NANNY_GetBeacon(char *path_out, const size_t path_size, uint32_t *interval_min)
{
    if (path_out != nullptr && path_size > 0u) {
        snprintf(path_out, path_size, "%s", s_beacon_path);
    }
    if (interval_min != nullptr) {
        *interval_min = s_interval_min;
    }
    return s_interval_min > 0u;
}
