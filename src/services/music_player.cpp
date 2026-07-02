#include "services/music_player.h"

#include "audio/audio_focus.h"
#include "driver/es8389.h"
#include "media/wav_reader.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>

static const char *TAG = "MUSIC";

namespace {

// One read/write block. 16 KB ~= 46 ms of 44.1k/16/2 audio, 4 blocks in
// flight through the I2S DMA keeps the TF-card read jitter inaudible while
// staying tiny next to the 192k worst case (16 KB = ~10 ms there, still
// fine because SDMMC reads far outpace 9.2 Mbit/s).
constexpr size_t kReadBlockBytes = 16 * 1024;
constexpr size_t kMaxPathLen = 128;

static TaskHandle_t s_player_task = nullptr;
static volatile bool s_stop_requested = false;
static volatile bool s_playing = false;
static char s_current_path[kMaxPathLen] = {};

static void player_task(void *)
{
    bool hifi_acquired = false;
    FILE *file = fopen(s_current_path, "rb");
    uint8_t *block = nullptr;
    int16_t *stereo = nullptr;

    do {
        if (file == nullptr) {
            ESP_LOGE(TAG, "open failed: %s", s_current_path);
            break;
        }

        WavInfo info = {};
        if (!WAV_ReadHeader(file, &info)) {
            ESP_LOGE(TAG, "not a supported WAV: %s", s_current_path);
            break;
        }
        if (info.bits_per_sample != 16u || (info.channels != 1u && info.channels != 2u)) {
            ESP_LOGE(TAG, "unsupported WAV format: %ubit %uch (16-bit 1/2ch only for now)",
                     static_cast<unsigned>(info.bits_per_sample),
                     static_cast<unsigned>(info.channels));
            break;
        }

        block = static_cast<uint8_t *>(heap_caps_malloc(kReadBlockBytes, MALLOC_CAP_SPIRAM));
        // Mono tracks are expanded to stereo here; always opening the codec
        // 2-channel avoids per-format bring-up risk on the I2S slot config.
        if (info.channels == 1u) {
            stereo = static_cast<int16_t *>(heap_caps_malloc(kReadBlockBytes * 2u, MALLOC_CAP_SPIRAM));
        }
        if (block == nullptr || (info.channels == 1u && stereo == nullptr)) {
            ESP_LOGE(TAG, "buffer alloc failed");
            break;
        }

        if (!ES8389_HifiAcquire(info.sample_rate_hz, 16u, 2u)) {
            ESP_LOGE(TAG, "hi-fi speaker unavailable (wrong board, or voice busy)");
            break;
        }
        hifi_acquired = true;

        ESP_LOGI(TAG, "playing %s: %luHz %ubit %uch %lu bytes",
                 s_current_path,
                 static_cast<unsigned long>(info.sample_rate_hz),
                 static_cast<unsigned>(info.bits_per_sample),
                 static_cast<unsigned>(info.channels),
                 static_cast<unsigned long>(info.data_bytes));

        uint32_t remaining = info.data_bytes;
        while (remaining > 0u && !s_stop_requested) {
            const size_t want = (remaining < kReadBlockBytes) ? remaining : kReadBlockBytes;
            const size_t got = fread(block, 1, want, file);
            if (got == 0u) {
                break;
            }
            remaining -= got;

            if (info.channels == 2u) {
                if (!ES8389_HifiWrite(block, got)) {
                    break;
                }
            } else {
                const size_t samples = got / sizeof(int16_t);
                const int16_t *mono = reinterpret_cast<const int16_t *>(block);
                for (size_t i = 0; i < samples; ++i) {
                    stereo[i * 2u] = mono[i];
                    stereo[i * 2u + 1u] = mono[i];
                }
                if (!ES8389_HifiWrite(stereo, samples * 2u * sizeof(int16_t))) {
                    break;
                }
            }
        }

        ESP_LOGI(TAG, "%s: %s", s_current_path, s_stop_requested ? "stopped" : "finished");
    } while (false);

    if (file != nullptr) {
        fclose(file);
    }
    if (block != nullptr) {
        heap_caps_free(block);
    }
    if (stereo != nullptr) {
        heap_caps_free(stereo);
    }
    if (hifi_acquired) {
        ES8389_HifiRelease();
    }

    s_playing = false;
    s_player_task = nullptr;
    vTaskDelete(nullptr);
}

static void on_voice_start(void)
{
    // Interrupt policy: incoming NRL voice stops the music so the voice
    // path (and the mic uplink) come back. Fire-and-forget by design.
    if (s_playing) {
        ESP_LOGI(TAG, "voice started, stopping music");
        MUSIC_Stop();
    }
}

} // namespace

extern "C" void MUSIC_Init(void)
{
    AudioFocus_RegisterVoiceStart(on_voice_start);
}

extern "C" bool MUSIC_PlayFile(const char *path)
{
    if (path == nullptr || path[0] == '\0' || strlen(path) >= kMaxPathLen) {
        return false;
    }

    MUSIC_Stop();
    // Wait for the previous player task to wind down (it releases the codec).
    for (int i = 0; i < 200 && s_player_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_player_task != nullptr) {
        ESP_LOGE(TAG, "previous track did not stop");
        return false;
    }

    strncpy(s_current_path, path, sizeof(s_current_path) - 1u);
    s_current_path[sizeof(s_current_path) - 1u] = '\0';
    s_stop_requested = false;
    s_playing = true;

    // Priority below the voice passthrough task (10): file reads and I2S
    // writes are throughput work, not latency-critical. Internal-RAM stack:
    // stdio/FatFS/SDMMC may run with flash cache paused.
    if (xTaskCreatePinnedToCore(player_task, "music_player", 6144, nullptr, 5, &s_player_task, 1) != pdPASS) {
        s_playing = false;
        s_player_task = nullptr;
        ESP_LOGE(TAG, "player task create failed");
        return false;
    }
    return true;
}

extern "C" void MUSIC_Stop(void)
{
    if (s_player_task != nullptr) {
        s_stop_requested = true;
    }
}

extern "C" bool MUSIC_IsPlaying(void)
{
    return s_playing;
}

extern "C" const char *MUSIC_CurrentPath(void)
{
    return s_current_path;
}
