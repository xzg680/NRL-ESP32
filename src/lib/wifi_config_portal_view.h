#ifndef SRC_LIB_WIFI_CONFIG_PORTAL_VIEW_H
#define SRC_LIB_WIFI_CONFIG_PORTAL_VIEW_H

#include "../app/driver/external_radio.h"

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

struct WifiConfigPortalScanEntry {
    String ssid;
    int32_t rssi;
};

struct WifiConfigPortalPageState {
    const char *title;
    const char *headline;
    const char *headline_key;
    const char *intro;
    const char *intro_key;
    const char *form_action;
    bool network_active;
    bool device_active;
    bool audio_active;
    String footer;
};

String WifiConfigPortalView_BuildNetworkSection(const ExternalRadioConfig *config,
                                                const WifiConfigPortalScanEntry *scan_entries,
                                                size_t scan_count);
String WifiConfigPortalView_BuildDeviceSections(const ExternalRadioConfig *config);
String WifiConfigPortalView_BuildAudioSections(const ExternalRadioConfig *config);
String WifiConfigPortalView_BuildConfigPage(const ExternalRadioConfig *config,
                                            const WifiConfigPortalPageState &state,
                                            const String &form_sections);
String WifiConfigPortalView_BuildUpdatePage(const char *headline,
                                            const char *headline_key,
                                            const char *intro,
                                            const char *intro_key);

#endif
