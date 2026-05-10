#include "wifi_config_portal.h"
#include "wifi_config_portal_page.generated.h"
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

constexpr byte kDnsPort = 53;
constexpr unsigned long kBootResetHoldMs = 5000UL;
constexpr unsigned long kApCloseDelayMs = 5000UL;
constexpr unsigned long kApReopenAfterStaDownMs = 60000UL;
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
    if (!s_ap_should_run || s_ap_started) {
        return;
    }

    WiFi.persistent(false);
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

static bool ensureScanReady()
{
    const wifi_mode_t mode = WiFi.getMode();
    if (mode != WIFI_MODE_APSTA && mode != WIFI_MODE_STA) {
        WiFi.mode(WIFI_AP_STA);
        delay(120);
    }

    return WiFi.getMode() == WIFI_MODE_APSTA || WiFi.getMode() == WIFI_MODE_STA;
}

static void restoreApOnlyAfterScan()
{
    if (WiFi.getMode() == WIFI_MODE_APSTA) {
        WiFi.scanDelete();
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
    String html = F("<section class=\"panel\">"
                    "<div class=\"section-head\">"
                    "<h2 data-i18n=\"network\">Network</h2>"
                    "<span class=\"hint\" id=\"scan-status\" data-i18n=\"scanIdle\">Click Scan to find nearby WiFi networks.</span>"
                    "</div>"
                    "<div class=\"grid\">"
                    "<div><label data-i18n=\"wifiSsid\">WiFi SSID</label><input id=\"wifi-ssid-input\" name=\"wifi_ssid\" value=\"");
    html += htmlEscape(config->wifi_ssid);
    html += F("\" autocomplete=\"off\"></div>"
              "<div><label data-i18n=\"wifiPassword\">WiFi Password</label><input name=\"wifi_password\" value=\"");
    html += htmlEscape(config->wifi_password);
    html += F("\"></div>"
              "<div class=\"span-2\"><label data-i18n=\"nearbyWifi\">Nearby WiFi</label><div class=\"row\">"
              "<select id=\"wifi-ssid-select\" onchange=\"applySelectedWifi()\">"
              "<option value=\"\" data-i18n=\"selectWifi\">Select detected WiFi...</option>"
              "</select>"
              "<button class=\"secondary btn-small\" type=\"button\" onclick=\"scanWifi()\" data-i18n=\"scan\">Scan</button>"
              "</div></div>"
              "</div>"
              "<div class=\"actions\"><button type=\"submit\" data-i18n=\"saveWifi\">Save WiFi Config</button></div>"
              "</section>");
    return html;
}

static String buildDeviceSections(const ExternalRadioConfig *config)
{
    String html = F("<section class=\"panel\">"
                    "<div class=\"section-head\"><h2 data-i18n=\"server\">Server</h2></div>"
                    "<div class=\"grid\">"
                    "<div><label data-i18n=\"serverHost\">Server Host / IP</label><input name=\"server_host\" value=\"");
    html += htmlEscape(config->server_host);
    html += F("\"></div>"
              "<div><label data-i18n=\"serverPort\">Server Port</label><input name=\"server_port\" value=\"");
    html += String(config->server_port);
    html += F("\"></div>"
              "</div></section>"
              "<section class=\"panel\">"
              "<div class=\"section-head\"><h2 data-i18n=\"radio\">Radio</h2></div>"
              "<div class=\"grid\">"
              "<div><label data-i18n=\"channel\">Channel (0-7)</label><input name=\"channel\" value=\"");
    html += String(config->channel);
    html += F("\"></div>"
              "<div><label data-i18n=\"callsign\">Callsign</label><input name=\"callsign\" value=\"");
    html += htmlEscape(config->callsign);
    html += F("\"></div>"
              "<div><label data-i18n=\"callsignSsid\">Callsign SSID</label><input name=\"callsign_ssid\" value=\"");
    html += String(config->callsign_ssid);
    html += F("\"></div>"
              "<div><label data-i18n=\"hpDrive\">ES8311 HP Drive</label><label class=\"hint\">"
              "<input type=\"checkbox\" name=\"hp_drive_enabled\" value=\"1\" ");
    if (config->hp_drive_enabled) {
        html += F("checked");
    }
    html += F("><span data-i18n=\"hpDriveText\">Enable headphone output drive (REG13 HPSW)</span></label></div>"
              "</div></section>"
              "<section class=\"panel\">"
              "<div class=\"section-head\"><h2 data-i18n=\"audio\">Audio</h2></div>"
              "<div class=\"grid\">"
              "<div><label data-i18n=\"micVolume\">Mic Volume (0-255)</label><input name=\"mic_volume\" value=\"");
    html += String(config->mic_volume);
    html += F("\"></div>"
              "<div><label data-i18n=\"lineOutVolume\">Line Out Volume (0-255)</label><input name=\"line_out_volume\" value=\"");
    html += String(config->line_out_volume);
    html += F("\"></div>"
              "</div>"
              "<div class=\"actions\"><button type=\"submit\" data-i18n=\"saveNrl\">Save NRL Config</button></div>"
              "</section>");
    return html;
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
                           const String &footer)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    String html = String(kWifiConfigPortalHtmlTemplate);
    replaceToken(html, "{{TITLE}}", title);
    replaceToken(html, "{{HEADLINE}}", headline);
    replaceToken(html, "{{HEADLINE_KEY}}", headline_key);
    replaceToken(html, "{{INTRO}}", intro);
    replaceToken(html, "{{INTRO_KEY}}", intro_key);
    replaceToken(html, "{{NETWORK_ACTIVE}}", network_active ? "active" : "");
    replaceToken(html, "{{DEVICE_ACTIVE}}", device_active ? "active" : "");
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
    replaceToken(html, "{{FORM_ACTION}}", form_action);
    replaceToken(html, "{{FORM_SECTIONS}}", form_sections);
    replaceToken(html, "{{FOOTER}}", footer);
    replaceToken(html, "{{VERSION}}", NRL_FIRMWARE_VERSION);
    s_server.send(200, "text/html; charset=utf-8", html);
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
                   "");
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

static void handleSaveWifi()
{
    bool ok = true;

    const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
    char old_wifi_ssid[33] = {};
    char old_wifi_password[65] = {};
    if (before != nullptr) {
        strncpy(old_wifi_ssid, before->wifi_ssid, sizeof(old_wifi_ssid) - 1u);
        strncpy(old_wifi_password, before->wifi_password, sizeof(old_wifi_password) - 1u);
    }

    if (ok) {
        ok = EXTERNAL_RADIO_SetWifiSsid(s_server.arg("wifi_ssid").c_str(), false);
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SetWifiPassword(s_server.arg("wifi_password").c_str(), false);
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SaveConfig();
    }

    const ExternalRadioConfig *after = EXTERNAL_RADIO_GetConfig();
    if (ok && after != nullptr) {
        const bool restart_wifi = strcmp(old_wifi_ssid, after->wifi_ssid) != 0 ||
                                  strcmp(old_wifi_password, after->wifi_password) != 0;
        if (restart_wifi) {
            NRLAudioBridge_ApplyConfig(true, true);
        }
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

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (ok && config != nullptr) {
        ES8311_ApplyAudioConfig(config->mic_volume, config->line_out_volume, config->hp_drive_enabled);
        const bool restart_udp = strcmp(old_server_host, config->server_host) != 0 ||
                                 old_server_port != config->server_port;
        if (restart_udp) {
            NRLAudioBridge_ApplyConfig(false, true);
        }
    }

    if (ok) {
        sendConfigPage("Applied",
                       "NRL Saved",
                       "nrlAppliedHeadline",
                       "NRL, server, and audio settings were saved.",
                       "nrlAppliedIntro",
                       "/save_nrl",
                       buildDeviceSections(config),
                       false,
                       true,
                       "");
    } else {
        sendConfigPage("Error",
                       "NRL Save Failed",
                       "nrlErrorHeadline",
                       "Check server address, port, channel, callsign, SSID, and audio values.",
                       "nrlErrorIntro",
                       "/save_nrl",
                       buildDeviceSections(config),
                       false,
                       true,
                       "");
    }
}

static String buildUpdatePageI18n(const char *headline,
                                  const char *headline_key,
                                  const char *intro,
                                  const char *intro_key)
{
    String html = F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                    "<title>Firmware Update</title>"
                    "<style>"
                    ":root{color-scheme:light;--bg:#eef2f5;--panel:#fff;--ink:#17202a;--muted:#5d6875;--line:#d8e0e8;--accent:#0f766e;--accent-2:#2457a6;}"
                    "*{box-sizing:border-box;}body{margin:0;background:var(--bg);color:var(--ink);font-family:Arial,Helvetica,sans-serif;font-size:15px;}"
                    ".shell{max-width:1040px;margin:0 auto;padding:20px;}.topbar{display:flex;align-items:flex-start;justify-content:space-between;gap:16px;margin-bottom:14px;}"
                    "h1{margin:0;font-size:26px;line-height:1.18;letter-spacing:0;}.sub{margin:6px 0 0;color:var(--muted);font-size:14px;}.language{margin-top:12px;}.language .lang-label{display:block;margin:0 0 6px;font-weight:700;font-size:13px;}.language .lang-radio{display:flex;gap:14px;flex-wrap:wrap;}.language .lang-radio label{display:inline-flex;align-items:center;margin:0;font-weight:400;font-size:14px;cursor:pointer;}input[type=radio]{width:auto;min-height:0;margin-right:6px;}"
                    ".status{display:grid;grid-template-columns:repeat(2,minmax(150px,1fr));gap:8px;min-width:320px;}.status div{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:10px 12px;}.status span{display:block;color:var(--muted);font-size:12px;margin-bottom:3px;}"
                    ".mono{font-family:Consolas,monospace;}.nav{display:flex;gap:8px;margin:0 0 14px;border-bottom:1px solid var(--line);}.nav a{display:inline-block;padding:10px 12px;text-decoration:none;color:var(--muted);border-bottom:3px solid transparent;font-weight:700;}.nav a.active{color:var(--accent);border-bottom-color:var(--accent);}"
                    ".panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:18px;margin-bottom:14px;}h2{margin:0 0 12px;font-size:17px;letter-spacing:0;}label{display:block;margin:0 0 6px;font-weight:700;font-size:13px;}"
                    "input,select{width:100%;min-height:40px;padding:9px 10px;border:1px solid #c5ced8;border-radius:6px;font-size:15px;background:#fff;color:var(--ink);}.actions{display:flex;gap:10px;align-items:center;justify-content:flex-end;flex-wrap:wrap;margin-top:16px;}"
                    "button,.button{display:inline-flex;align-items:center;justify-content:center;min-height:40px;padding:10px 14px;border:0;border-radius:6px;background:var(--accent);color:#fff;font-size:14px;font-weight:700;text-decoration:none;cursor:pointer;}button:disabled{cursor:not-allowed;opacity:.6;}.button.secondary{background:#e8edf3;color:#17202a;border:1px solid var(--line);}.notice{border-left:4px solid var(--accent-2);padding:10px 12px;background:#edf4ff;color:#25364c;border-radius:4px;margin:0 0 14px;}"
                    ".prog-wrap{margin-top:14px;}.prog-bar{height:14px;background:#e8edf3;border-radius:7px;overflow:hidden;border:1px solid var(--line);}.prog-bar>div{height:100%;width:0;background:var(--accent);transition:width .15s linear;}.prog-bar>div.done{background:#16a34a;}.prog-bar>div.failed{background:#b42318;}.prog-status{margin:8px 0 0;font-size:13px;font-weight:700;color:var(--ink);}"
                    "@media(max-width:760px){.shell{padding:14px;}.topbar{display:block;}.status{grid-template-columns:1fr;min-width:0;margin-top:12px;}.actions{justify-content:stretch;}button,.button{width:100%;}}"
                    "</style></head><body><main class=\"shell\"><div class=\"topbar\"><div><h1 data-i18n=\"");
    html += headline_key;
    html += F("\">");
    html += headline;
    html += F("</h1><p class=\"sub\" data-i18n=\"");
    html += intro_key;
    html += F("\">");
    html += intro;
    html += F("</p><div class=\"language\"><span class=\"lang-label\" data-i18n=\"language\">Language</span>"
              "<div id=\"language-select\" class=\"lang-radio\">"
              "<label><input type=\"radio\" name=\"lang\" value=\"en\">English</label>"
              "<label><input type=\"radio\" name=\"lang\" value=\"zh\">中文</label>"
              "</div></div>"
              "</div><div class=\"status\"><div><span data-i18n=\"configAp\">Config AP</span><strong class=\"mono\">");
    html += WiFi.softAPIP().toString();
    html += F("</strong></div><div><span data-i18n=\"stationIp\">Station IP</span><strong class=\"mono\">");
    html += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("not connected");
    html += F("</strong></div></div></div>"
              "<nav class=\"nav\"><a href=\"/\" data-i18n=\"wifiConfig\">WiFi Config</a><a href=\"/nrl\" data-i18n=\"nrlConfig\">NRL Config</a><a class=\"active\" href=\"/update\" data-i18n=\"firmwareUpdate\">Firmware Update</a></nav>"
              "<p class=\"notice\"><span data-i18n=\"firmwareVersion\">Firmware version</span> ");
    html += NRL_FIRMWARE_VERSION;
    html += F("</p><p class=\"notice\" data-i18n=\"otaNotice\">Select a firmware.bin file from support or from a trusted release. The device will reboot after a successful update.</p>"
              "<section class=\"panel\"><h2 data-i18n=\"otaTitle\">WiFi Firmware Update</h2>"
              "<form id=\"ota-form\" enctype=\"multipart/form-data\">"
              "<input id=\"ota-file\" type=\"file\" name=\"firmware\" accept=\".bin,application/octet-stream\" required>"
              "<div class=\"actions\"><button id=\"ota-submit\" type=\"submit\" data-i18n=\"uploadFirmware\">Upload firmware</button></div>"
              "<div id=\"ota-progress\" class=\"prog-wrap\" hidden>"
              "<div class=\"prog-bar\"><div id=\"ota-bar\"></div></div>"
              "<p id=\"ota-status\" class=\"prog-status\"></p>"
              "</div>"
              "</form></section></main><script>"
              "const translations={en:{language:'Language',wifiConfig:'WiFi Config',nrlConfig:'NRL Config',firmwareUpdate:'Firmware Update',configAp:'Config AP',stationIp:'Station IP',firmwareVersion:'Firmware version',updateHeadline:'Firmware Update',updateIntro:'Upload a firmware file over this WiFi connection.',updateDoneHeadline:'Update Complete',updateDoneIntro:'Firmware was written successfully. Rebooting shortly.',updateFailHeadline:'Update Failed',updateFailIntro:'Firmware upload failed. Check that the file is a valid firmware.bin.',otaNotice:'Select a firmware.bin file from support or from a trusted release. The device will reboot after a successful update.',otaTitle:'WiFi Firmware Update',uploadFirmware:'Upload firmware',otaUploading:'Uploading firmware...',otaInstalling:'Installing firmware, please wait...',otaDone:'Update complete. Device is rebooting.',otaRebooting:'Waiting for device to come back online...',otaRebooted:'Device is back online. Update finished.',otaRebootTimeout:'Could not reach the device. Please check it manually.',otaFailed:'Update failed. Please try again.'},zh:{language:'语言',wifiConfig:'WiFi配置',nrlConfig:'NRL配置',firmwareUpdate:'固件升级',configAp:'配置热点',stationIp:'联网地址',firmwareVersion:'固件版本',updateHeadline:'固件升级',updateIntro:'通过当前 WiFi 配置页面上传固件文件。',updateDoneHeadline:'升级完成',updateDoneIntro:'固件写入成功，设备即将重启。',updateFailHeadline:'升级失败',updateFailIntro:'固件上传失败，请确认文件是有效的 firmware.bin。',otaNotice:'请选择来自官方发布或售后支持的 firmware.bin 文件。升级成功后设备会自动重启。',otaTitle:'WiFi固件升级',uploadFirmware:'上传固件',otaUploading:'正在上传固件...',otaInstalling:'正在升级，请稍候...',otaDone:'升级完成，设备正在重启。',otaRebooting:'正在等待设备重新启动...',otaRebooted:'设备已重启完成，升级成功。',otaRebootTimeout:'无法连接到设备，请手动检查。',otaFailed:'升级失败，请重试。'}};"
              "function lang(){const s=localStorage.getItem('nrl_lang');if(s==='zh'||s==='en')return s;return navigator.language&&navigator.language.toLowerCase().startsWith('zh')?'zh':'en';}"
              "function t(k){const l=lang();return (translations[l]&&translations[l][k])||translations.en[k]||k;}"
              "function applyLanguage(l){localStorage.setItem('nrl_lang',l);document.documentElement.lang=l==='zh'?'zh-CN':'en';document.querySelectorAll('input[name=\"lang\"]').forEach((r)=>{r.checked=(r.value===l);});document.querySelectorAll('[data-i18n]').forEach((el)=>{const k=el.getAttribute('data-i18n');if(translations[l]&&translations[l][k])el.textContent=translations[l][k];});if(otaCurrentKey){setOta(otaCurrentKey,otaCurrentPct);}}"
              "let otaCurrentKey=null,otaCurrentPct=null;"
              "const otaForm=document.getElementById('ota-form'),otaFile=document.getElementById('ota-file'),otaSubmit=document.getElementById('ota-submit'),otaProgress=document.getElementById('ota-progress'),otaBar=document.getElementById('ota-bar'),otaStatus=document.getElementById('ota-status');"
              "function setOta(k,p){otaCurrentKey=k;otaCurrentPct=(p==null?null:p);otaStatus.removeAttribute('data-i18n');otaStatus.textContent=(p==null)?t(k):(t(k)+' '+p+'%');}"
              "function pollAlive(){let n=0;const max=30;const tick=()=>{if(n>=max){setOta('otaRebootTimeout',null);return;}n++;const c=new AbortController();const tm=setTimeout(()=>c.abort(),1500);fetch('/ping?_='+Date.now(),{cache:'no-store',signal:c.signal}).then((r)=>{clearTimeout(tm);if(r.ok){setOta('otaRebooted',null);}else{setTimeout(tick,2000);}}).catch(()=>{clearTimeout(tm);setTimeout(tick,2000);});};setTimeout(tick,3000);}"
              "if(otaForm){otaForm.addEventListener('submit',function(e){e.preventDefault();const f=otaFile.files&&otaFile.files[0];if(!f)return;otaSubmit.disabled=true;otaProgress.hidden=false;otaBar.classList.remove('done','failed');otaBar.style.width='0%';setOta('otaUploading',0);const fd=new FormData();fd.append('firmware',f);const xhr=new XMLHttpRequest();xhr.upload.onprogress=function(ev){if(ev.lengthComputable){const pct=Math.floor(ev.loaded*100/ev.total);otaBar.style.width=pct+'%';setOta('otaUploading',pct);}};xhr.upload.onload=function(){otaBar.style.width='100%';setOta('otaInstalling',null);};xhr.onload=function(){if(xhr.status===200){otaBar.style.width='100%';otaBar.classList.add('done');setOta('otaRebooting',null);pollAlive();}else{otaBar.classList.add('failed');setOta('otaFailed',null);otaSubmit.disabled=false;}};xhr.onerror=function(){otaBar.classList.add('failed');setOta('otaFailed',null);otaSubmit.disabled=false;};xhr.open('POST','/update');xhr.send(fd);});}"
              "applyLanguage(lang());document.querySelectorAll('input[name=\"lang\"]').forEach((r)=>{r.addEventListener('change',function(){if(this.checked)applyLanguage(this.value);});});"
              "</script></body></html>");
    return html;
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
    s_server.on("/scan", HTTP_GET, handleScan);
    s_server.on("/save_wifi", HTTP_POST, handleSaveWifi);
    s_server.on("/save_nrl", HTTP_POST, handleSaveNrl);
    s_server.on("/update", HTTP_GET, handleUpdatePage);
    s_server.on("/update", HTTP_POST, handleUpdateFinished, handleUpdateUpload);
    s_server.on("/ping", HTTP_GET, handlePing);
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
