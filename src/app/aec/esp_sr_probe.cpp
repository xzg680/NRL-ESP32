#include "driver/board_pins.h"

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE

#include <Arduino.h>
#include <esp_afe_config.h>
#include <esp_afe_sr_models.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <model_path.h>

// Runtime probe: build the same esp-sr VC/NSNET2 instance shape used by the
// audio path, query it, and destroy it. This catches missing model partitions
// and PSRAM allocation trouble early in boot.
extern "C" bool NRL_AEC_EspSrProbe(void)
{
    esp_log_level_set("AFE_VC", ESP_LOG_WARN);

    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == nullptr) {
        Serial.println("[AEC] model partition unavailable; esp-sr probe skipped");
        return false;
    }

    const bool use_ref =
#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
        true;
#else
        false;
#endif
    afe_config_t *cfg = afe_config_init(use_ref ? "MR" : "M",
                                        models,
                                        AFE_TYPE_VC,
                                        AFE_MODE_LOW_COST);
    if (cfg == nullptr) {
        Serial.println("[AEC] afe_config_init FAILED");
        esp_srmodel_deinit(models);
        return false;
    }

    cfg->aec_init = use_ref;
    cfg->vad_init = false;
    cfg->wakenet_init = false;
    cfg->agc_init = false;
    cfg->afe_ns_mode = AFE_NS_MODE_NET;

    const esp_afe_sr_iface_t *iface = esp_afe_handle_from_config(cfg);
    if (iface == nullptr || iface->create_from_config == nullptr) {
        Serial.println("[AEC] esp-sr AFE interface unavailable");
        afe_config_free(cfg);
        esp_srmodel_deinit(models);
        return false;
    }

    const size_t mem_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_afe_sr_data_t *afe = iface->create_from_config(cfg);
    afe_config_free(cfg);
    if (afe == nullptr) {
        Serial.println("[AEC] AFE create_from_config FAILED");
        esp_srmodel_deinit(models);
        return false;
    }

    const int feed_chunk = iface->get_feed_chunksize(afe);
    const int fetch_chunk = iface->get_fetch_chunksize(afe);
    const int feed_ch = iface->get_feed_channel_num(afe);
    const int samp_rate = iface->get_samp_rate(afe);
    const size_t mem_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    Serial.printf("[AEC] AFE NSNET2 probe: feed=%d fetch=%d feed_ch=%d "
                  "rate=%dHz psram_used=%uKB\n",
                  feed_chunk, fetch_chunk, feed_ch, samp_rate,
                  static_cast<unsigned>((mem_before - mem_after) / 1024u));

    iface->destroy(afe);
    esp_srmodel_deinit(models);
    Serial.println("[AEC] AFE destroyed -- esp-sr runtime probe OK");
    return true;
}

#else

extern "C" bool NRL_AEC_EspSrProbe(void)
{
    return false;
}

#endif
