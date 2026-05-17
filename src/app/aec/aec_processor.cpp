#include "aec/aec_processor.h"

#include "driver/board_pins.h"

#if defined(NRL_ENABLE_GEZIPAI_AEC) && NRL_ENABLE_GEZIPAI_AEC

#include <Arduino.h>
#include <esp_afe_sr_models.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---- Resampler ---------------------------------------------------------
// A windowed-sinc low-pass FIR shared by the 8k->16k upsampler and the
// 16k->8k downsampler. Cutoff 0.25 cycles/sample = 4 kHz at 16 kHz.
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

// Per-channel 8k->16k upsampler: history of recent 8 kHz input samples.
struct Upsampler {
    float hist[kFirHalf + 1];
};

// out must hold 2 * n samples.
void upsample2x(Upsampler &s, const int16_t *in, size_t n, int16_t *out) {
    for (size_t i = 0; i < n; ++i) {
        for (size_t h = kFirHalf; h > 0; --h) {
            s.hist[h] = s.hist[h - 1];
        }
        s.hist[0] = static_cast<float>(in[i]);

        float ye = 0.0f; // even polyphase: taps 0,2,..,30
        float yo = 0.0f; // odd  polyphase: taps 1,3,..,29
        for (size_t t = 0; t <= kFirHalf; ++t) {
            ye += s_fir[2 * t] * s.hist[t];
        }
        for (size_t t = 0; t < kFirHalf; ++t) {
            yo += s_fir[2 * t + 1] * s.hist[t];
        }
        out[2 * i] = clamp16(2.0f * ye);
        out[2 * i + 1] = clamp16(2.0f * yo);
    }
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

int16_t *s_feed_buf = nullptr; // interleaved [mic,ref] @16 kHz
size_t s_feed_cap = 0;         // per-channel capacity
size_t s_feed_fill = 0;        // per-channel samples buffered

Upsampler s_up_mic;
Upsampler s_up_ref;
Downsampler s_down;

int16_t s_out_frame[kOutFrameSamples];
size_t s_out_fill = 0;

AEC_OutputCallback s_out_cb = nullptr;
void *s_out_user = nullptr;

TaskHandle_t s_fetch_task = nullptr;
volatile bool s_running = false;
bool s_ready = false;

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
    }
    s_fetch_task = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

bool AEC_Init(void) {
    if (s_ready) {
        return true;
    }

    fir_init();

    afe_config_t cfg = AFE_CONFIG_DEFAULT();
    cfg.aec_init = true;
    cfg.se_init = true;
    cfg.vad_init = false;
    cfg.wakenet_init = false;
    cfg.voice_communication_init = true;
    cfg.voice_communication_agc_init = true;
    cfg.afe_mode = SR_MODE_HIGH_PERF;
    cfg.memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg.pcm_config.total_ch_num = 2;
    cfg.pcm_config.mic_num = 1;
    cfg.pcm_config.ref_num = 1;
    cfg.pcm_config.sample_rate = 16000;

    s_iface = &ESP_AFE_VC_HANDLE;
    s_afe = s_iface->create_from_config(&cfg);
    if (s_afe == nullptr) {
        Serial.println("[AEC] AFE create_from_config failed");
        return false;
    }

    s_feed_chunk = s_iface->get_feed_chunksize(s_afe);
    s_fetch_chunk = s_iface->get_fetch_chunksize(s_afe);
    if (s_feed_chunk <= 0) {
        Serial.println("[AEC] invalid feed chunk size");
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        return false;
    }

    // Capacity: one feed chunk plus one upsampled I2S frame of headroom.
    s_feed_cap = static_cast<size_t>(s_feed_chunk) + 256u;
    s_feed_buf = static_cast<int16_t *>(
        malloc(s_feed_cap * 2u * sizeof(int16_t)));
    if (s_feed_buf == nullptr) {
        Serial.println("[AEC] feed buffer alloc failed");
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        return false;
    }

    s_feed_fill = 0;
    s_out_fill = 0;
    memset(&s_up_mic, 0, sizeof(s_up_mic));
    memset(&s_up_ref, 0, sizeof(s_up_ref));
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
        return false;
    }

    s_ready = true;
    Serial.printf("[AEC] ready: feed_chunk=%d fetch_chunk=%d (16kHz), "
                  "8k<->16k resampling active\n",
                  s_feed_chunk, s_fetch_chunk);
    return true;
}

bool AEC_IsReady(void) {
    return s_ready;
}

void AEC_SetOutputCallback(AEC_OutputCallback callback, void *user_data) {
    s_out_cb = callback;
    s_out_user = user_data;
}

void AEC_SubmitCapture(const int16_t *mic_8k,
                       const int16_t *ref_8k,
                       size_t sample_count) {
    if (!s_ready || mic_8k == nullptr || ref_8k == nullptr ||
        sample_count == 0) {
        return;
    }

    // Upsample this 8 kHz frame to 16 kHz for both channels.
    static int16_t mic16[256];
    static int16_t ref16[256];
    if (sample_count * 2u > sizeof(mic16) / sizeof(mic16[0])) {
        return; // frame larger than expected
    }
    upsample2x(s_up_mic, mic_8k, sample_count, mic16);
    upsample2x(s_up_ref, ref_8k, sample_count, ref16);
    const size_t n16 = sample_count * 2u;

    if (s_feed_fill + n16 > s_feed_cap) {
        // Overflow guard: drop to resync rather than corrupt memory.
        s_feed_fill = 0;
        return;
    }

    // Append channel-interleaved: [mic, ref] (reference is the last channel).
    for (size_t i = 0; i < n16; ++i) {
        s_feed_buf[(s_feed_fill + i) * 2u] = mic16[i];
        s_feed_buf[(s_feed_fill + i) * 2u + 1u] = ref16[i];
    }
    s_feed_fill += n16;

    // Feed full chunks to the AFE.
    while (s_feed_fill >= static_cast<size_t>(s_feed_chunk)) {
        s_iface->feed(s_afe, s_feed_buf);
        const size_t remain = s_feed_fill - static_cast<size_t>(s_feed_chunk);
        if (remain > 0) {
            memmove(s_feed_buf,
                    s_feed_buf + static_cast<size_t>(s_feed_chunk) * 2u,
                    remain * 2u * sizeof(int16_t));
        }
        s_feed_fill = remain;
    }
}

#else // !NRL_ENABLE_GEZIPAI_AEC

bool AEC_Init(void) { return false; }
bool AEC_IsReady(void) { return false; }
void AEC_SetOutputCallback(AEC_OutputCallback, void *) {}
void AEC_SubmitCapture(const int16_t *, const int16_t *, size_t) {}

#endif // NRL_ENABLE_GEZIPAI_AEC
