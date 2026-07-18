#include "services/aprs_service.h"

#include "audio/audio_router.h"
#include "audio/audio_focus.h"
#include "driver/board_pins.h"
#include "driver/external_radio.h"
#include "lib/aprs/aprs_ax25.h"
#include "lib/aprs/aprs_modem.h"
#include "lib/aprs/parse_aprs.h"
#include "lib/aprs/pbuf.h"
#include "lib/nrl_version.h"
#include "lib/nrl_wifi.h"
#include "services/config_notify.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <nvs.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "APRS";

namespace {

constexpr const char *kNvsNamespace = "aprs";
constexpr const char *kNvsKey = "cfg";

constexpr uint32_t kPersistMagic = 0x41505253u; // 'APRS'
constexpr uint8_t kPersistVersion = 1u;
constexpr const char *kDefaultAprsServer = "asia.aprs2.net";

constexpr uint16_t kMinBeaconIntervalS = 10u;
constexpr uint16_t kMaxBeaconIntervalS = 3600u;
// Auto-interval (SmartBeaconing-style) pacing. With a fresh GPS fix the
// effective period scales inversely with speed -- 60+ km/h beacons at the
// 30 s floor, slower proportionally longer -- and a >kAutoMoveKm jump from
// the last-beaconed position sends one early. The configured
// beacon_interval_s always remains the stationary/no-GPS ceiling.
constexpr uint16_t kAutoMinIntervalS = 30u;
constexpr double kAutoFastSpeedKmh = 60.0; // at/above: beacon at the floor rate
constexpr double kAutoSlowSpeedKmh = 3.0;  // below: treated as stationary
constexpr double kAutoMoveKm = 0.3;        // early beacon after moving this far
// WGS-84 default: 3153.3100N, 11848.8460E.
constexpr int32_t kDefaultLatE6 = 31888500;
constexpr int32_t kDefaultLonE6 = 118814100;
constexpr uint32_t kGpsFixFreshMs = 10000u;   // fix older than this = stale
constexpr uint32_t kIsReconnectMs = 30000u;   // APRS-IS reconnect backoff
constexpr size_t kStationCount = 64u;         // PSRAM station table size
constexpr size_t kRxRingSamples = 16384u;     // ~1 s of 16 kHz mic tap
constexpr size_t kTxChunkSamples = 160u;      // 20 ms at MODEM_TX_SAMPLE_RATE
constexpr int16_t kTxAmplitude = 180;         // per 8-bit sine step (~0.7 FS)

// Persisted blob (NVS). New fields append at the END: loadConfig accepts a
// shorter (older) blob and the missing tail stays zero-initialised, so
// existing configs survive upgrades without a version bump.
struct PersistBlob {
    uint32_t magic;
    uint8_t version;
    uint8_t enabled;
    uint8_t net_enabled;
    uint8_t rf_tx_enabled;
    uint8_t rf_rx_enabled;
    uint8_t ssid;
    char symbol_table;
    char symbol_code;
    uint16_t beacon_interval_s;
    int32_t default_lat_e6;
    int32_t default_lon_e6;
    uint16_t server_port;
    char server_host[65];
    char path[17];
    char comment[41];
    uint8_t auto_interval; // appended field: absent in pre-0.6.7 blobs
} __attribute__((packed));

struct StationRec {
    char name[10];
    char symbol[3];
    float lat;
    float lon;
    float altitude_m;
    float speed_kmh;
    uint16_t course_deg;
    float derived_speed_kmh;
    float distance_km;
    uint16_t bearing_deg;
    uint32_t last_heard_ms;
    uint32_t pkt_count;
    uint8_t via_rf;
    char comment[40];
    bool used;
};

AprsConfig s_cfg = {};
SemaphoreHandle_t s_cfg_mutex = nullptr;
SemaphoreHandle_t s_station_mutex = nullptr;

StationRec *s_stations = nullptr; // PSRAM, kStationCount entries
uint32_t s_station_revision = 0;

TaskHandle_t s_task = nullptr;

ParseAPRS s_parser;

// GPS state (updated from the task's NMEA parsing only)
volatile bool s_gps_fix = false;
double s_gps_lat = 0.0;
double s_gps_lon = 0.0;
double s_gps_alt_m = 0.0;
float s_gps_speed_kmh = -1.0f;
uint16_t s_gps_course = 0;
uint32_t s_gps_last_fix_ms = 0;

// NMEA byte tee ring (producer: bridge task, consumer: aprs task)
constexpr size_t kNmeaRingSize = 1024u;
uint8_t s_nmea_ring[kNmeaRingSize];
volatile size_t s_nmea_head = 0;
volatile size_t s_nmea_tail = 0;
char s_nmea_line[128];
size_t s_nmea_line_len = 0;

// RF RX PCM ring (producer: audio task via router sink, consumer: aprs task)
int16_t *s_rx_ring = nullptr; // PSRAM
volatile size_t s_rx_head = 0;
volatile size_t s_rx_tail = 0;
// 16000 -> 9600 linear-interpolation resampler state (Q16 phase)
uint32_t s_resample_phase = 0;
int16_t s_resample_last = 0;
// running RMS estimate of the mic tap, for the modem's signal report
uint32_t s_rx_level_acc = 0;
uint16_t s_rx_level_mv = 0;
uint32_t s_rx_level_n = 0;

// APRS-IS client state. The socket is owned exclusively by the APRS task;
// other threads request a reconnect via the flag instead of closing it.
int s_is_socket = -1;
bool s_is_logged_in = false;
bool s_is_status_sent = false;
volatile bool s_is_reconnect_requested = false;
uint32_t s_is_last_attempt_ms = 0;
char s_is_line[600];
size_t s_is_line_len = 0;

// counters / last packet
uint32_t s_rx_count = 0;
uint32_t s_tx_count = 0;
char s_last_packet[300] = {0};
uint32_t s_last_packet_seq = 0;

// TX pacing
bool s_tx_pumping = false;
uint32_t s_last_beacon_ms = 0; // 0 = beacon at the next enabled task tick
// Position when the last beacon left with a live fix, for the auto-interval
// movement trigger. Task-private (aprsTask writes and reads).
double s_last_beacon_lat = 0.0;
double s_last_beacon_lon = 0.0;
bool s_last_beacon_pos_valid = false;

inline uint32_t nowMs()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

const char *boardType()
{
#if NRL_BOARD == NRL_BOARD_GEZIPAI
    return "gezipai";
#elif NRL_BOARD == NRL_BOARD_BH4TDV
    return "bh4tdv";
#elif NRL_BOARD == NRL_BOARD_S31_KORVO
    return "s31_korvo";
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
    return "s31_function_coreboard";
#else
    return "unknown";
#endif
}

void defaultConfig(AprsConfig &cfg)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = false;
    cfg.net_enabled = true;
    cfg.rf_tx_enabled = false;
    cfg.rf_rx_enabled = false;
    cfg.ssid = 5;
    cfg.symbol_table = '/';
    cfg.symbol_code = 'I'; // APRS TCP/IP symbol (/I)
    cfg.beacon_interval_s = 60;
    cfg.auto_interval = false;
    cfg.default_lat_e6 = kDefaultLatE6;
    cfg.default_lon_e6 = kDefaultLonE6;
    cfg.server_port = 14580;
    snprintf(cfg.server_host, sizeof(cfg.server_host), "%s", kDefaultAprsServer);
    snprintf(cfg.path, sizeof(cfg.path), "WIDE1-1");
    cfg.comment[0] = '\0';
}

void persistConfig()
{
    PersistBlob blob = {};
    blob.magic = kPersistMagic;
    blob.version = kPersistVersion;
    blob.enabled = s_cfg.enabled ? 1 : 0;
    blob.net_enabled = s_cfg.net_enabled ? 1 : 0;
    blob.rf_tx_enabled = s_cfg.rf_tx_enabled ? 1 : 0;
    blob.rf_rx_enabled = s_cfg.rf_rx_enabled ? 1 : 0;
    blob.ssid = s_cfg.ssid;
    blob.symbol_table = s_cfg.symbol_table;
    blob.symbol_code = s_cfg.symbol_code;
    blob.beacon_interval_s = s_cfg.beacon_interval_s;
    blob.auto_interval = s_cfg.auto_interval ? 1 : 0;
    blob.default_lat_e6 = s_cfg.default_lat_e6;
    blob.default_lon_e6 = s_cfg.default_lon_e6;
    blob.server_port = s_cfg.server_port;
    memcpy(blob.server_host, s_cfg.server_host, sizeof(blob.server_host));
    memcpy(blob.path, s_cfg.path, sizeof(blob.path));
    memcpy(blob.comment, s_cfg.comment, sizeof(blob.comment));

    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_blob(nvs, kNvsKey, &blob, sizeof(blob));
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    CONFIG_NOTIFY_Bump();
}

void loadConfig()
{
    defaultConfig(s_cfg);

    PersistBlob blob = {};
    size_t size = sizeof(blob);
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    const esp_err_t err = nvs_get_blob(nvs, kNvsKey, &blob, &size);
    nvs_close(nvs);
    // Accept older, shorter blobs: appended tail fields keep their zero init.
    if (err != ESP_OK || size < offsetof(PersistBlob, auto_interval) ||
        size > sizeof(blob) ||
        blob.magic != kPersistMagic || blob.version != kPersistVersion) {
        return;
    }

    s_cfg.enabled = blob.enabled != 0;
    s_cfg.net_enabled = blob.net_enabled != 0;
    s_cfg.rf_tx_enabled = blob.rf_tx_enabled != 0;
    s_cfg.rf_rx_enabled = blob.rf_rx_enabled != 0;
    s_cfg.ssid = blob.ssid & 0x0F;
    s_cfg.symbol_table = blob.symbol_table ? blob.symbol_table : '/';
    s_cfg.symbol_code = blob.symbol_code ? blob.symbol_code : 'I';
    const bool migrate_tcpip_symbol =
        s_cfg.symbol_table == '/' && s_cfg.symbol_code == '&';
    if (migrate_tcpip_symbol) {
        s_cfg.symbol_code = 'I';
    }
    s_cfg.beacon_interval_s = blob.beacon_interval_s;
    if (s_cfg.beacon_interval_s < kMinBeaconIntervalS ||
        s_cfg.beacon_interval_s > kMaxBeaconIntervalS) {
        s_cfg.beacon_interval_s = 60;
    }
    s_cfg.auto_interval = blob.auto_interval != 0;
    const bool migrate_zero_position =
        blob.default_lat_e6 == 0 && blob.default_lon_e6 == 0;
    s_cfg.default_lat_e6 = migrate_zero_position ? kDefaultLatE6 : blob.default_lat_e6;
    s_cfg.default_lon_e6 = migrate_zero_position ? kDefaultLonE6 : blob.default_lon_e6;
    s_cfg.server_port = blob.server_port ? blob.server_port : 14580;
    memcpy(s_cfg.server_host, blob.server_host, sizeof(s_cfg.server_host));
    s_cfg.server_host[sizeof(s_cfg.server_host) - 1] = '\0';
    const bool migrate_aprs_server =
        s_cfg.server_host[0] == '\0' || strcmp(s_cfg.server_host, "aprs.tv") == 0;
    if (migrate_aprs_server) {
        snprintf(s_cfg.server_host, sizeof(s_cfg.server_host), "%s", kDefaultAprsServer);
    }
    memcpy(s_cfg.path, blob.path, sizeof(s_cfg.path));
    s_cfg.path[sizeof(s_cfg.path) - 1] = '\0';
    memcpy(s_cfg.comment, blob.comment, sizeof(s_cfg.comment));
    s_cfg.comment[sizeof(s_cfg.comment) - 1] = '\0';
    if (migrate_zero_position || migrate_aprs_server || migrate_tcpip_symbol) {
        ESP_LOGI(TAG, "migrating APRS defaults: position=%d server=%d symbol=%d",
                 (int)migrate_zero_position, (int)migrate_aprs_server,
                 (int)migrate_tcpip_symbol);
        persistConfig();
    }
}

void lockCfg() { xSemaphoreTake(s_cfg_mutex, portMAX_DELAY); }
void unlockCfg() { xSemaphoreGive(s_cfg_mutex); }
void lockStations() { xSemaphoreTake(s_station_mutex, portMAX_DELAY); }
void unlockStations() { xSemaphoreGive(s_station_mutex); }

// ---------------------------------------------------------------- callsign --

void myCallsign(char *out, size_t out_size)
{
    const ExternalRadioConfig *rc = EXTERNAL_RADIO_GetConfig();
    const char *call = (rc != nullptr && rc->callsign[0] != '\0') ? rc->callsign : "N0CALL";
    if (s_cfg.ssid > 0) {
        snprintf(out, out_size, "%s-%u", call, (unsigned)s_cfg.ssid);
    } else {
        snprintf(out, out_size, "%s", call);
    }
}

// -------------------------------------------------------------------- NMEA --

double nmeaCoordToDeg(const char *field, const char *hemi)
{
    // ddmm.mmmm / dddmm.mmmm
    const double raw = atof(field);
    const double deg = floor(raw / 100.0);
    const double min = raw - deg * 100.0;
    double val = deg + min / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') {
        val = -val;
    }
    return val;
}

// Split a NMEA sentence into comma fields (in place). Returns field count.
size_t splitNmea(char *line, const char *fields[], size_t max_fields)
{
    size_t count = 0;
    char *p = line;
    while (count < max_fields) {
        fields[count++] = p;
        char *comma = strchr(p, ',');
        if (comma == nullptr) {
            break;
        }
        *comma = '\0';
        p = comma + 1;
    }
    return count;
}

void handleNmeaLine(char *line)
{
    // strip checksum
    char *star = strchr(line, '*');
    if (star != nullptr) {
        *star = '\0';
    }
    if (line[0] != '$' || strlen(line) < 10) {
        return;
    }
    const char *type = line + 3; // skip "$GP"/"$GN"/"$BD"...

    const char *f[20] = {nullptr};
    if (strncmp(type, "RMC", 3) == 0) {
        const size_t n = splitNmea(line, f, 20);
        // $xxRMC,time,status,lat,NS,lon,EW,speed_kn,course,date,...
        if (n >= 9 && f[2][0] == 'A' && f[3][0] != '\0' && f[5][0] != '\0') {
            s_gps_lat = nmeaCoordToDeg(f[3], f[4]);
            s_gps_lon = nmeaCoordToDeg(f[5], f[6]);
            s_gps_speed_kmh = (f[7][0] != '\0') ? (float)(atof(f[7]) * 1.852) : -1.0f;
            s_gps_course = (f[8][0] != '\0') ? (uint16_t)atof(f[8]) : 0;
            s_gps_last_fix_ms = nowMs();
            s_gps_fix = true;
        } else if (n >= 3 && f[2][0] == 'V') {
            s_gps_fix = false;
        }
    } else if (strncmp(type, "GGA", 3) == 0) {
        const size_t n = splitNmea(line, f, 20);
        // $xxGGA,time,lat,NS,lon,EW,fix,sats,hdop,alt,M,...
        if (n >= 10 && f[6][0] != '\0' && f[6][0] != '0' && f[2][0] != '\0') {
            s_gps_lat = nmeaCoordToDeg(f[2], f[3]);
            s_gps_lon = nmeaCoordToDeg(f[4], f[5]);
            if (f[9][0] != '\0') {
                s_gps_alt_m = atof(f[9]);
            }
            s_gps_last_fix_ms = nowMs();
            s_gps_fix = true;
        }
    }
}

void drainNmeaRing()
{
    while (s_nmea_tail != s_nmea_head) {
        const uint8_t c = s_nmea_ring[s_nmea_tail];
        s_nmea_tail = (s_nmea_tail + 1) % kNmeaRingSize;
        if (c == '\n' || c == '\r') {
            if (s_nmea_line_len > 0) {
                s_nmea_line[s_nmea_line_len] = '\0';
                handleNmeaLine(s_nmea_line);
                s_nmea_line_len = 0;
            }
        } else if (s_nmea_line_len < sizeof(s_nmea_line) - 1) {
            if (s_nmea_line_len == 0 && c != '$') {
                continue; // resync on sentence start
            }
            s_nmea_line[s_nmea_line_len++] = (char)c;
        } else {
            s_nmea_line_len = 0;
        }
    }
}

bool ownPosition(double *lat, double *lon, double *alt_m)
{
    const bool fresh = s_gps_fix && (nowMs() - s_gps_last_fix_ms) < kGpsFixFreshMs;
    if (fresh) {
        *lat = s_gps_lat;
        *lon = s_gps_lon;
        if (alt_m != nullptr) {
            *alt_m = s_gps_alt_m;
        }
        return true;
    }
    *lat = (double)s_cfg.default_lat_e6 / 1e6;
    *lon = (double)s_cfg.default_lon_e6 / 1e6;
    if (alt_m != nullptr) {
        *alt_m = 0.0;
    }
    return false;
}

// ---------------------------------------------------------------- stations --

void touchLastPacket(const char *tnc2)
{
    lockStations(); // shares the station mutex with APRS_SERVICE_GetLastPacket
    strncpy(s_last_packet, tnc2, sizeof(s_last_packet) - 1);
    s_last_packet[sizeof(s_last_packet) - 1] = '\0';
    s_last_packet_seq++;
    unlockStations();
}

void updateStation(const struct pbuf_t *pb, bool via_rf)
{
    if (!(pb->flags & F_HASPOS) || pb->srcname == nullptr || pb->srcname_len == 0) {
        return;
    }

    char name[10] = {0};
    const size_t name_len =
        (pb->srcname_len < sizeof(name) - 1) ? pb->srcname_len : sizeof(name) - 1;
    memcpy(name, pb->srcname, name_len);

    const uint32_t now = nowMs();

    lockStations();
    StationRec *slot = nullptr;
    StationRec *free_slot = nullptr;
    StationRec *oldest = nullptr;
    for (size_t i = 0; i < kStationCount; ++i) {
        StationRec &rec = s_stations[i];
        if (rec.used && strcmp(rec.name, name) == 0) {
            slot = &rec;
            break;
        }
        if (!rec.used) {
            if (free_slot == nullptr) {
                free_slot = &rec;
            }
        } else if (oldest == nullptr ||
                   (int32_t)(rec.last_heard_ms - oldest->last_heard_ms) < 0) {
            oldest = &rec;
        }
    }
    if (slot == nullptr) {
        slot = (free_slot != nullptr) ? free_slot : oldest;
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->name, name, sizeof(slot->name));
        slot->altitude_m = NAN;
        slot->speed_kmh = NAN;
        slot->derived_speed_kmh = NAN;
        slot->distance_km = NAN;
        slot->used = true;
    }

    // speed derived from consecutive packets of the same station (>5 s apart
    // so position jitter doesn't explode the figure)
    if (slot->pkt_count > 0) {
        const uint32_t dt_ms = now - slot->last_heard_ms;
        if (dt_ms > 5000u) {
            const double moved_km =
                s_parser.distance(slot->lon, slot->lat, pb->lng, pb->lat);
            const double hours = (double)dt_ms / 3600000.0;
            if (moved_km < 1000.0) {
                slot->derived_speed_kmh = (float)(moved_km / hours);
            }
        }
    }

    slot->lat = pb->lat;
    slot->lon = pb->lng;
    memcpy(slot->symbol, pb->symbol, sizeof(slot->symbol));
    if (pb->flags & F_ALT) {
        slot->altitude_m = (float)pb->altitude;
    }
    if (pb->flags & F_CSRSPD) {
        slot->speed_kmh = (float)pb->speed;
        slot->course_deg = pb->course;
    }
    if (pb->comment != nullptr && pb->comment_len > 0) {
        const size_t clen = (pb->comment_len < sizeof(slot->comment) - 1)
                                ? pb->comment_len
                                : sizeof(slot->comment) - 1;
        memcpy(slot->comment, pb->comment, clen);
        slot->comment[clen] = '\0';
    }

    double own_lat = 0.0, own_lon = 0.0;
    ownPosition(&own_lat, &own_lon, nullptr);
    if (own_lat != 0.0 || own_lon != 0.0) {
        slot->distance_km =
            (float)s_parser.distance(own_lon, own_lat, slot->lon, slot->lat);
        slot->bearing_deg =
            (uint16_t)s_parser.direction(own_lon, own_lat, slot->lon, slot->lat);
    }

    slot->last_heard_ms = now;
    slot->pkt_count++;
    slot->via_rf = via_rf ? 1 : 0;
    s_station_revision++;
    unlockStations();
}

// ------------------------------------------------------------- TNC2 parsing --

// Prepare a pbuf_t from a TNC2 monitor line ("SRC>DST,PATH:info") and run the
// aprsc parser over it (same pointer preparation as upstream dispWindow()).
bool parseTnc2(const char *line_in, bool via_rf)
{
    size_t len = strlen(line_in);
    if (len < 10 || len >= 290) {
        return false;
    }

    // third-party traffic ("...:}SRC>DST,...") -- parse the inner packet
    const char *third = strstr(line_in, ":}");
    if (third != nullptr) {
        const char *inner = third + 2;
        const char *gt = strchr(inner, '>');
        if (gt != nullptr && (gt - inner) > 3 && (gt - inner) < 12) {
            line_in = inner;
            len = strlen(line_in);
        }
    }

    const char *gt = strchr(line_in, '>');
    const char *colon = strchr(line_in, ':');
    if (gt == nullptr || colon == nullptr || (gt - line_in) < 4 ||
        (gt - line_in) > 11 || colon < gt) {
        return false;
    }

    static struct pbuf_t pb; // large; keep off the task stack
    memset(&pb, 0, sizeof(pb));
    pb.buf_len = sizeof(pb.data);
    pb.packet_len = (int)len;
    memcpy(pb.data, line_in, len);
    pb.data[len] = '\0';

    const size_t gt_off = (size_t)(gt - line_in);
    const size_t colon_off = (size_t)(colon - line_in);

    const char *comma = strchr(line_in + gt_off, ',');
    size_t end_ssid_off =
        (comma != nullptr && (size_t)(comma - line_in) < colon_off)
            ? (size_t)(comma - line_in)
            : colon_off;

    const char *dash = strchr(line_in + gt_off, '-');
    if (dash != nullptr && (size_t)(dash - line_in) < end_ssid_off) {
        pb.dstcall_end_or_ssid = &pb.data[dash - line_in];
    } else {
        pb.dstcall_end_or_ssid = &pb.data[end_ssid_off];
    }
    pb.info_start = &pb.data[colon_off + 1];
    pb.dstname = &pb.data[gt_off + 1];
    pb.dstname_len = (uint8_t)(end_ssid_off - gt_off - 1);
    pb.dstcall_end = &pb.data[end_ssid_off];
    pb.srccall_end = &pb.data[gt_off];

    if (s_parser.parse_aprs(&pb) == 0) {
        return false;
    }

    touchLastPacket(line_in);
    updateStation(&pb, via_rf);
    return true;
}

// --------------------------------------------------------------- APRS-IS ----

void isDisconnect()
{
    if (s_is_socket >= 0) {
        close(s_is_socket);
        s_is_socket = -1;
    }
    s_is_logged_in = false;
    s_is_status_sent = false;
    s_is_line_len = 0;
}

void isSendLine(const char *line)
{
    if (s_is_socket < 0) {
        return;
    }
    char buf[600];
    const int n = snprintf(buf, sizeof(buf), "%s\r\n", line);
    if (n > 0) {
        if (send(s_is_socket, buf, (size_t)n, 0) < 0) {
            ESP_LOGW(TAG, "APRS-IS send failed, reconnecting");
            isDisconnect();
        }
    }
}

void isTryConnect()
{
    const uint32_t now = nowMs();
    if ((now - s_is_last_attempt_ms) < kIsReconnectMs && s_is_last_attempt_ms != 0) {
        return;
    }
    s_is_last_attempt_ms = now;

    char host[65];
    uint16_t port;
    lockCfg();
    strncpy(host, s_cfg.server_host, sizeof(host));
    host[sizeof(host) - 1] = '\0';
    port = s_cfg.server_port;
    unlockCfg();

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == nullptr) {
        ESP_LOGW(TAG, "APRS-IS DNS failed for %s", host);
        return;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return;
    }
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "APRS-IS connect to %s:%u failed", host, (unsigned)port);
        close(sock);
        freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res);

    // non-blocking from here on; the task polls recv()
    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    s_is_socket = sock;

    char call[16];
    lockCfg();
    myCallsign(call, sizeof(call));
    unlockCfg();
    char basecall[16];
    strncpy(basecall, call, sizeof(basecall));
    basecall[sizeof(basecall) - 1] = '\0';
    char *dash = strchr(basecall, '-');
    if (dash != nullptr) {
        *dash = '\0';
    }
    const uint16_t pass = s_parser.passCode(basecall);

    char login[160];
    snprintf(login, sizeof(login),
             "user %s pass %u vers NRL-ESP32 1.0 filter m/100", call, (unsigned)pass);
    isSendLine(login);
    s_is_logged_in = true;
    ESP_LOGI(TAG, "APRS-IS connected to %s:%u as %s", host, (unsigned)port, call);
}

void isPoll()
{
    if (s_is_socket < 0) {
        return;
    }
    char buf[256];
    for (;;) {
        const int n = recv(s_is_socket, buf, sizeof(buf), 0);
        if (n == 0) {
            ESP_LOGW(TAG, "APRS-IS closed by server");
            isDisconnect();
            return;
        }
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return;
            }
            ESP_LOGW(TAG, "APRS-IS recv error %d", errno);
            isDisconnect();
            return;
        }
        for (int i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (s_is_line_len > 0) {
                    s_is_line[s_is_line_len] = '\0';
                    if (s_is_line[0] != '#') { // '#' = server chatter
                        parseTnc2(s_is_line, false);
                    }
                    s_is_line_len = 0;
                }
            } else if (s_is_line_len < sizeof(s_is_line) - 1) {
                s_is_line[s_is_line_len++] = c;
            } else {
                s_is_line_len = 0;
            }
        }
    }
}

// ----------------------------------------------------------------- beacons --

// "!DDMM.mmN/DDDMM.mmE&/A=001234 comment"
void buildPositionInfo(char *out, size_t out_size, const char *comment)
{
    double lat = 0.0, lon = 0.0, alt = 0.0;
    ownPosition(&lat, &lon, &alt);

    const std::string lat_str = s_parser.deg2lat(lat);
    const std::string lon_str = s_parser.deg2lon(lon);
    const long alt_ft = lround(alt * 3.28084);

    if (comment != nullptr && comment[0] != '\0') {
        snprintf(out, out_size, "!%s%c%s%c/A=%06ld %s",
                 lat_str.c_str(), s_cfg.symbol_table, lon_str.c_str(),
                 s_cfg.symbol_code, alt_ft, comment);
    } else {
        snprintf(out, out_size, "!%s%c%s%c/A=%06ld",
                 lat_str.c_str(), s_cfg.symbol_table, lon_str.c_str(),
                 s_cfg.symbol_code, alt_ft);
    }
}

void sendBeacon()
{
    lockCfg();
    const bool net = s_cfg.net_enabled;
    const bool rf = s_cfg.rf_tx_enabled;
    char call[16];
    myCallsign(call, sizeof(call));
    char path[17];
    strncpy(path, s_cfg.path, sizeof(path));
    path[sizeof(path) - 1] = '\0';
    char rf_info[160];
    buildPositionInfo(rf_info, sizeof(rf_info), s_cfg.comment);

    const ExternalRadioConfig *radio = EXTERNAL_RADIO_GetConfig();
    const char *nrl_host = (radio != nullptr && radio->server_host[0] != '\0')
                               ? radio->server_host
                               : "unknown";
    const uint16_t nrl_port = (radio != nullptr) ? radio->server_port : 0u;
    char net_comment[112];
    snprintf(net_comment, sizeof(net_comment), "@udp://%s:%u,NRL-ESP32",
             nrl_host, (unsigned)nrl_port);
    char net_info[200];
    buildPositionInfo(net_info, sizeof(net_info), net_comment);
    unlockCfg();

    if (net && s_is_socket >= 0 && s_is_logged_in) {
        char line[300];
        snprintf(line, sizeof(line), "%s>NRLBOX,TCPIP*:%s", call, net_info);
        isSendLine(line);
        // Board type and firmware are static for the lifetime of a connection.
        // Send this APRS status packet once per login so map sites retain the
        // second information row without counting two packets every interval.
        if (!s_is_status_sent && s_is_socket >= 0 && s_is_logged_in) {
            snprintf(line, sizeof(line), "%s>NRLBOX,TCPIP*:>%s,v%s",
                     call, boardType(), NRL_FIRMWARE_VERSION);
            isSendLine(line);
            s_is_status_sent = s_is_socket >= 0 && s_is_logged_in;
        }
        s_tx_count++;
    }

    if (rf) {
        char frame_txt[300];
        if (path[0] != '\0') {
            snprintf(frame_txt, sizeof(frame_txt), "%s>NRLBOX,%s:%s", call, path, rf_info);
        } else {
            snprintf(frame_txt, sizeof(frame_txt), "%s>NRLBOX:%s", call, rf_info);
        }
        ax25frame frame;
        if (ax25_encode(frame, frame_txt, (int)strlen(frame_txt))) {
            static AX25Ctx ctx;
            static uint8_t raw[AX25_FRAME_MAX_SIZE];
            const int size = hdlcFrame(raw, sizeof(raw), &ctx, &frame);
            if (size > 0 && Ax25WriteTxFrame(raw, (uint16_t)size) != nullptr) {
                Ax25TransmitBuffer();
                s_tx_count++;
            }
        }
    }
}

// ------------------------------------------------------------------ RF TX ---

void txPump()
{
    static int16_t chunk[kTxChunkSamples];
    static int64_t tx_start_us = 0;
    static uint32_t tx_pushed_samples = 0;

    if (!ModemIsTransmitting()) {
        s_tx_pumping = false;
        return;
    }
    if (!s_tx_pumping) {
        s_tx_pumping = true;
        tx_start_us = esp_timer_get_time();
        tx_pushed_samples = 0;
        AudioFocus_NotifyVoiceStart(); // pause music playback for the beacon
    }
    // Keep the pushed sample count ~200 ms ahead of wall clock. The speaker
    // queue (~1.3 s) absorbs the lead; generating strictly against real time
    // avoids both overflowing the queue (dropped samples tear the AFSK frame)
    // and underruns when the task tick jitters.
    const int64_t elapsed_us = esp_timer_get_time() - tx_start_us;
    const uint32_t target = (uint32_t)(elapsed_us * MODEM_TX_SAMPLE_RATE / 1000000) +
                            MODEM_TX_SAMPLE_RATE / 5;
    while (tx_pushed_samples < target && ModemIsTransmitting()) {
        for (size_t i = 0; i < kTxChunkSamples; ++i) {
            const uint8_t s = MODEM_BAUDRATE_TIMER_HANDLER();
            chunk[i] = (int16_t)(((int)s - 128) * kTxAmplitude);
            if (!ModemIsTransmitting()) {
                // frame tail reached mid-chunk: pad with silence
                for (size_t k = i + 1; k < kTxChunkSamples; ++k) {
                    chunk[k] = 0;
                }
                break;
            }
        }
        AudioRouter_PushFrame(AUDIO_SRC_APRS, MODEM_TX_SAMPLE_RATE, chunk,
                              kTxChunkSamples);
        tx_pushed_samples += kTxChunkSamples;
    }
}

// ------------------------------------------------------------------ RF RX ---

// Audio-router sink callback (runs on the audio task): stash the mic tap into
// the PSRAM ring; the APRS task resamples and demodulates it.
void aprsSinkWrite(uint8_t /*source_id*/, const int16_t *samples,
                   size_t sample_count, void *)
{
    if (s_rx_ring == nullptr) {
        return;
    }
    size_t head = s_rx_head;
    for (size_t i = 0; i < sample_count; ++i) {
        const size_t next = (head + 1) % kRxRingSamples;
        if (next == s_rx_tail) {
            break; // ring full; drop the rest
        }
        s_rx_ring[head] = samples[i];
        head = next;
    }
    s_rx_head = head;
}

void rxDemodPoll()
{
    if (s_rx_ring == nullptr) {
        return;
    }
    // Resample 16 kHz -> 9600 Hz (linear interpolation, 5:3) and feed the
    // demodulator sample-by-sample.
    constexpr uint32_t kStep = (uint32_t)(65536.0 * 16000.0 / MODEM_RX_SAMPLE_RATE);
    while (s_rx_tail != s_rx_head) {
        const int16_t sample = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1) % kRxRingSamples;

        s_rx_level_acc += (uint32_t)abs(sample);
        if (++s_rx_level_n >= 1024) {
            // rough mean-abs -> mV-ish figure for the modem signal report
            s_rx_level_mv = (uint16_t)((s_rx_level_acc / 1024) >> 4);
            s_rx_level_acc = 0;
            s_rx_level_n = 0;
        }

        s_resample_phase += 65536u;
        while (s_resample_phase >= kStep) {
            s_resample_phase -= kStep;
            // interpolate between the previous and current input sample --
            // linear is plenty for AFSK tones at this oversampling. frac in
            // [0..256]: 0 = at the current sample, 256 = at the previous one.
            const int32_t frac =
                (int32_t)(((uint64_t)s_resample_phase << 8) / kStep);
            const int32_t out =
                (int32_t)sample +
                ((((int32_t)s_resample_last - (int32_t)sample) * frac) >> 8);
            MODEM_DECODE((int16_t)out, s_rx_level_mv);
        }
        s_resample_last = sample;
    }
}

void rxFramePoll()
{
    while (Ax25NewRxFrames()) {
        uint8_t *frame = nullptr;
        uint16_t size = 0;
        int8_t peak = 0, valley = 0;
        uint8_t level = 0, corrected = 0;
        uint16_t mv = 0;
        if (!Ax25ReadNextRxFrame(&frame, &size, &peak, &valley, &level, &corrected, &mv)) {
            break;
        }

        AX25Msg msg = {};
        ax25_decode(frame, size, mv, &msg);
        if (msg.ctrl != AX25_CTRL_UI || msg.pid != AX25_PID_NOLAYER3) {
            continue;
        }

        // rebuild the TNC2 monitor line
        char line[400];
        int off = snprintf(line, sizeof(line), "%s", msg.src.call);
        if (msg.src.ssid > 0) {
            off += snprintf(line + off, sizeof(line) - off, "-%u", msg.src.ssid);
        }
        off += snprintf(line + off, sizeof(line) - off, ">%s", msg.dst.call);
        if (msg.dst.ssid > 0) {
            off += snprintf(line + off, sizeof(line) - off, "-%u", msg.dst.ssid);
        }
        for (uint8_t i = 0; i < msg.rpt_count && off < (int)sizeof(line) - 16; ++i) {
            off += snprintf(line + off, sizeof(line) - off, ",%s", msg.rpt_list[i].call);
            if (msg.rpt_list[i].ssid > 0) {
                off += snprintf(line + off, sizeof(line) - off, "-%u", msg.rpt_list[i].ssid);
            }
            if (msg.rpt_flags & (1u << i)) {
                off += snprintf(line + off, sizeof(line) - off, "*");
            }
        }
        const size_t info_len =
            (msg.len < sizeof(line) - (size_t)off - 2) ? msg.len : sizeof(line) - off - 2;
        off += snprintf(line + off, sizeof(line) - off, ":");
        memcpy(line + off, msg.info, info_len);
        line[off + (int)info_len] = '\0';

        s_rx_count++;
        ESP_LOGI(TAG, "RF RX: %s", line);
        parseTnc2(line, true);

        // iGate: forward RF-heard traffic to APRS-IS with the qAR construct
        lockCfg();
        const bool net = s_cfg.net_enabled;
        char call[16];
        myCallsign(call, sizeof(call));
        unlockCfg();
        if (net && s_is_socket >= 0 && s_is_logged_in) {
            char fwd[440];
            const char *colon = strchr(line, ':');
            if (colon != nullptr) {
                const size_t head_len = (size_t)(colon - line);
                snprintf(fwd, sizeof(fwd), "%.*s,qAR,%s%s",
                         (int)head_len, line, call, colon);
                isSendLine(fwd);
            }
        }
    }
}

// -------------------------------------------------------------------- task --

void modemPtt(bool on)
{
    // VOX keying only: the TXDelay preamble gives the radio's VOX time to
    // open. Kept as a hook for boards that grow a hard PTT line.
    ESP_LOGI(TAG, "PTT %s", on ? "on" : "off");
}

void applyRouteState()
{
    lockCfg();
    const bool rx = s_cfg.enabled && s_cfg.rf_rx_enabled;
    const bool tx = s_cfg.enabled && s_cfg.rf_tx_enabled;
    unlockCfg();
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_APRS, rx);
    AudioRouter_SetRoute(AUDIO_SRC_APRS, AUDIO_SINK_SPEAKER, tx);
}

void aprsTask(void *)
{
    for (;;) {
        lockCfg();
        const bool enabled = s_cfg.enabled;
        const bool net = s_cfg.net_enabled;
        const uint16_t interval_s = s_cfg.beacon_interval_s;
        const bool auto_interval = s_cfg.auto_interval;
        unlockCfg();

        if (!enabled) {
            if (s_is_socket >= 0) {
                isDisconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        drainNmeaRing();

        if (s_is_reconnect_requested) {
            s_is_reconnect_requested = false;
            isDisconnect();
            s_is_last_attempt_ms = 0; // retry immediately with the new server
        }

        if (net) {
            if (s_is_socket < 0 && wifiIsConnected()) {
                isTryConnect();
            }
            isPoll();
        } else if (s_is_socket >= 0) {
            isDisconnect();
        }

        const uint32_t now = nowMs();
        uint32_t effective_ms = (uint32_t)interval_s * 1000u;
        bool moved_far = false;
        if (auto_interval) {
            double lat = 0.0, lon = 0.0;
            if (ownPosition(&lat, &lon, nullptr)) { // true only on a fresh fix
                const double speed = (double)s_gps_speed_kmh;
                if (speed >= kAutoSlowSpeedKmh) {
                    double scaled_s = (double)kAutoMinIntervalS * kAutoFastSpeedKmh / speed;
                    if (scaled_s < (double)kAutoMinIntervalS) {
                        scaled_s = (double)kAutoMinIntervalS;
                    }
                    const uint32_t scaled_ms = (uint32_t)(scaled_s * 1000.0);
                    if (scaled_ms < effective_ms) {
                        effective_ms = scaled_ms;
                    }
                }
                if (s_last_beacon_pos_valid &&
                    s_parser.distance(s_last_beacon_lon, s_last_beacon_lat, lon, lat) >=
                        kAutoMoveKm) {
                    moved_far = true;
                }
            }
        }
        const uint32_t elapsed_ms = now - s_last_beacon_ms;
        const bool due = s_last_beacon_ms == 0 || elapsed_ms >= effective_ms;
        // A large position jump beacons early, but never more often than the
        // auto floor -- a fast mover is already pacing at that rate.
        const bool early = moved_far && elapsed_ms >= (uint32_t)kAutoMinIntervalS * 1000u;
        if (due || early) {
            sendBeacon();
            s_last_beacon_ms = (now != 0u) ? now : 1u;
            s_last_beacon_pos_valid =
                ownPosition(&s_last_beacon_lat, &s_last_beacon_lon, nullptr);
        }

        Ax25TransmitCheck();
        txPump();
        rxDemodPoll();
        rxFramePoll();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

} // namespace

// ------------------------------------------------------------------ public --

extern "C" void APRS_SERVICE_Init(void)
{
    if (s_task != nullptr) {
        return;
    }
    s_cfg_mutex = xSemaphoreCreateMutex();
    s_station_mutex = xSemaphoreCreateMutex();
    loadConfig();

    s_stations = static_cast<StationRec *>(
        heap_caps_calloc(kStationCount, sizeof(StationRec), MALLOC_CAP_SPIRAM));
    s_rx_ring = static_cast<int16_t *>(
        heap_caps_malloc(kRxRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (s_stations == nullptr || s_rx_ring == nullptr) {
        ESP_LOGE(TAG, "PSRAM allocation failed, APRS disabled");
        return;
    }

    ModemConfig.modem = MODEM_1200;
    ModemConfig.flatAudioIn = 0;
    ModemInit();
    Ax25Init(0);
    ModemSetPttCallback(modemPtt);

    // Mic tap for the demodulator. Registered at 16 kHz: raw mic frames pass
    // through unconverted and 8 kHz AEC frames get upsampled at the edge.
    AudioRouter_RegisterSink(AUDIO_SINK_APRS, 16000u, aprsSinkWrite, nullptr);
    applyRouteState();

    if (xTaskCreatePinnedToCoreWithCaps(aprsTask, "aprs", 8192, nullptr, 5,
                                        &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        // PSRAM stack unavailable -- fall back to an internal-RAM stack
        if (xTaskCreate(aprsTask, "aprs", 6144, nullptr, 5, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "task create failed");
            s_task = nullptr;
        }
    }
    ESP_LOGI(TAG, "init: enabled=%d net=%d rf_tx=%d rf_rx=%d server=%s:%u",
             (int)s_cfg.enabled, (int)s_cfg.net_enabled, (int)s_cfg.rf_tx_enabled,
             (int)s_cfg.rf_rx_enabled, s_cfg.server_host, (unsigned)s_cfg.server_port);
}

extern "C" bool APRS_SERVICE_SetEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.enabled = enabled;
    persistConfig();
    unlockCfg();
    applyRouteState();
    s_last_beacon_ms = 0; // beacon immediately on (re)enable
    return true;
}

extern "C" bool APRS_SERVICE_IsEnabled(void)
{
    return s_cfg.enabled;
}

extern "C" bool APRS_SERVICE_SetNetEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.net_enabled = enabled;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetRfTxEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.rf_tx_enabled = enabled;
    persistConfig();
    unlockCfg();
    applyRouteState();
    return true;
}

extern "C" bool APRS_SERVICE_SetRfRxEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.rf_rx_enabled = enabled;
    persistConfig();
    unlockCfg();
    applyRouteState();
    return true;
}

extern "C" bool APRS_SERVICE_SetServer(const char *host, uint16_t port)
{
    if (s_cfg_mutex == nullptr || host == nullptr || host[0] == '\0' || port == 0) {
        return false;
    }
    lockCfg();
    strncpy(s_cfg.server_host, host, sizeof(s_cfg.server_host) - 1);
    s_cfg.server_host[sizeof(s_cfg.server_host) - 1] = '\0';
    s_cfg.server_port = port;
    persistConfig();
    unlockCfg();
    s_is_reconnect_requested = true; // the APRS task owns the socket
    return true;
}

extern "C" bool APRS_SERVICE_SetSsid(uint8_t ssid)
{
    if (s_cfg_mutex == nullptr || ssid > 15) {
        return false;
    }
    lockCfg();
    s_cfg.ssid = ssid;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetSymbol(char table, char code)
{
    if (s_cfg_mutex == nullptr || table < '!' || table > '~' || code < '!' || code > '~') {
        return false;
    }
    lockCfg();
    s_cfg.symbol_table = table;
    s_cfg.symbol_code = code;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetBeaconInterval(uint16_t seconds)
{
    if (s_cfg_mutex == nullptr || seconds < kMinBeaconIntervalS ||
        seconds > kMaxBeaconIntervalS) {
        return false;
    }
    lockCfg();
    s_cfg.beacon_interval_s = seconds;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetAutoInterval(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.auto_interval = enabled;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_ParseAprsCoord(const char *text, bool is_lat, double *deg_out)
{
    if (text == nullptr || deg_out == nullptr) {
        return false;
    }
    while (*text == ' ') {
        ++text;
    }
    char buf[24] = {};
    strncpy(buf, text, sizeof(buf) - 1);
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] == ' ') {
        buf[--len] = '\0';
    }
    if (len == 0) {
        return false;
    }

    double sign = 1.0;
    bool has_hemi = false;
    const char hemi = (char)toupper((unsigned char)buf[len - 1]);
    if (hemi == 'N' || hemi == 'S' || hemi == 'E' || hemi == 'W') {
        if (is_lat && hemi != 'N' && hemi != 'S') {
            return false;
        }
        if (!is_lat && hemi != 'E' && hemi != 'W') {
            return false;
        }
        sign = (hemi == 'S' || hemi == 'W') ? -1.0 : 1.0;
        buf[--len] = '\0';
        has_hemi = true;
    }
    if (len == 0) {
        return false;
    }

    char *end = nullptr;
    double v = strtod(buf, &end);
    if (end == buf || (end != nullptr && *end != '\0') || !isfinite(v)) {
        return false;
    }
    if (v < 0.0) {
        if (has_hemi) {
            return false; // "-3113.82N" is ambiguous
        }
        sign = -1.0;
        v = -v;
    }

    const double d = floor(v / 100.0);
    const double m = v - d * 100.0;
    if (m >= 60.0) {
        return false;
    }
    const double deg = d + m / 60.0;
    if ((is_lat && deg > 90.0) || (!is_lat && deg > 180.0)) {
        return false;
    }
    *deg_out = sign * deg;
    return true;
}

extern "C" void APRS_SERVICE_FormatAprsCoord(double deg, bool is_lat, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return;
    }
    const char hemi = (deg >= 0.0) ? (is_lat ? 'N' : 'E') : (is_lat ? 'S' : 'W');
    deg = fabs(deg);
    int d = (int)deg;
    double m = (deg - (double)d) * 60.0;
    if (m >= 59.99995) { // avoid "60.0000" after rounding
        m = 0.0;
        ++d;
    }
    snprintf(out, out_size, is_lat ? "%02d%07.4f%c" : "%03d%07.4f%c", d, m, hemi);
}

extern "C" bool APRS_SERVICE_SetDefaultPosition(double lat, double lon)
{
    if (s_cfg_mutex == nullptr || lat < -90.0 || lat > 90.0 || lon < -180.0 ||
        lon > 180.0) {
        return false;
    }
    lockCfg();
    s_cfg.default_lat_e6 = (int32_t)lround(lat * 1e6);
    s_cfg.default_lon_e6 = (int32_t)lround(lon * 1e6);
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetPath(const char *path)
{
    if (s_cfg_mutex == nullptr || path == nullptr) {
        return false;
    }
    lockCfg();
    strncpy(s_cfg.path, path, sizeof(s_cfg.path) - 1);
    s_cfg.path[sizeof(s_cfg.path) - 1] = '\0';
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetComment(const char *comment)
{
    if (s_cfg_mutex == nullptr || comment == nullptr) {
        return false;
    }
    lockCfg();
    strncpy(s_cfg.comment, comment, sizeof(s_cfg.comment) - 1);
    s_cfg.comment[sizeof(s_cfg.comment) - 1] = '\0';
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" void APRS_SERVICE_GetConfig(AprsConfig *out)
{
    if (out == nullptr) {
        return;
    }
    if (s_cfg_mutex == nullptr) {
        defaultConfig(*out);
        return;
    }
    lockCfg();
    *out = s_cfg;
    unlockCfg();
}

extern "C" bool APRS_SERVICE_SendBeaconNow(void)
{
    if (s_task == nullptr || !s_cfg.enabled) {
        return false;
    }
    s_last_beacon_ms = 0;
    return true;
}

extern "C" void APRS_SERVICE_FeedSciBytes(const uint8_t *data, size_t size)
{
    if (data == nullptr || !s_cfg.enabled) {
        return;
    }
    size_t head = s_nmea_head;
    for (size_t i = 0; i < size; ++i) {
        const size_t next = (head + 1) % kNmeaRingSize;
        if (next == s_nmea_tail) {
            break;
        }
        s_nmea_ring[head] = data[i];
        head = next;
    }
    s_nmea_head = head;
}

extern "C" bool APRS_SERVICE_GetOwnPosition(double *lat, double *lon, double *alt_m)
{
    double la = 0.0, lo = 0.0, al = 0.0;
    const bool fix = ownPosition(&la, &lo, &al);
    if (lat != nullptr) *lat = la;
    if (lon != nullptr) *lon = lo;
    if (alt_m != nullptr) *alt_m = al;
    return fix;
}

extern "C" bool APRS_SERVICE_IsNetConnected(void)
{
    return s_is_socket >= 0 && s_is_logged_in;
}

extern "C" uint32_t APRS_SERVICE_GetRxCount(void) { return s_rx_count; }
extern "C" uint32_t APRS_SERVICE_GetTxCount(void) { return s_tx_count; }

extern "C" bool APRS_SERVICE_GpsHasFix(void)
{
    return s_gps_fix && (nowMs() - s_gps_last_fix_ms) < kGpsFixFreshMs;
}

extern "C" size_t APRS_SERVICE_GetStations(AprsStationInfo *out, size_t max_count)
{
    if (out == nullptr || max_count == 0 || s_stations == nullptr) {
        return 0;
    }
    const uint32_t now = nowMs();
    lockStations();
    // collect used slots, most recent first (selection by repeated max-scan;
    // list is small)
    size_t count = 0;
    bool taken[kStationCount] = {false};
    while (count < max_count) {
        int best = -1;
        for (size_t i = 0; i < kStationCount; ++i) {
            if (!s_stations[i].used || taken[i]) {
                continue;
            }
            if (best < 0 ||
                (int32_t)(s_stations[i].last_heard_ms - s_stations[best].last_heard_ms) > 0) {
                best = (int)i;
            }
        }
        if (best < 0) {
            break;
        }
        taken[best] = true;
        const StationRec &rec = s_stations[best];
        AprsStationInfo &info = out[count++];
        memcpy(info.name, rec.name, sizeof(info.name));
        memcpy(info.symbol, rec.symbol, sizeof(info.symbol));
        info.lat = rec.lat;
        info.lon = rec.lon;
        info.altitude_m = rec.altitude_m;
        info.speed_kmh = rec.speed_kmh;
        info.course_deg = rec.course_deg;
        info.derived_speed_kmh = rec.derived_speed_kmh;
        info.distance_km = rec.distance_km;
        info.bearing_deg = rec.bearing_deg;
        info.age_s = (now - rec.last_heard_ms) / 1000u;
        info.pkt_count = rec.pkt_count;
        info.via_rf = rec.via_rf;
        memcpy(info.comment, rec.comment, sizeof(info.comment));
    }
    unlockStations();
    return count;
}

extern "C" uint32_t APRS_SERVICE_GetStationRevision(void)
{
    return s_station_revision;
}

extern "C" uint32_t APRS_SERVICE_GetLastPacket(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    lockStations();
    strncpy(out, s_last_packet, out_size - 1);
    out[out_size - 1] = '\0';
    const uint32_t seq = s_last_packet_seq;
    unlockStations();
    return seq;
}
