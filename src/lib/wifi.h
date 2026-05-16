#include <Arduino.h>
#include <WiFi.h>
#include "driver/external_radio.h"
#include <ctype.h>

//================ 配置/类型 =================

enum WifiConnResult : uint8_t {
  WIFI_CONN_OK = 0,          // 已连接到目标 SSID（本次或之前已在连）
  WIFI_ALREADY_ON_SSID,      // 之前就连着目标 SSID
  WIFI_CONN_TIMEOUT,         // 超时未连上
  WIFI_CONN_FAILED,          // 参数/硬件异常
};

struct NetProbeResult {
  bool       reachable = false;  // 是否判定能上网
  uint32_t   rtt_ms    = 0;      // 从发起到确认的毫秒耗时
  const char* method   = "";     // 命中的检测方法
};

//=============== 主机名（基于芯片ID） =================

static String makeHostnameFromEfuse() {
  uint64_t efuse = ESP.getEfuseMac();              // 48-bit 唯一ID（MAC）
  uint32_t tail  = static_cast<uint32_t>(efuse & 0xFFFFFFULL);
  char buf[32];
  snprintf(buf, sizeof(buf), "esp32s3-%06X", tail);
  return String(buf);
}

static void appendHostnameToken(String &hostname, const char *text) {
  if (text == nullptr) {
    return;
  }

  bool last_was_dash = false;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (isalnum(ch)) {
      hostname += static_cast<char>(tolower(ch));
      last_was_dash = false;
    } else if (!last_was_dash && hostname.length() > 0) {
      hostname += '-';
      last_was_dash = true;
    }
  }

  while (hostname.endsWith("-")) {
    hostname.remove(hostname.length() - 1);
  }
}

static String makeHostnameFromConfig() {
  const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
  if (config == nullptr || config->callsign[0] == '\0') {
    return makeHostnameFromEfuse();
  }

  const uint32_t tail = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFULL);
  char tail_text[8];
  snprintf(tail_text, sizeof(tail_text), "%06X", tail);

  String hostname = "nrl-";
  appendHostnameToken(hostname, config->callsign);
  if (hostname == "nrl") {
    return makeHostnameFromEfuse();
  }
  hostname += "-";
  hostname += String(static_cast<unsigned>(config->callsign_ssid));
  hostname += "-";
  appendHostnameToken(hostname, tail_text);
  return hostname;
}

//=============== Wi-Fi 辅助函数 =================

bool wifiIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

// Translate the numeric WiFi.status() code into readable text for diagnosing failures
static const char* wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD:       return "NO_SHIELD";
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
  }
}

// 由事件回调维护：STA 是否已完成 802.11 关联（L2 链路已建立）。
// 已关联但未拿到 IP 时，说明卡在 DHCP，而不是关联/认证。
static volatile bool s_wifi_sta_associated = false;

// WiFi event callback: prints low-level connection events (especially the STA
// disconnect reason code) to serial, which pinpoints the real failure cause.
static void wifiLogEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("[WiFi][evt] STA_START");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      s_wifi_sta_associated = true;
      Serial.printf("[WiFi][evt] STA_CONNECTED ssid=%.*s channel=%u\n",
                    (int)info.wifi_sta_connected.ssid_len,
                    (const char*)info.wifi_sta_connected.ssid,
                    (unsigned)info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi][evt] STA_GOT_IP %s\n",
                    IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      s_wifi_sta_associated = false;
      // reason码常见: 2=AUTH_EXPIRE 15=4WAY_HANDSHAKE_TIMEOUT(密码错)
      // 201=NO_AP_FOUND 205=CONNECTION_FAIL 3=ASSOC_EXPIRE
      Serial.printf("[WiFi][evt] STA_DISCONNECTED reason=%u\n",
                    (unsigned)info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_STOP:
      Serial.println("[WiFi][evt] STA_STOP");
      break;
    default:
      break;
  }
}

// Register the WiFi event logger exactly once.
static void wifiEnsureEventLogger() {
  static bool registered = false;
  if (!registered) {
    WiFi.onEvent(wifiLogEvent);
    registered = true;
  }
}

// 确保连接到指定 AP（若已在该 SSID 上则不动作；若连着别的先断开再连）
// timeout_ms 建议 10~20s
WifiConnResult wifiEnsureConnected(const char* ssid, const char* pass, uint32_t timeout_ms = 15000) {
  if (!ssid || !*ssid) {
    Serial.println("[WiFi] connect failed: empty SSID");
    return WIFI_CONN_FAILED;
  }

  Serial.printf("[WiFi] connecting: SSID=\"%s\", passLen=%u, timeout=%ums\n",
                ssid, (unsigned)(pass ? strlen(pass) : 0), timeout_ms);
  wifiEnsureEventLogger();

  if (wifiIsConnected()) {
    if (WiFi.SSID() == String(ssid)) {
      Serial.printf("[WiFi] already on target SSID, IP=%s, RSSI=%ddBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return WIFI_ALREADY_ON_SSID;
    }
    // 已连但不是目标 → 断开（第二个参数 true 会清除保存的凭据，避免自动回连旧 AP）
    Serial.printf("[WiFi] connected to other SSID=\"%s\", disconnecting first\n", WiFi.SSID().c_str());
    const bool keep_ap = WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA;
    WiFi.disconnect(!keep_ap /*wifi off*/, false /*erase saved*/);
    delay(100);
  }
  const wifi_mode_t prev_mode = WiFi.getMode();
  const wifi_mode_t want_mode =
      (prev_mode == WIFI_MODE_AP || prev_mode == WIFI_MODE_APSTA) ? WIFI_AP_STA : WIFI_STA;
  WiFi.mode(want_mode);
  if (prev_mode != want_mode) {
    // 切换模式后 STA 接口需要时间真正启动，过早 begin() 会让 esp_wifi_connect
    // 被丢弃、状态一直停在 IDLE。等待 STA 接口就绪。
    Serial.printf("[WiFi] mode switch %d -> %d, waiting for STA interface...\n",
                  (int)prev_mode, (int)want_mode);
    delay(300);
  }
  const String hostname = makeHostnameFromConfig();
  WiFi.setHostname(hostname.c_str());
  const ExternalRadioConfig *net_config = EXTERNAL_RADIO_GetConfig();
  if (net_config != nullptr && !net_config->wifi_dhcp_enabled &&
      net_config->wifi_ip != 0U && net_config->wifi_netmask != 0U && net_config->wifi_gateway != 0U) {
    const IPAddress ip(net_config->wifi_ip);
    const IPAddress gateway(net_config->wifi_gateway);
    const IPAddress netmask(net_config->wifi_netmask);
    const IPAddress dns(net_config->wifi_dns != 0U ? net_config->wifi_dns : net_config->wifi_gateway);
    if (WiFi.config(ip, gateway, netmask, dns)) {
      Serial.printf("[WiFi] static config: ip=%s mask=%s gateway=%s dns=%s\n",
                    ip.toString().c_str(),
                    netmask.toString().c_str(),
                    gateway.toString().c_str(),
                    dns.toString().c_str());
    } else {
      Serial.println("[WiFi] static config failed, continuing with DHCP");
    }
  } else {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }
  // 注意：不要调用 WiFi.setSleep(false)。本设备 BLE 与 WiFi 共用单射频，
  // 关闭 modem sleep 会让 WiFi 驱动直接 abort() 重启，必须保持省电开启。
  Serial.printf("[WiFi] mode=%d, hostname=%s, calling begin()...\n",
                (int)WiFi.getMode(), hostname.c_str());
  s_wifi_sta_associated = false;
  WiFi.begin(ssid, pass);

  const uint32_t t0 = millis();
  wl_status_t last_status = WL_NO_SHIELD;
  bool last_assoc = false;
  uint32_t last_log = 0;
  uint32_t last_rebegin = 0;
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeout_ms) {
    const wl_status_t status = WiFi.status();
    const bool assoc = s_wifi_sta_associated;
    const uint32_t elapsed = millis() - t0;
    // 状态变化、关联状态变化、或每 2 秒打印一次进度
    if (status != last_status || assoc != last_assoc || (elapsed - last_log) >= 2000) {
      Serial.printf("[WiFi] +%lums status=%d %s%s\n",
                    (unsigned long)elapsed, (int)status, wifiStatusToString(status),
                    assoc ? " (L2 associated, waiting for DHCP/IP)" : "");
      last_status = status;
      last_assoc = assoc;
      last_log = elapsed;
    }
    // 仅在“尚未完成 802.11 关联”时才重试 begin()。
    // 一旦已关联(STA_CONNECTED)，说明 L2 正常、正卡在 DHCP，
    // 此时重试 begin() 会断开关联并打断 DHCP，反而连不上。
    if (!assoc && (status == WL_IDLE_STATUS || status == WL_DISCONNECTED) &&
        elapsed >= 5000 && (elapsed - last_rebegin) >= 5000) {
      Serial.println("[WiFi] still not associating, re-issuing begin()");
      WiFi.disconnect(false /*wifi off*/, false /*erase saved*/);
      delay(50);
      WiFi.begin(ssid, pass);
      last_rebegin = elapsed;
    }
    delay(500);
  }

  const wl_status_t final_status = WiFi.status();
  if (final_status == WL_CONNECTED) {
    Serial.printf("[WiFi] connected! elapsed=%lums, IP=%s, gateway=%s, RSSI=%ddBm, channel=%d\n",
                  (unsigned long)(millis() - t0),
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.RSSI(), WiFi.channel());
    return WIFI_CONN_OK;
  }

  Serial.printf("[WiFi] connect timeout! elapsed=%lums, final status=%d %s\n",
                (unsigned long)(millis() - t0),
                (int)final_status, wifiStatusToString(final_status));
  // Hints for the most common failure causes
  if (s_wifi_sta_associated) {
    Serial.println("[WiFi]   -> hint: associated OK but no IP -> DHCP failed.");
    Serial.println("[WiFi]      check router DHCP pool / MAC filter / client limit, or assign a static IP");
  } else if (final_status == WL_NO_SSID_AVAIL) {
    Serial.println("[WiFi]   -> hint: SSID not found, check the name / 5GHz-only band / distance");
  } else if (final_status == WL_CONNECT_FAILED) {
    Serial.println("[WiFi]   -> hint: connection rejected, likely wrong password or unsupported auth");
  } else if (final_status == WL_DISCONNECTED || final_status == WL_IDLE_STATUS) {
    Serial.println("[WiFi]   -> hint: 802.11 association never completed, weak signal (RSSI<-80) / AP rejected");
  }
  return WIFI_CONN_TIMEOUT;
}

//=============== 外网探测（仅 223.5.5.5 & baidu） =================

// 直连 IP:port，返回是否可达，并输出连接 RTT（毫秒）
static bool tcpReachableIP_RTT(const IPAddress& ip, uint16_t port,
                               uint32_t /*timeout_ms*/, uint32_t& out_ms) {
  WiFiClient c;
  uint32_t t0 = millis();
  bool ok = c.connect(ip, port); // Arduino 核心常见为阻塞 connect
  out_ms = millis() - t0;
  if (ok) c.stop();
  return ok;
}

// HTTP 首行探测（Host 先 DNS 解析），返回是否拿到首行，并输出 RTT（毫秒）
static bool httpGetStatusLine_RTT(const char* host, uint16_t port, const char* path,
                                  String& outStatus, uint32_t timeout_ms, uint32_t& out_ms) {
  out_ms = 0;

  IPAddress ip;
  uint32_t t0 = millis();
  if (!WiFi.hostByName(host, ip)) return false;

  WiFiClient client;
  if (!client.connect(ip, port)) return false;

  client.print(String("GET ") + path + " HTTP/1.1\r\n"
               "Host: " + host + "\r\n"
               "Connection: close\r\n\r\n");

  while (millis() - t0 < timeout_ms) {
    if (client.available()) {
      outStatus = client.readStringUntil('\n');
      outStatus.trim();
      out_ms = millis() - t0;
      client.stop();
      return true;
    }
    delay(5);
  }
  client.stop();
  return false;
}

// 仅使用：
// 1) TCP 223.5.5.5:53（阿里DNS）
// 2) HTTP www.baidu.com（2xx/3xx 视为可达）
NetProbeResult internetProbeCN(uint32_t timeout_ms = 5000) {
  NetProbeResult r;
  if (!wifiIsConnected()) return r;

  // Step 1: 直连公共 DNS（不依赖 DNS）
  {
    IPAddress ali(223,5,5,5);
    uint32_t ms = 0;
    if (tcpReachableIP_RTT(ali, 53, timeout_ms, ms)) {
      r.reachable = true; r.rtt_ms = ms; r.method = "TCP 223.5.5.5:53";
      return r;
    }
  }

  // Step 2: 宽松判定（门户/劫持下可能返回 200/302，但说明能出网）
  {
    String status; uint32_t ms = 0;
    if (httpGetStatusLine_RTT("www.baidu.com", 80, "/", status, timeout_ms, ms)) {
      if (status.startsWith("HTTP/1.1 200") || status.startsWith("HTTP/1.0 200") ||
          status.startsWith("HTTP/1.1 30")  || status.startsWith("HTTP/1.0 30")) {
        r.reachable = true; r.rtt_ms = ms; r.method = "HTTP baidu 2xx/3xx";
        return r;
      }
    }
  }

  // 都失败
  return r;
}

// //=============== 可选：最小演示（打开下面宏即可在单文件里测试） ================
// // #define NET_UTILS_DEMO
// #ifdef NET_UTILS_DEMO
// void setup() {
//   Serial.begin(115200);
//   delay(200);

//   const char* SSID = "YOUR_SSID";
//   const char* PASS = "YOUR_PASS";

//   auto res = wifiEnsureConnected(SSID, PASS, 15000);
//   Serial.printf("wifiEnsureConnected -> %d\n", res);

//   if (wifiIsConnected()) {
//     Serial.print("Host: "); Serial.println(WiFi.getHostname());
//     Serial.print("IP  : "); Serial.println(WiFi.localIP());
//     auto pr = internetProbeCN(5000);
//     if (pr.reachable) {
//       Serial.printf("Internet OK, rtt=%ums via %s\n", pr.rtt_ms, pr.method);
//     } else {
//       Serial.println("Internet NOT reachable");
//     }
//   } else {
//     Serial.println("WiFi not connected.");
//   }
// }

// void loop() {
//   static uint32_t last = 0;
//   if (millis() - last > 10000) {
//     last = millis();
//     auto pr = internetProbeCN(3000);
//     Serial.printf("[link] %s, [net] %s (%ums via %s)\n",
//       wifiIsConnected() ? "UP" : "DOWN",
//       pr.reachable ? "OK" : "FAIL",
//       pr.rtt_ms, pr.method);
//   }
// }
// #endif
