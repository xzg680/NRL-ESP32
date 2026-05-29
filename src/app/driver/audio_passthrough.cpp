#include "driver/audio_passthrough.h"

#include "driver/board_pins.h"

#include <driver/i2s_common.h>
#include <driver/i2s_std.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
#include "aec/aec_processor.h"
#include "driver/external_radio.h"
#endif

static const char *TAG = "AUDIO";

#ifndef AUDIO_FORCE_DAC_SILENCE
#define AUDIO_FORCE_DAC_SILENCE 0
#endif

#ifndef AUDIO_ENABLE_MIC_DEBUG_LOG
#define AUDIO_ENABLE_MIC_DEBUG_LOG 0
#endif

namespace {

constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr int kSampleRate = 16000;
constexpr int kMclkRate = kSampleRate * 256;
constexpr size_t kFrameSamples = 160;
constexpr size_t kNetworkFrameSamples = kFrameSamples / 2u;
// I2S bus stays stereo (2 slots/frame) so BCLK timing matches the codec's
// clock divider expectations. The mic ADC drives the LEFT slot only; we
// duplicate mono content into both slots in i2s_write_frame.
constexpr size_t kI2sSlotCount = 2;
constexpr size_t kFrameBytes = kFrameSamples * sizeof(int16_t);
constexpr size_t kI2sFrameBytes = kFrameSamples * kI2sSlotCount * sizeof(int16_t);
constexpr uint32_t kI2sWaitMs = 20;

constexpr int kPinEspDout = NRL_PIN_I2S_DOUT;
constexpr int kPinEspDin = NRL_PIN_I2S_DIN;
constexpr int kPinBclk = NRL_PIN_I2S_BCLK;
constexpr int kPinLrclk = NRL_PIN_I2S_LRCLK;
constexpr int kPinMclk = NRL_PIN_I2S_MCLK;

constexpr float kToneFrequency = 440.0f;
constexpr float kToneAmplitude = 0.40f;
constexpr float kTwoPi = 6.283185307179586f;

static bool s_i2s_ready = false;
static bool s_i2s_driver_installed = false;
static i2s_chan_handle_t s_i2s_tx = nullptr;
static i2s_chan_handle_t s_i2s_rx = nullptr;
static TaskHandle_t s_passthrough_task = nullptr;
static volatile bool s_passthrough_running = false;
static AUDIO_Mode_t s_audio_mode = AUDIO_MODE_RECEIVE;
static AUDIO_FrameHook_t s_frame_hook = nullptr;
static void *s_frame_hook_user_data = nullptr;

constexpr size_t kOutputQueueSamples = kNetworkFrameSamples * 64u;
static int16_t s_output_queue[kOutputQueueSamples];
static size_t s_output_queue_head = 0;
static size_t s_output_queue_tail = 0;
static size_t s_output_queue_count = 0;
static SemaphoreHandle_t s_output_queue_mutex = nullptr;
static uint32_t s_last_output_queue_log_ms = 0;
static volatile uint8_t s_aec_reference_source = 0; // 0=network playback, 1=second mic
static constexpr size_t kAecNetworkRefDelayFrames = 12; // ~120 ms at 160 samples/frame
static int16_t s_aec_network_ref[kFrameSamples * kAecNetworkRefDelayFrames];
static size_t s_aec_network_ref_head = 0;
static size_t s_aec_network_ref_fill = 0;

// Software 4th-order IIR high-pass filter (two RBJ biquads, Direct Form I) on
// captured mic frames:
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
// Coefficients are pre-computed for a 4th-order Butterworth response, cutoff
// fc = 200 Hz, sample rate fs = 16000 Hz.
constexpr float kMicHpf1B0 =  0.93097528f;
constexpr float kMicHpf1B1 = -1.86195056f;
constexpr float kMicHpf1B2 =  0.93097528f;
constexpr float kMicHpf1A1 = -1.85907624f;
constexpr float kMicHpf1A2 =  0.86482488f;
constexpr float kMicHpf2B0 =  0.96935382f;
constexpr float kMicHpf2B1 = -1.93870765f;
constexpr float kMicHpf2B2 =  0.96935382f;
constexpr float kMicHpf2A1 = -1.93571484f;
constexpr float kMicHpf2A2 =  0.94170045f;
static volatile bool s_mic_hpf_enabled = false;
static float s_mic_hpf1_x1 = 0.0f;
static float s_mic_hpf1_x2 = 0.0f;
static float s_mic_hpf1_y1 = 0.0f;
static float s_mic_hpf1_y2 = 0.0f;
static float s_mic_hpf2_x1 = 0.0f;
static float s_mic_hpf2_x2 = 0.0f;
static float s_mic_hpf2_y1 = 0.0f;
static float s_mic_hpf2_y2 = 0.0f;

static inline void mic_hpf_reset(void) {
    s_mic_hpf1_x1 = 0.0f;
    s_mic_hpf1_x2 = 0.0f;
    s_mic_hpf1_y1 = 0.0f;
    s_mic_hpf1_y2 = 0.0f;
    s_mic_hpf2_x1 = 0.0f;
    s_mic_hpf2_x2 = 0.0f;
    s_mic_hpf2_y1 = 0.0f;
    s_mic_hpf2_y2 = 0.0f;
}

static inline void mic_hpf_apply(int16_t *frame, const size_t count) {
    if (!s_mic_hpf_enabled || frame == nullptr) {
        return;
    }
    float x11 = s_mic_hpf1_x1;
    float x12 = s_mic_hpf1_x2;
    float y11 = s_mic_hpf1_y1;
    float y12 = s_mic_hpf1_y2;
    float x21 = s_mic_hpf2_x1;
    float x22 = s_mic_hpf2_x2;
    float y21 = s_mic_hpf2_y1;
    float y22 = s_mic_hpf2_y2;
    for (size_t i = 0; i < count; ++i) {
        const float x = static_cast<float>(frame[i]);
        const float y_stage1 = kMicHpf1B0 * x + kMicHpf1B1 * x11 + kMicHpf1B2 * x12
                               - kMicHpf1A1 * y11 - kMicHpf1A2 * y12;
        x12 = x11;
        x11 = x;
        y12 = y11;
        y11 = y_stage1;

        const float y = kMicHpf2B0 * y_stage1 + kMicHpf2B1 * x21 + kMicHpf2B2 * x22
                        - kMicHpf2A1 * y21 - kMicHpf2A2 * y22;
        x22 = x21;
        x21 = y_stage1;
        y22 = y21;
        y21 = y;
        int32_t out = static_cast<int32_t>(y);
        if (out > INT16_MAX) { out = INT16_MAX; }
        else if (out < INT16_MIN) { out = INT16_MIN; }
        frame[i] = static_cast<int16_t>(out);
    }
    s_mic_hpf1_x1 = x11;
    s_mic_hpf1_x2 = x12;
    s_mic_hpf1_y1 = y11;
    s_mic_hpf1_y2 = y12;
    s_mic_hpf2_x1 = x21;
    s_mic_hpf2_x2 = x22;
    s_mic_hpf2_y1 = y21;
    s_mic_hpf2_y2 = y22;
}

static void i2s_teardown(void) {
    if (s_i2s_tx != nullptr) {
        (void)i2s_channel_disable(s_i2s_tx);
        (void)i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
    }

    if (s_i2s_rx != nullptr) {
        (void)i2s_channel_disable(s_i2s_rx);
        (void)i2s_del_channel(s_i2s_rx);
        s_i2s_rx = nullptr;
    }

    s_i2s_ready = false;
    s_i2s_driver_installed = false;
}

static void i2s_clear_dma(void) {
    if (s_i2s_tx == nullptr) {
        return;
    }

    int16_t silence[kFrameSamples * kI2sSlotCount] = {};
    for (int i = 0; i < 2; ++i) {
        size_t bytes_written = 0;
        (void)i2s_channel_write(s_i2s_tx, silence, sizeof(silence), &bytes_written, kI2sWaitMs);
    }
}

static bool i2s_setup(void) {
    if (s_i2s_ready) {
        return true;
    }

    if (s_i2s_driver_installed || s_i2s_tx != nullptr || s_i2s_rx != nullptr) {
        i2s_teardown();
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(kI2sPort, I2S_ROLE_MASTER);
    // Keep only a few 10 ms DMA frames queued. Larger rings hide scheduling
    // hiccups by building capture latency that can keep growing under load.
    channel_config.dma_desc_num = 3;
    channel_config.dma_frame_num = kFrameSamples;
    channel_config.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&channel_config, &s_i2s_tx, &s_i2s_rx);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "i2s_new_channel failed: err=%d", static_cast<int>(err));
        i2s_teardown();
        return false;
    }
    s_i2s_driver_installed = true;

    i2s_std_config_t std_config = {};
    std_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_config.gpio_cfg.mclk = static_cast<gpio_num_t>(kPinMclk);
    std_config.gpio_cfg.bclk = static_cast<gpio_num_t>(kPinBclk);
    std_config.gpio_cfg.ws = static_cast<gpio_num_t>(kPinLrclk);
    std_config.gpio_cfg.dout = static_cast<gpio_num_t>(kPinEspDout);
    std_config.gpio_cfg.din = static_cast<gpio_num_t>(kPinEspDin);
    std_config.gpio_cfg.invert_flags.mclk_inv = false;
    std_config.gpio_cfg.invert_flags.bclk_inv = false;
    std_config.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(s_i2s_tx, &std_config);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "i2s tx std init failed: err=%d", static_cast<int>(err));
        i2s_teardown();
        return false;
    }

    err = i2s_channel_init_std_mode(s_i2s_rx, &std_config);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "i2s rx std init failed: err=%d", static_cast<int>(err));
        i2s_teardown();
        return false;
    }

    err = i2s_channel_enable(s_i2s_tx);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "i2s tx enable failed: err=%d", static_cast<int>(err));
        i2s_teardown();
        return false;
    }

    err = i2s_channel_enable(s_i2s_rx);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "i2s rx enable failed: err=%d", static_cast<int>(err));
        i2s_teardown();
        return false;
    }

    s_i2s_ready = true;
    i2s_clear_dma();
    ESP_LOGI(TAG, "i2s std clocks: rate=%dHz bits=%d stereo mclk=%dHz",
             kSampleRate, 16, kMclkRate);
    return true;
}

static bool i2s_read_frame(int16_t *dst, int16_t *dst_ref = nullptr) {
    if (s_i2s_rx == nullptr) {
        return false;
    }

    static_assert(kI2sSlotCount == 2, "i2s_read_frame assumes stereo I2S frame");
    int16_t raw[kFrameSamples * kI2sSlotCount];
    size_t bytes_in_frame = 0;
    while (bytes_in_frame < kI2sFrameBytes) {
        size_t bytes_read = 0;
        if (i2s_channel_read(s_i2s_rx,
                             reinterpret_cast<uint8_t *>(raw) + bytes_in_frame,
                             kI2sFrameBytes - bytes_in_frame,
                             &bytes_read,
                             kI2sWaitMs) != ESP_OK) {
            return false;
        }

        if (bytes_read == 0) {
            vTaskDelay(1);
            continue;
        }

        bytes_in_frame += bytes_read;
    }

#if AUDIO_ENABLE_MIC_DEBUG_LOG
    {
        static uint32_t last_dump_ms = 0;
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if ((now - last_dump_ms) >= 5000u) {
            last_dump_ms = now;
            int32_t left_peak = 0;
            int32_t right_peak = 0;
            for (size_t i = 0; i < kFrameSamples; ++i) {
                const int32_t l = abs(static_cast<int32_t>(raw[i * 2]));
                const int32_t r = abs(static_cast<int32_t>(raw[i * 2 + 1]));
                if (l > left_peak) { left_peak = l; }
                if (r > right_peak) { right_peak = r; }
            }
            ESP_LOGI(TAG, "raw I2S slots: LEFT peak=%ld RIGHT peak=%ld | raw[0..7]=%d,%d %d,%d %d,%d %d,%d",
                     static_cast<long>(left_peak), static_cast<long>(right_peak),
                     raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
        }
    }
#endif

    // Take LEFT slot (mic). When dst_ref is given, also take the RIGHT slot,
    // which is only used as a board-specific AEC reference.
    //
    // On 格子派 (gezipai) the RIGHT slot is the ES7210's second ADC channel,
    // which is not wired to a meaningful AEC reference (no second mic, no
    // speaker tap). Feeding it to the AFE causes AEC to subtract unrelated
    // audio from the mic and squash the voice. Force it to silence at the
    // driver level so the MIC-source AEC reference path is harmless even if
    // the user happens to pick it.
    for (size_t i = 0; i < kFrameSamples; ++i) {
        dst[i] = raw[i * 2];
        if (dst_ref != nullptr) {
#if NRL_BOARD == NRL_BOARD_GEZIPAI
            dst_ref[i] = 0;
#else
            dst_ref[i] = raw[i * 2 + 1];
#endif
        }
    }
    return true;
}

static bool i2s_write_frame(const int16_t *src) {
    if (s_i2s_tx == nullptr) {
        return false;
    }

    static_assert(kI2sSlotCount == 2, "i2s_write_frame assumes stereo I2S frame");
    int16_t raw[kFrameSamples * kI2sSlotCount];
#if AUDIO_FORCE_DAC_SILENCE
    (void)src;
    memset(raw, 0, sizeof(raw));
#else
    for (size_t i = 0; i < kFrameSamples; ++i) {
        raw[i * 2]     = src[i];
        raw[i * 2 + 1] = src[i];
    }
#endif

    size_t bytes_out_frame = 0;
    while (bytes_out_frame < kI2sFrameBytes) {
        size_t bytes_written = 0;
        if (i2s_channel_write(s_i2s_tx,
                              reinterpret_cast<const uint8_t *>(raw) + bytes_out_frame,
                              kI2sFrameBytes - bytes_out_frame,
                              &bytes_written,
                              kI2sWaitMs) != ESP_OK) {
            return false;
        }

        if (bytes_written == 0) {
            vTaskDelay(1);
            continue;
        }

        bytes_out_frame += bytes_written;
    }
    return true;
}

static void output_queue_init(void) {
    if (s_output_queue_mutex == nullptr) {
        s_output_queue_mutex = xSemaphoreCreateMutex();
    }
}

static void output_queue_clear_locked(void) {
    s_output_queue_head = 0;
    s_output_queue_tail = 0;
    s_output_queue_count = 0;
}

static void aec_network_ref_clear(void) {
    memset(s_aec_network_ref, 0, sizeof(s_aec_network_ref));
    s_aec_network_ref_head = 0;
    s_aec_network_ref_fill = 0;
}

static void aec_network_ref_read(int16_t *dst, const size_t sample_count) {
    if (dst == nullptr || sample_count == 0) {
        return;
    }
    if (sample_count != kFrameSamples ||
        s_aec_network_ref_fill < kAecNetworkRefDelayFrames) {
        memset(dst, 0, sample_count * sizeof(int16_t));
        return;
    }
    memcpy(dst,
           s_aec_network_ref + (s_aec_network_ref_head * kFrameSamples),
           kFrameSamples * sizeof(int16_t));
}

static void aec_network_ref_push(const int16_t *src, const size_t sample_count) {
    if (src == nullptr || sample_count != kFrameSamples) {
        return;
    }
    memcpy(s_aec_network_ref + (s_aec_network_ref_head * kFrameSamples),
           src,
           kFrameSamples * sizeof(int16_t));
    s_aec_network_ref_head = (s_aec_network_ref_head + 1u) % kAecNetworkRefDelayFrames;
    if (s_aec_network_ref_fill < kAecNetworkRefDelayFrames) {
        ++s_aec_network_ref_fill;
    }
}

static void upsample_8k_to_16k_frame(const int16_t *src8, int16_t *dst16) {
    if (src8 == nullptr || dst16 == nullptr) {
        return;
    }
    for (size_t i = 0; i < kNetworkFrameSamples; ++i) {
        const int16_t current = src8[i];
        const int16_t next = (i + 1u < kNetworkFrameSamples) ? src8[i + 1u] : current;
        dst16[i * 2u] = current;
        dst16[i * 2u + 1u] = static_cast<int16_t>(
            (static_cast<int32_t>(current) + static_cast<int32_t>(next)) / 2);
    }
}

static size_t output_queue_push(const int16_t *samples, size_t sample_count) {
    if (samples == nullptr || sample_count == 0) {
        return 0;
    }

    output_queue_init();
    if (s_output_queue_mutex == nullptr) {
        return 0;
    }

    if (xSemaphoreTake(s_output_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }

    size_t written = 0;
    while (written < sample_count && s_output_queue_count < kOutputQueueSamples) {
        s_output_queue[s_output_queue_tail] = samples[written++];
        s_output_queue_tail = (s_output_queue_tail + 1u) % kOutputQueueSamples;
        ++s_output_queue_count;
    }

    xSemaphoreGive(s_output_queue_mutex);
    return written;
}

static size_t output_queue_pop_frame(int16_t *dst, const size_t sample_count) {
    if (dst == nullptr || sample_count == 0) {
        return 0;
    }

    output_queue_init();
    if (s_output_queue_mutex == nullptr) {
        memset(dst, 0, sample_count * sizeof(int16_t));
        return 0;
    }

    if (xSemaphoreTake(s_output_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        memset(dst, 0, sample_count * sizeof(int16_t));
        return 0;
    }

    size_t read = 0;
    while (read < sample_count && s_output_queue_count > 0) {
        dst[read++] = s_output_queue[s_output_queue_head];
        s_output_queue_head = (s_output_queue_head + 1u) % kOutputQueueSamples;
        --s_output_queue_count;
    }

    xSemaphoreGive(s_output_queue_mutex);

    if (read < sample_count) {
        memset(dst + read, 0, (sample_count - read) * sizeof(int16_t));
    }

    return read;
}

static void output_queue_clear(void) {
    output_queue_init();
    if (s_output_queue_mutex == nullptr) {
        return;
    }

    if (xSemaphoreTake(s_output_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }
    output_queue_clear_locked();
    aec_network_ref_clear();
    xSemaphoreGive(s_output_queue_mutex);
}

static void audio_log_mic_frame_stats(const int16_t *frame) {
#if AUDIO_ENABLE_MIC_DEBUG_LOG
    static uint32_t window_start_ms = 0;
    static uint32_t frame_count = 0;
    static int16_t window_peak = 0;
    static int16_t window_min = 0;
    static int16_t window_max = 0;
    static uint64_t window_sum_sq = 0;
    static uint32_t window_samples = 0;
    static uint32_t window_nonzero = 0;

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (window_start_ms == 0) {
        window_start_ms = now;
        window_min = frame[0];
        window_max = frame[0];
    }

    for (size_t i = 0; i < kFrameSamples; ++i) {
        const int16_t s = frame[i];
        const int32_t mag = (s < 0) ? -static_cast<int32_t>(s) : static_cast<int32_t>(s);
        if (mag > window_peak) {
            window_peak = static_cast<int16_t>(mag);
        }
        if (s < window_min) { window_min = s; }
        if (s > window_max) { window_max = s; }
        if (s != 0) { ++window_nonzero; }
        window_sum_sq += static_cast<uint64_t>(static_cast<int32_t>(s) * static_cast<int32_t>(s));
        ++window_samples;
    }
    ++frame_count;

    if ((now - window_start_ms) >= 1000u && window_samples > 0) {
        const uint32_t rms = static_cast<uint32_t>(
            sqrt(static_cast<double>(window_sum_sq) / static_cast<double>(window_samples)));
        ESP_LOGI(TAG, "mic frames=%lu samples=%lu peak=%d rms=%lu min=%d max=%d nonzero=%lu/%lu%s",
                 static_cast<unsigned long>(frame_count),
                 static_cast<unsigned long>(window_samples),
                 static_cast<int>(window_peak),
                 static_cast<unsigned long>(rms),
                 static_cast<int>(window_min),
                 static_cast<int>(window_max),
                 static_cast<unsigned long>(window_nonzero),
                 static_cast<unsigned long>(window_samples),
                 (window_peak == 0) ? "  <-- SILENT (ADC all zero)" : "");
        window_start_ms = now;
        frame_count = 0;
        window_peak = 0;
        window_min = frame[0];
        window_max = frame[0];
        window_sum_sq = 0;
        window_samples = 0;
        window_nonzero = 0;
    }
#else
    (void)frame;
#endif
}

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
// Sink for echo-cancelled audio from the AEC processor: forward it to the
// registered frame hook, exactly as raw mic capture would have been.
static void audio_aec_output(const int16_t *clean, size_t count, void *) {
    if (AEC_IsRuntimeActive() && s_frame_hook != nullptr) {
        s_frame_hook(clean, count, s_audio_mode, s_frame_hook_user_data);
    }
}
#endif

static void audio_passthrough_task(void *) {
    static int16_t frame[kFrameSamples];
    static int16_t playback_8k[kNetworkFrameSamples];
    static int16_t playback_frame[kFrameSamples];

    while (s_passthrough_running) {
        const AUDIO_Mode_t mode = s_audio_mode;

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
        static int16_t ref_frame[kFrameSamples];
        static int16_t network_ref_frame[kFrameSamples];
        const bool software_filter_enabled = s_mic_hpf_enabled;
        const bool afe_ready = AEC_IsReady();
        const bool processed_route = AEC_IsRuntimeActive();
        const bool needs_ref = afe_ready && AEC_UsesReference();
        if (!i2s_read_frame(frame, needs_ref ? ref_frame : nullptr)) {
            ESP_LOGI(TAG, "i2s_read_frame failed");
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        audio_log_mic_frame_stats(frame);
        if (software_filter_enabled) {
            mic_hpf_apply(frame, kFrameSamples);
        }

        // Full bypass when both AEC and AI noise are runtime-disabled: skip
        // AEC_SubmitCapture() entirely so the AFE goes idle (no NSNET2 RNN
        // inference, no feed-buffer churn). The frame hook below runs the
        // raw mic stream directly. When the user toggles either flag back
        // on, AEC_IsRuntimeActive() flips true and we start submitting
        // again; the AFE recovers state within a few frames.
        if (afe_ready && processed_route) {
            const int16_t *ref = nullptr;
            // Only resolve a reference when AEC is actually selected at
            // runtime. With AEC off but AI noise on (processed_route still
            // true), leave ref = nullptr so AEC_SubmitCapture feeds zeros
            // for the reference slot -- NSNET2 keeps running, echo
            // subtraction becomes a no-op.
            if (needs_ref && AEC_IsRuntimeAecEnabled()) {
                if (s_aec_reference_source == 1u) {
                    ref = ref_frame;
                } else {
                    aec_network_ref_read(network_ref_frame, kFrameSamples);
                    ref = network_ref_frame;
                }
            }
            AEC_SubmitCapture(frame, ref, kFrameSamples);
        }
        if (!processed_route && s_frame_hook != nullptr) {
            s_frame_hook(frame, kFrameSamples, mode, s_frame_hook_user_data);
        }
#else
        if (!i2s_read_frame(frame)) {
            ESP_LOGI(TAG, "i2s_read_frame failed");
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        audio_log_mic_frame_stats(frame);
        mic_hpf_apply(frame, kFrameSamples);

        if (s_frame_hook != nullptr) {
            s_frame_hook(frame, kFrameSamples, mode, s_frame_hook_user_data);
        }
#endif

        // RX mode: DAC plays whatever is in the output queue (NRL downlink).
        // If the queue is empty, write silence so the DAC stays at VMID.
        const size_t popped = output_queue_pop_frame(playback_8k, kNetworkFrameSamples);
        (void)popped;
        upsample_8k_to_16k_frame(playback_8k, playback_frame);
        aec_network_ref_push(playback_frame, kFrameSamples);
        if (!i2s_write_frame(playback_frame)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        taskYIELD();
    }

    i2s_clear_dma();
    s_passthrough_task = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

extern "C" bool AUDIO_SetupI2S(void) {
    return i2s_setup();
}

extern "C" bool AUDIO_IsPassthroughRunning(void) {
    return s_passthrough_task != nullptr;
}

extern "C" bool AUDIO_StartPassthrough(void) {
    if (!s_i2s_ready && !i2s_setup()) {
        return false;
    }

    if (s_passthrough_task != nullptr) {
        return true;
    }

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
    // Bring up esp-sr before the passthrough task starts feeding it. Runtime
    // switches only choose whether to use processed frames and which reference
    // source to feed; the resident AFE stays alive.
    const ExternalRadioConfig *aec_cfg = EXTERNAL_RADIO_GetConfig();
    const bool runtime_ai_noise = (aec_cfg != nullptr) && aec_cfg->ai_noise_enabled;
    AUDIO_SetAecReferenceSource((aec_cfg != nullptr) ? aec_cfg->aec_reference_source : 0u);
#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
    const bool afe_has_aec = true;
    const bool runtime_aec = (aec_cfg != nullptr) && aec_cfg->aec_enabled;
#else
    const bool afe_has_aec = false;
    const bool runtime_aec = false;
#endif
    const bool afe_has_ai_noise = true;
    AEC_SetOutputCallback(audio_aec_output, nullptr);
    if (AEC_Init(afe_has_aec, afe_has_ai_noise)) {
        AEC_SetRuntimeEnabled(runtime_aec, runtime_ai_noise);
        ESP_LOGI(TAG, "esp-sr resident: aec_cap=%u ai_cap=%u route_aec=%u route_ai=%u",
                 afe_has_aec ? 1u : 0u,
                 afe_has_ai_noise ? 1u : 0u,
                 runtime_aec ? 1u : 0u,
                 runtime_ai_noise ? 1u : 0u);
    } else {
        ESP_LOGI(TAG, "esp-sr resident init failed -- mic uplink falls back to raw");
    }
#endif

    s_passthrough_running = true;
    // Stack lives in PSRAM (MALLOC_CAP_SPIRAM): 8 KB is too big to find as a
    // contiguous block in internal SRAM once AEC_Init has done its ~50 KB of
    // mallocs just above. The passthrough task only touches DMA buffers,
    // I2S/I2C drivers, codec register state, AEC feed/output queues, and the
    // (already PSRAM-resident) G.711 encode LUT -- none of which require
    // stack access while flash cache is disabled, so PSRAM stack is safe.
    //
    // Pinned to core 1: WiFi driver and lwIP TCPIP task run on core 0, and
    // unpinned audio task migration onto core 0 mid-frame is one of the
    // sources of voice-packet send-time jitter visible on the wire. Keeping
    // the audio task isolated on core 1 also matches the AEC fetch task,
    // which is already core-1 pinned.
    // Priority 10: above the priority-5 mainLoopTask (which round-robins
    // WifiConfigPortal_Poll, BLEConfig_Poll, Display_Poll on the same core).
    // At equal priority the polls block audio frames for tens of ms at a
    // time, which shows up as a "2-packet pair every ~100 ms" pattern on
    // the wire. Audio task must preempt the polls, not share with them.
    // Well below WiFi (23) / TCPIP (~18) so we don't starve the network.
    if (xTaskCreatePinnedToCoreWithCaps(audio_passthrough_task,
                                        "audio_passthrough",
                                        8192,
                                        nullptr,
                                        10,
                                        &s_passthrough_task,
                                        1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_passthrough_running = false;
        s_passthrough_task = nullptr;
        return false;
    }

    return true;
}

extern "C" void AUDIO_StopPassthrough(void) {
    if (s_passthrough_task == nullptr) {
        return;
    }

    s_passthrough_running = false;
    for (int wait = 0; wait < 50 && s_passthrough_task != nullptr; ++wait) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

extern "C" void AUDIO_SetMode(const AUDIO_Mode_t mode) {
    s_audio_mode = mode;
}

extern "C" AUDIO_Mode_t AUDIO_GetMode(void) {
    return s_audio_mode;
}

extern "C" void AUDIO_SetFrameHook(AUDIO_FrameHook_t hook, void *user_data) {
    s_frame_hook = hook;
    s_frame_hook_user_data = user_data;
}

extern "C" size_t AUDIO_QueueOutputSamples(const int16_t *samples, size_t sample_count) {
    const size_t written = output_queue_push(samples, sample_count);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (written != sample_count && (now - s_last_output_queue_log_ms) >= 1000u) {
        s_last_output_queue_log_ms = now;
        ESP_LOGI(TAG, "queue short write samples=%u written=%u",
                 static_cast<unsigned>(sample_count),
                 static_cast<unsigned>(written));
    }
    return written;
}

extern "C" void AUDIO_ClearOutputQueue(void) {
    output_queue_clear();
}

extern "C" void AUDIO_SetAecReferenceSource(const uint8_t source) {
    s_aec_reference_source = (source == 1u) ? 1u : 0u;
    aec_network_ref_clear();
}

extern "C" void AUDIO_SetMicHpfEnabled(const bool enabled) {
    if (s_mic_hpf_enabled != enabled) {
        s_mic_hpf_enabled = enabled;
        mic_hpf_reset();
        ESP_LOGI(TAG, "mic HPF %s (4th-order Butterworth, fc=200Hz @ fs=%dHz)",
                 enabled ? "ENABLED" : "disabled", kSampleRate);
    }
}

extern "C" bool AUDIO_GetMicHpfEnabled(void) {
    return s_mic_hpf_enabled;
}

extern "C" int AUDIO_GetSampleRate(void) {
    return kSampleRate;
}

extern "C" size_t AUDIO_GetFrameSamples(void) {
    return kFrameSamples;
}

extern "C" bool AUDIO_PlayTestTone(const uint32_t durationMs) {
    if (!s_i2s_ready && !i2s_setup()) {
        return false;
    }

    const AUDIO_Mode_t previous_mode = s_audio_mode;
    const bool was_running = (s_passthrough_task != nullptr);
    if (was_running) {
        AUDIO_StopPassthrough();
    }

    s_audio_mode = AUDIO_MODE_RECEIVE;

    static int16_t frame[kFrameSamples];
    size_t total_samples = (static_cast<size_t>(kSampleRate) * durationMs) / 1000u;
    if (total_samples == 0) {
        total_samples = kFrameSamples;
    }

    float phase = 0.0f;
    const float step = (kTwoPi * kToneFrequency) / static_cast<float>(kSampleRate);

    bool ok = true;
    size_t produced = 0;
    while (produced < total_samples) {
        const size_t samples_this = (total_samples - produced < kFrameSamples)
                                      ? (total_samples - produced)
                                      : kFrameSamples;

        for (size_t i = 0; i < samples_this; ++i) {
            const int16_t sample = static_cast<int16_t>(sinf(phase) * kToneAmplitude * static_cast<float>(INT16_MAX));
            frame[i] = sample;

            phase += step;
            if (phase >= kTwoPi) {
                phase -= kTwoPi;
            }
        }

        if (samples_this < kFrameSamples) {
            memset(frame + samples_this, 0, (kFrameSamples - samples_this) * sizeof(int16_t));
        }

        if (!i2s_write_frame(frame)) {
            ok = false;
            break;
        }

        produced += samples_this;
    }

    i2s_clear_dma();

    if (was_running && !AUDIO_StartPassthrough()) {
        ok = false;
    }

    s_audio_mode = previous_mode;
    return ok;
}

extern "C" bool AUDIO_RecordMicAndPlayback(uint32_t durationMs) {
    if (!s_i2s_ready && !i2s_setup()) {
        return false;
    }

    if (durationMs == 0U) {
        durationMs = 3000U;
    }

    size_t total_samples = (static_cast<size_t>(kSampleRate) * durationMs) / 1000U;
    if (total_samples == 0U) {
        total_samples = kFrameSamples;
    }

    int16_t *recorded = static_cast<int16_t *>(malloc(total_samples * sizeof(int16_t)));
    if (!recorded) {
        return false;
    }

    const AUDIO_Mode_t previous_mode = s_audio_mode;
    const bool was_running = (s_passthrough_task != nullptr);
    if (was_running) {
        AUDIO_StopPassthrough();
    }

    bool ok = true;
    static int16_t frame[kFrameSamples];
    size_t captured = 0U;

    s_audio_mode = AUDIO_MODE_RECEIVE;
    i2s_clear_dma();
    while (captured < total_samples) {
        if (!i2s_read_frame(frame)) {
            ok = false;
            break;
        }

        const size_t samples_this = ((total_samples - captured) < kFrameSamples)
                                        ? (total_samples - captured)
                                        : kFrameSamples;
        memcpy(recorded + captured, frame, samples_this * sizeof(int16_t));
        captured += samples_this;
    }

    if (ok) {
        i2s_clear_dma();

        size_t played = 0U;
        while (played < captured) {
            const size_t samples_this = ((captured - played) < kFrameSamples)
                                            ? (captured - played)
                                            : kFrameSamples;

            if (samples_this < kFrameSamples) {
                memcpy(frame, recorded + played, samples_this * sizeof(int16_t));
                memset(frame + samples_this, 0, (kFrameSamples - samples_this) * sizeof(int16_t));
            } else {
                memcpy(frame, recorded + played, kFrameBytes);
            }

            if (!i2s_write_frame(frame)) {
                ok = false;
                break;
            }
            played += samples_this;
        }

        i2s_clear_dma();
    }

    free(recorded);

    if (was_running && !AUDIO_StartPassthrough()) {
        ok = false;
    }

    s_audio_mode = previous_mode;
    return ok;
}
