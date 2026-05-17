#ifndef DRIVER_EXTERNAL_RADIO_H
#define DRIVER_EXTERNAL_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void EXTERNAL_RADIO_Init(void);
uint8_t EXTERNAL_RADIO_GetChannel(void);
bool EXTERNAL_RADIO_SetChannel(uint8_t channel, bool persist);
bool EXTERNAL_RADIO_SaveConfig(void);
bool EXTERNAL_RADIO_ResetNetworkConfig(void);

#ifdef __cplusplus
}

struct SciSerialConfig {
    uint32_t baud;
    uint8_t data_bits;
    char parity;
    uint8_t stop_bits;
};

struct ExternalRadioConfig {
    uint8_t channel;
    uint16_t server_port;
    uint16_t local_port;
    uint8_t callsign_ssid;
    uint8_t device_mode;
    uint8_t mic_volume;
    uint8_t line_out_volume;
    bool hp_drive_enabled;
    bool drc_enabled;
    uint8_t drc_winsize;
    uint8_t drc_maxlevel;
    uint8_t drc_minlevel;
    uint8_t dac_ramprate;
    bool dac_eq_bypass;
    uint32_t daceq_b0;
    uint32_t daceq_b1;
    uint32_t daceq_a1;
    uint32_t wifi_ip;
    uint32_t wifi_netmask;
    uint32_t wifi_gateway;
    uint32_t wifi_dns;
    bool wifi_dhcp_enabled;
    SciSerialConfig sci;
    char wifi_ssid[33];
    char wifi_password[65];
    char server_host[65];
    char callsign[7];
};

const ExternalRadioConfig *EXTERNAL_RADIO_GetConfig(void);
bool EXTERNAL_RADIO_SetWifiSsid(const char *value, bool persist);
bool EXTERNAL_RADIO_SetWifiPassword(const char *value, bool persist);
bool EXTERNAL_RADIO_SetWifiIp(uint32_t value, bool persist);
bool EXTERNAL_RADIO_SetWifiNetmask(uint32_t value, bool persist);
bool EXTERNAL_RADIO_SetWifiGateway(uint32_t value, bool persist);
bool EXTERNAL_RADIO_SetWifiDns(uint32_t value, bool persist);
bool EXTERNAL_RADIO_SetWifiDhcpEnabled(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetServerHost(const char *value, bool persist);
bool EXTERNAL_RADIO_SetServerPort(uint16_t value, bool persist);
bool EXTERNAL_RADIO_SetLocalPort(uint16_t value, bool persist);
bool EXTERNAL_RADIO_SetCallsign(const char *value, bool persist);
bool EXTERNAL_RADIO_SetCallsignSsid(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetMicVolume(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetLineOutVolume(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetHpDriveEnabled(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetDrcEnabled(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetDrcWinsize(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetDrcMaxlevel(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetDrcMinlevel(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetDacRamprate(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetDacEqBypass(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetDacEqCoefficients(uint32_t b0, uint32_t b1, uint32_t a1, bool persist);
bool EXTERNAL_RADIO_SetSciConfig(uint32_t baud, uint8_t data_bits, char parity, uint8_t stop_bits, bool persist);
#endif

#endif // DRIVER_EXTERNAL_RADIO_H
