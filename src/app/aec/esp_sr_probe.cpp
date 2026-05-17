#include "driver/board_pins.h"

#if defined(NRL_ENABLE_GEZIPAI_AEC) && NRL_ENABLE_GEZIPAI_AEC

#if NRL_BOARD != NRL_BOARD_GEZIPAI
#error "NRL_ENABLE_GEZIPAI_AEC is only valid for the Gezipai board"
#endif

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <esp_afe_sr_models.h>

// Runtime probe: actually build an esp-sr AFE (voice-communication) instance
// the way the AEC path will use it -- 2 channels (1 mic + 1 reference),
// 16 kHz, AEC + speech-enhancement + AGC, PSRAM-heavy allocation. Confirms
// esp-sr links AND functions on this board before the real AFE glue is wired
// in. The instance is created, queried, and destroyed.
extern "C" bool NRL_AEC_EspSrProbe(void)
{
    // Quiet the AFE_VC info logs so they do not interleave with and garble
    // this probe's own Serial output.
    esp_log_level_set("AFE_VC", ESP_LOG_WARN);

    afe_config_t cfg = AFE_CONFIG_DEFAULT();
    cfg.aec_init = true;
    cfg.se_init = true;
    cfg.vad_init = false;
    cfg.wakenet_init = false;
    cfg.voice_communication_init = true;
    cfg.voice_communication_agc_init = true;
    cfg.afe_mode = SR_MODE_HIGH_PERF;
    cfg.memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg.pcm_config.total_ch_num = 2; // 1 mic + 1 reference (channel-interleaved)
    cfg.pcm_config.mic_num = 1;
    cfg.pcm_config.ref_num = 1;
    cfg.pcm_config.sample_rate = 16000; // esp-sr AFE is fixed at 16 kHz

    const esp_afe_sr_iface_t *iface = &ESP_AFE_VC_HANDLE;
    if (iface == nullptr || iface->create_from_config == nullptr) {
        Serial.println("[AEC] esp-sr AFE interface unavailable");
        return false;
    }

    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_afe_sr_data_t *afe = iface->create_from_config(&cfg);
    if (afe == nullptr) {
        Serial.println("[AEC] AFE create_from_config FAILED");
        return false;
    }

    const int feed_chunk = iface->get_feed_chunksize(afe);
    const int fetch_chunk = iface->get_fetch_chunksize(afe);
    const int total_ch = iface->get_total_channel_num(afe);
    const int mic_ch = iface->get_channel_num(afe);
    const int samp_rate = iface->get_samp_rate(afe);
    const size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    Serial.printf("[AEC] AFE created: feed=%d fetch=%d total_ch=%d mic_ch=%d "
                  "rate=%dHz psram_used=%uKB\n",
                  feed_chunk, fetch_chunk, total_ch, mic_ch, samp_rate,
                  static_cast<unsigned>((psram_before - psram_after) / 1024u));

    iface->destroy(afe);
    Serial.println("[AEC] AFE destroyed -- esp-sr runtime probe OK");
    return true;
}

#else

extern "C" bool NRL_AEC_EspSrProbe(void)
{
    return false;
}

#endif
