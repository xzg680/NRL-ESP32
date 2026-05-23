#include "wifi_config_portal.h"
#include "wifi_config_portal_view.h"
#include "wifi_portal_assets.generated.h"
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

// Look up the canonical, just-saved value for one form field name. The save
// handler echoes these back so the client can refresh its inputs from device
// truth (post-clamp, post-sanitize) without re-rendering the whole page.
static String savedValueForArg(const ExternalRadioConfig *config, const String &name)
{
    if (config == nullptr) {
        return String();
    }
    if (name == "wifi_ssid") return String(config->wifi_ssid);
    if (name == "wifi_password") return String(config->wifi_password);
    if (name == "wifi_dhcp_enabled") return config->wifi_dhcp_enabled ? "1" : "0";
    if (name == "wifi_ip") return ipToString(config->wifi_ip);
    if (name == "wifi_mask") return ipToString(config->wifi_netmask);
    if (name == "wifi_gateway") return ipToString(config->wifi_gateway);
    if (name == "wifi_dns") return ipToString(config->wifi_dns);
    if (name == "server_host") return String(config->server_host);
    if (name == "server_port") return String(config->server_port);
    if (name == "channel") return String(config->channel);
    if (name == "callsign") return String(config->callsign);
    if (name == "callsign_ssid") return String(config->callsign_ssid);
    if (name == "ptt_timeout") return String(config->ptt_timeout_s);
    if (name == "mic_volume") return String(config->mic_volume);
    if (name == "line_out_volume") return String(config->line_out_volume);
    if (name == "hp_drive_enabled") return config->hp_drive_enabled ? "1" : "0";
    if (name == "aec_enabled") return config->aec_enabled ? "1" : "0";
    if (name == "adc_dmic_enabled") return config->adc_dmic_enabled ? "1" : "0";
    if (name == "adc_linsel") return config->adc_linsel ? "1" : "0";
    if (name == "adc_pga_gain") return String(config->adc_pga_gain);
    if (name == "adc_ramprate") return String(config->adc_ramprate);
    if (name == "adc_scale") return String(config->adc_scale);
    if (name == "adc_dmic_sense") return config->adc_dmic_sense ? "1" : "0";
    if (name == "adc_sync") return config->adc_sync ? "1" : "0";
    if (name == "adc_inv") return config->adc_inv ? "1" : "0";
    if (name == "adc_ramclr") return config->adc_ramclr ? "1" : "0";
    if (name == "alc_enabled") return config->alc_enabled ? "1" : "0";
    if (name == "adc_automute_enabled") return config->adc_automute_enabled ? "1" : "0";
    if (name == "alc_winsize") return String(config->alc_winsize);
    if (name == "alc_maxlevel") return String(config->alc_maxlevel);
    if (name == "alc_minlevel") return String(config->alc_minlevel);
    if (name == "adc_automute_winsize") return String(config->adc_automute_winsize);
    if (name == "adc_automute_noise_gate") return String(config->adc_automute_noise_gate);
    if (name == "adc_automute_volume") return String(config->adc_automute_volume);
    if (name == "adc_hpfs1") return String(config->adc_hpfs1);
    if (name == "adc_hpfs2") return String(config->adc_hpfs2);
    if (name == "adc_eq_bypass") return config->adc_eq_bypass ? "1" : "0";
    if (name == "adc_hpf") return config->adc_hpf ? "1" : "0";
    if (name == "adceq_b0") return String(config->adceq_b0);
    if (name == "adceq_a1") return String(config->adceq_a1);
    if (name == "adceq_a2") return String(config->adceq_a2);
    if (name == "adceq_b1") return String(config->adceq_b1);
    if (name == "adceq_b2") return String(config->adceq_b2);
    if (name == "drc_enabled") return config->drc_enabled ? "1" : "0";
    if (name == "drc_winsize") return String(config->drc_winsize);
    if (name == "drc_maxlevel") return String(config->drc_maxlevel);
    if (name == "drc_minlevel") return String(config->drc_minlevel);
    if (name == "dac_ramprate") return String(config->dac_ramprate);
    if (name == "dac_eq_bypass") return config->dac_eq_bypass ? "1" : "0";
    if (name == "daceq_b0") return String(config->daceq_b0);
    if (name == "daceq_b1") return String(config->daceq_b1);
    if (name == "daceq_a1") return String(config->daceq_a1);
    return String();
}

// Diff the config snapshot taken before save against the live config and log
// every field whose value actually changed. Keeps the serial trace honest:
// each save line lists exactly the fields the user touched, nothing else.
static void logChangedFields(const ExternalRadioConfig *before,
                             const ExternalRadioConfig *after)
{
    if (before == nullptr || after == nullptr) {
        return;
    }
    String out;
    auto sep = [&out]() {
        if (out.length() > 0) out += ' ';
    };
#define LOG_BOOL(field) \
    if (before->field != after->field) { \
        sep(); \
        out += #field "="; \
        out += after->field ? "1" : "0"; \
    }
#define LOG_UINT(field) \
    if (before->field != after->field) { \
        sep(); \
        out += #field "="; \
        out += String(static_cast<unsigned>(after->field)); \
    }
#define LOG_U32(field) \
    if (before->field != after->field) { \
        sep(); \
        out += #field "="; \
        out += String(static_cast<unsigned long>(after->field)); \
    }
#define LOG_IP(field) \
    if (before->field != after->field) { \
        sep(); \
        out += #field "="; \
        out += ipToString(after->field); \
    }
#define LOG_STR(field) \
    if (strcmp(before->field, after->field) != 0) { \
        sep(); \
        out += #field "=\""; \
        out += after->field; \
        out += '"'; \
    }
    LOG_STR(wifi_ssid);
    if (strcmp(before->wifi_password, after->wifi_password) != 0) {
        sep();
        out += "wifi_password=";
        out += maskSecret(after->wifi_password);
    }
    LOG_BOOL(wifi_dhcp_enabled);
    LOG_IP(wifi_ip);
    LOG_IP(wifi_netmask);
    LOG_IP(wifi_gateway);
    LOG_IP(wifi_dns);
    LOG_STR(server_host);
    LOG_UINT(server_port);
    LOG_UINT(channel);
    LOG_STR(callsign);
    LOG_UINT(callsign_ssid);
    LOG_UINT(ptt_timeout_s);
    LOG_UINT(mic_volume);
    LOG_UINT(line_out_volume);
    LOG_BOOL(hp_drive_enabled);
    LOG_BOOL(aec_enabled);
    LOG_BOOL(drc_enabled);
    LOG_UINT(drc_winsize);
    LOG_UINT(drc_maxlevel);
    LOG_UINT(drc_minlevel);
    LOG_UINT(dac_ramprate);
    LOG_BOOL(dac_eq_bypass);
    LOG_U32(daceq_b0);
    LOG_U32(daceq_b1);
    LOG_U32(daceq_a1);
    LOG_BOOL(adc_dmic_enabled);
    LOG_BOOL(adc_linsel);
    LOG_UINT(adc_pga_gain);
    LOG_UINT(adc_ramprate);
    LOG_BOOL(adc_dmic_sense);
    LOG_BOOL(adc_sync);
    LOG_BOOL(adc_inv);
    LOG_BOOL(adc_ramclr);
    LOG_UINT(adc_scale);
    LOG_BOOL(alc_enabled);
    LOG_BOOL(adc_automute_enabled);
    LOG_UINT(alc_winsize);
    LOG_UINT(alc_maxlevel);
    LOG_UINT(alc_minlevel);
    LOG_UINT(adc_automute_winsize);
    LOG_UINT(adc_automute_noise_gate);
    LOG_UINT(adc_automute_volume);
    LOG_UINT(adc_hpfs1);
    LOG_UINT(adc_hpfs2);
    LOG_BOOL(adc_eq_bypass);
    LOG_BOOL(adc_hpf);
    LOG_U32(adceq_b0);
    LOG_U32(adceq_a1);
    LOG_U32(adceq_a2);
    LOG_U32(adceq_b1);
    LOG_U32(adceq_b2);
#undef LOG_BOOL
#undef LOG_UINT
#undef LOG_U32
#undef LOG_IP
#undef LOG_STR
    if (out.length() > 0) {
        Serial.print(F("[CFG] saved: "));
        Serial.println(out);
    } else {
        Serial.println(F("[CFG] saved: (no change)"));
        return;
    }

    // For ES8311-backed fields, print the affected register address, its full
    // 8-bit value (hex + binary), and the bit breakdown so the change can be
    // cross-checked against the datasheet without having to recompute the
    // packed value. Register layouts mirror es8311.cpp's es8311_*_regNN().
    auto bin8 = [](uint8_t v) -> String {
        String s;
        s.reserve(8);
        for (int i = 7; i >= 0; --i) {
            s += (v & (1u << i)) ? '1' : '0';
        }
        return s;
    };
    auto bin32 = [](uint32_t v) -> String {
        // Group 32 bits by byte ("0b00000000_00000000_00110000_00111001")
        // so 30-bit EQ coefficients stay legible.
        String s;
        s.reserve(35);
        for (int i = 31; i >= 0; --i) {
            s += (v & (1u << i)) ? '1' : '0';
            if (i > 0 && (i % 8) == 0) s += '_';
        }
        return s;
    };
    auto reg13 = [](const ExternalRadioConfig *c) -> uint8_t {
        // REG13 HPSW = bit4; 0x10 = drive HP, 0x00 = drive line.
        return c->hp_drive_enabled ? 0x10u : 0x00u;
    };
    auto reg14 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->adc_dmic_enabled ? 0x40u : 0u) |
                                    (c->adc_linsel ? 0x10u : 0u) |
                                    (c->adc_pga_gain & 0x0fu));
    };
    auto reg15 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->adc_ramprate & 0x0fu) << 4) |
                                    (c->adc_dmic_sense ? 0x01u : 0u));
    };
    auto reg16 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->adc_sync ? 0x20u : 0u) |
                                    (c->adc_inv ? 0x10u : 0u) |
                                    (c->adc_ramclr ? 0x08u : 0u) |
                                    (c->adc_scale & 0x07u));
    };
    auto reg18 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->alc_enabled ? 0x80u : 0u) |
                                    (c->adc_automute_enabled ? 0x40u : 0u) |
                                    (c->alc_winsize & 0x0fu));
    };
    auto reg19 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->alc_maxlevel & 0x0fu) << 4) |
                                    (c->alc_minlevel & 0x0fu));
    };
    auto reg1a = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->adc_automute_winsize & 0x0fu) << 4) |
                                    (c->adc_automute_noise_gate & 0x0fu));
    };
    auto reg1b = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->adc_automute_volume & 0x07u) << 5) |
                                    (c->adc_hpfs1 & 0x1fu));
    };
    auto reg1c = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->adc_eq_bypass ? 0x40u : 0u) |
                                    (c->adc_hpf ? 0x20u : 0u) |
                                    (c->adc_hpfs2 & 0x1fu));
    };
    auto reg34 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->drc_enabled ? 0x80u : 0u) |
                                    (c->drc_winsize & 0x0fu));
    };
    auto reg35 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->drc_maxlevel & 0x0fu) << 4) |
                                    (c->drc_minlevel & 0x0fu));
    };
    auto reg37 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->dac_ramprate & 0x0fu) << 4) |
                                    (c->dac_eq_bypass ? 0x08u : 0u));
    };

    if (before->hp_drive_enabled != after->hp_drive_enabled) {
        const uint8_t v = reg13(after);
        Serial.printf("[CFG]   REG13=0x%02X 0b%s (hp_drive=%u)\n",
                      v, bin8(v).c_str(),
                      after->hp_drive_enabled ? 1u : 0u);
    }
    if (before->adc_dmic_enabled != after->adc_dmic_enabled ||
        before->adc_linsel != after->adc_linsel ||
        before->adc_pga_gain != after->adc_pga_gain) {
        const uint8_t v = reg14(after);
        Serial.printf("[CFG]   REG14=0x%02X 0b%s (dmic_enabled=%u linsel=%u pga_gain=%u)\n",
                      v, bin8(v).c_str(),
                      after->adc_dmic_enabled ? 1u : 0u,
                      after->adc_linsel ? 1u : 0u,
                      static_cast<unsigned>(after->adc_pga_gain));
    }
    if (before->adc_ramprate != after->adc_ramprate ||
        before->adc_dmic_sense != after->adc_dmic_sense) {
        const uint8_t v = reg15(after);
        Serial.printf("[CFG]   REG15=0x%02X 0b%s (adc_ramprate=%u dmic_sense=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->adc_ramprate),
                      after->adc_dmic_sense ? 1u : 0u);
    }
    if (before->adc_sync != after->adc_sync ||
        before->adc_inv != after->adc_inv ||
        before->adc_ramclr != after->adc_ramclr ||
        before->adc_scale != after->adc_scale) {
        const uint8_t v = reg16(after);
        Serial.printf("[CFG]   REG16=0x%02X 0b%s (sync=%u inv=%u ramclr=%u scale=%u)\n",
                      v, bin8(v).c_str(),
                      after->adc_sync ? 1u : 0u,
                      after->adc_inv ? 1u : 0u,
                      after->adc_ramclr ? 1u : 0u,
                      static_cast<unsigned>(after->adc_scale));
    }
    if (before->mic_volume != after->mic_volume) {
        Serial.printf("[CFG]   REG17=0x%02X 0b%s (mic_volume=%u)\n",
                      after->mic_volume, bin8(after->mic_volume).c_str(),
                      static_cast<unsigned>(after->mic_volume));
    }
    if (before->alc_enabled != after->alc_enabled ||
        before->adc_automute_enabled != after->adc_automute_enabled ||
        before->alc_winsize != after->alc_winsize) {
        const uint8_t v = reg18(after);
        Serial.printf("[CFG]   REG18=0x%02X 0b%s (alc_enabled=%u automute_enabled=%u alc_winsize=%u)\n",
                      v, bin8(v).c_str(),
                      after->alc_enabled ? 1u : 0u,
                      after->adc_automute_enabled ? 1u : 0u,
                      static_cast<unsigned>(after->alc_winsize));
    }
    if (before->alc_maxlevel != after->alc_maxlevel ||
        before->alc_minlevel != after->alc_minlevel) {
        const uint8_t v = reg19(after);
        Serial.printf("[CFG]   REG19=0x%02X 0b%s (alc_maxlevel=%u alc_minlevel=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->alc_maxlevel),
                      static_cast<unsigned>(after->alc_minlevel));
    }
    if (before->adc_automute_winsize != after->adc_automute_winsize ||
        before->adc_automute_noise_gate != after->adc_automute_noise_gate) {
        const uint8_t v = reg1a(after);
        Serial.printf("[CFG]   REG1A=0x%02X 0b%s (automute_winsize=%u automute_noise_gate=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->adc_automute_winsize),
                      static_cast<unsigned>(after->adc_automute_noise_gate));
    }
    if (before->adc_automute_volume != after->adc_automute_volume ||
        before->adc_hpfs1 != after->adc_hpfs1) {
        const uint8_t v = reg1b(after);
        Serial.printf("[CFG]   REG1B=0x%02X 0b%s (automute_volume=%u hpfs1=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->adc_automute_volume),
                      static_cast<unsigned>(after->adc_hpfs1));
    }
    if (before->adc_eq_bypass != after->adc_eq_bypass ||
        before->adc_hpf != after->adc_hpf ||
        before->adc_hpfs2 != after->adc_hpfs2) {
        const uint8_t v = reg1c(after);
        Serial.printf("[CFG]   REG1C=0x%02X 0b%s (eq_bypass=%u dynamic_hpf=%u hpfs2=%u)\n",
                      v, bin8(v).c_str(),
                      after->adc_eq_bypass ? 1u : 0u,
                      after->adc_hpf ? 1u : 0u,
                      static_cast<unsigned>(after->adc_hpfs2));
    }
    if (before->line_out_volume != after->line_out_volume) {
        Serial.printf("[CFG]   REG32=0x%02X 0b%s (line_out_volume=%u)\n",
                      after->line_out_volume, bin8(after->line_out_volume).c_str(),
                      static_cast<unsigned>(after->line_out_volume));
    }
    if (before->drc_enabled != after->drc_enabled ||
        before->drc_winsize != after->drc_winsize) {
        const uint8_t v = reg34(after);
        Serial.printf("[CFG]   REG34=0x%02X 0b%s (drc_enabled=%u drc_winsize=%u)\n",
                      v, bin8(v).c_str(),
                      after->drc_enabled ? 1u : 0u,
                      static_cast<unsigned>(after->drc_winsize));
    }
    if (before->drc_maxlevel != after->drc_maxlevel ||
        before->drc_minlevel != after->drc_minlevel) {
        const uint8_t v = reg35(after);
        Serial.printf("[CFG]   REG35=0x%02X 0b%s (drc_maxlevel=%u drc_minlevel=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->drc_maxlevel),
                      static_cast<unsigned>(after->drc_minlevel));
    }
    if (before->dac_ramprate != after->dac_ramprate ||
        before->dac_eq_bypass != after->dac_eq_bypass) {
        const uint8_t v = reg37(after);
        Serial.printf("[CFG]   REG37=0x%02X 0b%s (dac_ramprate=%u dac_eq_bypass=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->dac_ramprate),
                      after->dac_eq_bypass ? 1u : 0u);
    }
    // 30-bit EQ coefficients occupy 4 consecutive registers each. Print the
    // raw 32-bit value (top 2 bits unused) in hex + binary, plus the address
    // range.
    auto log_eq = [&bin32](const char *range, uint32_t value, const char *field_name) {
        const uint32_t v = value & kDacEqCoefficientMax;
        Serial.printf("[CFG]   %s=0x%08lX 0b%s (%s=%lu)\n",
                      range,
                      static_cast<unsigned long>(v),
                      bin32(v).c_str(),
                      field_name,
                      static_cast<unsigned long>(value));
    };
    if (before->adceq_b0 != after->adceq_b0) log_eq("REG1D-20", after->adceq_b0, "adceq_b0");
    if (before->adceq_a1 != after->adceq_a1) log_eq("REG21-24", after->adceq_a1, "adceq_a1");
    if (before->adceq_a2 != after->adceq_a2) log_eq("REG25-28", after->adceq_a2, "adceq_a2");
    if (before->adceq_b1 != after->adceq_b1) log_eq("REG29-2C", after->adceq_b1, "adceq_b1");
    if (before->adceq_b2 != after->adceq_b2) log_eq("REG2D-30", after->adceq_b2, "adceq_b2");
    if (before->daceq_b0 != after->daceq_b0) log_eq("REG38-3B", after->daceq_b0, "daceq_b0");
    if (before->daceq_b1 != after->daceq_b1) log_eq("REG3C-3F", after->daceq_b1, "daceq_b1");
    if (before->daceq_a1 != after->daceq_a1) log_eq("REG40-43", after->daceq_a1, "daceq_a1");
}

// Reply to /save_* with {"ok": bool, "fields": {name: stored_value, ...}}.
// Only fields that the client actually submitted are echoed (skipping the
// hidden _present markers). The client updates its inputs from these values
// so the form always reflects on-device truth without a page reload.
static void sendSavedFieldsJson(const bool ok)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    String body;
    body.reserve(1024);
    body += "{\"ok\":";
    body += ok ? "true" : "false";
    body += ",\"fields\":{";
    bool first = true;
    for (int i = 0; i < s_server.args(); ++i) {
        const String name = s_server.argName(i);
        if (name.endsWith("_present")) {
            continue;
        }
        const String value = savedValueForArg(config, name);
        if (!first) {
            body += ",";
        }
        first = false;
        body += "\"";
        body += jsonEscape(name);
        body += "\":\"";
        body += jsonEscape(value);
        body += "\"";
    }
    body += "}}";
    s_server.send(ok ? 200 : 400, "application/json; charset=utf-8", body);
}

static void sendChunkedHtml(const int code, const String &html)
{
    // WebServer::send writes the body in a single _currentClientWrite() call
    // and ignores the return value, so a body that overflows the TCP send
    // buffer (~5.7 KB by default) gets silently truncated. Switch to chunked
    // transfer encoding and feed the body in 1 KB pieces so each write fits.
    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.send(code, "text/html; charset=utf-8", "");
    constexpr size_t kChunkSize = 1024;
    const char *data = html.c_str();
    size_t remaining = html.length();
    while (remaining > 0) {
        const size_t n = (remaining < kChunkSize) ? remaining : kChunkSize;
        s_server.sendContent(data, n);
        data += n;
        remaining -= n;
    }
    s_server.sendContent("");
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
    sendChunkedHtml(200, WifiConfigPortalView_BuildConfigPage(config, state, form_sections));
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

    ExternalRadioConfig before_snapshot = {};
    bool have_snapshot = false;
    if (const ExternalRadioConfig *p = EXTERNAL_RADIO_GetConfig()) {
        before_snapshot = *p;
        have_snapshot = true;
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
    if (ok && after != nullptr && have_snapshot) {
        logChangedFields(&before_snapshot, after);
        const bool restart_wifi = strcmp(before_snapshot.wifi_ssid, after->wifi_ssid) != 0 ||
                                  strcmp(before_snapshot.wifi_password, after->wifi_password) != 0 ||
                                  before_snapshot.wifi_dhcp_enabled != after->wifi_dhcp_enabled ||
                                  before_snapshot.wifi_ip != after->wifi_ip ||
                                  before_snapshot.wifi_netmask != after->wifi_netmask ||
                                  before_snapshot.wifi_gateway != after->wifi_gateway ||
                                  before_snapshot.wifi_dns != after->wifi_dns;
        if (restart_wifi) {
            Serial.printf("[CFG] WiFi credentials changed, reconnecting to \"%s\"\n", after->wifi_ssid);
            NRLAudioBridge_ApplyConfig(true, true);
        }
    } else if (!ok) {
        Serial.println("[CFG] WiFi config save via web failed (invalid params or EEPROM write error)");
    }

    sendSavedFieldsJson(ok);
}

static void handleSaveNrl()
{
    bool ok = true;

    ExternalRadioConfig before_snapshot = {};
    bool have_snapshot = false;
    if (const ExternalRadioConfig *p = EXTERNAL_RADIO_GetConfig()) {
        before_snapshot = *p;
        have_snapshot = true;
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
    if (ok && s_server.hasArg("ptt_timeout")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("ptt_timeout"), &value) &&
             value >= 5UL && value <= 3600UL &&
             EXTERNAL_RADIO_SetPttTimeout(static_cast<uint16_t>(value), false);
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
    if (ok && config != nullptr && have_snapshot) {
        logChangedFields(&before_snapshot, config);
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
        const bool restart_udp = strcmp(before_snapshot.server_host, config->server_host) != 0 ||
                                 before_snapshot.server_port != config->server_port;
        if (restart_udp) {
            Serial.printf("[CFG] server address changed, rebuilding UDP connection to %s:%u\n",
                          config->server_host, static_cast<unsigned>(config->server_port));
            NRLAudioBridge_ApplyConfig(false, true);
        }
    } else if (!ok) {
        Serial.println("[CFG] NRL config save via web failed (invalid params or EEPROM write error)");
    }

    sendSavedFieldsJson(ok);
}

static void handleUpdatePage()
{
    sendChunkedHtml(200,
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

    sendChunkedHtml(ok ? 200 : 500,
                    buildUpdatePageI18n(ok ? "Update Complete" : "Update Failed",
                                        ok ? "updateDoneHeadline" : "updateFailHeadline",
                                        ok ? "Firmware was written successfully. Rebooting shortly."
                                           : "Firmware upload failed. Check that the file is a valid firmware.bin.",
                                        ok ? "updateDoneIntro" : "updateFailIntro"));
}

// Begin a fresh OTA flash. Shared by the raw and multipart paths.
static void otaBegin(const size_t expected_size)
{
    Serial.printf("[OTA] upload start (content-length=%u)\n",
                  static_cast<unsigned>(s_server.clientContentLength()));
    // The accept code sets the client read timeout to HTTP_MAX_SEND_WAIT
    // (5 s); a flash erase+write stall can exceed that. Bump it so a slow
    // flash block doesn't abort mid-stream.
    s_server.client().setTimeout(60000);
    if (!Update.begin(expected_size > 0 ? expected_size : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        Update.printError(Serial);
    }
}

static void otaWriteChunk(uint8_t *buf, const size_t len)
{
    if (Update.write(buf, len) != len) {
        Update.printError(Serial);
    }
}

static void otaEnd(const size_t total)
{
    if (Update.end(true)) {
        Serial.printf("[OTA] update complete: %u bytes\n", static_cast<unsigned>(total));
    } else {
        Update.printError(Serial);
    }
}

static void otaAbort(const size_t total)
{
    Update.abort();
    Serial.printf("[OTA] upload aborted after %u bytes\n", static_cast<unsigned>(total));
}

static void handleUpdateRaw()
{
    // The Arduino WebServer's FunctionRequestHandler dispatches the same
    // user function for BOTH the multipart upload path (HTTPUpload) and the
    // raw POST path (HTTPRaw). Only one of `_currentUpload` / `_currentRaw`
    // is allocated per request -- dereferencing the other one crashes with
    // LoadProhibited at 0x0. Look at the Content-Type header to pick the
    // right interface. (We rely on s_server collecting "Content-Type" --
    // see ensureServerRunning() for the collectHeaders() call.)
    //
    // Prefer the raw path: a 2 MB upload over multipart goes byte-by-byte
    // through _uploadReadByte() and takes minutes; the raw path reads in
    // 1436-byte chunks via client.readBytes() and finishes in ~30 s. The
    // matching client code in wifi_update_portal.js sends the firmware as
    // application/octet-stream, but we still need to handle multipart
    // gracefully in case an older cached JS is being served.
    const String content_type = s_server.header("Content-Type");
    const bool is_multipart = content_type.indexOf("multipart/") >= 0;

    if (is_multipart) {
        HTTPUpload &upload = s_server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            otaBegin(s_server.clientContentLength());
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            otaWriteChunk(upload.buf, upload.currentSize);
        } else if (upload.status == UPLOAD_FILE_END) {
            otaEnd(upload.totalSize);
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            otaAbort(upload.totalSize);
        }
        return;
    }

    HTTPRaw &raw = s_server.raw();
    if (raw.status == RAW_START) {
        otaBegin(s_server.clientContentLength());
    } else if (raw.status == RAW_WRITE) {
        otaWriteChunk(raw.buf, raw.currentSize);
    } else if (raw.status == RAW_END) {
        otaEnd(raw.totalSize);
    } else if (raw.status == RAW_ABORTED) {
        otaAbort(raw.totalSize);
    }
}

static void sendChunkedAsset(const char *content_type, const char *body, const size_t length)
{
    // Long cache lifetime is safe because every reference is versioned via the
    // ?v={{VERSION}} query string in the HTML template.
    s_server.sendHeader("Cache-Control", "public, max-age=2592000, immutable");
    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.send(200, content_type, "");
    constexpr size_t kChunkSize = 1024;
    size_t remaining = length;
    const char *cursor = body;
    while (remaining > 0) {
        const size_t n = (remaining < kChunkSize) ? remaining : kChunkSize;
        s_server.sendContent(cursor, n);
        cursor += n;
        remaining -= n;
    }
    s_server.sendContent("");
}

static void handlePortalCss()
{
    sendChunkedAsset("text/css; charset=utf-8", kWifiConfigPortalCss, sizeof(kWifiConfigPortalCss) - 1);
}

static void handlePortalJs()
{
    sendChunkedAsset("application/javascript; charset=utf-8", kWifiConfigPortalJs, sizeof(kWifiConfigPortalJs) - 1);
}

static void handleUpdateCss()
{
    sendChunkedAsset("text/css; charset=utf-8", kWifiUpdatePortalCss, sizeof(kWifiUpdatePortalCss) - 1);
}

static void handleUpdateJs()
{
    sendChunkedAsset("application/javascript; charset=utf-8", kWifiUpdatePortalJs, sizeof(kWifiUpdatePortalJs) - 1);
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
    s_server.on("/update", HTTP_POST, handleUpdateFinished, handleUpdateRaw);
    s_server.on("/portal.css", HTTP_GET, handlePortalCss);
    s_server.on("/portal.js", HTTP_GET, handlePortalJs);
    s_server.on("/update.css", HTTP_GET, handleUpdateCss);
    s_server.on("/update.js", HTTP_GET, handleUpdateJs);
    s_server.on("/ping", HTTP_GET, handlePing);
    s_server.on("/favicon.ico", HTTP_GET, handleFavicon);
    s_server.on("/generate_204", HTTP_GET, handleGenerate204);
    s_server.on("/hotspot-detect.html", HTTP_GET, handleHotspotDetect);
    s_server.on("/connecttest.txt", HTTP_GET, handleConnectTest);
    s_server.on("/ncsi.txt", HTTP_GET, handleNcsi);
    s_server.onNotFound(handleNotFound);

    // The OTA handler needs to read Content-Type to tell multipart uploads
    // from raw octet-stream POSTs (see handleUpdateRaw). WebServer doesn't
    // store arbitrary request headers by default.
    static const char *kCollectedHeaders[] = { "Content-Type" };
    s_server.collectHeaders(kCollectedHeaders, 1);

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
