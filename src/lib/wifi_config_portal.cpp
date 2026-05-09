#include "wifi_config_portal.h"
#include "wifi_config_portal_page.generated.h"
#include "nrl_audio_bridge.h"

#include "../app/driver/es8311.h"
#include "../app/driver/external_radio.h"
#include "../app/driver/board_pins.h"

#include <Arduino.h>
#include <DNSServer.h>
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
bool s_boot_pressed = false;
unsigned long s_boot_press_started_ms = 0UL;

constexpr byte kDnsPort = 53;
constexpr unsigned long kBootResetHoldMs = 5000UL;
const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApGateway(192, 168, 4, 1);
const IPAddress kApSubnet(255, 255, 255, 0);

static String htmlEscape(const char *text)
{
    String out;
    if (text == nullptr) {
        return out;
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        switch (text[i]) {
            case '&':
                out += F("&amp;");
                break;
            case '<':
                out += F("&lt;");
                break;
            case '>':
                out += F("&gt;");
                break;
            case '"':
                out += F("&quot;");
                break;
            default:
                out += text[i];
                break;
        }
    }
    return out;
}

static void replaceToken(String &html, const char *token, const String &value)
{
    html.replace(token, value);
}

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
    const String url = String("http://") + WiFi.softAPIP().toString() + "/";
    s_server.sendHeader("Location", url, true);
    s_server.send(302, "text/plain", "");
}

static void ensureApRunning()
{
    if (s_ap_started) {
        return;
    }

    WiFi.persistent(false);
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(50);
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);

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

static bool ensureScanReady()
{
    const wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_MODE_AP) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.setSleep(false);
        delay(120);
    } else if (mode == WIFI_MODE_NULL) {
        ensureApRunning();
        WiFi.mode(WIFI_AP_STA);
        WiFi.setSleep(false);
        delay(120);
    }

    return WiFi.getMode() == WIFI_MODE_APSTA || WiFi.getMode() == WIFI_MODE_STA;
}

static void restoreApOnlyAfterScan()
{
    if (WiFi.getMode() == WIFI_MODE_APSTA) {
        WiFi.scanDelete();
        WiFi.disconnect(false, false);
        WiFi.mode(WIFI_AP);
        WiFi.setSleep(false);
        delay(120);
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
    if (s_dns_started) {
        return;
    }

    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(kDnsPort, "*", WiFi.softAPIP());
    s_dns_started = true;
    Serial.printf("[CFG] DNS captive portal ready on %u\n", static_cast<unsigned>(kDnsPort));
}

static void handleRoot()
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    String html = String(kWifiConfigPortalHtmlTemplate);
    replaceToken(html, "{{TITLE}}", "NRL3188-ESP32 Config");
    replaceToken(html, "{{HEADLINE}}", "NRL3188-ESP32 Setup");
    replaceToken(html, "{{INTRO}}", "Connect to this open hotspot for first-time setup. Save applies immediately.");
    replaceToken(html, "{{AP_IP}}", WiFi.softAPIP().toString());
    replaceToken(html, "{{STA_IP}}", (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("not connected"));
    replaceToken(html, "{{SSID_OPTIONS}}", "");
    replaceToken(html, "{{WIFI_SSID}}", htmlEscape(config->wifi_ssid));
    replaceToken(html, "{{WIFI_PASSWORD}}", htmlEscape(config->wifi_password));
    replaceToken(html, "{{SERVER_HOST}}", htmlEscape(config->server_host));
    replaceToken(html, "{{SERVER_PORT}}", String(config->server_port));
    replaceToken(html, "{{CHANNEL}}", String(config->channel));
    replaceToken(html, "{{CALLSIGN}}", htmlEscape(config->callsign));
    replaceToken(html, "{{CALLSIGN_SSID}}", String(config->callsign_ssid));
    replaceToken(html, "{{MIC_VOLUME}}", String(config->mic_volume));
    replaceToken(html, "{{LINE_OUT_VOLUME}}", String(config->line_out_volume));
    replaceToken(html, "{{HP_DRIVE_CHECKED}}", config->hp_drive_enabled ? "checked" : "");
    replaceToken(html, "{{SUBMIT_LABEL}}", "Save and Apply");
    replaceToken(html, "{{FOOTER}}", "");
    s_server.send(200, "text/html; charset=utf-8", html);
}

static void handleScan()
{
    if (!ensureScanReady()) {
        s_server.send(500, "application/json; charset=utf-8", "[]");
        return;
    }

    int scan_count = WiFi.scanComplete();
    if (scan_count == WIFI_SCAN_RUNNING) {
        WiFi.scanDelete();
        scan_count = WIFI_SCAN_FAILED;
    }

    if (scan_count < 0) {
        scan_count = WiFi.scanNetworks(false, true);
    }

    String json = "[";
    bool first = true;

    for (int i = 0; i < scan_count; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) {
            continue;
        }

        if (!first) {
            json += ",";
        }
        first = false;

        const String escaped_ssid = jsonEscape(ssid);
        json += "{\"ssid\":\"";
        json += escaped_ssid;
        json += "\",\"label\":\"";
        json += escaped_ssid;
        json += " (";
        json += String(WiFi.RSSI(i));
        json += " dBm)\"}";
    }

    json += "]";
    restoreApOnlyAfterScan();
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

static void handleSave()
{
    bool ok = true;
    bool restart_wifi = false;
    bool restart_udp = false;

    const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
    char old_wifi_ssid[33] = {};
    char old_wifi_password[65] = {};
    char old_server_host[65] = {};
    uint16_t old_server_port = 0u;
    if (before != nullptr) {
        strncpy(old_wifi_ssid, before->wifi_ssid, sizeof(old_wifi_ssid) - 1u);
        strncpy(old_wifi_password, before->wifi_password, sizeof(old_wifi_password) - 1u);
        strncpy(old_server_host, before->server_host, sizeof(old_server_host) - 1u);
        old_server_port = before->server_port;
    }

    if (ok) {
        ok = EXTERNAL_RADIO_SetWifiSsid(s_server.arg("wifi_ssid").c_str(), false);
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SetWifiPassword(s_server.arg("wifi_password").c_str(), false);
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SetServerHost(s_server.arg("server_host").c_str(), false);
    }
    if (ok) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("server_port"), &value) &&
             value > 0UL && value <= 65535UL &&
             EXTERNAL_RADIO_SetServerPort(static_cast<uint16_t>(value), false);
    }
    if (ok) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("channel"), &value) &&
             value <= 7UL &&
             EXTERNAL_RADIO_SetChannel(static_cast<uint8_t>(value), false);
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SetCallsign(s_server.arg("callsign").c_str(), false);
    }
    if (ok) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("callsign_ssid"), &value) &&
             value <= 255UL &&
             EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(value), false);
    }
    if (ok) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("mic_volume"), &value) &&
             value <= 255UL &&
             EXTERNAL_RADIO_SetMicVolume(static_cast<uint8_t>(value), false);
    }
    if (ok) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("line_out_volume"), &value) &&
             value <= 255UL &&
             EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(value), false);
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SetHpDriveEnabled(s_server.hasArg("hp_drive_enabled"), false);
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SaveConfig();
    }

    const ExternalRadioConfig *after = EXTERNAL_RADIO_GetConfig();
    if (ok && after != nullptr) {
        restart_wifi = strcmp(old_wifi_ssid, after->wifi_ssid) != 0 ||
                       strcmp(old_wifi_password, after->wifi_password) != 0;
        restart_udp = restart_wifi ||
                      strcmp(old_server_host, after->server_host) != 0 ||
                      old_server_port != after->server_port;
        NRLAudioBridge_ApplyConfig(restart_wifi, restart_udp);
        ES8311_ApplyAudioConfig(after->mic_volume, after->line_out_volume, after->hp_drive_enabled);
    }

    String html = String(kWifiConfigPortalHtmlTemplate);
    if (ok) {
        replaceToken(html, "{{TITLE}}", "Applied");
        replaceToken(html, "{{HEADLINE}}", "Configuration Applied");
        replaceToken(html, "{{INTRO}}", "New settings were saved and applied immediately.");
    } else {
        replaceToken(html, "{{TITLE}}", "Error");
        replaceToken(html, "{{HEADLINE}}", "Save Failed");
        replaceToken(html, "{{INTRO}}", "Please check WiFi name, server address, ports, and channel range.");
    }
    replaceToken(html, "{{AP_IP}}", WiFi.softAPIP().toString());
    replaceToken(html, "{{STA_IP}}", (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("not connected"));
    replaceToken(html, "{{SSID_OPTIONS}}", "");
    replaceToken(html, "{{WIFI_SSID}}", "");
    replaceToken(html, "{{WIFI_PASSWORD}}", "");
    replaceToken(html, "{{SERVER_HOST}}", "");
    replaceToken(html, "{{SERVER_PORT}}", "");
    replaceToken(html, "{{CHANNEL}}", "");
    replaceToken(html, "{{CALLSIGN}}", "");
    replaceToken(html, "{{CALLSIGN_SSID}}", "");
    replaceToken(html, "{{MIC_VOLUME}}", "");
    replaceToken(html, "{{LINE_OUT_VOLUME}}", "");
    replaceToken(html, "{{HP_DRIVE_CHECKED}}", "");
    replaceToken(html, "{{SUBMIT_LABEL}}", "Save and Apply");
    replaceToken(html, "{{FOOTER}}", "<p><a href=\"/\">Back to setup page</a></p>");
    s_server.send(ok ? 200 : 400, "text/html; charset=utf-8", html);
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
    s_server.on("/scan", HTTP_GET, handleScan);
    s_server.on("/save", HTTP_POST, handleSave);
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
    s_dns.processNextRequest();
    s_server.handleClient();
}

void WifiConfigPortal_EnterFallbackMode(void)
{
    WiFi.scanDelete();
    WiFi.disconnect(false, false);
    delay(50);
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    ensureApRunning();
    ensureDnsRunning();
    ensureServerRunning();
    Serial.printf("[CFG] Fallback config AP active: ssid=%s ip=%s\n",
                  buildApSsid().c_str(),
                  WiFi.softAPIP().toString().c_str());
}
