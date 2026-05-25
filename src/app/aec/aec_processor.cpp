#include "aec/aec_processor.h"

#include "driver/board_pins.h"

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE

#include <Arduino.h>
#include <esp_afe_config.h>
#include <esp_afe_sr_models.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <model_path.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---- Resampler ---------------------------------------------------------
// Windowed-sinc low-pass FIR for the 16k->8k downsampler.
// Cutoff 0.25 cycles/sample = 4 kHz at 16 kHz.
constexpr size_t kFirTaps = 31;
constexpr size_t kFirHalf = kFirTaps / 2; // 15
float s_fir[kFirTaps];

void fir_init(void) {
    double sum = 0.0;
    const double fc = 0.25;
    for (size_t i = 0; i < kFirTaps; ++i) {
        const int k = static_cast<int>(i) - static_cast<int>(kFirHalf);
        const double sinc = (k == 0)
            ? 2.0 * fc
            : sin(2.0 * M_PI * fc * k) / (M_PI * k);
        const double win = 0.54 - 0.46 * cos(2.0 * M_PI * i / (kFirTaps - 1));
        s_fir[i] = static_cast<float>(sinc * win);
        sum += s_fir[i];
    }
    for (size_t i = 0; i < kFirTaps; ++i) {
        s_fir[i] = static_cast<float>(s_fir[i] / sum); // unity DC gain
    }
}

inline int16_t clamp16(float v) {
    long r = lroundf(v);
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    return static_cast<int16_t>(r);
}

// 16k->8k downsampler: history of recent 16 kHz input samples.
struct Downsampler {
    float hist[kFirTaps];
    bool phase;
};

// out must hold n / 2 samples; returns the number produced.
size_t downsample2x(Downsampler &s, const int16_t *in, size_t n, int16_t *out) {
    size_t produced = 0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t h = kFirTaps - 1; h > 0; --h) {
            s.hist[h] = s.hist[h - 1];
        }
        s.hist[0] = static_cast<float>(in[i]);
        s.phase = !s.phase;
        if (!s.phase) { // emit one output per two inputs
            float y = 0.0f;
            for (size_t t = 0; t < kFirTaps; ++t) {
                y += s_fir[t] * s.hist[t];
            }
            out[produced++] = clamp16(y);
        }
    }
    return produced;
}

// ---- AFE state ---------------------------------------------------------
constexpr size_t kOutFrameSamples = 80; // 8 kHz NRL uplink frame

const esp_afe_sr_iface_t *s_iface = nullptr;
esp_afe_sr_data_t *s_afe = nullptr;
int s_feed_chunk = 0;  // per-channel samples per feed (16 kHz)
int s_fetch_chunk = 0; // samples per fetch (16 kHz)

int16_t *s_feed_buf = nullptr; // interleaved channels @16 kHz
size_t s_feed_cap = 0;         // per-channel capacity
size_t s_feed_fill = 0;        // per-channel samples buffered
size_t s_feed_channels = 1;    // 1=mic only, 2=mic+ref

Downsampler s_down;

int16_t s_out_frame[kOutFrameSamples];
size_t s_out_fill = 0;

AEC_OutputCallback s_out_cb = nullptr;
void *s_out_user = nullptr;

TaskHandle_t s_fetch_task = nullptr;
volatile bool s_running = false;
bool s_ready = false;
bool s_has_reference = false;
bool s_has_ai_noise = false;
volatile bool s_runtime_aec_enabled = false;
volatile bool s_runtime_ai_noise_enabled = false;
srmodel_list_t *s_models = nullptr;

const char *input_format_for(const bool use_ref_channel) {
    return use_ref_channel ? "MR" : "M";
}

void aec_fetch_task(void *) {
    static int16_t down8[1024];
    while (s_running) {
        afe_fetch_result_t *res = s_iface->fetch(s_afe);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        const size_t n16 = static_cast<size_t>(res->data_size) / sizeof(int16_t);
        size_t off = 0;
        while (off < n16) {
            const size_t block = (n16 - off > 2048u) ? 2048u : (n16 - off);
            const size_t n8 = downsample2x(s_down, res->data + off, block, down8);
            for (size_t i = 0; i < n8; ++i) {
                s_out_frame[s_out_fill++] = down8[i];
                if (s_out_fill == kOutFrameSamples) {
                    if (s_out_cb != nullptr) {
                        s_out_cb(s_out_frame, kOutFrameSamples, s_out_user);
                    }
                    s_out_fill = 0;
                }
            }
            off += block;
        }
        vTaskDelay(1);
    }
    s_fetch_task = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

bool AEC_Init(const bool enable_aec, const bool enable_ai_noise) {
    if (s_ready) {
        return true;
    }
    if (!enable_aec && !enable_ai_noise) {
        return false;
    }

    fir_init();

    const bool use_ref_channel = enable_aec;
    s_models = esp_srmodel_init("model");
    if (s_models == nullptr) {
        Serial.println("[AEC] esp_srmodel_init('model') failed -- NSNET2 model partition missing?");
        return false;
    }

    afe_config_t *cfg = afe_config_init(input_format_for(use_ref_channel),
                                        s_models,
                                        AFE_TYPE_VC,
                                        AFE_MODE_LOW_COST);
    if (cfg == nullptr) {
        Serial.println("[AEC] afe_config_init failed");
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    cfg->aec_init = use_ref_channel;
    cfg->se_init = false;
    cfg->vad_init = false;
    cfg->wakenet_init = false;
    cfg->agc_init = false;
    cfg->afe_ns_mode = AFE_NS_MODE_NET;

    s_iface = esp_afe_handle_from_config(cfg);
    if (s_iface == nullptr) {
        Serial.println("[AEC] esp_afe_handle_from_config failed");
        afe_config_free(cfg);
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_afe = s_iface->create_from_config(cfg);
    afe_config_free(cfg);
    if (s_afe == nullptr) {
        Serial.println("[AEC] AFE create_from_config failed");
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_feed_chunk = s_iface->get_feed_chunksize(s_afe);
    s_fetch_chunk = s_iface->get_fetch_chunksize(s_afe);
    s_feed_channels = static_cast<size_t>(s_iface->get_feed_channel_num(s_afe));
    if (s_feed_channels == 0u) {
        s_feed_channels = use_ref_channel ? 2u : 1u;
    }
    if (s_feed_chunk <= 0) {
        Serial.println("[AEC] invalid feed chunk size");
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    // Capacity: one feed chunk plus one 16 kHz I2S frame of headroom.
    s_feed_cap = static_cast<size_t>(s_feed_chunk) + 256u;
    s_feed_buf = static_cast<int16_t *>(
        malloc(s_feed_cap * s_feed_channels * sizeof(int16_t)));
    if (s_feed_buf == nullptr) {
        Serial.println("[AEC] feed buffer alloc failed");
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_feed_fill = 0;
    s_out_fill = 0;
    memset(&s_down, 0, sizeof(s_down));

    s_running = true;
    // 8 KB: aec_fetch_task calls into esp-sr fetch().
    if (xTaskCreatePinnedToCore(aec_fetch_task, "aec_fetch", 8192, nullptr, 4,
                                &s_fetch_task, 1) != pdPASS) {
        Serial.println("[AEC] fetch task create failed");
        s_running = false;
        free(s_feed_buf);
        s_feed_buf = nullptr;
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_ready = true;
    s_has_reference = enable_aec;
    s_has_ai_noise = enable_ai_noise;
    AEC_SetRuntimeEnabled(enable_aec, enable_ai_noise);
    Serial.printf("[AEC] ready: feed_chunk=%d fetch_chunk=%d (16kHz), "
                  "capture=16k output=8k, ch=%u aec=%u ai_noise=%u\n",
                  s_feed_chunk,
                  s_fetch_chunk,
                  static_cast<unsigned>(s_feed_channels),
                  s_has_reference ? 1u : 0u,
                  s_has_ai_noise ? 1u : 0u);
    return true;
}

bool AEC_IsReady(void) {
    return s_ready;
}

void AEC_SetRuntimeEnabled(const bool enable_aec, const bool enable_ai_noise) {
    s_runtime_aec_enabled = s_has_reference && enable_aec;
    s_runtime_ai_noise_enabled = s_has_ai_noise && enable_ai_noise;
}

bool AEC_UsesReference(void) {
    return s_ready && s_has_reference;
}

bool AEC_IsRuntimeActive(void) {
    return s_ready && (s_runtime_aec_enabled || s_runtime_ai_noise_enabled);
}

void AEC_SetOutputCallback(AEC_OutputCallback callback, void *user_data) {
    s_out_cb = callback;
    s_out_user = user_data;
}

void AEC_SubmitCapture(const int16_t *mic_16k,
                       const int16_t *ref_16k,
                       size_t sample_count) {
    if (!s_ready || mic_16k == nullptr || sample_count == 0) {
        return;
    }
    if (s_feed_channels > 1u && ref_16k == nullptr) {
        return;
    }

    if (s_feed_fill + sample_count > s_feed_cap) {
        // Overflow guard: drop to resync rather than corrupt memory.
        s_feed_fill = 0;
        return;
    }

    // Append channel-interleaved: [mic] or [mic, ref].
    for (size_t i = 0; i < sample_count; ++i) {
        s_feed_buf[(s_feed_fill + i) * s_feed_channels] = mic_16k[i];
        if (s_feed_channels > 1u) {
            s_feed_buf[(s_feed_fill + i) * s_feed_channels + 1u] = ref_16k[i];
        }
    }
    s_feed_fill += sample_count;

    // Feed full chunks to the AFE.
    while (s_feed_fill >= static_cast<size_t>(s_feed_chunk)) {
        s_iface->feed(s_afe, s_feed_buf);
        vTaskDelay(1);
        const size_t remain = s_feed_fill - static_cast<size_t>(s_feed_chunk);
        if (remain > 0) {
            memmove(s_feed_buf,
                    s_feed_buf + static_cast<size_t>(s_feed_chunk) * s_feed_channels,
                    remain * s_feed_channels * sizeof(int16_t));
        }
        s_feed_fill = remain;
    }
}

#else // !NRL_ENABLE_AUDIO_AFE

bool AEC_Init(bool, bool) { return false; }
bool AEC_IsReady(void) { return false; }
void AEC_SetRuntimeEnabled(bool, bool) {}
bool AEC_UsesReference(void) { return false; }
bool AEC_IsRuntimeActive(void) { return false; }
void AEC_SetOutputCallback(AEC_OutputCallback, void *) {}
void AEC_SubmitCapture(const int16_t *, const int16_t *, size_t) {}

#endif // NRL_ENABLE_AUDIO_AFE
