#include "ble_config.h"

#include "nrl_audio_bridge.h"
#include "nrl_net_compat.h"
#include "nrl_version.h"

#include "../app/driver/external_radio.h"

#include <NimBLEDevice.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

static const char *TAG = "BLECFG";

namespace {

constexpr char kBleDeviceName[] = "NRL-ESP32-CFG";
constexpr char kServiceUuid[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kRxUuid[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kTxUuid[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
constexpr size_t kCommandBufferSize = 160;

NimBLEServer *s_server = nullptr;
NimBLECharacteristic *s_tx = nullptr;
NimBLECharacteristic *s_rx = nullptr;
bool s_initialized = false;
bool s_client_connected = false;
bool s_advertising = false;
char s_command_buffer[kCommandBufferSize] = {};
size_t s_command_size = 0;
bool s_reboot_pending = false;
uint32_t s_reboot_at_ms = 0;

// BT/WiFi coexistence management. The single radio is shared; while the BT
// controller is up, coexist time-slices airtime and bunches voice packets.
// We keep BLE up only while the WiFi STA is down (provisioning fallback) and
// tear it down once the STA has been solidly connected for a grace period.
constexpr uint32_t kBleStopAfterStaUpMs = 4000u;   // STA stable this long -> drop BLE
constexpr uint32_t kBleStartAfterStaDownMs = 3000u; // STA down this long -> bring BLE back
bool s_ble_auto_stopped = false;     // true once we tore BLE down due to WiFi
uint32_t s_sta_up_since_ms = 0u;     // 0 = STA not currently up
uint32_t s_sta_down_since_ms = 0u;   // 0 = STA not currently down

static inline uint32_t nowMsBle()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

static void sendLine(const char *line)
{
    if (line == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "%s", line);
    if (s_tx == nullptr || !s_client_connected) {
        return;
    }

    s_tx->setValue(reinterpret_cast<const uint8_t *>(line), strlen(line));
    s_tx->notify();
}

static void sendConfig(void)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (config == nullptr) {
        sendLine("ERR CONFIG");
        return;
    }

    char line[128];
    snprintf(line, sizeof(line), "WIFI_SSID=%s", config->wifi_ssid);
    sendLine(line);
    snprintf(line, sizeof(line), "SERVER_HOST=%s", config->server_host);
    sendLine(line);
    snprintf(line, sizeof(line), "SERVER_PORT=%u", static_cast<unsigned>(config->server_port));
    sendLine(line);
    snprintf(line, sizeof(line), "CHANNEL=%u", static_cast<unsigned>(config->channel));
    sendLine(line);
    snprintf(line, sizeof(line), "CALLSIGN=%s", config->callsign);
    sendLine(line);
    snprintf(line, sizeof(line), "CALL_SSID=%u", static_cast<unsigned>(config->callsign_ssid));
    sendLine(line);
    sendLine("OK GET");
}

static bool parseUnsigned(const char *text, unsigned long *out)
{
    if (text == nullptr || out == nullptr || *text == '\0') {
        return false;
    }

    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || (end != nullptr && *end != '\0')) {
        return false;
    }

    *out = value;
    return true;
}

static bool startsWith(const char *text, const char *prefix)
{
    return text != nullptr &&
           prefix != nullptr &&
           strncmp(text, prefix, strlen(prefix)) == 0;
}

static void trimInPlace(char *text)
{
    if (text == nullptr) {
        return;
    }

    char *start = text;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                       text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
}

static bool setField(const char *key, const char *value, bool *restart_wifi, bool *restart_udp)
{
    if (key == nullptr || value == nullptr || restart_wifi == nullptr || restart_udp == nullptr) {
        return false;
    }

    if (strcmp(key, "WIFI_SSID") == 0 || strcmp(key, "SSID") == 0) {
        const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
        const char *old_value = before != nullptr ? before->wifi_ssid : "";
        const bool changed = strcmp(old_value, value) != 0;
        if (!EXTERNAL_RADIO_SetWifiSsid(value, false)) {
            return false;
        }
        *restart_wifi = *restart_wifi || changed;
        return true;
    }

    if (strcmp(key, "WIFI_PASS") == 0 || strcmp(key, "WIFI_PASSWORD") == 0 || strcmp(key, "PASS") == 0) {
        const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
        const char *old_value = before != nullptr ? before->wifi_password : "";
        const bool changed = strcmp(old_value, value) != 0;
        if (!EXTERNAL_RADIO_SetWifiPassword(value, false)) {
            return false;
        }
        *restart_wifi = *restart_wifi || changed;
        return true;
    }

    if (strcmp(key, "SERVER_HOST") == 0 || strcmp(key, "D_IP") == 0) {
        const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
        const char *old_value = before != nullptr ? before->server_host : "";
        const bool changed = strcmp(old_value, value) != 0;
        if (!EXTERNAL_RADIO_SetServerHost(value, false)) {
            return false;
        }
        *restart_udp = *restart_udp || changed;
        return true;
    }

    if (strcmp(key, "SERVER_PORT") == 0 || strcmp(key, "D_PORT") == 0) {
        unsigned long port = 0;
        if (!parseUnsigned(value, &port) || port == 0 || port > 65535) {
            return false;
        }

        const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
        const uint16_t old_value = before != nullptr ? before->server_port : 0;
        const bool changed = old_value != static_cast<uint16_t>(port);
        if (!EXTERNAL_RADIO_SetServerPort(static_cast<uint16_t>(port), false)) {
            return false;
        }
        *restart_udp = *restart_udp || changed;
        return true;
    }

    if (strcmp(key, "CHANNEL") == 0 || strcmp(key, "CH") == 0) {
        unsigned long channel = 0;
        return parseUnsigned(value, &channel) &&
               channel <= 7 &&
               EXTERNAL_RADIO_SetChannel(static_cast<uint8_t>(channel), false);
    }

    if (strcmp(key, "CALLSIGN") == 0 || strcmp(key, "CALL") == 0) {
        return EXTERNAL_RADIO_SetCallsign(value, false);
    }

    if (strcmp(key, "CALL_SSID") == 0 || strcmp(key, "CALLSIGN_SSID") == 0) {
        unsigned long ssid = 0;
        return parseUnsigned(value, &ssid) &&
               ssid <= 255 &&
               EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(ssid), false);
    }

    return false;
}

static void sendHelp(void)
{
    sendLine("NRL BLE config commands:");
    sendLine("GET");
    sendLine("SET WIFI_SSID=<ssid>");
    sendLine("SET WIFI_PASS=<password>");
    sendLine("SET SERVER_HOST=<host_or_ip>");
    sendLine("SET SERVER_PORT=<1..65535>");
    sendLine("SET CHANNEL=<0..7>");
    sendLine("SET CALLSIGN=<callsign>");
    sendLine("SET CALL_SSID=<0..255>");
    sendLine("SAVE");
    sendLine("APPLY");
    sendLine("RESET_NET");
    sendLine("REBOOT");
}

static void handleCommand(char *command)
{
    trimInPlace(command);
    if (command[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "rx: %s", command);

    if (strcasecmp(command, "HELP") == 0 || strcmp(command, "?") == 0) {
        sendHelp();
        return;
    }

    if (strcasecmp(command, "GET") == 0) {
        sendConfig();
        return;
    }

    if (strcasecmp(command, "SAVE") == 0) {
        sendLine(EXTERNAL_RADIO_SaveConfig() ? "OK SAVE" : "ERR SAVE");
        return;
    }

    if (strcasecmp(command, "APPLY") == 0) {
        NRLAudioBridge_ApplyConfig(true, true);
        sendLine("OK APPLY");
        return;
    }

    if (strcasecmp(command, "RESET_NET") == 0) {
        if (EXTERNAL_RADIO_ResetNetworkConfig()) {
            NRLAudioBridge_ApplyConfig(true, true);
            sendLine("OK RESET_NET");
        } else {
            sendLine("ERR RESET_NET");
        }
        return;
    }

    if (strcasecmp(command, "REBOOT") == 0) {
        s_reboot_pending = true;
        s_reboot_at_ms = nowMsBle() + 500u;
        sendLine("OK REBOOT");
        return;
    }

    if (startsWith(command, "SET ")) {
        char *assignment = command + 4;
        trimInPlace(assignment);
        char *equals = strchr(assignment, '=');
        if (equals == nullptr) {
            sendLine("ERR SET");
            return;
        }

        *equals = '\0';
        char *key = assignment;
        char *value = equals + 1;
        trimInPlace(key);
        trimInPlace(value);
        for (char *p = key; *p != '\0'; ++p) {
            *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
        }

        bool restart_wifi = false;
        bool restart_udp = false;
        if (!setField(key, value, &restart_wifi, &restart_udp) || !EXTERNAL_RADIO_SaveConfig()) {
            sendLine("ERR SET");
            return;
        }

        if (restart_wifi || restart_udp) {
            NRLAudioBridge_ApplyConfig(restart_wifi, restart_udp);
        }
        sendLine("OK SET");
        return;
    }

    sendLine("ERR UNKNOWN");
}

static void appendCommandData(const uint8_t *data, const size_t size)
{
    if (data == nullptr || size == 0) {
        return;
    }

    for (size_t i = 0; i < size; ++i) {
        const char ch = static_cast<char>(data[i]);
        if (ch == '\n' || ch == '\r') {
            if (s_command_size > 0) {
                s_command_buffer[s_command_size] = '\0';
                handleCommand(s_command_buffer);
                s_command_size = 0;
            }
            continue;
        }

        if (s_command_size + 1 >= sizeof(s_command_buffer)) {
            s_command_size = 0;
            sendLine("ERR LINE_TOO_LONG");
            continue;
        }

        s_command_buffer[s_command_size++] = ch;
    }
}

class ConfigServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer * /*pServer*/, NimBLEConnInfo & /*connInfo*/) override
    {
        s_client_connected = true;
        s_advertising = false;
        ESP_LOGI(TAG, "client connected");
        sendLine(NRL_FIRMWARE_BANNER " BLE config ready. Send HELP.");
    }

    void onDisconnect(NimBLEServer * /*pServer*/, NimBLEConnInfo & /*connInfo*/, int reason) override
    {
        s_client_connected = false;
        s_advertising = false;
        ESP_LOGI(TAG, "client disconnected reason=%d", reason);
        // NimBLE doesn't auto-restart advertising; kick it off explicitly via
        // the BLEConfig_Poll() path (s_advertising = false triggers restart).
    }
};

class ConfigRxCallbacks final : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo & /*connInfo*/) override
    {
        if (characteristic == nullptr) {
            return;
        }
        const NimBLEAttValue value = characteristic->getValue();
        appendCommandData(value.data(), value.size());
    }
};

} // namespace

bool BLEConfig_Init(void)
{
    if (s_initialized) {
        return true;
    }

    NimBLEDevice::init(kBleDeviceName);
    // The shared BLE/WiFi radio cannot afford to lose modem-sleep cycles; keep
    // the default TX power. Larger advertising payloads cap out around 31 B
    // anyway for legacy 4.2 advertising.

    s_server = NimBLEDevice::createServer();
    if (s_server == nullptr) {
        ESP_LOGE(TAG, "server create failed");
        return false;
    }
    s_server->setCallbacks(new ConfigServerCallbacks());

    NimBLEService *service = s_server->createService(kServiceUuid);
    if (service == nullptr) {
        ESP_LOGE(TAG, "service create failed");
        return false;
    }

    s_tx = service->createCharacteristic(kTxUuid,
                                         NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
    s_rx = service->createCharacteristic(kRxUuid,
                                         NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    if (s_tx == nullptr || s_rx == nullptr) {
        ESP_LOGE(TAG, "characteristic create failed");
        return false;
    }

    s_rx->setCallbacks(new ConfigRxCallbacks());
    s_tx->setValue(NRL_FIRMWARE_BANNER " BLE config");

    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(kServiceUuid);
    advertising->enableScanResponse(true);
    advertising->start();

    s_initialized = true;
    s_advertising = true;
    ESP_LOGI(TAG, "advertising as %s", kBleDeviceName);
    return true;
}

void BLEConfig_Stop(void)
{
    if (!s_initialized) {
        return;
    }
    // deinit(true) stops advertising, closes any connection, shuts the NimBLE
    // host down and disables/deinits the BT controller -- which is what
    // actually drops WiFi/BT coexistence and frees the radio for WiFi. All
    // server/characteristic objects are freed, so reset our pointers; a later
    // BLEConfig_Init() rebuilds the GATT table from scratch.
    NimBLEDevice::deinit(true);
    s_server = nullptr;
    s_tx = nullptr;
    s_rx = nullptr;
    s_initialized = false;
    s_advertising = false;
    s_client_connected = false;
    ESP_LOGI(TAG, "BLE stopped (radio freed for WiFi)");
}

// Bring BLE up or down based on how long the WiFi STA has been up/down, so the
// shared radio is dedicated to WiFi during normal voice operation but BLE
// provisioning returns automatically when WiFi is unavailable.
static void manageCoexistence(void)
{
    const uint32_t now = nowMsBle();
    const bool sta_up = nrlWifiStaConnected();

    if (sta_up) {
        s_sta_down_since_ms = 0u;
        if (s_sta_up_since_ms == 0u) {
            s_sta_up_since_ms = now;
        }
        // Don't yank BLE out from under an in-progress provisioning session.
        if (!s_ble_auto_stopped && s_initialized && !s_client_connected &&
            (now - s_sta_up_since_ms) >= kBleStopAfterStaUpMs) {
            ESP_LOGI(TAG, "WiFi STA stable, stopping BLE to end coexistence");
            BLEConfig_Stop();
            s_ble_auto_stopped = true;
        }
    } else {
        s_sta_up_since_ms = 0u;
        if (s_sta_down_since_ms == 0u) {
            s_sta_down_since_ms = now;
        }
        if (s_ble_auto_stopped && !s_initialized &&
            (now - s_sta_down_since_ms) >= kBleStartAfterStaDownMs) {
            ESP_LOGI(TAG, "WiFi STA down, restarting BLE for fallback provisioning");
            BLEConfig_Init();
            s_ble_auto_stopped = false;
        }
    }
}

void BLEConfig_Poll(void)
{
    manageCoexistence();

    if (!s_initialized) {
        return;
    }

    if (!s_client_connected && !s_advertising) {
        NimBLEDevice::startAdvertising();
        s_advertising = true;
    }

    if (s_reboot_pending && static_cast<int32_t>(nowMsBle() - s_reboot_at_ms) >= 0) {
        ESP_LOGI(TAG, "reboot now");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}
