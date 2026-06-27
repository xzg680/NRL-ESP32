#include "wifi_config_portal.h"
#include "wifi_config_portal_view.h"
#include "wifi_portal_assets.generated.h"
#include "nrl_audio_bridge.h"
#include "nrl_captive_dns.h"
#include "nrl_net_compat.h"
#include "nrl_version.h"
#include "nrl_wifi.h"

#include "../app/driver/es8311.h"
#include "../app/driver/external_radio.h"
#include "../app/driver/board_pins.h"
#include "../app/driver/display.h"

#include <driver/gpio.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lwip/sockets.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

static const char *TAG = "CFG";

namespace {

// ----------------------------------------------------------------------------
// FormBody + PortalRequest adapter
//
// The handler functions used to call `s_server.arg("foo")` etc. against
// Arduino's `WebServer`. With esp_http_server we pre-parse the POST body once
// at handler entry and expose the parsed key/value pairs through a small
// adapter that mimics the WebServer accessor surface so handler bodies don't
// have to change. Default esp_http_server config runs one handler task at a
// time, so a single global PortalRequest is safe.
// ----------------------------------------------------------------------------

constexpr size_t kFormParamMax = 48u;
constexpr size_t kFormKeyMax = 48u;
constexpr size_t kFormValueMax = 192u;

struct FormParam {
    char key[kFormKeyMax];
    char value[kFormValueMax];
};

struct FormBody {
    FormParam params[kFormParamMax];
    size_t count;
};

static int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// In-place URL decode (handles %XX and '+'). Returns the new length.
static size_t urlDecodeInPlace(char *s)
{
    if (s == nullptr) {
        return 0u;
    }
    char *out = s;
    for (char *in = s; *in != '\0'; ) {
        if (*in == '+') {
            *out++ = ' ';
            ++in;
        } else if (*in == '%' && in[1] != '\0' && in[2] != '\0') {
            const int hi = hexDigit(in[1]);
            const int lo = hexDigit(in[2]);
            if (hi >= 0 && lo >= 0) {
                *out++ = static_cast<char>((hi << 4) | lo);
                in += 3;
            } else {
                *out++ = *in++;
            }
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
    return static_cast<size_t>(out - s);
}

// Read the entire request body and parse application/x-www-form-urlencoded.
// Returns true on success. Body must fit in heap (up to ~4 KB form bodies are
// typical for the audio config page).
static bool parseFormBody(httpd_req_t *req, FormBody &out)
{
    out.count = 0;
    const size_t total = req->content_len;
    if (total == 0) {
        return true;
    }
    if (total > 8192u) {
        ESP_LOGW(TAG, "form body too large (%u bytes), refusing", static_cast<unsigned>(total));
        return false;
    }
    char *buf = static_cast<char *>(malloc(total + 1u));
    if (buf == nullptr) {
        return false;
    }
    size_t got = 0;
    while (got < total) {
        const int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) {
            free(buf);
            return false;
        }
        got += static_cast<size_t>(n);
    }
    buf[got] = '\0';

    char *cursor = buf;
    while (cursor < buf + got && out.count < kFormParamMax) {
        char *amp = strchr(cursor, '&');
        char *eq = strchr(cursor, '=');
        if (amp != nullptr) {
            *amp = '\0';
        }
        if (eq != nullptr && (amp == nullptr || eq < amp)) {
            *eq = '\0';
            FormParam &p = out.params[out.count];
            const size_t key_len = strlen(cursor);
            const size_t val_len = strlen(eq + 1);
            if (key_len < kFormKeyMax && val_len < kFormValueMax) {
                memcpy(p.key, cursor, key_len + 1u);
                memcpy(p.value, eq + 1, val_len + 1u);
                urlDecodeInPlace(p.key);
                urlDecodeInPlace(p.value);
                ++out.count;
            }
        } else if (cursor[0] != '\0') {
            // Bare key with no '=' sign.
            FormParam &p = out.params[out.count];
            const size_t key_len = strlen(cursor);
            if (key_len < kFormKeyMax) {
                memcpy(p.key, cursor, key_len + 1u);
                p.value[0] = '\0';
                urlDecodeInPlace(p.key);
                ++out.count;
            }
        }
        if (amp == nullptr) {
            break;
        }
        cursor = amp + 1;
    }
    free(buf);
    return true;
}

static const FormParam *formFindParam(const FormBody &body, const char *key)
{
    if (key == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < body.count; ++i) {
        if (strcmp(body.params[i].key, key) == 0) {
            return &body.params[i];
        }
    }
    return nullptr;
}

// Mimics arduino-esp32 WebServer's accessor surface so handler bodies that
// were written against `s_server.arg/hasArg/send/sendContent/...` keep
// compiling unchanged. Stores the current httpd_req_t and the parsed body.
struct PortalRequest {
    httpd_req_t *req = nullptr;
    FormBody body{};
    char ct_header[96] = {};

    void bind(httpd_req_t *r)
    {
        req = r;
        body.count = 0;
        ct_header[0] = '\0';
        if (r != nullptr) {
            httpd_req_get_hdr_value_str(r, "Content-Type", ct_header, sizeof(ct_header));
        }
    }

    bool bindPost(httpd_req_t *r)
    {
        bind(r);
        return parseFormBody(r, body);
    }

    std::string arg(const char *name) const
    {
        const FormParam *p = formFindParam(body, name);
        return (p != nullptr) ? std::string(p->value) : std::string();
    }
    bool hasArg(const char *name) const { return formFindParam(body, name) != nullptr; }
    int args() const { return static_cast<int>(body.count); }
    std::string argName(int i) const
    {
        return (i >= 0 && static_cast<size_t>(i) < body.count) ? std::string(body.params[i].key) : std::string();
    }
    std::string header(const char *name) const
    {
        if (name != nullptr && strcasecmp(name, "Content-Type") == 0) {
            return std::string(ct_header);
        }
        char buf[160] = {};
        if (req != nullptr && name != nullptr) {
            httpd_req_get_hdr_value_str(req, name, buf, sizeof(buf));
        }
        return std::string(buf);
    }
    size_t clientContentLength() const { return req != nullptr ? req->content_len : 0u; }

    void setContentLength(size_t /*unused*/) {}  // chunked encoding handled by httpd_resp_send_chunk

    void sendHeader(const char *name, const char *value, bool /*first*/ = false) const
    {
        if (req != nullptr) {
            httpd_resp_set_hdr(req, name, value);
        }
    }

    void send(int code, const char *content_type, const std::string &body_text) const
    {
        sendBytes(code, content_type, body_text.c_str(), body_text.size());
    }
    void send(int code, const char *content_type, const char *body_text) const
    {
        sendBytes(code, content_type, body_text != nullptr ? body_text : "",
                  body_text != nullptr ? strlen(body_text) : 0u);
    }

    void sendContent(const char *data, size_t n) const
    {
        if (req != nullptr && n > 0u) {
            httpd_resp_send_chunk(req, data, n);
        }
    }
    void sendContent(const char *data) const
    {
        if (req == nullptr) {
            return;
        }
        if (data != nullptr && *data != '\0') {
            httpd_resp_send_chunk(req, data, strlen(data));
        } else {
            httpd_resp_send_chunk(req, nullptr, 0);  // end marker
        }
    }

private:
    void sendBytes(int code, const char *ct, const char *body_text, size_t body_len) const
    {
        if (req == nullptr) {
            return;
        }
        char status[32];
        const char *reason = "OK";
        if (code == 302) reason = "Found";
        else if (code == 204) reason = "No Content";
        else if (code == 400) reason = "Bad Request";
        else if (code == 404) reason = "Not Found";
        else if (code == 500) reason = "Internal Server Error";
        snprintf(status, sizeof(status), "%d %s", code, reason);
        httpd_resp_set_status(req, status);
        if (ct != nullptr && *ct != '\0') {
            httpd_resp_set_type(req, ct);
        }
        httpd_resp_send(req, body_text, body_len);
    }
};

PortalRequest s_server;
httpd_handle_t s_httpd = nullptr;
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

static inline unsigned long nowMsCfg()
{
    return static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
}

// Cached WiFi scan results. The scan runs once before the config AP starts
// (no portal client connected yet, so the channel-hopping scan disturbs
// nobody); the portal then serves this cache and never does a live scan,
// which would otherwise drop the connected client.
const size_t kWifiScanCacheMax = 24u;
WifiConfigPortalScanEntry s_wifi_scan_cache[kWifiScanCacheMax];
int s_wifi_scan_count = 0;
bool s_wifi_prescan_done = false;

constexpr unsigned long kBootResetHoldMs = 5000UL;
constexpr unsigned long kApCloseDelayMs = 5000UL;
constexpr unsigned long kApReopenAfterStaDownMs = 60000UL;
constexpr unsigned long kWifiPrescanTimeoutMs = 12000UL;
constexpr unsigned long kDacEqCoefficientMax = 1073741823UL;
constexpr uint8_t kApChannel = 1;
constexpr uint8_t kApMaxConn = 4;
constexpr uint32_t kApIp = NRL_IPV4(192, 168, 4, 1);
constexpr uint32_t kApGateway = NRL_IPV4(192, 168, 4, 1);
constexpr uint32_t kApSubnet = NRL_IPV4(255, 255, 255, 0);

static std::string jsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += ch;     break;
        }
    }
    return out;
}

// Mask secret fields (e.g. WiFi password) for serial logs: show length only,
// never the plaintext.
static std::string maskSecret(const char *text)
{
    if (text == nullptr || *text == '\0') {
        return std::string("(empty)");
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "****** (%u chars)", static_cast<unsigned>(strlen(text)));
    return std::string(buf);
}

static std::string ipToString(const uint32_t value)
{
    char buf[16] = {};
    nrlIpToString(value, buf, sizeof(buf));
    return std::string(buf);
}

static std::string buildApSsid()
{
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    const uint32_t tail = (static_cast<uint32_t>(mac[3]) << 16) |
                          (static_cast<uint32_t>(mac[4]) << 8) |
                          mac[5];
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "NRL3188-ESP32-%06lX",
             static_cast<unsigned long>(tail));
    return std::string(buffer);
}

static void redirectToPortal()
{
    s_server.sendHeader("Location", "/");
    s_server.send(302, "text/plain", "");
}

static esp_err_t handleFavicon(httpd_req_t *req)
{
    s_server.bind(req);
    s_server.sendHeader("Cache-Control", "max-age=86400");
    s_server.send(204, "image/x-icon", "");
    return ESP_OK;
}

// Scan nearby WiFi and store the result in s_wifi_scan_cache.
static void performWifiPrescan()
{
    ESP_LOGI(TAG, "pre-scanning WiFi before AP start...");
    s_wifi_scan_count = 0;
    if (!nrlWifiScanStartBlocking(static_cast<uint32_t>(kWifiPrescanTimeoutMs))) {
        ESP_LOGW(TAG, "WiFi pre-scan failed");
        return;
    }

    NrlWifiScanResult results[kWifiScanCacheMax];
    const size_t got = nrlWifiScanGetCache(results, kWifiScanCacheMax);
    for (size_t i = 0; i < got && static_cast<size_t>(s_wifi_scan_count) < kWifiScanCacheMax; ++i) {
        if (results[i].ssid[0] == '\0') {
            continue;
        }
        s_wifi_scan_cache[s_wifi_scan_count].ssid = results[i].ssid;
        s_wifi_scan_cache[s_wifi_scan_count].rssi = results[i].rssi;
        ++s_wifi_scan_count;
    }
    ESP_LOGI(TAG, "pre-scan cached %d WiFi networks", s_wifi_scan_count);
}

static void ensureApRunning()
{
    if (!s_ap_should_run || s_ap_started) {
        return;
    }

    // Make sure the WiFi stack is up before we scan or open the AP.
    nrlWifiInit();

    // First time the config AP comes up: scan nearby WiFi while no portal
    // client is connected yet, and cache it. The page then shows the list
    // immediately and never needs a live scan (which would drop the client).
    if (!s_wifi_prescan_done) {
        s_wifi_prescan_done = true;
        performWifiPrescan();
    }

    const std::string ap_ssid = buildApSsid();
    const bool ap_ok = nrlWifiApStart(ap_ssid.c_str(), kApChannel, kApMaxConn,
                                      kApIp, kApGateway, kApSubnet);
    if (ap_ok) {
        s_ap_started = true;
        const size_t station_count = nrlWifiApGetStationCount();
        char ip_buf[16] = {};
        nrlIpToString(nrlWifiApIp(), ip_buf, sizeof(ip_buf));
        ESP_LOGI(TAG, "AP ready: ssid=%s open ip=%s channel=%u stations=%u",
                 ap_ssid.c_str(), ip_buf,
                 static_cast<unsigned>(kApChannel),
                 static_cast<unsigned>(station_count));
    } else {
        ESP_LOGE(TAG, "AP start failed");
    }
}

static void shutdownDnsAndAp()
{
    if (s_dns_started) {
        NRL_CaptiveDNS_Stop();
        s_dns_started = false;
    }
    if (s_ap_started) {
        nrlWifiApStop();
        s_ap_started = false;
        ESP_LOGI(TAG, "AP closed (STA online)");
    }
    s_ap_should_run = false;
}

static void manageApLifecycle()
{
    const bool sta_connected = nrlWifiStaConnected();
    const unsigned long now = nowMsCfg();

    if (sta_connected) {
        s_sta_disconnect_started_ms = 0UL;
        if (!s_sta_was_connected) {
            s_sta_was_connected = true;
            s_ap_close_scheduled = true;
            s_ap_close_at_ms = now + kApCloseDelayMs;
            char ip_buf[16] = {};
            nrlIpToString(nrlWifiStaIp(), ip_buf, sizeof(ip_buf));
            ESP_LOGI(TAG, "STA connected sta_ip=%s, AP will close in %lu ms",
                     ip_buf, static_cast<unsigned long>(kApCloseDelayMs));
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
        ESP_LOGI(TAG, "STA dropped, monitoring for reopen");
        return;
    }

    if ((now - s_sta_disconnect_started_ms) >= kApReopenAfterStaDownMs) {
        s_sta_was_connected = false;
        s_ap_close_scheduled = false;
        s_sta_disconnect_started_ms = 0UL;
        s_ap_should_run = true;
        ESP_LOGI(TAG, "STA down too long, reopening config AP");
    }
}

static void pollBootResetGesture()
{
    if (NRL_PIN_BOOT_BUTTON < 0) {
        return;  // no boot button on this board (pin reused for I2C, etc.)
    }
    const bool pressed = gpio_get_level((gpio_num_t)NRL_PIN_BOOT_BUTTON) == 0;
    const unsigned long now = nowMsCfg();

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
            ESP_LOGI(TAG, "BOOT held 5s, network config reset to defaults");
        } else {
            ESP_LOGE(TAG, "BOOT held 5s, network config reset failed");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

static void ensureDnsRunning()
{
    if (s_dns_started || !s_ap_should_run || !s_ap_started) {
        return;
    }

    if (!NRL_CaptiveDNS_Start(nrlWifiApIp())) {
        ESP_LOGE(TAG, "captive DNS start failed");
        return;
    }
    s_dns_started = true;
}

static std::string buildNetworkSection(const ExternalRadioConfig *config)
{
    return WifiConfigPortalView_BuildNetworkSection(config,
                                                    s_wifi_scan_cache,
                                                    static_cast<size_t>(s_wifi_scan_count));
}

static std::string buildDeviceSections(const ExternalRadioConfig *config)
{
    return WifiConfigPortalView_BuildDeviceSections(config);
}

static std::string buildAudioSections(const ExternalRadioConfig *config)
{
    return WifiConfigPortalView_BuildAudioSections(config);
}

// Look up the canonical, just-saved value for one form field name. The save
// handler echoes these back so the client can refresh its inputs from device
// truth (post-clamp, post-sanitize) without re-rendering the whole page.
static std::string savedValueForArg(const ExternalRadioConfig *config, const std::string &name)
{
    if (config == nullptr) {
        return std::string();
    }
    if (name == "wifi_ssid") return std::string(config->wifi_ssid);
    if (name == "wifi_password") return std::string(config->wifi_password);
    if (name == "wifi_dhcp_enabled") return config->wifi_dhcp_enabled ? "1" : "0";
    if (name == "wifi_ip") return ipToString(config->wifi_ip);
    if (name == "wifi_mask") return ipToString(config->wifi_netmask);
    if (name == "wifi_gateway") return ipToString(config->wifi_gateway);
    if (name == "wifi_dns") return ipToString(config->wifi_dns);
    if (name == "server_host") return std::string(config->server_host);
    if (name == "server_port") return std::to_string(config->server_port);
    if (name == "channel") return std::to_string(config->channel);
    if (name == "callsign") return std::string(config->callsign);
    if (name == "callsign_ssid") return std::to_string(config->callsign_ssid);
    if (name == "ptt_timeout") return std::to_string(config->ptt_timeout_s);
    if (name == "voice_payload_bytes") return std::to_string(config->voice_payload_bytes);
    if (name == "tail_suppress_ms") return std::to_string(config->tail_suppress_ms);
    if (name == "battery_cal_milli") return std::to_string(config->battery_cal_milli);
    if (name == "mic_volume") return std::to_string(config->mic_volume);
    if (name == "line_out_volume") return std::to_string(config->line_out_volume);
    if (name == "hp_drive_enabled") return config->hp_drive_enabled ? "1" : "0";
    if (name == "aec_enabled") return config->aec_enabled ? "1" : "0";
    if (name == "aec_reference_source") return std::to_string(config->aec_reference_source);
    if (name == "ai_noise_enabled") return config->ai_noise_enabled ? "1" : "0";
    if (name == "adc_dmic_enabled") return config->adc_dmic_enabled ? "1" : "0";
    if (name == "adc_linsel") return config->adc_linsel ? "1" : "0";
    if (name == "adc_pga_gain") return std::to_string(config->adc_pga_gain);
    if (name == "adc_ramprate") return std::to_string(config->adc_ramprate);
    if (name == "adc_scale") return std::to_string(config->adc_scale);
    if (name == "adc_dmic_sense") return config->adc_dmic_sense ? "1" : "0";
    if (name == "adc_sync") return config->adc_sync ? "1" : "0";
    if (name == "adc_inv") return config->adc_inv ? "1" : "0";
    if (name == "adc_ramclr") return config->adc_ramclr ? "1" : "0";
    if (name == "alc_enabled") return config->alc_enabled ? "1" : "0";
    if (name == "adc_automute_enabled") return config->adc_automute_enabled ? "1" : "0";
    if (name == "alc_winsize") return std::to_string(config->alc_winsize);
    if (name == "alc_maxlevel") return std::to_string(config->alc_maxlevel);
    if (name == "alc_minlevel") return std::to_string(config->alc_minlevel);
    if (name == "adc_automute_winsize") return std::to_string(config->adc_automute_winsize);
    if (name == "adc_automute_noise_gate") return std::to_string(config->adc_automute_noise_gate);
    if (name == "adc_automute_volume") return std::to_string(config->adc_automute_volume);
    if (name == "adc_hpfs1") return std::to_string(config->adc_hpfs1);
    if (name == "adc_hpfs2") return std::to_string(config->adc_hpfs2);
    if (name == "adc_eq_bypass") return config->adc_eq_bypass ? "1" : "0";
    if (name == "adc_hpf") return config->adc_hpf ? "1" : "0";
    if (name == "mic_hpf_enabled") return config->mic_hpf_enabled ? "1" : "0";
    if (name == "adceq_b0") return std::to_string(config->adceq_b0);
    if (name == "adceq_a1") return std::to_string(config->adceq_a1);
    if (name == "adceq_a2") return std::to_string(config->adceq_a2);
    if (name == "adceq_b1") return std::to_string(config->adceq_b1);
    if (name == "adceq_b2") return std::to_string(config->adceq_b2);
    if (name == "drc_enabled") return config->drc_enabled ? "1" : "0";
    if (name == "drc_winsize") return std::to_string(config->drc_winsize);
    if (name == "drc_maxlevel") return std::to_string(config->drc_maxlevel);
    if (name == "drc_minlevel") return std::to_string(config->drc_minlevel);
    if (name == "dac_ramprate") return std::to_string(config->dac_ramprate);
    if (name == "dac_eq_bypass") return config->dac_eq_bypass ? "1" : "0";
    if (name == "daceq_b0") return std::to_string(config->daceq_b0);
    if (name == "daceq_b1") return std::to_string(config->daceq_b1);
    if (name == "daceq_a1") return std::to_string(config->daceq_a1);
    return std::string();
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
    std::string out;
    auto sep = [&out]() {
        if (!out.empty()) out += ' ';
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
        out += std::to_string(static_cast<unsigned>(after->field)); \
    }
#define LOG_U32(field) \
    if (before->field != after->field) { \
        sep(); \
        out += #field "="; \
        out += std::to_string(static_cast<unsigned long>(after->field)); \
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
    LOG_UINT(battery_cal_milli);
    LOG_UINT(voice_payload_bytes);
    LOG_UINT(tail_suppress_ms);
    LOG_UINT(mic_volume);
    LOG_UINT(line_out_volume);
    LOG_BOOL(hp_drive_enabled);
    LOG_BOOL(aec_enabled);
    LOG_BOOL(ai_noise_enabled);
    LOG_BOOL(mic_hpf_enabled);
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
    if (!out.empty()) {
        ESP_LOGI(TAG, "saved: %s", out.c_str());
    } else {
        ESP_LOGI(TAG, "saved: (no change)");
        return;
    }

    // For ES8311-backed fields, print the affected register address, its full
    // 8-bit value (hex + binary), and the bit breakdown so the change can be
    // cross-checked against the datasheet without having to recompute the
    // packed value. Register layouts mirror es8311.cpp's es8311_*_regNN().
    auto bin8 = [](uint8_t v) -> std::string {
        std::string s;
        s.reserve(8);
        for (int i = 7; i >= 0; --i) {
            s += (v & (1u << i)) ? '1' : '0';
        }
        return s;
    };
    auto bin32 = [](uint32_t v) -> std::string {
        // Group 32 bits by byte ("0b00000000_00000000_00110000_00111001")
        // so 30-bit EQ coefficients stay legible.
        std::string s;
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
        ESP_LOGI(TAG,"[CFG]   REG13=0x%02X 0b%s (hp_drive=%u)\n",
                      v, bin8(v).c_str(),
                      after->hp_drive_enabled ? 1u : 0u);
    }
    if (before->adc_dmic_enabled != after->adc_dmic_enabled ||
        before->adc_linsel != after->adc_linsel ||
        before->adc_pga_gain != after->adc_pga_gain) {
        const uint8_t v = reg14(after);
        ESP_LOGI(TAG,"[CFG]   REG14=0x%02X 0b%s (dmic_enabled=%u linsel=%u pga_gain=%u)\n",
                      v, bin8(v).c_str(),
                      after->adc_dmic_enabled ? 1u : 0u,
                      after->adc_linsel ? 1u : 0u,
                      static_cast<unsigned>(after->adc_pga_gain));
    }
    if (before->adc_ramprate != after->adc_ramprate ||
        before->adc_dmic_sense != after->adc_dmic_sense) {
        const uint8_t v = reg15(after);
        ESP_LOGI(TAG,"[CFG]   REG15=0x%02X 0b%s (adc_ramprate=%u dmic_sense=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->adc_ramprate),
                      after->adc_dmic_sense ? 1u : 0u);
    }
    if (before->adc_sync != after->adc_sync ||
        before->adc_inv != after->adc_inv ||
        before->adc_ramclr != after->adc_ramclr ||
        before->adc_scale != after->adc_scale) {
        const uint8_t v = reg16(after);
        ESP_LOGI(TAG,"[CFG]   REG16=0x%02X 0b%s (sync=%u inv=%u ramclr=%u scale=%u)\n",
                      v, bin8(v).c_str(),
                      after->adc_sync ? 1u : 0u,
                      after->adc_inv ? 1u : 0u,
                      after->adc_ramclr ? 1u : 0u,
                      static_cast<unsigned>(after->adc_scale));
    }
    if (before->mic_volume != after->mic_volume) {
        ESP_LOGI(TAG,"[CFG]   REG17=0x%02X 0b%s (mic_volume=%u)\n",
                      after->mic_volume, bin8(after->mic_volume).c_str(),
                      static_cast<unsigned>(after->mic_volume));
    }
    if (before->alc_enabled != after->alc_enabled ||
        before->adc_automute_enabled != after->adc_automute_enabled ||
        before->alc_winsize != after->alc_winsize) {
        const uint8_t v = reg18(after);
        ESP_LOGI(TAG,"[CFG]   REG18=0x%02X 0b%s (alc_enabled=%u automute_enabled=%u alc_winsize=%u)\n",
                      v, bin8(v).c_str(),
                      after->alc_enabled ? 1u : 0u,
                      after->adc_automute_enabled ? 1u : 0u,
                      static_cast<unsigned>(after->alc_winsize));
    }
    if (before->alc_maxlevel != after->alc_maxlevel ||
        before->alc_minlevel != after->alc_minlevel) {
        const uint8_t v = reg19(after);
        ESP_LOGI(TAG,"[CFG]   REG19=0x%02X 0b%s (alc_maxlevel=%u alc_minlevel=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->alc_maxlevel),
                      static_cast<unsigned>(after->alc_minlevel));
    }
    if (before->adc_automute_winsize != after->adc_automute_winsize ||
        before->adc_automute_noise_gate != after->adc_automute_noise_gate) {
        const uint8_t v = reg1a(after);
        ESP_LOGI(TAG,"[CFG]   REG1A=0x%02X 0b%s (automute_winsize=%u automute_noise_gate=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->adc_automute_winsize),
                      static_cast<unsigned>(after->adc_automute_noise_gate));
    }
    if (before->adc_automute_volume != after->adc_automute_volume ||
        before->adc_hpfs1 != after->adc_hpfs1) {
        const uint8_t v = reg1b(after);
        ESP_LOGI(TAG,"[CFG]   REG1B=0x%02X 0b%s (automute_volume=%u hpfs1=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->adc_automute_volume),
                      static_cast<unsigned>(after->adc_hpfs1));
    }
    if (before->adc_eq_bypass != after->adc_eq_bypass ||
        before->adc_hpf != after->adc_hpf ||
        before->adc_hpfs2 != after->adc_hpfs2) {
        const uint8_t v = reg1c(after);
        ESP_LOGI(TAG,"[CFG]   REG1C=0x%02X 0b%s (eq_bypass=%u dynamic_hpf=%u hpfs2=%u)\n",
                      v, bin8(v).c_str(),
                      after->adc_eq_bypass ? 1u : 0u,
                      after->adc_hpf ? 1u : 0u,
                      static_cast<unsigned>(after->adc_hpfs2));
    }
    if (before->line_out_volume != after->line_out_volume) {
        ESP_LOGI(TAG,"[CFG]   REG32=0x%02X 0b%s (line_out_volume=%u)\n",
                      after->line_out_volume, bin8(after->line_out_volume).c_str(),
                      static_cast<unsigned>(after->line_out_volume));
    }
    if (before->drc_enabled != after->drc_enabled ||
        before->drc_winsize != after->drc_winsize) {
        const uint8_t v = reg34(after);
        ESP_LOGI(TAG,"[CFG]   REG34=0x%02X 0b%s (drc_enabled=%u drc_winsize=%u)\n",
                      v, bin8(v).c_str(),
                      after->drc_enabled ? 1u : 0u,
                      static_cast<unsigned>(after->drc_winsize));
    }
    if (before->drc_maxlevel != after->drc_maxlevel ||
        before->drc_minlevel != after->drc_minlevel) {
        const uint8_t v = reg35(after);
        ESP_LOGI(TAG,"[CFG]   REG35=0x%02X 0b%s (drc_maxlevel=%u drc_minlevel=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->drc_maxlevel),
                      static_cast<unsigned>(after->drc_minlevel));
    }
    if (before->dac_ramprate != after->dac_ramprate ||
        before->dac_eq_bypass != after->dac_eq_bypass) {
        const uint8_t v = reg37(after);
        ESP_LOGI(TAG,"[CFG]   REG37=0x%02X 0b%s (dac_ramprate=%u dac_eq_bypass=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->dac_ramprate),
                      after->dac_eq_bypass ? 1u : 0u);
    }
    // 30-bit EQ coefficients occupy 4 consecutive registers each. Print the
    // raw 32-bit value (top 2 bits unused) in hex + binary, plus the address
    // range.
    auto log_eq = [&bin32](const char *range, uint32_t value, const char *field_name) {
        const uint32_t v = value & kDacEqCoefficientMax;
        ESP_LOGI(TAG,"[CFG]   %s=0x%08lX 0b%s (%s=%lu)\n",
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
static bool endsWith(const std::string &s, const char *suffix)
{
    if (suffix == nullptr) return false;
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static void sendSavedFieldsJson(const bool ok)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    std::string body;
    body.reserve(1024);
    body += "{\"ok\":";
    body += ok ? "true" : "false";
    body += ",\"fields\":{";
    bool first = true;
    auto appendField = [&](const std::string &name) {
        const std::string value = savedValueForArg(config, name);
        if (value.empty() && name != "wifi_ssid" && name != "wifi_password" &&
            name != "server_host" && name != "callsign") {
            return;
        }
        if (!first) {
            body += ",";
        }
        first = false;
        body += "\"";
        body += jsonEscape(name);
        body += "\":\"";
        body += jsonEscape(value);
        body += "\"";
    };
    for (int i = 0; i < s_server.args(); ++i) {
        const std::string name = s_server.argName(i);
        if (endsWith(name, "_present") || name == "audio_reset_defaults") {
            continue;
        }
        appendField(name);
    }
    if (s_server.hasArg("audio_reset_defaults")) {
        static const char *kAudioFields[] = {
            "mic_volume", "line_out_volume", "hp_drive_enabled",
            "aec_enabled", "aec_reference_source", "ai_noise_enabled",
            "mic_hpf_enabled", "drc_enabled", "drc_winsize",
            "drc_maxlevel", "drc_minlevel", "dac_ramprate",
            "dac_eq_bypass", "daceq_b0", "daceq_b1", "daceq_a1",
            "adc_dmic_enabled", "adc_linsel", "adc_pga_gain",
            "adc_ramprate", "adc_scale", "adc_dmic_sense",
            "adc_sync", "adc_inv", "adc_ramclr", "alc_enabled",
            "adc_automute_enabled", "alc_winsize", "alc_maxlevel",
            "alc_minlevel", "adc_automute_winsize",
            "adc_automute_noise_gate", "adc_automute_volume",
            "adc_hpfs1", "adc_hpfs2", "adc_eq_bypass", "adc_hpf",
            "adceq_b0", "adceq_a1", "adceq_a2", "adceq_b1", "adceq_b2",
        };
        for (const char *name : kAudioFields) {
            appendField(name);
        }
    }
    body += "}}";
    s_server.send(ok ? 200 : 400, "application/json; charset=utf-8", body);
}

static void sendChunkedHtml(const int code, const std::string &html)
{
    httpd_req_t *req = s_server.req;
    if (req == nullptr) {
        return;
    }
    char status[32];
    const char *reason = (code == 200) ? "OK" : (code == 500) ? "Internal Server Error" : "OK";
    snprintf(status, sizeof(status), "%d %s", code, reason);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    constexpr size_t kChunkSize = 1024;
    const char *data = html.c_str();
    size_t remaining = html.size();
    while (remaining > 0) {
        const size_t n = (remaining < kChunkSize) ? remaining : kChunkSize;
        httpd_resp_send_chunk(req, data, n);
        data += n;
        remaining -= n;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
}

static void sendConfigPage(const char *title,
                           const char *headline,
                           const char *headline_key,
                           const char *intro,
                           const char *intro_key,
                           const char *form_action,
                           const std::string &form_sections,
                           const bool network_active,
                           const bool device_active,
                           const bool audio_active,
                           const std::string &footer)
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

static std::string buildUpdatePageI18n(const char *headline,
                                       const char *headline_key,
                                       const char *intro,
                                       const char *intro_key)
{
    return WifiConfigPortalView_BuildUpdatePage(headline, headline_key, intro, intro_key);
}

static esp_err_t handleRoot(httpd_req_t *req)
{
    s_server.bind(req);
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " WiFi Config").c_str(),
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
    return ESP_OK;
}

static esp_err_t handleNrlPage(httpd_req_t *req)
{
    s_server.bind(req);
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " NRL Config").c_str(),
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
    return ESP_OK;
}

static esp_err_t handleAudioPage(httpd_req_t *req)
{
    s_server.bind(req);
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " Audio Settings").c_str(),
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
    return ESP_OK;
}

static esp_err_t handleScan(httpd_req_t *req)
{
    s_server.bind(req);
    // Serve the cache captured before the AP started. No live scan -- a live
    // scan hops channels and drops the portal client that is connected now.
    std::string json = "[";
    for (int i = 0; i < s_wifi_scan_count; ++i) {
        if (i != 0) {
            json += ",";
        }
        const std::string escaped_ssid = jsonEscape(s_wifi_scan_cache[i].ssid);
        json += "{\"ssid\":\"";
        json += escaped_ssid;
        json += "\",\"label\":\"";
        json += escaped_ssid;
        json += " (";
        json += std::to_string(s_wifi_scan_cache[i].rssi);
        json += " dBm)\"}";
    }
    json += "]";
    s_server.send(200, "application/json; charset=utf-8", json);
    return ESP_OK;
}

static bool parseUIntArg(const std::string &text, unsigned long *out_value)
{
    if (out_value == nullptr || text.empty()) {
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

static bool parseIpArg(const std::string &text, uint32_t *out_value)
{
    if (out_value == nullptr || text.empty()) {
        return false;
    }
    struct in_addr addr = {};
    if (inet_aton(text.c_str(), &addr) == 0) {
        return false;
    }
    *out_value = addr.s_addr;
    return true;
}

static esp_err_t handleSaveWifi(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    // Diagnostic: log Content-Type, body size and parsed param count so we
    // can tell whether the browser is sending urlencoded (parsed) or
    // multipart (one giant unparsed blob). Drop once form parsing is stable.
    ESP_LOGI(TAG, "save_wifi: ct=\"%s\" body=%u parsed=%d args:",
             s_server.ct_header,
             static_cast<unsigned>(req->content_len),
             s_server.args());
    for (int i = 0; i < s_server.args(); ++i) {
        ESP_LOGI(TAG, "  [%d] %s = (%u chars)",
                 i,
                 s_server.argName(i).c_str(),
                 static_cast<unsigned>(s_server.arg(s_server.argName(i).c_str()).size()));
    }
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
            ESP_LOGI(TAG,"[CFG] WiFi credentials changed, reconnecting to \"%s\"\n", after->wifi_ssid);
            NRLAudioBridge_ApplyConfig(true, true);
        }
    } else if (!ok) {
        ESP_LOGE(TAG, "WiFi config save via web failed (invalid params or EEPROM write error)");
    }

    sendSavedFieldsJson(ok);
    return ESP_OK;
}

static esp_err_t handleSaveNrl(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    bool ok = true;

    ExternalRadioConfig before_snapshot = {};
    bool have_snapshot = false;
    if (const ExternalRadioConfig *p = EXTERNAL_RADIO_GetConfig()) {
        before_snapshot = *p;
        have_snapshot = true;
    }

    const bool reset_audio_defaults = s_server.hasArg("audio_reset_defaults");
    if (ok && reset_audio_defaults) {
        ok = EXTERNAL_RADIO_ResetAudioConfig(false);
    }
    if (ok && !reset_audio_defaults) {
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
    if (ok && s_server.hasArg("voice_payload_bytes")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("voice_payload_bytes"), &value) &&
             value >= 160UL && value <= 500UL &&
             EXTERNAL_RADIO_SetVoicePayloadBytes(static_cast<uint16_t>(value), false);
    }
    if (ok && s_server.hasArg("tail_suppress_ms")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("tail_suppress_ms"), &value) &&
             value <= 5000UL &&
             EXTERNAL_RADIO_SetTailSuppressMs(static_cast<uint16_t>(value), false);
    }
#if NRL_BOARD == NRL_BOARD_GEZIPAI
    // Auto-calibrate the battery sense from a multimeter reading. Empty value
    // skips it (so the form can be submitted with only the manual scale).
    if (ok && s_server.hasArg("battery_actual_mv") && !s_server.arg("battery_actual_mv").empty()) {
        unsigned long actual_mv = 0UL;
        const int raw_mv = Display_GetBatteryRawMv();
        if (parseUIntArg(s_server.arg("battery_actual_mv"), &actual_mv) &&
            actual_mv >= 1000UL && actual_mv <= 9000UL && raw_mv > 0) {
            const unsigned long scale = (actual_mv * 1000UL + static_cast<unsigned long>(raw_mv) / 2UL) /
                                        static_cast<unsigned long>(raw_mv);
            ok = scale >= 500UL && scale <= 2000UL &&
                 EXTERNAL_RADIO_SetBatteryCalibration(static_cast<uint16_t>(scale), false);
        } else {
            ok = false;
        }
    }
    if (ok && s_server.hasArg("battery_cal_milli")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("battery_cal_milli"), &value) &&
             value >= 500UL && value <= 2000UL &&
             EXTERNAL_RADIO_SetBatteryCalibration(static_cast<uint16_t>(value), false);
    }
#endif
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
#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
    if (ok && s_server.hasArg("aec_present")) {
        ok = EXTERNAL_RADIO_SetAecEnabled(s_server.hasArg("aec_enabled"), false);
    }
    if (ok && s_server.hasArg("aec_reference_source")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("aec_reference_source"), &value) &&
             value <= EXTERNAL_RADIO_AEC_REF_MIC &&
             EXTERNAL_RADIO_SetAecReferenceSource(static_cast<uint8_t>(value), false);
    }
#endif
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
    if (ok && s_server.hasArg("ai_noise_present")) {
        ok = EXTERNAL_RADIO_SetAiNoiseEnabled(s_server.hasArg("ai_noise_enabled"), false);
    }
#endif
    if (ok && s_server.hasArg("mic_hpf_enabled_present")) {
        ok = EXTERNAL_RADIO_SetMicHpfEnabled(s_server.hasArg("mic_hpf_enabled"), false);
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
    if (ok && (s_server.hasArg("adc_dmic_present") ||
               s_server.hasArg("adc_linsel_present") ||
               s_server.hasArg("adc_pga_gain"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        bool dmic = current != nullptr && current->adc_dmic_enabled;
        bool linsel = current == nullptr || current->adc_linsel;
        uint8_t pga = current != nullptr ? current->adc_pga_gain : 10u;
        unsigned long value = 0UL;
        if (s_server.hasArg("adc_dmic_enabled")) {
            dmic = s_server.arg("adc_dmic_enabled") == "1";
        } else if (s_server.hasArg("adc_dmic_present")) {
            dmic = false;
        }
        if (s_server.hasArg("adc_linsel")) {
            linsel = s_server.arg("adc_linsel") == "1";
        } else if (s_server.hasArg("adc_linsel_present")) {
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
    if (ok && (s_server.hasArg("adc_dmic_sense_present") || s_server.hasArg("adc_ramprate"))) {
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
        } else if (s_server.hasArg("adc_dmic_sense_present")) {
            dmic_sense = false;
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcRampConfig(ramprate, dmic_sense, false);
        }
    }
    if (ok && (s_server.hasArg("adc_sync_present") ||
               s_server.hasArg("adc_inv_present") ||
               s_server.hasArg("adc_ramclr_present") ||
               s_server.hasArg("adc_scale"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        bool sync = current == nullptr || current->adc_sync;
        bool inv = current != nullptr && current->adc_inv;
        bool ramclr = current != nullptr && current->adc_ramclr;
        uint8_t scale = current != nullptr ? current->adc_scale : 4u;
        unsigned long value = 0UL;
        if (s_server.hasArg("adc_sync")) {
            sync = s_server.arg("adc_sync") == "1";
        } else if (s_server.hasArg("adc_sync_present")) {
            sync = false;
        }
        if (s_server.hasArg("adc_inv")) {
            inv = s_server.arg("adc_inv") == "1";
        } else if (s_server.hasArg("adc_inv_present")) {
            inv = false;
        }
        if (s_server.hasArg("adc_ramclr")) {
            ramclr = s_server.arg("adc_ramclr") == "1";
        } else if (s_server.hasArg("adc_ramclr_present")) {
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
    if (ok && (s_server.hasArg("alc_enabled_present") ||
               s_server.hasArg("adc_automute_enabled_present") ||
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
        } else if (s_server.hasArg("alc_enabled_present")) {
            alc = false;
        }
        if (s_server.hasArg("adc_automute_enabled")) {
            automute = s_server.arg("adc_automute_enabled") == "1";
        } else if (s_server.hasArg("adc_automute_enabled_present")) {
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
    if (ok && (s_server.hasArg("adc_eq_bypass_present") ||
               s_server.hasArg("adc_hpf_present") ||
               s_server.hasArg("adc_hpfs1") ||
               s_server.hasArg("adc_hpfs2"))) {
        // Each REG1C-affecting control posts only its own _present marker
        // and its own checkbox; the other two stay in the saved config and
        // are preserved via init-from-current. The previous design had both
        // EQ-bypass and Dynamic-HPF forms posting a shared adc_hpf_present
        // plus stale cross-form hidden mirrors of the other checkbox, which
        // could revert a freshly-unchecked checkbox the next time its
        // sibling was toggled.
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
        } else if (s_server.hasArg("adc_eq_bypass_present")) {
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
            ESP_LOGI(TAG,"[CFG] server address changed, rebuilding UDP connection to %s:%u\n",
                          config->server_host, static_cast<unsigned>(config->server_port));
            NRLAudioBridge_ApplyConfig(false, true);
        }
    } else if (!ok) {
        ESP_LOGE(TAG, "NRL config save via web failed (invalid params or EEPROM write error)");
    }

    sendSavedFieldsJson(ok);
    return ESP_OK;
}

static esp_err_t handleUpdatePage(httpd_req_t *req)
{
    s_server.bind(req);
    sendChunkedHtml(200,
                    buildUpdatePageI18n("Firmware Update",
                                        "updateHeadline",
                                        "Upload a firmware file over this WiFi connection.",
                                        "updateIntro"));
    return ESP_OK;
}

// OTA upload handler. Reads the raw octet-stream body in chunks, streaming
// each chunk into esp_ota_write. Multipart uploads are no longer supported --
// wifi_update_portal.js sends application/octet-stream; refusing multipart
// keeps the body parser trivial. Stale clients see a 415.
static esp_err_t handleUpdate(httpd_req_t *req)
{
    s_server.bind(req);

    char content_type[96] = {};
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    if (strstr(content_type, "multipart/") != nullptr) {
        ESP_LOGW(TAG, "OTA upload rejected: multipart no longer supported (Content-Type=%s)",
                 content_type);
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Multipart not supported -- refresh the update page", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const size_t content_len = req->content_len;
    ESP_LOGI(TAG, "OTA upload start (content-length=%u)", static_cast<unsigned>(content_len));

    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        ESP_LOGE(TAG, "no OTA target partition");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_OK;
    }
    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(target,
                                  content_len > 0u ? content_len : OTA_SIZE_UNKNOWN,
                                  &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    char buf[1536];
    size_t total = 0u;
    bool ok = true;
    while (ok) {
        const int got = httpd_req_recv(req, buf, sizeof(buf));
        if (got > 0) {
            err = esp_ota_write(ota_handle, buf, static_cast<size_t>(got));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed after %u bytes: %s",
                         static_cast<unsigned>(total), esp_err_to_name(err));
                ok = false;
                break;
            }
            total += static_cast<size_t>(got);
            continue;
        }
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            // Slow client / flash stall -- one more retry.
            continue;
        }
        if (got == 0) {
            break;  // end of body
        }
        ESP_LOGE(TAG, "httpd_req_recv error %d after %u bytes",
                 got, static_cast<unsigned>(total));
        ok = false;
        break;
    }

    if (ok) {
        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            ok = false;
        } else {
            err = esp_ota_set_boot_partition(target);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
                ok = false;
            }
        }
    } else {
        esp_ota_abort(ota_handle);
    }

    if (ok) {
        ESP_LOGI(TAG, "OTA upload complete: %u bytes -> %s",
                 static_cast<unsigned>(total), target->label);
        s_update_reboot_pending = true;
        s_update_reboot_at_ms = nowMsCfg() + 1000UL;
    } else {
        ESP_LOGE(TAG, "OTA upload failed after %u bytes", static_cast<unsigned>(total));
    }

    sendChunkedHtml(ok ? 200 : 500,
                    buildUpdatePageI18n(ok ? "Update Complete" : "Update Failed",
                                        ok ? "updateDoneHeadline" : "updateFailHeadline",
                                        ok ? "Firmware was written successfully. Rebooting shortly."
                                           : "Firmware upload failed. Check that the file is a valid firmware.bin.",
                                        ok ? "updateDoneIntro" : "updateFailIntro"));
    return ESP_OK;
}

static void sendChunkedAsset(httpd_req_t *req, const char *content_type,
                             const char *body, const size_t length)
{
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, content_type);
    // Long cache lifetime is safe because every reference is versioned via the
    // ?v={{VERSION}} query string in the HTML template.
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=2592000, immutable");
    constexpr size_t kChunkSize = 1024;
    size_t remaining = length;
    const char *cursor = body;
    while (remaining > 0) {
        const size_t n = (remaining < kChunkSize) ? remaining : kChunkSize;
        httpd_resp_send_chunk(req, cursor, n);
        cursor += n;
        remaining -= n;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t handlePortalCss(httpd_req_t *req)
{
    sendChunkedAsset(req, "text/css; charset=utf-8", kWifiConfigPortalCss, sizeof(kWifiConfigPortalCss) - 1);
    return ESP_OK;
}

static esp_err_t handlePortalJs(httpd_req_t *req)
{
    sendChunkedAsset(req, "application/javascript; charset=utf-8", kWifiConfigPortalJs, sizeof(kWifiConfigPortalJs) - 1);
    return ESP_OK;
}

static esp_err_t handleUpdateCss(httpd_req_t *req)
{
    sendChunkedAsset(req, "text/css; charset=utf-8", kWifiUpdatePortalCss, sizeof(kWifiUpdatePortalCss) - 1);
    return ESP_OK;
}

static esp_err_t handleUpdateJs(httpd_req_t *req)
{
    sendChunkedAsset(req, "application/javascript; charset=utf-8", kWifiUpdatePortalJs, sizeof(kWifiUpdatePortalJs) - 1);
    return ESP_OK;
}

static esp_err_t handlePing(httpd_req_t *req)
{
    s_server.bind(req);
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "text/plain", "ok\n");
    return ESP_OK;
}

static esp_err_t handleCaptiveRedirect(httpd_req_t *req)
{
    s_server.bind(req);
    redirectToPortal();
    return ESP_OK;
}

static esp_err_t handleNotFound(httpd_req_t *req, httpd_err_code_t /*err*/)
{
    s_server.bind(req);
    redirectToPortal();
    return ESP_OK;
}

static void ensureServerRunning()
{
    if (s_server_started) {
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 24;
    // Templated audio config page builds via std::string concatenation can
    // exceed the default 4 KB worker stack.
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    // OTA upload can be 2 MB, takes ~30 s. Default 5 s recv timeout would
    // abort mid-stream when a flash erase block stalls the read.
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    struct UriEntry {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    };
    static const UriEntry kRoutes[] = {
        { "/",                     HTTP_GET,  handleRoot },
        { "/nrl",                  HTTP_GET,  handleNrlPage },
        { "/audio",                HTTP_GET,  handleAudioPage },
        { "/scan",                 HTTP_GET,  handleScan },
        { "/save_wifi",            HTTP_POST, handleSaveWifi },
        { "/save_nrl",             HTTP_POST, handleSaveNrl },
        { "/update",               HTTP_GET,  handleUpdatePage },
        { "/update",               HTTP_POST, handleUpdate },
        { "/portal.css",           HTTP_GET,  handlePortalCss },
        { "/portal.js",            HTTP_GET,  handlePortalJs },
        { "/update.css",           HTTP_GET,  handleUpdateCss },
        { "/update.js",            HTTP_GET,  handleUpdateJs },
        { "/ping",                 HTTP_GET,  handlePing },
        { "/favicon.ico",          HTTP_GET,  handleFavicon },
        { "/generate_204",         HTTP_GET,  handleCaptiveRedirect },
        { "/hotspot-detect.html",  HTTP_GET,  handleCaptiveRedirect },
        { "/connecttest.txt",      HTTP_GET,  handleCaptiveRedirect },
        { "/ncsi.txt",             HTTP_GET,  handleCaptiveRedirect },
    };
    for (const UriEntry &r : kRoutes) {
        httpd_uri_t uri = {};
        uri.uri = r.uri;
        uri.method = r.method;
        uri.handler = r.handler;
        uri.user_ctx = nullptr;
        if (httpd_register_uri_handler(s_httpd, &uri) != ESP_OK) {
            ESP_LOGW(TAG, "register %s failed", r.uri);
        }
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, handleNotFound);

    s_server_started = true;
    ESP_LOGI(TAG, "HTTP config server started on port 80");
}

} // namespace

bool WifiConfigPortal_Init(void)
{
    EXTERNAL_RADIO_Init();
    if (NRL_PIN_BOOT_BUTTON >= 0) {
        gpio_reset_pin((gpio_num_t)NRL_PIN_BOOT_BUTTON);
        gpio_set_direction((gpio_num_t)NRL_PIN_BOOT_BUTTON, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)NRL_PIN_BOOT_BUTTON, GPIO_PULLUP_ONLY);
    }
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
        NRL_CaptiveDNS_Poll();
    }
    // esp_http_server runs its own task; no per-loop polling needed.
    if (s_update_reboot_pending && (nowMsCfg() - s_update_reboot_at_ms) < 0x80000000UL) {
        ESP_LOGI(TAG, "OTA reboot now");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

void WifiConfigPortal_EnterFallbackMode(void)
{
    s_ap_should_run = true;
    s_sta_was_connected = false;
    s_ap_close_scheduled = false;
    s_sta_disconnect_started_ms = 0UL;
    ensureApRunning();
    ensureDnsRunning();
    ensureServerRunning();
    char ip_buf[16] = {};
    nrlIpToString(nrlWifiApIp(), ip_buf, sizeof(ip_buf));
    ESP_LOGI(TAG, "Fallback config AP active: ssid=%s ip=%s",
             buildApSsid().c_str(), ip_buf);
}
