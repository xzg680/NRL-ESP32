#include "services/music_player.h"

#include "audio/audio_focus.h"
#include "audio/voice_resampler.h"
#include "services/config_notify.h"
#include "driver/es8389.h"
#include "lib/nrl_audio_bridge.h"
#include "lib/nrl_bt_hfp.h"
#include "media/media_decoder.h"
#include "services/storage_service.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <math.h>
#include <stdio.h>
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

// Local output device (speaker hi-fi path vs BT headset A2DP).
static volatile int s_output = MUSIC_OUTPUT_SPEAKER;

// 44.1 kHz stereo scratch + linear resampler state for the A2DP branch.
static int16_t *s_bt_buffer = nullptr;
static size_t s_bt_capacity = 0; // in interleaved samples

struct BtResampler {
    float pos;
    float step;
    uint8_t channels;
    int16_t carry_l;
    int16_t carry_r;
    int has_carry;
};

static void bt_resampler_init(BtResampler *rs, const uint32_t in_rate, const uint8_t channels)
{
    memset(rs, 0, sizeof(*rs));
    rs->step = static_cast<float>(in_rate) / 44100.0f;
    rs->channels = channels;
}

// in: interleaved PCM16 (mono or stereo) frames; out: 44.1 kHz stereo
// interleaved. Same floorf/carry scheme as voice_resampler so chunk
// boundaries stay continuous.
static size_t bt_resampler_process(BtResampler *rs, const int16_t *in, const size_t in_frames,
                                   int16_t *out, const size_t out_cap_frames)
{
    if (in_frames == 0u) {
        return 0;
    }
    const uint8_t ch = rs->channels;
    size_t produced = 0;
    float pos = rs->pos;
    while (produced < out_cap_frames) {
        const float fbase = floorf(pos);
        const long base = static_cast<long>(fbase);
        if (base + 1 >= static_cast<long>(in_frames)) {
            break;
        }
        const float frac = pos - fbase;
        int16_t l0, r0;
        if (base < 0) {
            l0 = rs->has_carry ? rs->carry_l : in[0];
            r0 = rs->has_carry ? rs->carry_r : ((ch == 2u) ? in[1] : in[0]);
        } else {
            l0 = in[base * ch];
            r0 = (ch == 2u) ? in[base * ch + 1] : l0;
        }
        const size_t nf = (base < 0) ? 0u : static_cast<size_t>(base + 1);
        const int16_t l1 = in[nf * ch];
        const int16_t r1 = (ch == 2u) ? in[nf * ch + 1] : l1;
        out[produced * 2u] = static_cast<int16_t>(l0 + (l1 - l0) * frac);
        out[produced * 2u + 1u] = static_cast<int16_t>(r0 + (r1 - r0) * frac);
        ++produced;
        pos += rs->step;
    }
    rs->pos = pos - static_cast<float>(in_frames);
    rs->carry_l = in[(in_frames - 1u) * ch];
    rs->carry_r = (ch == 2u) ? in[(in_frames - 1u) * ch + 1u] : rs->carry_l;
    rs->has_carry = 1;
    return produced;
}

static bool ensure_bt_capacity(const size_t samples)
{
    if (s_bt_capacity >= samples) {
        return true;
    }
    int16_t *grown = static_cast<int16_t *>(
        heap_caps_realloc(s_bt_buffer, samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (grown == nullptr) {
        return false;
    }
    s_bt_buffer = grown;
    s_bt_capacity = samples;
    return true;
}

// Playback target + net-radio station persist in NVS so the web portal and
// LCD UI survive a reboot (the AT path shares the same setters).
constexpr const char *kMediaNvsNamespace = "media";
static char s_radio_url[kMaxPathLen] = {};

static void media_config_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kMediaNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t target = MUSIC_TARGET_LOCAL;
    if (nvs_get_u8(nvs, "target", &target) == ESP_OK &&
        target <= MUSIC_TARGET_BOTH) {
        s_target = target;
    }
    uint8_t output = MUSIC_OUTPUT_SPEAKER;
    if (nvs_get_u8(nvs, "output", &output) == ESP_OK &&
        output <= MUSIC_OUTPUT_BT) {
        s_output = output;
    }
    size_t len = sizeof(s_radio_url);
    if (nvs_get_str(nvs, "radio_url", s_radio_url, &len) != ESP_OK) {
        s_radio_url[0] = '\0';
    }
    nvs_close(nvs);
}

static void media_config_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kMediaNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "media config persist failed");
        return;
    }
    (void)nvs_set_u8(nvs, "target", static_cast<uint8_t>(s_target));
    (void)nvs_set_u8(nvs, "output", static_cast<uint8_t>(s_output));
    (void)nvs_set_str(nvs, "radio_url", s_radio_url);
    (void)nvs_commit(nvs);
    nvs_close(nvs);
    CONFIG_NOTIFY_Bump();
}

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

static size_t pcm24le_to_pcm32le_stereo(const uint8_t *src, const size_t bytes, int32_t *dst)
{
    if (src == nullptr || dst == nullptr) {
        return 0;
    }
    const size_t samples = bytes / 3u;
    for (size_t i = 0; i < samples; ++i) {
        const uint8_t *p = src + i * 3u;
        int32_t sample = static_cast<int32_t>(p[0]) |
                         (static_cast<int32_t>(p[1]) << 8) |
                         (static_cast<int32_t>(p[2]) << 16);
        if ((sample & 0x00800000) != 0) {
            sample |= static_cast<int32_t>(0xFF000000);
        }
        dst[i] = sample << 8;
    }
    return samples * sizeof(int32_t);
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

// Stream format of the current track for the UI's format line; written by
// the player task when the first frame decodes, cleared when it exits.
static MediaDecoderInfo s_stream_info = {};
static volatile bool s_stream_info_valid = false;

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
    bool bt_active = false;
    bool format_ready = false;
    bool reached_end = false;
    VoiceResampler resampler = {};
    BtResampler bt_resampler = {};
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
            const bool speaker_24bit =
                info.bits_per_sample == 24u && info.channels == 2u &&
                to_local && !to_net;
            if ((info.bits_per_sample != 16u && !speaker_24bit) ||
                (info.channels != 1u && info.channels != 2u)) {
                ESP_LOGE(TAG, "unsupported PCM: %ubit %uch (16-bit 1/2ch, or speaker 24-bit stereo)",
                         static_cast<unsigned>(info.bits_per_sample),
                         static_cast<unsigned>(info.channels));
                break;
            }
            if (to_local) {
                // Preferred local device: BT headset (A2DP) when selected and
                // reachable, otherwise the onboard hi-fi speaker.
                if (s_output == MUSIC_OUTPUT_BT && info.bits_per_sample == 16u &&
                    NRL_BtA2dp_RequestStart()) {
                    for (int i = 0; i < 40 && !NRL_BtA2dp_IsStreaming() && !s_stop_requested; ++i) {
                        vTaskDelay(pdMS_TO_TICKS(100)); // connect + negotiate + start
                    }
                    bt_active = NRL_BtA2dp_IsStreaming();
                    if (bt_active) {
                        bt_resampler_init(&bt_resampler, info.sample_rate_hz, info.channels);
                    } else {
                        ESP_LOGW(TAG, "A2DP not ready, falling back to speaker");
                    }
                }
                if (!bt_active) {
                    const uint8_t speaker_bits = speaker_24bit ? 32u : 16u;
                    if (!ES8389_HifiAcquire(info.sample_rate_hz, speaker_bits, 2u)) {
                        ESP_LOGE(TAG, "hi-fi speaker unavailable (wrong board, or voice busy)");
                        break;
                    }
                    hifi_acquired = true;
                }
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
            s_stream_info = info;
            s_stream_info_valid = true;
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

        if (to_local && bt_active) {
            const size_t in_frames = bytes / (sizeof(int16_t) * info.channels);
            const size_t out_max = static_cast<size_t>(in_frames * 44100ull / info.sample_rate_hz) + 8u;
            if (!ensure_bt_capacity(out_max * 2u)) {
                ESP_LOGE(TAG, "bt buffer alloc failed");
                break;
            }
            const size_t out_frames = bt_resampler_process(
                &bt_resampler, reinterpret_cast<const int16_t *>(pcm), in_frames,
                s_bt_buffer, out_max);
            if (out_frames > 0u &&
                NRL_BtA2dp_Write(s_bt_buffer, out_frames) == 0u && !NRL_BtA2dp_IsStreaming()) {
                ESP_LOGW(TAG, "A2DP stream lost, stopping track");
                break;
            }
        } else if (to_local) {
            if (info.bits_per_sample == 24u && info.channels == 2u) {
                const size_t out_bytes = (bytes / 3u) * sizeof(int32_t);
                if (!ensure_stereo_capacity(out_bytes)) {
                    ESP_LOGE(TAG, "pcm32 buffer alloc failed");
                    break;
                }
                const size_t converted = pcm24le_to_pcm32le_stereo(
                    pcm, bytes, reinterpret_cast<int32_t *>(s_stereo_buffer));
                if (converted == 0u || !ES8389_HifiWrite(s_stereo_buffer, converted)) {
                    break;
                }
                continue;
            }
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
    if (bt_active) {
        // Suspend the media channel so HFP voice (SCO) has the bandwidth;
        // the A2DP connection stays for a quick restart on the next track.
        NRL_BtA2dp_RequestStop();
    }

    s_stream_info_valid = false;
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
    media_config_load();
}

extern "C" bool MUSIC_PlayFile(const char *path)
{
    if (path == nullptr || path[0] == '\0' || strlen(path) >= kMaxPathLen) {
        return false;
    }

    // Radio contention: SMB (network TCP bulk) cannot stream while Bluetooth holds
    // the single shared radio (SMB2 I/O timeouts). Refuse network tracks while BT
    // is on -- covers auto-advance / prev-next / AT, not just the UI. Local
    // SD/USB playback is unaffected. (The UI also hides SMB tracks while BT is on.)
    const char *smb_mp = STORAGE_SmbMountPoint();
    if (smb_mp != nullptr && smb_mp[0] != '\0' &&
        strncmp(path, smb_mp, strlen(smb_mp)) == 0 && NRL_BtHfp_IsEnabled()) {
        ESP_LOGW(TAG, "refusing SMB track while Bluetooth is on (radio contention): %s", path);
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

    snprintf(s_current_path, sizeof(s_current_path), "%s", path);
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
    if (target >= MUSIC_TARGET_LOCAL && target <= MUSIC_TARGET_BOTH &&
        target != s_target) {
        s_target = target;
        media_config_save();
    }
}

extern "C" int MUSIC_GetTarget(void)
{
    return s_target;
}

extern "C" void MUSIC_SetOutput(const int output)
{
    if (output >= MUSIC_OUTPUT_SPEAKER && output <= MUSIC_OUTPUT_BT &&
        output != s_output) {
        s_output = output;
        media_config_save();
    }
}

extern "C" int MUSIC_GetOutput(void)
{
    return s_output;
}

extern "C" bool MUSIC_SetRadioUrl(const char *url)
{
    if (url == nullptr || strlen(url) >= sizeof(s_radio_url)) {
        return false;
    }
    if (url[0] != '\0' &&
        strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }
    if (strcmp(url, s_radio_url) != 0) {
        snprintf(s_radio_url, sizeof(s_radio_url), "%s", url);
        media_config_save();
    }
    return true;
}

extern "C" void MUSIC_GetRadioUrl(char *out, const size_t out_size)
{
    if (out != nullptr && out_size > 0u) {
        snprintf(out, out_size, "%s", s_radio_url);
    }
}

extern "C" bool MUSIC_GetStreamInfo(uint32_t *sample_rate_hz,
                                    uint8_t *bits_per_sample,
                                    uint8_t *channels)
{
    if (!s_stream_info_valid) {
        return false;
    }
    if (sample_rate_hz != nullptr) {
        *sample_rate_hz = s_stream_info.sample_rate_hz;
    }
    if (bits_per_sample != nullptr) {
        *bits_per_sample = s_stream_info.bits_per_sample;
    }
    if (channels != nullptr) {
        *channels = s_stream_info.channels;
    }
    return true;
}
