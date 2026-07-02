#include "services/music_player.h"

#include "audio/audio_focus.h"
#include "audio/voice_resampler.h"
#include "driver/es8389.h"
#include "lib/nrl_audio_bridge.h"
#include "media/media_decoder.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>

static const char *TAG = "MUSIC";

namespace {

constexpr size_t kMaxPathLen = 256; // long enough for net-radio URLs

static TaskHandle_t s_player_task = nullptr;
static volatile bool s_stop_requested = false;
static volatile bool s_playing = false;
static char s_current_path[kMaxPathLen] = {};
static MediaTrackInfo s_track_info = {};

// Mono tracks are expanded to stereo before the codec: always opening the
// I2S/codec 2-channel avoids per-format bring-up risk on the slot config.
static int16_t *s_stereo_buffer = nullptr;
static size_t s_stereo_capacity = 0;
static volatile int s_target = MUSIC_TARGET_LOCAL;

// 8 kHz mono scratch for the network branch of the fan-out.
static int16_t *s_net_buffer = nullptr;
static size_t s_net_capacity = 0;

static bool ensure_stereo_capacity(const size_t bytes)
{
    if (s_stereo_capacity >= bytes) {
        return true;
    }
    int16_t *grown = static_cast<int16_t *>(heap_caps_realloc(s_stereo_buffer, bytes, MALLOC_CAP_SPIRAM));
    if (grown == nullptr) {
        return false;
    }
    s_stereo_buffer = grown;
    s_stereo_capacity = bytes;
    return true;
}

static bool ensure_net_capacity(const size_t samples)
{
    if (s_net_capacity >= samples) {
        return true;
    }
    int16_t *grown = static_cast<int16_t *>(
        heap_caps_realloc(s_net_buffer, samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (grown == nullptr) {
        return false;
    }
    s_net_buffer = grown;
    s_net_capacity = samples;
    return true;
}

static volatile MusicTrackEndCb_t s_track_end_cb = nullptr;

static void player_task(void *)
{
    // Tags first (cheap header/tail reads), so the UI has title/cover as
    // soon as -- or before -- the first PCM reaches the speaker.
    (void)MEDIA_META_Read(s_current_path, &s_track_info, true);

    // Playback target is latched per track (docs/architecture.md nanny
    // 三档切换): local hi-fi, NRL network uplink, or a fan-out of both --
    // one decode, two consumers.
    const int target = s_target;
    const bool to_local = (target != MUSIC_TARGET_NET);
    const bool to_net = (target != MUSIC_TARGET_LOCAL);

    bool hifi_acquired = false;
    bool format_ready = false;
    bool reached_end = false;
    VoiceResampler resampler = {};
    // Pacing for the net-only case: without the I2S DMA back-pressure the
    // decode loop would free-run; throttle to real time by the 8 kHz clock.
    int64_t net_start_us = 0;
    uint64_t net_sent_samples = 0;
    MediaDecoder *decoder = MEDIA_DECODER_Open(s_current_path);

    if (to_net && decoder != nullptr) {
        NRLAudioBridge_SetMediaUplinkActive(true);
    }

    while (decoder != nullptr && !s_stop_requested) {
        const uint8_t *pcm = nullptr;
        size_t bytes = 0;
        const int rc = MEDIA_DECODER_Decode(decoder, &pcm, &bytes);
        if (rc <= 0) {
            if (rc < 0) {
                ESP_LOGE(TAG, "decode failed: %s", s_current_path);
            } else {
                reached_end = format_ready;
            }
            break;
        }

        MediaDecoderInfo info = {};
        if (!format_ready) {
            if (!MEDIA_DECODER_GetInfo(decoder, &info)) {
                ESP_LOGE(TAG, "no format info from decoder");
                break;
            }
            if (info.bits_per_sample != 16u || (info.channels != 1u && info.channels != 2u)) {
                ESP_LOGE(TAG, "unsupported PCM: %ubit %uch (16-bit 1/2ch only for now)",
                         static_cast<unsigned>(info.bits_per_sample),
                         static_cast<unsigned>(info.channels));
                break;
            }
            if (to_local) {
                if (!ES8389_HifiAcquire(info.sample_rate_hz, 16u, 2u)) {
                    ESP_LOGE(TAG, "hi-fi speaker unavailable (wrong board, or voice busy)");
                    break;
                }
                hifi_acquired = true;
            }
            if (to_net) {
                if (!VOICE_RESAMPLER_Init(&resampler, info.sample_rate_hz, info.channels)) {
                    ESP_LOGE(TAG, "resampler init failed (%luHz)",
                             static_cast<unsigned long>(info.sample_rate_hz));
                    break;
                }
                net_start_us = esp_timer_get_time();
            }
            format_ready = true;
            ESP_LOGI(TAG, "playing %s: %luHz %ubit %uch target=%s",
                     s_current_path,
                     static_cast<unsigned long>(info.sample_rate_hz),
                     static_cast<unsigned>(info.bits_per_sample),
                     static_cast<unsigned>(info.channels),
                     (target == MUSIC_TARGET_BOTH) ? "both"
                                                   : (to_net ? "net" : "local"));
        }

        (void)MEDIA_DECODER_GetInfo(decoder, &info);

        if (to_net) {
            const size_t in_frames = bytes / (sizeof(int16_t) * info.channels);
            const size_t out_max = (in_frames * 8000u) / info.sample_rate_hz + 8u;
            if (!ensure_net_capacity(out_max)) {
                ESP_LOGE(TAG, "net buffer alloc failed");
                break;
            }
            const size_t out_n = VOICE_RESAMPLER_Process(
                &resampler, reinterpret_cast<const int16_t *>(pcm), in_frames,
                s_net_buffer, s_net_capacity);
            if (out_n > 0u) {
                NRLAudioBridge_SendMediaUplink(s_net_buffer, out_n);
                net_sent_samples += out_n;
            }
            if (!to_local) {
                // Throttle to real time: stay ~100 ms ahead of the 8 kHz clock.
                const int64_t audio_us = static_cast<int64_t>(net_sent_samples) * 1000000LL / 8000LL;
                const int64_t elapsed_us = esp_timer_get_time() - net_start_us;
                const int64_t lead_us = audio_us - elapsed_us - 100000LL;
                if (lead_us > 20000LL) {
                    vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(lead_us / 1000LL)));
                }
            }
        }

        if (to_local) {
            if (info.channels == 2u) {
                if (!ES8389_HifiWrite(pcm, bytes)) {
                    break;
                }
            } else {
                const size_t samples = bytes / sizeof(int16_t);
                if (!ensure_stereo_capacity(samples * 2u * sizeof(int16_t))) {
                    ESP_LOGE(TAG, "stereo buffer alloc failed");
                    break;
                }
                const int16_t *mono = reinterpret_cast<const int16_t *>(pcm);
                for (size_t i = 0; i < samples; ++i) {
                    s_stereo_buffer[i * 2u] = mono[i];
                    s_stereo_buffer[i * 2u + 1u] = mono[i];
                }
                if (!ES8389_HifiWrite(s_stereo_buffer, samples * 2u * sizeof(int16_t))) {
                    break;
                }
            }
        }
    }

    ESP_LOGI(TAG, "%s: %s", s_current_path, s_stop_requested ? "stopped" : "finished");

    if (to_net) {
        NRLAudioBridge_SetMediaUplinkActive(false);
    }
    if (decoder != nullptr) {
        MEDIA_DECODER_Close(decoder);
    }
    if (hifi_acquired) {
        ES8389_HifiRelease();
    }

    s_playing = false;
    s_player_task = nullptr;

    // Auto-advance hook: the codec is released and s_player_task cleared, so
    // the callback may call MUSIC_PlayFile (it spawns a fresh task) while
    // this one is about to delete itself.
    const MusicTrackEndCb_t end_cb = s_track_end_cb;
    if (reached_end && !s_stop_requested && end_cb != nullptr) {
        end_cb();
    }

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

    MEDIA_META_Free(&s_track_info);
    memset(&s_track_info, 0, sizeof(s_track_info));

    strncpy(s_current_path, path, sizeof(s_current_path) - 1u);
    s_current_path[sizeof(s_current_path) - 1u] = '\0';
    s_stop_requested = false;
    s_playing = true;

    // Priority below the voice passthrough task (10): file reads, decode and
    // I2S writes are throughput work, not latency-critical. Internal-RAM
    // stack: stdio/FatFS/SDMMC may run with flash cache paused.
    if (xTaskCreatePinnedToCore(player_task, "music_player", 8192, nullptr, 5, &s_player_task, 1) != pdPASS) {
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

extern "C" const MediaTrackInfo *MUSIC_GetTrackInfo(void)
{
    return &s_track_info;
}

extern "C" void MUSIC_SetTrackEndCallback(MusicTrackEndCb_t callback)
{
    s_track_end_cb = callback;
}

extern "C" void MUSIC_SetTarget(const int target)
{
    if (target >= MUSIC_TARGET_LOCAL && target <= MUSIC_TARGET_BOTH) {
        s_target = target;
    }
}

extern "C" int MUSIC_GetTarget(void)
{
    return s_target;
}
