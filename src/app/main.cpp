#include <esp_event.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "../lib/ble_config.h"
#include "../services/ai_assistant.h"
#include "../services/aprs_service.h"
#include "../services/espnow_link.h"
#include "../services/video_call.h"
#include "../services/music_player.h"
#include "../services/music_playlist.h"
#include "../services/nanny.h"
#include "../services/ota_service.h"
#include "../services/radio_favorites.h"
#include "../services/storage_service.h"
#include "../lib/nrl_audio_bridge.h"
#include "../lib/nrl_bt_hfp.h"
#include "../lib/nrl_ethernet.h"
#include "../lib/nrl_usb_console.h"
#include "../lib/nrl_version.h"
#include "../lib/wifi_config_portal.h"
#include "driver/board_pins.h"
#include "driver/display.h"
#include "driver/es7210.h"
#include "driver/es8311.h"
#include "driver/es8389.h"
#include "driver/external_radio.h"
#include "driver/status_io.h"

namespace {

constexpr unsigned long kPollIntervalMs = 20UL;
const char *TAG = "APP";

// Boot-time internal-DRAM ruler: logs free/largest-block after each init step
// so a regression that starves the audio/AEC init (which needs contiguous
// internal RAM late in boot) can be pinned to the step that ate it.
static void logDramMark(const char *step)
{
    ESP_LOGI(TAG, "[DRAM] after %-14s free=%6u largest=%6u", step,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

static void initSystem()
{
    // The IDF gpio driver logs every gpio_config() at INFO level; the
    // bit-bang I2C reconfigures SDA/SCL constantly and floods the console.
    esp_log_level_set("gpio", ESP_LOG_WARN);
    // esp-sr's AFE prints "Ringbuffer of AFE is empty, Please use feed() to
    // write data" at WARN whenever its fetch outruns feed. The design here
    // only feeds the AEC reference channel while network voice is actually
    // playing (gezipai + bh4tdv both); during idle/RX-only the WARN spams
    // every ~200 ms. Suppress it; real errors still surface at ESP_LOG_ERROR.
    esp_log_level_set("AFE", ESP_LOG_ERROR);

    // One-time stack init that Arduino used to do before setup(). Idempotent;
    // nrl_wifi.cpp's nrlWifiInit() re-runs these but they short-circuit on
    // INVALID_STATE. Doing them here keeps boot ordering deterministic.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();

    NRL_USB_Console_Init();

    ESP_LOGI(TAG, "%s startup", NRL_FIRMWARE_BANNER);
    uint32_t flash_size = 0;
    (void)esp_flash_get_size(nullptr, &flash_size);
    const size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "[BOOT] flash=%u bytes psram=%u bytes (psram %s)",
             static_cast<unsigned>(flash_size),
             static_cast<unsigned>(psram_size),
             psram_size > 0 ? "OK" : "absent");
}

static void initApp()
{
    logDramMark("boot");
    EXTERNAL_RADIO_Init();
    if (!nrlEthernetInit()) {
        ESP_LOGE(TAG, "Ethernet initialization failed; Wi-Fi fallback remains available.");
    }
    logDramMark("ethernet");
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        ES8311_ApplyAudioConfig(config->mic_volume,
                                config->line_out_volume,
                                config->hp_drive_enabled,
                                config->drc_enabled,
                                config->drc_winsize,
                                config->drc_maxlevel,
                                config->drc_minlevel,
                                config->dac_ramprate,
                                config->dac_eq_bypass,
                                config->daceq_b0,
                                config->daceq_b1,
                                config->daceq_a1,
                                config->adc_dmic_enabled,
                                config->adc_linsel,
                                config->adc_pga_gain,
                                config->adc_ramprate,
                                config->adc_dmic_sense,
                                config->adc_sync,
                                config->adc_inv,
                                config->adc_ramclr,
                                config->adc_scale,
                                config->alc_enabled,
                                config->adc_automute_enabled,
                                config->alc_winsize,
                                config->alc_maxlevel,
                                config->alc_minlevel,
                                config->adc_automute_winsize,
                                config->adc_automute_noise_gate,
                                config->adc_automute_volume,
                                config->adc_hpfs1,
                                config->adc_eq_bypass,
                                config->adc_hpf,
                                config->adc_hpfs2,
                                config->adceq_b0,
                                config->adceq_a1,
                                config->adceq_a2,
                                config->adceq_b1,
                                config->adceq_b2);
        AUDIO_SetMicHpfEnabled(config->mic_hpf_enabled);
    }
#else
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        AUDIO_SetMicHpfEnabled(config->mic_hpf_enabled);
    }
#endif

    STATUS_IO_Init();

    // Removable storage + music player (no-ops on boards without the
    // hardware). Mount before the display so the first UI frame can already
    // show storage state.
    STORAGE_Init();
    MUSIC_Init();
    PLAYLIST_Init();
    RADIO_FAV_Init();
    NANNY_Init();
    if (STORAGE_SdMounted()) {
        PLAYLIST_Scan();
    }
    logDramMark("storage+music");

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && !(defined(NRL_SKIP_DISPLAY_INIT) && NRL_SKIP_DISPLAY_INIT)
    // Bring the LCD up early so it shows a status frame while WiFi/BLE start.
    Display_Init();
    logDramMark("display");
#endif

    WifiConfigPortal_Init();
    OtaService_Init();
    logDramMark("portal+ota");
    // BLE provisioning is only needed until WiFi credentials exist: its NimBLE
    // host + controller cost ~66 KB of internal DRAM, which on the S3 display
    // boards is exactly what the audio/AEC init later in boot needs. Configured
    // devices boot WiFi-only; if the STA stays down, BLEConfig_Poll()'s
    // fallback brings BLE provisioning back up automatically.
    {
        const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
        const bool wifi_configured = (config != nullptr) && config->wifi_ssid[0] != '\0';
        if (wifi_configured) {
            ESP_LOGI(TAG, "WiFi configured; skipping boot BLE provisioning (Poll restores it if STA stays down)");
        } else {
            BLEConfig_Init();
        }
    }
    logDramMark("ble_config");

    // Bring the audio bridge up BEFORE ES8311_Init. ES8311_Init internally
    // calls AEC_Init which mallocs ~50 KB (WebRtcNs + AFE state) from the
    // internal SRAM heap; if the bridge task's 8 KB stack hasn't been
    // allocated by then there's no contiguous block left and xTaskCreate
    // fails -- which silently kills STA reconnect (the bridge owns the
    // WiFi state machine). NRLAudioBridge_Init only registers its audio
    // router sinks/routes; frames only flow once ES8311 starts its
    // passthrough task, so the ordering swap is safe.
    if (!NRLAudioBridge_Init()) {
        ESP_LOGE(TAG, "NRL audio bridge init failed.");
    }
    logDramMark("audio_bridge");

    // Bluetooth headset (HFP) voice link. Brought up only if enabled in config;
    // on non-S31 boards these are no-ops.
    NRL_BtHfp_Init();
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        NRL_BtHfp_SetEnabled(config->bt_enabled);
    }
    logDramMark("bt_hfp");

    // ESP-NOW off-grid voice link: restores its persisted enable state,
    // waiting for the bridge task to start WiFi if needed.
    ESPNOW_LINK_Init();
    logDramMark("espnow");

    // APRS transceiver: GPS beacons to APRS-IS and/or AFSK over the radio,
    // plus RF demodulation into the station list. Idles until enabled.
    APRS_SERVICE_Init();
    logDramMark("aprs");

    // xiaozhi AI assistant: reconnects in the background when configured.
    AI_Init();

    // Video call: passive RX buffering; camera TX starts on demand.
    VIDEO_Init();
    logDramMark("ai+video");

#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    if (ES8311_Init()) {
        ESP_LOGI(TAG, "ES8311 ready.");
    } else {
        ESP_LOGE(TAG, "ES8311 initialization failed.");
    }

    if (!ES8311_SetReceiveMode()) {
        ESP_LOGE(TAG, "ES8311 set receive mode failed.");
    }
#elif defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389
    if (ES8389_Init()) {
        ESP_LOGI(TAG, "ES8389 ready.");
    } else {
        ESP_LOGE(TAG, "ES8389 initialization failed.");
    }

    if (!ES8389_SetReceiveMode()) {
        ESP_LOGE(TAG, "ES8389 set receive mode failed.");
    }

    // The ES8389 driver has no pending-config cache (unlike ES8311), so the
    // persisted volume/gain must be pushed after the codec device exists.
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        ES8389_SetOutputVolume(config->line_out_volume);
        ES8389_SetInputGain(config->mic_volume);
    }
#endif

#if NRL_HAS_ES7210
    // The mic ADC on this board is a separate ES7210 chip. ES8311_Init()
    // above has already started I2S, so MCLK/BCLK/LRCK are running and the
    // ES7210 can lock its clock. Configure it now so I2S DIN carries audio.
    if (ES7210_Init()) {
        ESP_LOGI(TAG, "ES7210 mic ADC ready.");
    } else {
        ESP_LOGE(TAG, "ES7210 mic ADC initialization failed.");
    }
#endif

    ESP_LOGI(TAG, "[AT] serial console ready. Type \"AT\" for the command list, "
                  "e.g. AT+WIFI_SSID=MyNet then AT+WIFI_PASS=secret");
}

static void mainLoopTask(void *)
{
    while (true) {
        STATUS_IO_Poll();
        WifiConfigPortal_Poll();
        BLEConfig_Poll();
        NRL_BtHfp_Poll();
        NRLAudioBridge_PollSerialConsole();
#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && !(defined(NRL_SKIP_DISPLAY_INIT) && NRL_SKIP_DISPLAY_INIT)
        Display_Poll();
#endif
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

} // namespace

extern "C" void app_main(void)
{
    initSystem();
    initApp();
    // Detach the polling loop from the boot task so we can let app_main return
    // (IDF style). 6 KB stack is enough for the poll fan-out; bump if a
    // sub-system asks for more local stack down the line.
    //
    // Pinned to core 0 (where WiFi, lwIP TCPIP and bridgeTask live). Core 1
    // is reserved for the audio pipeline (es8311 passthrough + aec_fetch).
    // Keeping polls off core 1 stops Display_Poll / WifiConfigPortal_Poll
    // from stealing audio-task CPU and bunching outbound voice packets.
    xTaskCreatePinnedToCore(mainLoopTask, "nrl_main_loop", 6144, nullptr, 5, nullptr, 0);
}
