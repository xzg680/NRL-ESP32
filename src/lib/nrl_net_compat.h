#pragma once

#include <esp_netif.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Helpers that query the WiFi netifs (created by nrl_wifi.cpp or, for the
// remaining Arduino-WiFi callers, by arduino-esp32 itself) via IDF accessors
// so callers can drop <WiFi.h>.
//
// Construct an IPv4 address literal in our uint32_t convention (matches
// sockaddr_in.sin_addr.s_addr / ip4_addr_t::addr / IPAddress(uint32_t)).
#define NRL_IPV4(a,b,c,d) \
    ((uint32_t)((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24)))
//
// Byte ordering: ip4_addr_t::addr stores the IP with octet[0] in the LSB on
// little-endian targets, the same convention as Arduino IPAddress(uint32_t).
// uint32_t values produced here may flow directly to ExternalRadioConfig
// fields (config->wifi_ip etc.) and to lwIP sockaddr_in.sin_addr.s_addr.

static inline esp_netif_t *nrlWifiStaNetif(void)
{
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

static inline esp_netif_t *nrlWifiApNetif(void)
{
    return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
}

static inline bool nrlWifiStaConnected(void)
{
    esp_netif_t *netif = nrlWifiStaNetif();
    if (netif == NULL || !esp_netif_is_netif_up(netif)) {
        return false;
    }
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return false;
    }
    return info.ip.addr != 0;
}

static inline uint32_t nrlWifiStaIp(void)
{
    esp_netif_t *netif = nrlWifiStaNetif();
    if (netif == NULL) {
        return 0u;
    }
    esp_netif_ip_info_t info = {};
    return (esp_netif_get_ip_info(netif, &info) == ESP_OK) ? info.ip.addr : 0u;
}

static inline uint32_t nrlWifiStaNetmask(void)
{
    esp_netif_t *netif = nrlWifiStaNetif();
    if (netif == NULL) {
        return 0u;
    }
    esp_netif_ip_info_t info = {};
    return (esp_netif_get_ip_info(netif, &info) == ESP_OK) ? info.netmask.addr : 0u;
}

static inline uint32_t nrlWifiStaGateway(void)
{
    esp_netif_t *netif = nrlWifiStaNetif();
    if (netif == NULL) {
        return 0u;
    }
    esp_netif_ip_info_t info = {};
    return (esp_netif_get_ip_info(netif, &info) == ESP_OK) ? info.gw.addr : 0u;
}

static inline uint32_t nrlWifiStaDns(void)
{
    esp_netif_t *netif = nrlWifiStaNetif();
    if (netif == NULL) {
        return 0u;
    }
    esp_netif_dns_info_t dns = {};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        return 0u;
    }
    return dns.ip.u_addr.ip4.addr;
}

static inline uint32_t nrlWifiApIp(void)
{
    esp_netif_t *netif = nrlWifiApNetif();
    if (netif == NULL) {
        return 0u;
    }
    esp_netif_ip_info_t info = {};
    return (esp_netif_get_ip_info(netif, &info) == ESP_OK) ? info.ip.addr : 0u;
}

static inline void nrlIpToString(uint32_t ip, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0u) {
        return;
    }
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(buf, buf_size, "%u.%u.%u.%u",
             (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3]);
}
