#include <Arduino.h>

#include "../lib/ble_config.h"
#include "../lib/nrl_audio_bridge.h"
#include "../lib/nrl_version.h"
#include "../lib/wifi_config_portal.h"
#include "driver/board_pins.h"
#include "driver/es7210.h"
#include "driver/es8311.h"
#include "driver/external_radio.h"
#include "driver/status_io.h"

namespace {

constexpr unsigned long kPollIntervalMs = 20UL;
unsigned long s_last_poll_ms = 0UL;

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println(NRL_FIRMWARE_BANNER " startup");
    Serial.printf("[BOOT] flash=%u bytes psram=%u bytes (psram %s)\n",
                  static_cast<unsigned>(ESP.getFlashChipSize()),
                  static_cast<unsigned>(ESP.getPsramSize()),
                  ESP.getPsramSize() > 0 ? "OK" : "absent");

    EXTERNAL_RADIO_Init();
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
                                config->daceq_a1);
    }

    STATUS_IO_Init();
    WifiConfigPortal_Init();
    BLEConfig_Init();

    if (ES8311_Init()) {
        Serial.println("ES8311 ready.");
    } else {
        Serial.println("ES8311 initialization failed.");
    }

    if (!ES8311_SetReceiveMode()) {
        Serial.println("ES8311 set receive mode failed.");
    }

#if NRL_HAS_ES7210
    // The mic ADC on this board is a separate ES7210 chip. ES8311_Init()
    // above has already started I2S, so MCLK/BCLK/LRCK are running and the
    // ES7210 can lock its clock. Configure it now so I2S DIN carries audio.
    if (ES7210_Init()) {
        Serial.println("ES7210 mic ADC ready.");
    } else {
        Serial.println("ES7210 mic ADC initialization failed.");
    }
#endif

    if (!NRLAudioBridge_Init()) {
        Serial.println("NRL audio bridge init failed.");
    }

    Serial.println("[AT] serial console ready. Type \"AT\" for the command list, "
                    "e.g. AT+WIFI_SSID=MyNet then AT+WIFI_PASS=secret");
}

void loop()
{
    const unsigned long now = millis();
    if ((now - s_last_poll_ms) >= kPollIntervalMs) {
        s_last_poll_ms = now;
        STATUS_IO_Poll();
        WifiConfigPortal_Poll();
        BLEConfig_Poll();
        NRLAudioBridge_PollSerialConsole();
    }

    delay(1);
}
