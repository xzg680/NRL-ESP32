#include "wifi_config_portal.h"
#include "wifi_config_portal_page.generated.h"
#include "wifi_update_portal_page.generated.h"
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
struct WifiScanEntry {
    String ssid;
    int32_t rssi;
};
const size_t kWifiScanCacheMax = 24u;
WifiScanEntry s_wifi_scan_cache[kWifiScanCacheMax];
int s_wifi_scan_count = 0;
bool s_wifi_prescan_done = false;

constexpr byte kDnsPort = 53;
constexpr unsigned long kBootResetHoldMs = 5000UL;
constexpr unsigned long kApCloseDelayMs = 5000UL;
constexpr unsigned long kApReopenAfterStaDownMs = 60000UL;
constexpr unsigned long kDacEqCoefficientMax = 1073741823UL;
const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApGateway(192, 168, 4, 1);
const IPAddress kApSubnet(255, 255, 255, 0);

static String formatHex32(const uint32_t value)
{
    char buffer[12];
    snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(value));
    return String(buffer);
}

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

static String wifiDisplayIp(const ExternalRadioConfig *config,
                            const uint32_t configured_value,
                            const IPAddress &dhcp_value)
{
    if (config != nullptr && config->wifi_dhcp_enabled && WiFi.status() == WL_CONNECTED) {
        return dhcp_value.toString();
    }
    return ipToString(configured_value);
}

static String buildDacEqSlider(const char *field_name, const char *label, const uint32_t value)
{
    String html = F("<form class=\"item-form eq-control\" method=\"post\" action=\"/save_nrl\"><label>");
    html += label;
    html += F("</label><div class=\"eq-row\"><input class=\"eq-slider\" type=\"range\" min=\"0\" max=\"1073741823\" step=\"1\" name=\"");
    html += field_name;
    html += F("\" value=\"");
    html += String(value);
    html += F("\" oninput=\"syncEqValue(this)\" onpointerup=\"submitEqSlider(this)\"><input class=\"eq-value mono\" type=\"text\" value=\"");
    html += formatHex32(value);
    html += F("\" oninput=\"syncEqSlider(this)\"></div></form>");
    return html;
}

static String buildAutoSubmitSlider(const char *field_name,
                                    const char *label,
                                    const char *i18n_key,
                                    const uint32_t min_value,
                                    const uint32_t max_value,
                                    const uint32_t value)
{
    String html = F("<form class=\"item-form\" method=\"post\" action=\"/save_nrl\"><label");
    if (i18n_key != nullptr && i18n_key[0] != '\0') {
        html += F(" data-i18n=\"");
        html += i18n_key;
        html += F("\"");
    }
    html += F(">");
    html += label;
    html += F("</label><div class=\"eq-row\"><input class=\"auto-slider\" type=\"range\" name=\"");
    html += field_name;
    html += F("\" min=\"");
    html += String(min_value);
    html += F("\" max=\"");
    html += String(max_value);
    html += F("\" step=\"1\" value=\"");
    html += String(value);
    html += F("\" oninput=\"syncAutoSliderValue(this)\" onpointerup=\"submitAutoSlider(this)\" onchange=\"syncAutoSliderValue(this)\"><input class=\"eq-value mono\" type=\"number\" value=\"");
    html += String(value);
    html += F("\" readonly tabindex=\"-1\"></div></form>");
    return html;
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
    const int found = WiFi.scanNetworks(false /*async*/, true /*show_hidden*/);
    s_wifi_scan_count = 0;
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
    if (found >= 0) {
        WiFi.scanDelete();
    }
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
    String html = F("<section class=\"panel\">"
                    "<div class=\"section-head\">"
                    "<h2 data-i18n=\"network\">Network</h2>"
                    "<span class=\"hint\" id=\"scan-status\" data-i18n=\"scanIdle\">Click Scan to find nearby WiFi networks.</span>"
                    "</div>"
                    "<div class=\"grid\">"
                    "<form class=\"item-form span-2\" method=\"post\" action=\"/save_wifi\"><div class=\"subgrid\">"
                    "<div><label data-i18n=\"wifiSsid\">WiFi SSID</label><div class=\"row\">"
                    "<input id=\"wifi-ssid-input\" name=\"wifi_ssid\" list=\"wifi-ssid-options\" autocomplete=\"off\" value=\"");
    html += htmlEscape(config->wifi_ssid);
    html += F("\"><button class=\"secondary btn-small icon-btn\" type=\"button\" onclick=\"scanWifi()\" data-i18n-title=\"scan\" title=\"Scan\" aria-label=\"Scan\">&#x21BB;</button>"
              "</div><datalist id=\"wifi-ssid-options\">");
    for (int i = 0; i < s_wifi_scan_count; ++i) {
        const String escaped = htmlEscape(s_wifi_scan_cache[i].ssid.c_str());
        html += F("<option value=\"");
        html += escaped;
        html += F("\" label=\"(");
        html += String(s_wifi_scan_cache[i].rssi);
        html += F(" dBm)\"></option>");
    }
    html += F("</datalist></div>"
              "<div><label data-i18n=\"wifiPassword\">WiFi Password</label><input name=\"wifi_password\" value=\"");
    html += htmlEscape(config->wifi_password);
    html += F("\"></div></div><div class=\"actions\"><button class=\"btn-small\" type=\"submit\" data-i18n=\"saveItem\">Save</button></div></form>"
              "<form class=\"item-form span-2\" method=\"post\" action=\"/save_wifi\"><div class=\"subgrid\">"
              "<div><label>DHCP</label><input type=\"hidden\" name=\"wifi_dhcp_present\" value=\"1\"><label class=\"hint\"><input id=\"wifi-dhcp-enabled\" type=\"checkbox\" name=\"wifi_dhcp_enabled\" value=\"1\" onchange=\"syncDhcpFields()\" ");
    if (config->wifi_dhcp_enabled) {
        html += F("checked");
    }
    html += F("><span>Use DHCP for station IP</span></label></div>"
              "<div><label>WiFi IP</label><input class=\"wifi-static-field\" name=\"wifi_ip\" value=\"");
    html += wifiDisplayIp(config, config->wifi_ip, WiFi.localIP());
    html += F("\"></div>"
              "<div><label>WiFi Mask</label><input class=\"wifi-static-field\" name=\"wifi_mask\" value=\"");
    html += wifiDisplayIp(config, config->wifi_netmask, WiFi.subnetMask());
    html += F("\"></div>"
              "<div><label>WiFi Gateway</label><input class=\"wifi-static-field\" name=\"wifi_gateway\" value=\"");
    html += wifiDisplayIp(config, config->wifi_gateway, WiFi.gatewayIP());
    html += F("\"></div>"
              "<div><label>WiFi DNS</label><input class=\"wifi-static-field\" name=\"wifi_dns\" value=\"");
    html += wifiDisplayIp(config, config->wifi_dns, WiFi.dnsIP());
    html += F("\"></div></div><div class=\"actions\"><button class=\"btn-small\" type=\"submit\" data-i18n=\"saveItem\">Save</button></div></form>"
              "</div>"
              "</section>");
    return html;
}

static String buildDeviceSections(const ExternalRadioConfig *config)
{
    String html = F("<section class=\"panel\">"
                    "<div class=\"section-head\"><h2 data-i18n=\"server\">Server</h2></div>"
                    "<div class=\"grid\">"
                    "<form class=\"item-form span-2\" method=\"post\" action=\"/save_nrl\"><div class=\"subgrid\"><div><label data-i18n=\"serverHost\">Server Host / IP</label><input name=\"server_host\" value=\"");
    html += htmlEscape(config->server_host);
    html += F("\"></div>"
              "<div><label data-i18n=\"serverPort\">Server Port</label><input name=\"server_port\" value=\"");
    html += String(config->server_port);
    html += F("\"></div></div><div class=\"actions\"><button class=\"btn-small\" type=\"submit\" data-i18n=\"saveItem\">Save</button></div></form>"
              "</div></section>"
              "<section class=\"panel\">"
              "<div class=\"section-head\"><h2 data-i18n=\"radio\">Radio</h2></div>"
              "<div class=\"grid\">"
              "<form class=\"item-form span-2\" method=\"post\" action=\"/save_nrl\"><div class=\"subgrid\"><div class=\"span-2\"><label data-i18n=\"channel\">Channel (0-7)</label><input name=\"channel\" value=\"");
    html += String(config->channel);
    html += F("\"></div>"
              "<div><label data-i18n=\"callsign\">Callsign</label><input name=\"callsign\" value=\"");
    html += htmlEscape(config->callsign);
    html += F("\"></div>"
              "<div><label data-i18n=\"callsignSsid\">Callsign SSID</label><input name=\"callsign_ssid\" value=\"");
    html += String(config->callsign_ssid);
    html += F("\"></div></div><div class=\"actions\"><button class=\"btn-small\" type=\"submit\" data-i18n=\"saveItem\">Save</button></div></form>"
              "</div></section>");
    return html;
}

static String buildAudioSections(const ExternalRadioConfig *config)
{
    String html = F("<section class=\"panel\">"
                    "<div class=\"section-head\"><h2 data-i18n=\"audio\">Audio</h2></div>"
                    "<div class=\"grid\">"
              "<div class=\"subpanel\"><h3>Output</h3><div class=\"subgrid\">"
              "<form class=\"item-form\" method=\"post\" action=\"/save_nrl\"><label data-i18n=\"hpDrive\">ES8311 HP Drive</label><input type=\"hidden\" name=\"hp_drive_present\" value=\"1\"><label class=\"hint\">"
              "<input type=\"checkbox\" name=\"hp_drive_enabled\" value=\"1\" onchange=\"submitSwitch(this)\" ");
    if (config->hp_drive_enabled) {
        html += F("checked");
    }
    html += F("><span data-i18n=\"hpDriveText\">Enable headphone output drive (REG13 HPSW)</span></label></form></div></div>"
              "<div class=\"subpanel\"><h3>Volume</h3><div class=\"subgrid\">");
    html += buildAutoSubmitSlider("mic_volume", "Mic Volume (0-255)", "micVolume", 0u, 255u, config->mic_volume);
    html += buildAutoSubmitSlider("line_out_volume", "Line Out Volume (0-255)", "lineOutVolume", 0u, 255u, config->line_out_volume);
    html += F("</div></div>"
              "<div class=\"subpanel\"><h3>DRC</h3><div class=\"subgrid\">"
              "<form class=\"item-form span-2\" method=\"post\" action=\"/save_nrl\"><label>ES8311 DRC Enable</label><input type=\"hidden\" name=\"drc_present\" value=\"1\"><label class=\"hint\">"
              "<input type=\"checkbox\" name=\"drc_enabled\" value=\"1\" onchange=\"submitSwitch(this)\" ");
    if (config->drc_enabled) {
        html += F("checked");
    }
    html += F("><span>Enable DAC DRC (REG34 bit7)</span></label></form>");
    html += buildAutoSubmitSlider("drc_winsize", "DRC Window Size (0-15)", nullptr, 0u, 15u, config->drc_winsize);
    html += buildAutoSubmitSlider("drc_maxlevel", "DRC Max Level (0-15)", nullptr, 0u, 15u, config->drc_maxlevel);
    html += buildAutoSubmitSlider("drc_minlevel", "DRC Min Level (0-15)", nullptr, 0u, 15u, config->drc_minlevel);
    html += buildAutoSubmitSlider("dac_ramprate", "DAC Ramp Rate (0-15)", nullptr, 0u, 15u, config->dac_ramprate);
    html += F("</div></div>"
              "<div class=\"subpanel\"><h3>EQ</h3><div class=\"subgrid\">"
              "<form class=\"item-form\" method=\"post\" action=\"/save_nrl\"><label>DAC EQ Bypass</label><input type=\"hidden\" name=\"dac_eq_bypass_present\" value=\"1\"><label class=\"hint\">"
              "<input type=\"checkbox\" name=\"dac_eq_bypass\" value=\"1\" onchange=\"submitSwitch(this)\" ");
    if (config->dac_eq_bypass) {
        html += F("checked");
    }
    html += F("><span>Bypass DAC EQ (REG37 bit3)</span></label></form>"
              "<div class=\"span-2 eq-panel\"><div class=\"section-head\"><h2>DAC EQ Coefficients</h2>"
              "<span class=\"hint\">30-bit unsigned register values for B0/B1/A1</span></div>"
              "<div class=\"eq-presets\">"
              "<button class=\"secondary btn-small\" type=\"button\" onclick=\"applyDacEqPreset('flat')\">Flat</button>"
              "<button class=\"secondary btn-small\" type=\"button\" onclick=\"applyDacEqPreset('neutral')\">Neutral EQ</button>"
              "<button class=\"secondary btn-small\" type=\"button\" onclick=\"applyDacEqPreset('voice_bright')\">Voice Bright</button>"
              "<button class=\"secondary btn-small\" type=\"button\" onclick=\"applyDacEqPreset('voice_soft')\">Voice Soft</button>"
              "<button class=\"secondary btn-small\" type=\"button\" onclick=\"applyDacEqPreset('low_cut')\">Low Cut</button>"
              "</div>");
    html += buildDacEqSlider("daceq_b0", "DACEQ B0 (REG38-3B)", config->daceq_b0);
    html += buildDacEqSlider("daceq_b1", "DACEQ B1 (REG3C-3F)", config->daceq_b1);
    html += buildDacEqSlider("daceq_a1", "DACEQ A1 (REG40-43)", config->daceq_a1);
    html += F("</div></div></div>"
              "</div>"
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
                           const bool audio_active,
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
    replaceToken(html, "{{AUDIO_ACTIVE}}", audio_active ? "active" : "");
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
                               s_server.hasArg("drc_present") ||
                               s_server.hasArg("drc_winsize") ||
                               s_server.hasArg("drc_maxlevel") ||
                               s_server.hasArg("drc_minlevel") ||
                               s_server.hasArg("dac_ramprate") ||
                               s_server.hasArg("dac_eq_bypass_present") ||
                               s_server.hasArg("daceq_b0") ||
                               s_server.hasArg("daceq_b1") ||
                               s_server.hasArg("daceq_a1");

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

static String buildUpdatePageI18n(const char *headline,
                                  const char *headline_key,
                                  const char *intro,
                                  const char *intro_key)
{
    String html = String(kWifiUpdatePortalHtmlTemplate);
    replaceToken(html, "{{HEADLINE}}", headline);
    replaceToken(html, "{{HEADLINE_KEY}}", headline_key);
    replaceToken(html, "{{INTRO}}", intro);
    replaceToken(html, "{{INTRO_KEY}}", intro_key);
    replaceToken(html, "{{AP_IP}}", WiFi.softAPIP().toString());
    replaceToken(html, "{{STA_IP}}", (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("not connected"));
    replaceToken(html, "{{VERSION}}", NRL_FIRMWARE_VERSION);
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

