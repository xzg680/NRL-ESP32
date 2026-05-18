#include "wifi_config_portal.h"
#include "wifi_config_portal_view.h"
#include "nrl_audio_bridge.h"
#include "nrl_version.h"

#include "../app/driver/es8311.h"
#include "../app/driver/external_radio.h"
#include "../app/driver/board_pins.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include <IPAddress.h>
#include <stdio.h>
#include <string.h>

namespace {

WebServer s_server(80);
DNSServer s_dns;
bool s_server_started = false;
bool s_dns_started = false;
bool s_ap_started = false;
bool s_ap_should_run = true;
bool s_sta_was_connected = false;
bool s_ap_close_scheduled = false;
unsigned long s_ap_close_at_ms = 0UL;
unsigned long s_sta_disconnect_started_ms = 0UL;
bool s_boot_pressed = false;
bool s_update_reboot_pending = false;
unsigned long s_boot_press_started_ms = 0UL;
unsigned long s_update_reboot_at_ms = 0UL;

// Cached WiFi scan results. The scan runs once before the config AP starts
// (no portal client connected yet, so the channel-hopping scan disturbs
// nobody); the portal then serves this cache and never does a live scan,
// which would otherwise drop the connected client.
const size_t kWifiScanCacheMax = 24u;
WifiConfigPortalScanEntry s_wifi_scan_cache[kWifiScanCacheMax];
int s_wifi_scan_count = 0;
bool s_wifi_prescan_done = false;

constexpr byte kDnsPort = 53;
constexpr unsigned long kBootResetHoldMs = 5000UL;
constexpr unsigned long kApCloseDelayMs = 5000UL;
constexpr unsigned long kApReopenAfterStaDownMs = 60000UL;
constexpr unsigned long kWifiPrescanTimeoutMs = 12000UL;
constexpr unsigned long kDacEqCoefficientMax = 1073741823UL;
const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApGateway(192, 168, 4, 1);
const IPAddress kApSubnet(255, 255, 255, 0);

static String jsonEscape(const String &text)
{
    String out;
    for (size_t i = 0; i < text.length(); ++i) {
        const char ch = text[i];
        switch (ch) {
            case '\\':
                out += F("\\\\");
                break;
            case '"':
                out += F("\\\"");
                break;
            case '\b':
                out += F("\\b");
                break;
            case '\f':
                out += F("\\f");
                break;
            case '\n':
                out += F("\\n");
                break;
            case '\r':
                out += F("\\r");
                break;
            case '\t':
                out += F("\\t");
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

// Mask secret fields (e.g. WiFi password) for serial logs: show length only,
// never the plaintext.
static String maskSecret(const char *text)
{
    if (text == nullptr || *text == '\0') {
        return String("(empty)");
    }
    return String("****** (") + String(static_cast<unsigned>(strlen(text))) + " chars)";
}

static String ipToString(const uint32_t value)
{
    return IPAddress(value).toString();
}

static String buildApSsid()
{
    const uint64_t efuse = ESP.getEfuseMac();
    const uint32_t tail = static_cast<uint32_t>(efuse & 0xFFFFFFULL);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "NRL3188-ESP32-%06X", tail);
    return String(buffer);
}

static void redirectToPortal()
{
    s_server.sendHeader("Location", "/", true);
    s_server.send(302, "text/plain", "");
}

static void handleFavicon()
{
    s_server.sendHeader("Cache-Control", "max-age=86400");
    s_server.send(204, "image/x-icon", "");
}

// Scan nearby WiFi and store the result in s_wifi_scan_cache.
static void performWifiPrescan()
{
    Serial.println("[CFG] pre-scanning WiFi before AP start...");
    s_wifi_scan_count = 0;
    WiFi.scanDelete();
    if (WiFi.scanNetworks(true /*async*/, true /*show_hidden*/) == WIFI_SCAN_FAILED) {
        Serial.println("[CFG] WiFi pre-scan failed to start");
        return;
    }

    int found = WIFI_SCAN_RUNNING;
    const unsigned long started_ms = millis();
    while ((millis() - started_ms) < kWifiPrescanTimeoutMs) {
        found = WiFi.scanComplete();
        if (found != WIFI_SCAN_RUNNING) {
            break;
        }
        delay(50);
    }
    if (found == WIFI_SCAN_RUNNING) {
        WiFi.scanDelete();
        Serial.println("[CFG] WiFi pre-scan timed out");
        return;
    }
    if (found < 0) {
        Serial.printf("[CFG] WiFi pre-scan failed: %d\n", found);
        return;
    }

    for (int i = 0; i < found && static_cast<size_t>(s_wifi_scan_count) < kWifiScanCacheMax; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) {
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < s_wifi_scan_count; ++j) {
            if (s_wifi_scan_cache[j].ssid == ssid) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        s_wifi_scan_cache[s_wifi_scan_count].ssid = ssid;
        s_wifi_scan_cache[s_wifi_scan_count].rssi = WiFi.RSSI(i);
        ++s_wifi_scan_count;
    }
    WiFi.scanDelete();
    Serial.printf("[CFG] pre-scan cached %d WiFi networks\n", s_wifi_scan_count);
}

static void ensureApRunning()
{
    if (!s_ap_should_run || s_ap_started) {
        return;
    }

    WiFi.persistent(false);

    // First time the config AP comes up: scan nearby WiFi while no portal
    // client is connected yet, and cache it. The page then shows the list
    // immediately and never needs a live scan (which would drop the client).
    if (!s_wifi_prescan_done) {
        s_wifi_prescan_done = true;
        WiFi.mode(WIFI_STA);
        delay(100);
        performWifiPrescan();
    }

    const wifi_mode_t mode = WiFi.getMode();
    if (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
        // Stay in plain AP at boot so BLE coexistence init in BLEConfig_Init
        // does not collide with an active STA interface. The bridge task will
        // upgrade to AP_STA once it actually needs to connect to a router.
        WiFi.mode(WIFI_AP);
        delay(20);
    }

    const String ap_ssid = buildApSsid();
    const bool ip_ok = WiFi.softAPConfig(kApIp, kApGateway, kApSubnet);
    const bool ap_ok = ip_ok && WiFi.softAP(ap_ssid.c_str(), nullptr, 1, false, 4);
    if (ap_ok) {
        s_ap_started = true;
        const int station_count = static_cast<int>(WiFi.softAPgetStationNum());
        Serial.printf("[CFG] AP ready: ssid=%s open ip=%s channel=%d hidden=%d stations=%d\n",
                      ap_ssid.c_str(),
                      WiFi.softAPIP().toString().c_str(),
                      WiFi.channel(),
                      0,
                      station_count);
    } else {
        Serial.printf("[CFG] AP start failed, mode=%d status=%d ipcfg=%d\n",
                      static_cast<int>(WiFi.getMode()),
                      static_cast<int>(WiFi.status()),
                      static_cast<int>(ip_ok));
    }
}

static void shutdownDnsAndAp()
{
    if (s_dns_started) {
        s_dns.stop();
        s_dns_started = false;
    }
    if (s_ap_started) {
        WiFi.softAPdisconnect(true);
        s_ap_started = false;
        Serial.println("[CFG] AP closed (STA online)");
    }
    s_ap_should_run = false;
}

static void manageApLifecycle()
{
    const bool sta_connected = WiFi.status() == WL_CONNECTED;
    const unsigned long now = millis();

    if (sta_connected) {
        s_sta_disconnect_started_ms = 0UL;
        if (!s_sta_was_connected) {
            s_sta_was_connected = true;
            s_ap_close_scheduled = true;
            s_ap_close_at_ms = now + kApCloseDelayMs;
            Serial.printf("[CFG] STA connected sta_ip=%s, AP will close in %lu ms\n",
                          WiFi.localIP().toString().c_str(),
                          static_cast<unsigned long>(kApCloseDelayMs));
        }
        if (s_ap_close_scheduled && static_cast<int32_t>(now - s_ap_close_at_ms) >= 0) {
            shutdownDnsAndAp();
            s_ap_close_scheduled = false;
        }
        return;
    }

    if (!s_sta_was_connected) {
        return;
    }

    if (s_sta_disconnect_started_ms == 0UL) {
        s_sta_disconnect_started_ms = now;
        Serial.println("[CFG] STA dropped, monitoring for reopen");
        return;
    }

    if ((now - s_sta_disconnect_started_ms) >= kApReopenAfterStaDownMs) {
        s_sta_was_connected = false;
        s_ap_close_scheduled = false;
        s_sta_disconnect_started_ms = 0UL;
        s_ap_should_run = true;
        Serial.println("[CFG] STA down too long, reopening config AP");
    }
}

static void pollBootResetGesture()
{
    const bool pressed = digitalRead(NRL_PIN_BOOT_BUTTON) == LOW;
    const unsigned long now = millis();

    if (!pressed) {
        s_boot_pressed = false;
        s_boot_press_started_ms = 0UL;
        return;
    }

    if (!s_boot_pressed) {
        s_boot_pressed = true;
        s_boot_press_started_ms = now;
        return;
    }

    if (s_boot_press_started_ms != 0UL && (now - s_boot_press_started_ms) >= kBootResetHoldMs) {
        if (EXTERNAL_RADIO_ResetNetworkConfig()) {
            Serial.println("[CFG] BOOT held 5s, network config reset to defaults");
        } else {
            Serial.println("[CFG] BOOT held 5s, network config reset failed");
        }
        delay(100);
        ESP.restart();
    }
}

static void ensureDnsRunning()
{
    if (s_dns_started || !s_ap_should_run || !s_ap_started) {
        return;
    }

    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(kDnsPort, "*", WiFi.softAPIP());
    s_dns_started = true;
    Serial.printf("[CFG] DNS captive portal ready on %u\n", static_cast<unsigned>(kDnsPort));
}

static String buildNetworkSection(const ExternalRadioConfig *config)
{
    return WifiConfigPortalView_BuildNetworkSection(config,
                                                    s_wifi_scan_cache,
                                                    static_cast<size_t>(s_wifi_scan_count));
}

static String buildDeviceSections(const ExternalRadioConfig *config)
{
    return WifiConfigPortalView_BuildDeviceSections(config);
}

static String buildAudioSections(const ExternalRadioConfig *config)
{
    return WifiConfigPortalView_BuildAudioSections(config);
}

static void sendConfigPage(const char *title,
                           const char *headline,
                           const char *headline_key,
                           const char *intro,
                           const char *intro_key,
                           const char *form_action,
                           const String &form_sections,
                           const bool network_active,
                           const bool device_active,
                           const bool audio_active,
                           const String &footer)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const WifiConfigPortalPageState state = {
        .title = title,
        .headline = headline,
        .headline_key = headline_key,
        .intro = intro,
        .intro_key = intro_key,
        .form_action = form_action,
        .network_active = network_active,
        .device_active = device_active,
        .audio_active = audio_active,
        .footer = footer,
    };
    s_server.send(200,
                  "text/html; charset=utf-8",
                  WifiConfigPortalView_BuildConfigPage(config, state, form_sections));
}

static String buildUpdatePageI18n(const char *headline,
                                  const char *headline_key,
                                  const char *intro,
                                  const char *intro_key)
{
    return WifiConfigPortalView_BuildUpdatePage(headline, headline_key, intro, intro_key);
}

static void handleRoot()
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    sendConfigPage((String(NRL_FIRMWARE_NAME) + " WiFi Config").c_str(),
                   "WiFi Config",
                   "wifiHeadline",
                   "Set the WiFi network and server address used by the device.",
                   "wifiIntro",
                   "/save_wifi",
                   buildNetworkSection(config),
                   true,
                   false,
                   false,
                   "");
}

static void handleNrlPage()
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    sendConfigPage((String(NRL_FIRMWARE_NAME) + " NRL Config").c_str(),
                   "NRL Config",
                   "nrlHeadline",
                   "Set radio identity and audio behavior.",
                   "nrlIntro",
                   "/save_nrl",
                   buildDeviceSections(config),
                   false,
                   true,
                   false,
                   "");
}

static void handleAudioPage()
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    sendConfigPage((String(NRL_FIRMWARE_NAME) + " Audio Settings").c_str(),
                   "Audio Settings",
                   "audioHeadline",
                   "Set ES8311 output, volume, DRC, and EQ.",
                   "audioIntro",
                   "/save_nrl",
                   buildAudioSections(config),
                   false,
                   false,
                   true,
                   "");
}

static void handleScan()
{
    // Serve the cache captured before the AP started. No live scan -- a live
    // scan hops channels and drops the portal client that is connected now.
    String json = "[";
    for (int i = 0; i < s_wifi_scan_count; ++i) {
        if (i != 0) {
            json += ",";
        }
        const String escaped_ssid = jsonEscape(s_wifi_scan_cache[i].ssid);
        json += "{\"ssid\":\"";
        json += escaped_ssid;
        json += "\",\"label\":\"";
        json += escaped_ssid;
        json += " (";
        json += String(s_wifi_scan_cache[i].rssi);
        json += " dBm)\"}";
    }
    json += "]";
    s_server.send(200, "application/json; charset=utf-8", json);
}

static bool parseUIntArg(const String &text, unsigned long *out_value)
{
    if (out_value == nullptr || text.length() == 0) {
        return false;
    }

    char *end = nullptr;
    const unsigned long value = strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || (end != nullptr && *end != '\0')) {
        return false;
    }

    *out_value = value;
    return true;
}

static bool parseIpArg(const String &text, uint32_t *out_value)
{
    if (out_value == nullptr || text.length() == 0) {
        return false;
    }
    IPAddress ip;
    if (!ip.fromString(text)) {
        return false;
    }
    *out_value = static_cast<uint32_t>(ip);
    return true;
}

static void handleSaveWifi()
{
    bool ok = true;

    const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
    char old_wifi_ssid[33] = {};
    char old_wifi_password[65] = {};
    bool old_wifi_dhcp_enabled = true;
    uint32_t old_wifi_ip = 0u;
    uint32_t old_wifi_netmask = 0u;
    uint32_t old_wifi_gateway = 0u;
    uint32_t old_wifi_dns = 0u;
    if (before != nullptr) {
        strncpy(old_wifi_ssid, before->wifi_ssid, sizeof(old_wifi_ssid) - 1u);
        strncpy(old_wifi_password, before->wifi_password, sizeof(old_wifi_password) - 1u);
        old_wifi_dhcp_enabled = before->wifi_dhcp_enabled;
        old_wifi_ip = before->wifi_ip;
        old_wifi_netmask = before->wifi_netmask;
        old_wifi_gateway = before->wifi_gateway;
        old_wifi_dns = before->wifi_dns;
    }

    if (ok && s_server.hasArg("wifi_ssid")) {
        ok = EXTERNAL_RADIO_SetWifiSsid(s_server.arg("wifi_ssid").c_str(), false);
    }
    if (ok && s_server.hasArg("wifi_password")) {
        ok = EXTERNAL_RADIO_SetWifiPassword(s_server.arg("wifi_password").c_str(), false);
    }
    if (ok && s_server.hasArg("wifi_dhcp_present")) {
        ok = EXTERNAL_RADIO_SetWifiDhcpEnabled(s_server.hasArg("wifi_dhcp_enabled"), false);
    }
    if (ok && s_server.hasArg("wifi_ip") && s_server.arg("wifi_ip").length() > 0) {
        uint32_t value = 0u;
        ok = parseIpArg(s_server.arg("wifi_ip"), &value) &&
             EXTERNAL_RADIO_SetWifiIp(value, false);
    }
    if (ok && s_server.hasArg("wifi_mask") && s_server.arg("wifi_mask").length() > 0) {
        uint32_t value = 0u;
        ok = parseIpArg(s_server.arg("wifi_mask"), &value) &&
             EXTERNAL_RADIO_SetWifiNetmask(value, false);
    }
    if (ok && s_server.hasArg("wifi_gateway") && s_server.arg("wifi_gateway").length() > 0) {
        uint32_t value = 0u;
        ok = parseIpArg(s_server.arg("wifi_gateway"), &value) &&
             EXTERNAL_RADIO_SetWifiGateway(value, false);
    }
    if (ok && s_server.hasArg("wifi_dns") && s_server.arg("wifi_dns").length() > 0) {
        uint32_t value = 0u;
        ok = parseIpArg(s_server.arg("wifi_dns"), &value) &&
             EXTERNAL_RADIO_SetWifiDns(value, false);
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SaveConfig();
    }

    const ExternalRadioConfig *after = EXTERNAL_RADIO_GetConfig();
    if (ok && after != nullptr) {
        if (s_server.hasArg("wifi_ssid") || s_server.hasArg("wifi_password")) {
            Serial.printf("[CFG] WiFi account saved via web: ssid=\"%s\" password=%s\n",
                          after->wifi_ssid, maskSecret(after->wifi_password).c_str());
        }
        if (s_server.hasArg("wifi_dhcp_present") ||
            s_server.hasArg("wifi_ip") ||
            s_server.hasArg("wifi_mask") ||
            s_server.hasArg("wifi_gateway") ||
            s_server.hasArg("wifi_dns")) {
            Serial.printf("[CFG] WiFi network saved via web: dhcp=%s ip=%s mask=%s gateway=%s dns=%s\n",
                          after->wifi_dhcp_enabled ? "on" : "off",
                          ipToString(after->wifi_ip).c_str(),
                          ipToString(after->wifi_netmask).c_str(),
                          ipToString(after->wifi_gateway).c_str(),
                          ipToString(after->wifi_dns).c_str());
        }
        const bool restart_wifi = strcmp(old_wifi_ssid, after->wifi_ssid) != 0 ||
                                  strcmp(old_wifi_password, after->wifi_password) != 0 ||
                                  old_wifi_dhcp_enabled != after->wifi_dhcp_enabled ||
                                  old_wifi_ip != after->wifi_ip ||
                                  old_wifi_netmask != after->wifi_netmask ||
                                  old_wifi_gateway != after->wifi_gateway ||
                                  old_wifi_dns != after->wifi_dns;
        if (restart_wifi) {
            Serial.printf("[CFG] WiFi credentials changed, reconnecting to \"%s\"\n", after->wifi_ssid);
            NRLAudioBridge_ApplyConfig(true, true);
        } else {
            Serial.println("[CFG] WiFi config unchanged, keeping current connection");
        }
    } else if (!ok) {
        Serial.println("[CFG] WiFi config save via web failed (invalid params or EEPROM write error)");
    }

    if (ok) {
        const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
        sendConfigPage("Applied",
                       "WiFi Saved",
                       "wifiAppliedHeadline",
                       "WiFi settings were saved.",
                       "wifiAppliedIntro",
                       "/save_wifi",
                       buildNetworkSection(config),
                       true,
                       false,
                       false,
                       "");
    } else {
        const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
        sendConfigPage("Error",
                       "WiFi Save Failed",
                       "wifiErrorHeadline",
                       "Check the WiFi name and password.",
                       "wifiErrorIntro",
                       "/save_wifi",
                       buildNetworkSection(config),
                       true,
                       false,
                       false,
                       "");
    }
}

static void handleSaveNrl()
{
    bool ok = true;

    const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
    char old_server_host[65] = {};
    uint16_t old_server_port = 0u;
    if (before != nullptr) {
        strncpy(old_server_host, before->server_host, sizeof(old_server_host) - 1u);
        old_server_port = before->server_port;
    }

    if (ok && s_server.hasArg("server_host")) {
        ok = EXTERNAL_RADIO_SetServerHost(s_server.arg("server_host").c_str(), false);
    }
    if (ok && s_server.hasArg("server_port")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("server_port"), &value) &&
             value > 0UL && value <= 65535UL &&
             EXTERNAL_RADIO_SetServerPort(static_cast<uint16_t>(value), false);
    }
    if (ok && s_server.hasArg("channel")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("channel"), &value) &&
             value <= 7UL &&
             EXTERNAL_RADIO_SetChannel(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("callsign")) {
        ok = EXTERNAL_RADIO_SetCallsign(s_server.arg("callsign").c_str(), false);
    }
    if (ok && s_server.hasArg("callsign_ssid")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("callsign_ssid"), &value) &&
             value <= 255UL &&
             EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("mic_volume")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("mic_volume"), &value) &&
             value <= 255UL &&
             EXTERNAL_RADIO_SetMicVolume(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("line_out_volume")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("line_out_volume"), &value) &&
             value <= 255UL &&
             EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("hp_drive_present")) {
        ok = EXTERNAL_RADIO_SetHpDriveEnabled(s_server.hasArg("hp_drive_enabled"), false);
    }
#if defined(NRL_ENABLE_GEZIPAI_AEC) && NRL_ENABLE_GEZIPAI_AEC
    if (ok && s_server.hasArg("aec_present")) {
        ok = EXTERNAL_RADIO_SetAecEnabled(s_server.hasArg("aec_enabled"), false);
    }
#endif
    if (ok && s_server.hasArg("drc_present")) {
        ok = EXTERNAL_RADIO_SetDrcEnabled(s_server.hasArg("drc_enabled"), false);
    }
    if (ok && s_server.hasArg("drc_winsize")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("drc_winsize"), &value) &&
             value <= 15UL &&
             EXTERNAL_RADIO_SetDrcWinsize(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("drc_maxlevel")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("drc_maxlevel"), &value) &&
             value <= 15UL &&
             EXTERNAL_RADIO_SetDrcMaxlevel(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("drc_minlevel")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("drc_minlevel"), &value) &&
             value <= 15UL &&
             EXTERNAL_RADIO_SetDrcMinlevel(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("dac_ramprate")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("dac_ramprate"), &value) &&
             value <= 15UL &&
             EXTERNAL_RADIO_SetDacRamprate(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("dac_eq_bypass_present")) {
        ok = EXTERNAL_RADIO_SetDacEqBypass(s_server.hasArg("dac_eq_bypass"), false);
    }
    if (ok && (s_server.hasArg("daceq_b0") || s_server.hasArg("daceq_b1") || s_server.hasArg("daceq_a1"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint32_t b0 = current != nullptr ? current->daceq_b0 : 0u;
        uint32_t b1 = current != nullptr ? current->daceq_b1 : 0u;
        uint32_t a1 = current != nullptr ? current->daceq_a1 : 0u;
        unsigned long value = 0UL;
        if (ok && s_server.hasArg("daceq_b0")) {
            ok = parseUIntArg(s_server.arg("daceq_b0"), &value) && value <= kDacEqCoefficientMax;
            b0 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("daceq_b1")) {
            ok = parseUIntArg(s_server.arg("daceq_b1"), &value) && value <= kDacEqCoefficientMax;
            b1 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("daceq_a1")) {
            ok = parseUIntArg(s_server.arg("daceq_a1"), &value) && value <= kDacEqCoefficientMax;
            a1 = static_cast<uint32_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetDacEqCoefficients(b0, b1, a1, false);
        }
    }
    if (ok && (s_server.hasArg("adc_system_present") || s_server.hasArg("adc_pga_gain"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        bool dmic = current != nullptr && current->adc_dmic_enabled;
        bool linsel = current == nullptr || current->adc_linsel;
        uint8_t pga = current != nullptr ? current->adc_pga_gain : 10u;
        unsigned long value = 0UL;
        if (s_server.hasArg("adc_dmic_enabled")) {
            dmic = s_server.arg("adc_dmic_enabled") == "1";
        } else if (s_server.hasArg("adc_system_present")) {
            dmic = false;
        }
        if (s_server.hasArg("adc_linsel")) {
            linsel = s_server.arg("adc_linsel") == "1";
        } else if (s_server.hasArg("adc_system_present")) {
            linsel = false;
        }
        if (ok && s_server.hasArg("adc_pga_gain")) {
            ok = parseUIntArg(s_server.arg("adc_pga_gain"), &value) && value <= 10UL;
            pga = static_cast<uint8_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcSystemConfig(dmic, linsel, pga, false);
        }
    }
    if (ok && (s_server.hasArg("adc_ramp_present") || s_server.hasArg("adc_ramprate"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint8_t ramprate = current != nullptr ? current->adc_ramprate : 4u;
        bool dmic_sense = current != nullptr && current->adc_dmic_sense;
        unsigned long value = 0UL;
        if (s_server.hasArg("adc_ramprate")) {
            ok = parseUIntArg(s_server.arg("adc_ramprate"), &value) && value <= 15UL;
            ramprate = static_cast<uint8_t>(value);
        }
        if (s_server.hasArg("adc_dmic_sense")) {
            dmic_sense = s_server.arg("adc_dmic_sense") == "1";
        } else if (s_server.hasArg("adc_ramp_present")) {
            dmic_sense = false;
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcRampConfig(ramprate, dmic_sense, false);
        }
    }
    if (ok && (s_server.hasArg("adc_scale_present") || s_server.hasArg("adc_scale"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        bool sync = current == nullptr || current->adc_sync;
        bool inv = current != nullptr && current->adc_inv;
        bool ramclr = current != nullptr && current->adc_ramclr;
        uint8_t scale = current != nullptr ? current->adc_scale : 4u;
        unsigned long value = 0UL;
        if (s_server.hasArg("adc_sync")) {
            sync = s_server.arg("adc_sync") == "1";
        } else if (s_server.hasArg("adc_scale_present")) {
            sync = false;
        }
        if (s_server.hasArg("adc_inv")) {
            inv = s_server.arg("adc_inv") == "1";
        } else if (s_server.hasArg("adc_scale_present")) {
            inv = false;
        }
        if (s_server.hasArg("adc_ramclr")) {
            ramclr = s_server.arg("adc_ramclr") == "1";
        } else if (s_server.hasArg("adc_scale_present")) {
            ramclr = false;
        }
        if (ok && s_server.hasArg("adc_scale")) {
            ok = parseUIntArg(s_server.arg("adc_scale"), &value) && value <= 7UL;
            scale = static_cast<uint8_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcScaleConfig(sync, inv, ramclr, scale, false);
        }
    }
    if (ok && (s_server.hasArg("alc_present") ||
               s_server.hasArg("alc_winsize") ||
               s_server.hasArg("alc_maxlevel") ||
               s_server.hasArg("alc_minlevel"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        bool alc = current != nullptr && current->alc_enabled;
        bool automute = current != nullptr && current->adc_automute_enabled;
        uint8_t winsize = current != nullptr ? current->alc_winsize : 0u;
        uint8_t maxlevel = current != nullptr ? current->alc_maxlevel : 0u;
        uint8_t minlevel = current != nullptr ? current->alc_minlevel : 0u;
        unsigned long value = 0UL;
        if (s_server.hasArg("alc_enabled")) {
            alc = s_server.arg("alc_enabled") == "1";
        } else if (s_server.hasArg("alc_present")) {
            alc = false;
        }
        if (s_server.hasArg("adc_automute_enabled")) {
            automute = s_server.arg("adc_automute_enabled") == "1";
        } else if (s_server.hasArg("alc_present")) {
            automute = false;
        }
        if (ok && s_server.hasArg("alc_winsize")) {
            ok = parseUIntArg(s_server.arg("alc_winsize"), &value) && value <= 15UL;
            winsize = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("alc_maxlevel")) {
            ok = parseUIntArg(s_server.arg("alc_maxlevel"), &value) && value <= 15UL;
            maxlevel = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("alc_minlevel")) {
            ok = parseUIntArg(s_server.arg("alc_minlevel"), &value) && value <= 15UL;
            minlevel = static_cast<uint8_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAlcConfig(alc, automute, winsize, maxlevel, minlevel, false);
        }
    }
    if (ok && (s_server.hasArg("adc_automute_winsize") ||
               s_server.hasArg("adc_automute_noise_gate") ||
               s_server.hasArg("adc_automute_volume"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint8_t winsize = current != nullptr ? current->adc_automute_winsize : 0u;
        uint8_t noise_gate = current != nullptr ? current->adc_automute_noise_gate : 0u;
        uint8_t volume = current != nullptr ? current->adc_automute_volume : 0u;
        unsigned long value = 0UL;
        if (ok && s_server.hasArg("adc_automute_winsize")) {
            ok = parseUIntArg(s_server.arg("adc_automute_winsize"), &value) && value <= 15UL;
            winsize = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("adc_automute_noise_gate")) {
            ok = parseUIntArg(s_server.arg("adc_automute_noise_gate"), &value) && value <= 15UL;
            noise_gate = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("adc_automute_volume")) {
            ok = parseUIntArg(s_server.arg("adc_automute_volume"), &value) && value <= 7UL;
            volume = static_cast<uint8_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcAutomuteConfig(winsize, noise_gate, volume, false);
        }
    }
    if (ok && (s_server.hasArg("adc_hpf_present") ||
               s_server.hasArg("adc_hpfs1") ||
               s_server.hasArg("adc_hpfs2"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint8_t hpfs1 = current != nullptr ? current->adc_hpfs1 : 10u;
        bool eq_bypass = current == nullptr || current->adc_eq_bypass;
        bool hpf = current == nullptr || current->adc_hpf;
        uint8_t hpfs2 = current != nullptr ? current->adc_hpfs2 : 10u;
        unsigned long value = 0UL;
        if (ok && s_server.hasArg("adc_hpfs1")) {
            ok = parseUIntArg(s_server.arg("adc_hpfs1"), &value) && value <= 31UL;
            hpfs1 = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("adc_hpfs2")) {
            ok = parseUIntArg(s_server.arg("adc_hpfs2"), &value) && value <= 31UL;
            hpfs2 = static_cast<uint8_t>(value);
        }
        if (s_server.hasArg("adc_eq_bypass")) {
            eq_bypass = s_server.arg("adc_eq_bypass") == "1";
        } else if (s_server.hasArg("adc_hpf_present")) {
            eq_bypass = false;
        }
        if (s_server.hasArg("adc_hpf")) {
            hpf = s_server.arg("adc_hpf") == "1";
        } else if (s_server.hasArg("adc_hpf_present")) {
            hpf = false;
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcHpfConfig(hpfs1, eq_bypass, hpf, hpfs2, false);
        }
    }
    if (ok && (s_server.hasArg("adceq_b0") ||
               s_server.hasArg("adceq_a1") ||
               s_server.hasArg("adceq_a2") ||
               s_server.hasArg("adceq_b1") ||
               s_server.hasArg("adceq_b2"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint32_t b0 = current != nullptr ? current->adceq_b0 : 0u;
        uint32_t a1 = current != nullptr ? current->adceq_a1 : 0u;
        uint32_t a2 = current != nullptr ? current->adceq_a2 : 0u;
        uint32_t b1 = current != nullptr ? current->adceq_b1 : 0u;
        uint32_t b2 = current != nullptr ? current->adceq_b2 : 0u;
        unsigned long value = 0UL;
        if (ok && s_server.hasArg("adceq_b0")) {
            ok = parseUIntArg(s_server.arg("adceq_b0"), &value) && value <= kDacEqCoefficientMax;
            b0 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("adceq_a1")) {
            ok = parseUIntArg(s_server.arg("adceq_a1"), &value) && value <= kDacEqCoefficientMax;
            a1 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("adceq_a2")) {
            ok = parseUIntArg(s_server.arg("adceq_a2"), &value) && value <= kDacEqCoefficientMax;
            a2 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("adceq_b1")) {
            ok = parseUIntArg(s_server.arg("adceq_b1"), &value) && value <= kDacEqCoefficientMax;
            b1 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("adceq_b2")) {
            ok = parseUIntArg(s_server.arg("adceq_b2"), &value) && value <= kDacEqCoefficientMax;
            b2 = static_cast<uint32_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcEqCoefficients(b0, a1, a2, b1, b2, false);
        }
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SaveConfig();
    }

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (ok && config != nullptr) {
        if (s_server.hasArg("server_host") || s_server.hasArg("server_port")) {
            Serial.printf("[CFG] NRL server saved via web: server=%s:%u\n",
                          config->server_host,
                          static_cast<unsigned>(config->server_port));
        }
        if (s_server.hasArg("channel") || s_server.hasArg("callsign") || s_server.hasArg("callsign_ssid")) {
            Serial.printf("[CFG] Radio identity saved via web: channel=%u callsign=%s-%u\n",
                          static_cast<unsigned>(config->channel),
                          config->callsign,
                          static_cast<unsigned>(config->callsign_ssid));
        }
        if (s_server.hasArg("mic_volume") || s_server.hasArg("line_out_volume")) {
            Serial.printf("[CFG] Audio volume saved via web: mic_volume=%u line_out_volume=%u\n",
                          static_cast<unsigned>(config->mic_volume),
                          static_cast<unsigned>(config->line_out_volume));
        }
        if (s_server.hasArg("hp_drive_present")) {
            Serial.printf("[CFG] ES8311 HP drive saved via web: hp_drive=%s\n",
                          config->hp_drive_enabled ? "on" : "off");
        }
        if (s_server.hasArg("drc_present") ||
            s_server.hasArg("drc_winsize") ||
            s_server.hasArg("drc_maxlevel") ||
            s_server.hasArg("drc_minlevel") ||
            s_server.hasArg("dac_ramprate")) {
            Serial.printf("[CFG] ES8311 DRC saved via web: enabled=%u winsize=%u maxlevel=%u minlevel=%u ramp=%u\n",
                          config->drc_enabled ? 1u : 0u,
                          static_cast<unsigned>(config->drc_winsize),
                          static_cast<unsigned>(config->drc_maxlevel),
                          static_cast<unsigned>(config->drc_minlevel),
                          static_cast<unsigned>(config->dac_ramprate));
        }
        if (s_server.hasArg("dac_eq_bypass_present") ||
            s_server.hasArg("daceq_b0") ||
            s_server.hasArg("daceq_b1") ||
            s_server.hasArg("daceq_a1")) {
            Serial.printf("[CFG] ES8311 EQ saved via web: bypass=%u b0=%lu b1=%lu a1=%lu\n",
                          config->dac_eq_bypass ? 1u : 0u,
                          static_cast<unsigned long>(config->daceq_b0),
                          static_cast<unsigned long>(config->daceq_b1),
                          static_cast<unsigned long>(config->daceq_a1));
        }
        if (s_server.hasArg("adc_system_present") ||
            s_server.hasArg("adc_pga_gain") ||
            s_server.hasArg("adc_ramp_present") ||
            s_server.hasArg("adc_ramprate") ||
            s_server.hasArg("adc_scale_present") ||
            s_server.hasArg("adc_scale") ||
            s_server.hasArg("alc_present") ||
            s_server.hasArg("alc_winsize") ||
            s_server.hasArg("alc_maxlevel") ||
            s_server.hasArg("alc_minlevel") ||
            s_server.hasArg("adc_automute_winsize") ||
            s_server.hasArg("adc_automute_noise_gate") ||
            s_server.hasArg("adc_automute_volume") ||
            s_server.hasArg("adc_hpf_present") ||
            s_server.hasArg("adc_hpfs1") ||
            s_server.hasArg("adc_hpfs2") ||
            s_server.hasArg("adceq_b0") ||
            s_server.hasArg("adceq_a1") ||
            s_server.hasArg("adceq_a2") ||
            s_server.hasArg("adceq_b1") ||
            s_server.hasArg("adceq_b2")) {
            Serial.printf("[CFG] ES8311 ADC saved via web: reg14=0x%02X reg15=0x%02X reg16=0x%02X alc=%u automute=%u hpf=%u/%u adceq=%lu,%lu,%lu,%lu,%lu\n",
                          static_cast<unsigned>((config->adc_dmic_enabled ? 0x40u : 0x00u) |
                                                (config->adc_linsel ? 0x10u : 0x00u) |
                                                (config->adc_pga_gain & 0x0fu)),
                          static_cast<unsigned>(((config->adc_ramprate & 0x0fu) << 4) |
                                                (config->adc_dmic_sense ? 0x01u : 0x00u)),
                          static_cast<unsigned>((config->adc_sync ? 0x20u : 0x00u) |
                                                (config->adc_inv ? 0x10u : 0x00u) |
                                                (config->adc_ramclr ? 0x08u : 0x00u) |
                                                (config->adc_scale & 0x07u)),
                          config->alc_enabled ? 1u : 0u,
                          config->adc_automute_enabled ? 1u : 0u,
                          static_cast<unsigned>(config->adc_hpfs1),
                          static_cast<unsigned>(config->adc_hpfs2),
                          static_cast<unsigned long>(config->adceq_b0),
                          static_cast<unsigned long>(config->adceq_a1),
                          static_cast<unsigned long>(config->adceq_a2),
                          static_cast<unsigned long>(config->adceq_b1),
                          static_cast<unsigned long>(config->adceq_b2));
        }
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
        const bool restart_udp = strcmp(old_server_host, config->server_host) != 0 ||
                                 old_server_port != config->server_port;
        if (restart_udp) {
            Serial.printf("[CFG] server address changed, rebuilding UDP connection to %s:%u\n",
                          config->server_host, static_cast<unsigned>(config->server_port));
            NRLAudioBridge_ApplyConfig(false, true);
        }
    } else if (!ok) {
        Serial.println("[CFG] NRL config save via web failed (invalid params or EEPROM write error)");
    }

    const bool audio_request = s_server.hasArg("mic_volume") ||
                               s_server.hasArg("line_out_volume") ||
                               s_server.hasArg("hp_drive_present") ||
                               s_server.hasArg("aec_present") ||
                               s_server.hasArg("drc_present") ||
                               s_server.hasArg("drc_winsize") ||
                               s_server.hasArg("drc_maxlevel") ||
                               s_server.hasArg("drc_minlevel") ||
                               s_server.hasArg("dac_ramprate") ||
                               s_server.hasArg("dac_eq_bypass_present") ||
                               s_server.hasArg("daceq_b0") ||
                               s_server.hasArg("daceq_b1") ||
                               s_server.hasArg("daceq_a1") ||
                               s_server.hasArg("adc_system_present") ||
                               s_server.hasArg("adc_pga_gain") ||
                               s_server.hasArg("adc_ramp_present") ||
                               s_server.hasArg("adc_ramprate") ||
                               s_server.hasArg("adc_scale_present") ||
                               s_server.hasArg("adc_scale") ||
                               s_server.hasArg("alc_present") ||
                               s_server.hasArg("alc_winsize") ||
                               s_server.hasArg("alc_maxlevel") ||
                               s_server.hasArg("alc_minlevel") ||
                               s_server.hasArg("adc_automute_winsize") ||
                               s_server.hasArg("adc_automute_noise_gate") ||
                               s_server.hasArg("adc_automute_volume") ||
                               s_server.hasArg("adc_hpf_present") ||
                               s_server.hasArg("adc_hpfs1") ||
                               s_server.hasArg("adc_hpfs2") ||
                               s_server.hasArg("adceq_b0") ||
                               s_server.hasArg("adceq_a1") ||
                               s_server.hasArg("adceq_a2") ||
                               s_server.hasArg("adceq_b1") ||
                               s_server.hasArg("adceq_b2");

    if (ok) {
        sendConfigPage("Applied",
                       audio_request ? "Audio Settings Saved" : "NRL Saved",
                       audio_request ? "audioHeadline" : "nrlAppliedHeadline",
                       audio_request ? "Audio settings were saved." : "NRL settings were saved.",
                       audio_request ? "audioIntro" : "nrlAppliedIntro",
                       "/save_nrl",
                       audio_request ? buildAudioSections(config) : buildDeviceSections(config),
                       false,
                       !audio_request,
                       audio_request,
                       "");
    } else {
        sendConfigPage("Error",
                       audio_request ? "Audio Save Failed" : "NRL Save Failed",
                       "nrlErrorHeadline",
                       "Check server address, port, channel, callsign, SSID, and audio values.",
                       "nrlErrorIntro",
                       "/save_nrl",
                       audio_request ? buildAudioSections(config) : buildDeviceSections(config),
                       false,
                       !audio_request,
                       audio_request,
                       "");
    }
}

static void handleUpdatePage()
{
    s_server.send(200,
                  "text/html; charset=utf-8",
                  buildUpdatePageI18n("Firmware Update",
                                      "updateHeadline",
                                      "Upload a firmware file over this WiFi connection.",
                                      "updateIntro"));
}

static void handleUpdateFinished()
{
    const bool ok = !Update.hasError();
    if (ok) {
        s_update_reboot_pending = true;
        s_update_reboot_at_ms = millis() + 1000UL;
    }

    s_server.send(ok ? 200 : 500,
                  "text/html; charset=utf-8",
                  buildUpdatePageI18n(ok ? "Update Complete" : "Update Failed",
                                      ok ? "updateDoneHeadline" : "updateFailHeadline",
                                      ok ? "Firmware was written successfully. Rebooting shortly."
                                         : "Firmware upload failed. Check that the file is a valid firmware.bin.",
                                      ok ? "updateDoneIntro" : "updateFailIntro"));
}

static void handleUpdateUpload()
{
    HTTPUpload &upload = s_server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] upload start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[OTA] update complete: %u bytes\n", static_cast<unsigned>(upload.totalSize));
        } else {
            Update.printError(Serial);
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Serial.println("[OTA] upload aborted");
    }
}

static void handlePing()
{
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "text/plain", "ok\n");
}

static void handleGenerate204()
{
    redirectToPortal();
}

static void handleHotspotDetect()
{
    redirectToPortal();
}

static void handleConnectTest()
{
    redirectToPortal();
}

static void handleNcsi()
{
    redirectToPortal();
}

static void handleNotFound()
{
    redirectToPortal();
}

static void ensureServerRunning()
{
    if (s_server_started) {
        return;
    }

    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/nrl", HTTP_GET, handleNrlPage);
    s_server.on("/audio", HTTP_GET, handleAudioPage);
    s_server.on("/scan", HTTP_GET, handleScan);
    s_server.on("/save_wifi", HTTP_POST, handleSaveWifi);
    s_server.on("/save_nrl", HTTP_POST, handleSaveNrl);
    s_server.on("/update", HTTP_GET, handleUpdatePage);
    s_server.on("/update", HTTP_POST, handleUpdateFinished, handleUpdateUpload);
    s_server.on("/ping", HTTP_GET, handlePing);
    s_server.on("/favicon.ico", HTTP_GET, handleFavicon);
    s_server.on("/generate_204", HTTP_GET, handleGenerate204);
    s_server.on("/hotspot-detect.html", HTTP_GET, handleHotspotDetect);
    s_server.on("/connecttest.txt", HTTP_GET, handleConnectTest);
    s_server.on("/ncsi.txt", HTTP_GET, handleNcsi);
    s_server.onNotFound(handleNotFound);
    s_server.begin();
    s_server_started = true;
    Serial.println("[CFG] HTTP config server started on port 80");
}

} // namespace

bool WifiConfigPortal_Init(void)
{
    EXTERNAL_RADIO_Init();
    pinMode(NRL_PIN_BOOT_BUTTON, INPUT_PULLUP);
    s_ap_should_run = true;
    ensureApRunning();
    ensureDnsRunning();
    ensureServerRunning();
    return true;
}

void WifiConfigPortal_Poll(void)
{
    ensureApRunning();
    ensureDnsRunning();
    ensureServerRunning();
    pollBootResetGesture();
    manageApLifecycle();
    if (s_dns_started) {
        s_dns.processNextRequest();
    }
    s_server.handleClient();
    if (s_update_reboot_pending && (millis() - s_update_reboot_at_ms) < 0x80000000UL) {
        Serial.println("[OTA] reboot now");
        Serial.flush();
        ESP.restart();
    }
}

void WifiConfigPortal_EnterFallbackMode(void)
{
    WiFi.scanDelete();
    s_ap_should_run = true;
    s_sta_was_connected = false;
    s_ap_close_scheduled = false;
    s_sta_disconnect_started_ms = 0UL;
    ensureApRunning();
    ensureDnsRunning();
    ensureServerRunning();
    Serial.printf("[CFG] Fallback config AP active: ssid=%s ip=%s\n",
                  buildApSsid().c_str(),
                  WiFi.softAPIP().toString().c_str());
}
