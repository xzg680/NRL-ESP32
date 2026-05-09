#include <Arduino.h>

#include "../lib/nrl_audio_bridge.h"
#include "../lib/wifi_config_portal.h"
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
    Serial.println("NRL NRL + ES8311 startup");

    EXTERNAL_RADIO_Init();
    if (const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig()) {
        ES8311_ApplyAudioConfig(config->mic_volume,
                                config->line_out_volume,
                                config->hp_drive_enabled);
    }

    STATUS_IO_Init();
    WifiConfigPortal_Init();

    if (ES8311_Init()) {
        Serial.println("ES8311 ready.");
    } else {
        Serial.println("ES8311 initialization failed.");
    }

    if (!ES8311_SetReceiveMode()) {
        Serial.println("ES8311 set receive mode failed.");
    }

    if (!NRLAudioBridge_Init()) {
        Serial.println("NRL audio bridge init failed.");
    }
}

void loop()
{
    const unsigned long now = millis();
    if ((now - s_last_poll_ms) >= kPollIntervalMs) {
        s_last_poll_ms = now;
        STATUS_IO_Poll();
        WifiConfigPortal_Poll();
    }

    delay(1);
}
