#include "nrl_bt_hfp.h"

#include "sdkconfig.h"

#if defined(CONFIG_BT_HFP_AG_ENABLE)

#include "nrl_audio_bridge.h"
#include "../app/driver/external_radio.h"
#include "../app/driver/status_io.h"

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_hf_ag_api.h>
#include <esp_hf_defs.h>
#include <esp_coexist.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

#include <string.h>

namespace {

const char *TAG = "BTHFP";

constexpr uint32_t kAudioRetryIntervalMs = 2000;
// How often to retry reconnecting to the chosen headset after a drop.
constexpr uint32_t kReconnectIntervalMs = 4000;
// Abandon a stuck in-flight SLC attempt after this long (then a retry is allowed).
constexpr uint32_t kConnectTimeoutMs = 12000;
// Inquiry length in 1.28 s units (~10 s) and unlimited responses.
constexpr uint8_t kInquiryLen = 8;

// Discovered-device table for the on-screen pick list.
constexpr size_t kMaxDevices = 12;
// HFP speaker gain is a 0..15 scale. Each headset remembers its own last gain
// (spk_volume), persisted with the saved list, so reconnecting restores it.
constexpr uint8_t kSpkVolumeMax = 15;
constexpr uint8_t kDefaultSpkVolume = 9;  // ~60 %, adjustable both ways
struct BtDevice {
    esp_bd_addr_t bda;
    char name[32];
    uint8_t spk_volume;  // saved HFP speaker gain (0..15) for this headset
};
BtDevice s_devices[kMaxDevices];
size_t s_device_count = 0;

// Saved (previously paired) headsets, persisted to NVS so they auto-reconnect
// after a reboot and can be deleted by the user. Most-recent first.
constexpr size_t kMaxSaved = 4;
constexpr const char *kNvsNamespace = "nrlbt";
constexpr const char *kNvsKeySaved = "saved";
BtDevice s_saved[kMaxSaved];
size_t s_saved_count = 0;

// Defined below stackUp(); used from the GAP auth-complete callback above it.
void addSaved(const esp_bd_addr_t bda, const char *name);
// Per-headset volume helpers, also used from the HFP callback above their defs.
int findSavedIndex(const esp_bd_addr_t bda);
void savePeerVolume();
void applyPeerVolume();

bool s_enabled = false;        // actual on/off of the stack (applied by the task)
bool s_stack_up = false;       // Bluedroid + controller actually running

// Bringing the stack up/down is slow (controller + Bluedroid init, and a teardown
// that waits up to ~1 s for a clean headset disconnect). Doing that inside the UI
// touch callback froze the screen, so the switch never repainted -- it felt
// unresponsive and users tapped repeatedly. Instead SetEnabled just records the
// desired state here; the next NRL_BtHfp_Poll() (main loop) applies it, so the
// touch callback returns immediately and LVGL can repaint. Rapid toggles coalesce
// to the latest request; the one-time stack transition still runs on the main
// loop but only once per toggle (not every frame).
volatile bool s_request_pending = false;   // a SetEnabled request is queued
volatile bool s_request_enabled = false;   // its desired state
volatile bool s_transitioning = false;     // Poll is mid stack up/down
bool s_ble_mem_released = false;
bool s_connected = false;      // service-level connection to a headset
bool s_audio_active = false;   // SCO/voice link up
esp_bd_addr_t s_peer_bda = {};
char s_peer_name[32] = {};

// Connected headset's speaker gain (0..15). Loaded from the saved entry on
// connect, pushed to the headset, and updated live when the headset's own volume
// buttons report a change (+VGS / ESP_HF_VOLUME_CONTROL_EVT). s_peer_volume_dirty
// defers the NVS write to Poll (out of the BT callback context). During the
// restore window after connect, incoming +VGS reports are ignored so the
// headset's auto-reported volume can't clobber the value we are restoring.
uint8_t s_peer_volume = kDefaultSpkVolume;
volatile bool s_peer_volume_dirty = false;
uint32_t s_volume_restore_until_ms = 0;

// Downlink jitter buffer: a plain lock-free single-producer/single-consumer ring
// of int16 PCM samples. (A FreeRTOS byte-ring mangled the audio into static on
// this path.) PushPlayback (network task) writes; the SCO outgoing callback
// reads one frame per request, padding silence on underrun.
constexpr size_t kPcmCap = 2048;       // 256 ms @ 8 kHz
int16_t s_pcm[kPcmCap];
volatile size_t s_pcm_head = 0;        // write index (PushPlayback)
volatile size_t s_pcm_tail = 0;        // read index (SCO callback)
// SCO TX (HFP-AG-over-HCI) only sends when the app calls
// esp_hf_ag_outgoing_data_ready(). We drive that from the SCO RX callback
// (scoIncomingCb), which the controller calls once per eSCO interval, so our TX
// rate stays locked to the link rate. CVSD = 7.5 ms / 120-byte (60-sample) frame.
// Downlink jitter buffer: wait until this many samples are queued before playing
// (~100 ms @ 8 kHz), so bursty/jittery network packets don't underrun the SCO
// stream. Trades latency for not having to insert silence.
bool s_pb_priming = true;
constexpr size_t kPrimeSamples = 800u;
// Uplink gain: the headset mic (CVSD) comes in quiet; multiply before sending.
constexpr int32_t kUplinkGain = 4;

// Headset PTT button (HFP voice-recognition button) -> applied from Poll so the
// STATUS_IO update never runs inside the BT host callback context.
volatile bool s_headset_ptt = false;
volatile bool s_headset_ptt_dirty = false;

bool s_discovering = false;
bool s_have_peer = false;        // user has selected a headset to (re)connect to
bool s_connecting = false;       // an SLC attempt is in flight (don't start another)
uint32_t s_connect_started_ms = 0;
uint32_t s_last_audio_retry_ms = 0;
uint32_t s_last_reconnect_ms = 0;

// On-demand SCO: the voice (SCO) link is only opened while there is actual voice
// to carry -- network downlink arriving or the local PTT held. When neither has
// happened for kVoiceHangoverMs, the SCO is dropped so the ACL can enter sniff
// (power save) while the SLC connection stays up. s_last_voice_ms is refreshed by
// PushPlayback (downlink) and by PTT in Poll.
uint32_t s_last_voice_ms = 0;
bool s_call_active = false;       // our "voice burst in progress" notion
// Keep the SCO up for 10 s after the last voice so a whole conversation (gaps
// under this) never re-incurs the ~0.3-0.5 s SCO setup / first-syllable loss;
// only a >10 s silence drops it back to the SLC-only (sniff) power-saving state.
constexpr uint32_t kVoiceHangoverMs = 10000;

inline uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

// Bias the shared radio toward Bluetooth while a connection/SLC handshake is in
// progress (heavy Wi-Fi voice traffic otherwise starves the RFCOMM AT exchange
// and the SLC times out). Back to balanced once the link is up.
void coexPreferBt(bool prefer)
{
    esp_coex_preference_set(prefer ? ESP_COEX_PREFER_BT : ESP_COEX_PREFER_BALANCE);
}

// ---- SCO data path (PCM over HCI) ------------------------------------------

// Headset microphone -> network uplink. PCM16 mono 8 kHz. sendVoiceFrame (via
// the bridge) is already PTT-gated and handles G.711 packetisation + UDP send.
void scoIncomingCb(const uint8_t *buf, uint32_t len)
{
    if (buf == nullptr || len < 2u) {
        return;
    }
    const int16_t *mic = reinterpret_cast<const int16_t *>(buf);
    size_t mic_n = len / 2u;
    int16_t gained[256];
    if (mic_n > 256u) {
        mic_n = 256u;  // SCO frames are ~60 samples; cap for safety
    }
    for (size_t i = 0; i < mic_n; ++i) {
        // Boost the quiet headset mic before uplink (with clipping).
        int32_t v = static_cast<int32_t>(mic[i]) * kUplinkGain;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        gained[i] = static_cast<int16_t>(v);
    }
    NRLAudioBridge_FeedExternalMic(gained, mic_n);

    // Clock SCO TX off SCO RX: the controller delivers one RX frame per eSCO
    // interval, so requesting one outgoing frame here keeps our TX rate locked to
    // the link rate -- no free-running-timer drift. Rate-limit to ~one frame per
    // eSCO interval so a burst of RX frames (e.g. coming out of sniff) can't queue
    // several TX frames at once and overflow the controller's SCO xmit queue.
    if (s_audio_active) {
        static int64_t last_tx_signal_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_tx_signal_us >= 6500) {  // eSCO interval is ~7.5 ms
            last_tx_signal_us = now_us;
            esp_hf_ag_outgoing_data_ready();
        }
    }
}

// Network downlink -> headset speaker. Fill `len` bytes from the playback ring,
// pad the remainder with silence, and always return `len` so the SCO frame is
// well-formed.
uint32_t scoOutgoingCb(uint8_t *buf, uint32_t len)
{
    if (buf == nullptr || len == 0u) {
        return 0u;
    }
    int16_t *out = reinterpret_cast<int16_t *>(buf);
    const size_t want = len / 2u;
    const size_t head = s_pcm_head;          // snapshot (producer side)
    size_t tail = s_pcm_tail;
    const size_t avail = (head + kPcmCap - tail) % kPcmCap;
    // Jitter buffer: wait until enough is queued before starting playback.
    if (s_pb_priming) {
        if (avail < kPrimeSamples) {
            memset(buf, 0, len);
            return len;
        }
        s_pb_priming = false;
    }
    size_t got = 0u;
    while (got < want && tail != head) {
        out[got++] = s_pcm[tail];
        tail = (tail + 1u) % kPcmCap;
    }
    s_pcm_tail = tail;
    if (got < want) {
        // Underrun: hold the last sample (simple packet-loss concealment) rather
        // than inserting a silence gap, and don't re-prime. Silence gaps on every
        // underrun were the "spring" warble; holding is far less audible.
        const int16_t last = (got > 0) ? out[got - 1] : 0;
        for (; got < want; ++got) {
            out[got] = last;
        }
    }
    return len;
}

// ---- HFP AG + GAP callbacks -------------------------------------------------

void hfAgCb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param)
{
    if (param == nullptr) {
        return;
    }
    switch (event) {
    case ESP_HF_PROF_STATE_EVT:
        // AG profile init finished -> register the SCO (voice) data callbacks.
        esp_hf_ag_register_data_callback(scoIncomingCb, scoOutgoingCb);
        ESP_LOGI(TAG, "HFP AG profile ready");
        break;
    case ESP_HF_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
            memcpy(s_peer_bda, param->conn_stat.remote_bda, sizeof(s_peer_bda));
            s_connected = true;
            s_connecting = false;
            // Restore this headset's saved speaker volume and push it now. Ignore
            // any +VGS the headset auto-reports for the next couple of seconds so
            // it can't overwrite the value we are restoring.
            {
                const int idx = findSavedIndex(s_peer_bda);
                s_peer_volume = (idx >= 0) ? s_saved[idx].spk_volume : kDefaultSpkVolume;
                if (s_peer_volume > kSpkVolumeMax) {
                    s_peer_volume = kSpkVolumeMax;
                }
            }
            s_volume_restore_until_ms = nowMs() + 2000u;
            applyPeerVolume();
            // Linked but idle: let the radio share fairly and let the link sniff
            // for power save. The SCO voice link is opened on demand (see Poll),
            // and only then do we bias the radio toward BT.
            coexPreferBt(false);
            s_call_active = false;
            ESP_LOGI(TAG, "headset SLC connected (idle; SCO opens on demand)");
        } else if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "headset disconnected");
            s_connected = false;
            s_connecting = false;
            s_audio_active = false;
            s_call_active = false;
        }
        // CONNECTED (RFCOMM up, SLC handshake in progress) keeps s_connecting set
        // so Poll won't fire a second slc_connect and collide.
        break;
    case ESP_HF_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ||
            param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
            s_audio_active = true;
            // Drop whatever buffered downlink piled up during SCO setup so
            // playback starts fresh (low latency) instead of carrying the setup
            // backlog as permanent delay. The jitter buffer re-primes from here.
            s_pcm_tail = s_pcm_head;
            s_pb_priming = true;
            // Re-assert this headset's saved speaker volume -- a headset can reset
            // its gain (sometimes to 0) when the "call" SCO opens, which would make
            // downlink network voice inaudible.
            esp_hf_ag_volume_control(param->audio_stat.remote_addr,
                                     ESP_HF_VOLUME_CONTROL_TARGET_SPK, s_peer_volume);
            ESP_LOGI(TAG, "SCO audio up (%s) -- voice routes through the headset",
                     param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC ? "mSBC" : "CVSD");
        } else if (param->audio_stat.state == ESP_HF_AUDIO_STATE_DISCONNECTED) {
            s_audio_active = false;
            ESP_LOGI(TAG, "SCO audio down");
        }
        break;
    case ESP_HF_BVRA_RESPONSE_EVT:
        // Headset multifunction / voice-dial button. Map its enabled/disabled
        // toggle straight onto soft-PTT (applied from Poll).
        s_headset_ptt = (param->vra_rep.value == ESP_HF_VR_STATE_ENABLED);
        s_headset_ptt_dirty = true;
        ESP_LOGI(TAG, "headset button -> PTT %s", s_headset_ptt ? "ON" : "OFF");
        break;
    case ESP_HF_VOLUME_CONTROL_EVT:
        // The headset's own volume buttons (+VGS speaker / +VGM mic). Track the
        // speaker gain so the on-screen readout follows it and it is saved for
        // this headset. Skip reports during the post-connect restore window so the
        // headset's auto-report can't clobber the value we just restored.
        if (param->volume_control.type == ESP_HF_VOLUME_TYPE_SPK &&
            nowMs() >= s_volume_restore_until_ms) {
            int v = param->volume_control.volume;
            if (v < 0) {
                v = 0;
            } else if (v > kSpkVolumeMax) {
                v = kSpkVolumeMax;
            }
            s_peer_volume = static_cast<uint8_t>(v);
            s_peer_volume_dirty = true;  // persist from Poll (not in this context)
            ESP_LOGI(TAG, "headset volume -> %u/15", (unsigned)s_peer_volume);
        }
        break;
    // The following replies are required to complete the HFP service-level
    // connection (SLC) handshake -- without them Bluedroid waits on the AT
    // exchange and the link times out after ~5 s. We're a voice bridge with no
    // cellular state, so report "idle, in service".
    case ESP_HF_CIND_RESPONSE_EVT:
        esp_hf_ag_cind_response(param->cind_rep.remote_addr,
                                static_cast<esp_hf_call_status_t>(1),         // call active (keep SCO)
                                static_cast<esp_hf_call_setup_status_t>(0),   // no call setup
                                static_cast<esp_hf_network_state_t>(1),       // service available
                                4,                                           // signal strength
                                static_cast<esp_hf_roaming_status_t>(0),      // not roaming
                                4,                                           // battery level
                                static_cast<esp_hf_call_held_status_t>(0));   // no held call
        break;
    case ESP_HF_COPS_RESPONSE_EVT:
        esp_hf_ag_cops_response(param->cops_rep.remote_addr, const_cast<char *>("NRL"));
        break;
    case ESP_HF_UNAT_RESPONSE_EVT:
        // Acknowledge any unsupported AT command so the handshake can proceed.
        ESP_LOGI(TAG, "headset AT cmd (unknown)");
        esp_hf_ag_unknown_at_send(param->unat_rep.remote_addr, nullptr);
        break;
    case ESP_HF_ATA_RESPONSE_EVT:
        ESP_LOGI(TAG, "headset button: ANSWER (ATA)");
        break;
    case ESP_HF_CHUP_RESPONSE_EVT:
        ESP_LOGI(TAG, "headset button: HANGUP (CHUP)");
        break;
    case ESP_HF_VTS_RESPONSE_EVT:
        ESP_LOGI(TAG, "headset button: DTMF key");
        break;
    default:
        // Surface any other headset action (button presses show up here) to help
        // map which HFP event a given headset sends.
        ESP_LOGI(TAG, "HFP evt %d", (int)event);
        break;
    }
}

void gapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (param == nullptr) {
        return;
    }
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        // Collect each discovered device into the pick list (the user chooses one
        // on screen). Parse Class-of-Device to keep audio devices (headsets /
        // speakers) and pull a display name from the BDNAME prop or EIR.
        uint32_t cod = 0;
        const char *name = nullptr;
        int name_len = 0;
        uint8_t *eir = nullptr;
        for (int i = 0; i < param->disc_res.num_prop; ++i) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            if (p->val == nullptr) {
                continue;
            }
            if (p->type == ESP_BT_GAP_DEV_PROP_COD) {
                cod = *reinterpret_cast<uint32_t *>(p->val);
            } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                name = reinterpret_cast<const char *>(p->val);
                name_len = p->len;
            } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR) {
                eir = reinterpret_cast<uint8_t *>(p->val);
            }
        }
        // Only list audio devices (headsets/speakers) to keep the list relevant.
        const bool is_audio = (esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV) ||
                              (esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_AUDIO);
        if (!is_audio) {
            break;
        }
        // De-dup by address.
        bool exists = false;
        for (size_t i = 0; i < s_device_count; ++i) {
            if (memcmp(s_devices[i].bda, param->disc_res.bda, sizeof(esp_bd_addr_t)) == 0) {
                exists = true;
                break;
            }
        }
        if (exists || s_device_count >= kMaxDevices) {
            break;
        }
        BtDevice &dev = s_devices[s_device_count];
        memcpy(dev.bda, param->disc_res.bda, sizeof(esp_bd_addr_t));
        // Resolve a name: BDNAME prop -> EIR complete/short name -> address.
        if ((name == nullptr || name_len == 0) && eir != nullptr) {
            uint8_t rlen = 0;
            uint8_t *rname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rlen);
            if (rname == nullptr || rlen == 0) {
                rname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rlen);
            }
            name = reinterpret_cast<const char *>(rname);
            name_len = rlen;
        }
        if (name != nullptr && name_len > 0) {
            const size_t n = (static_cast<size_t>(name_len) < sizeof(dev.name) - 1u)
                                 ? static_cast<size_t>(name_len) : sizeof(dev.name) - 1u;
            memcpy(dev.name, name, n);
            dev.name[n] = '\0';
        } else {
            const uint8_t *a = param->disc_res.bda;
            snprintf(dev.name, sizeof(dev.name), "%02X:%02X:%02X:%02X:%02X:%02X",
                     a[0], a[1], a[2], a[3], a[4], a[5]);
        }
        ++s_device_count;
        ESP_LOGI(TAG, "found device %u: %s", (unsigned)s_device_count, dev.name);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        s_discovering = (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        // Legacy pairing: most headsets use PIN "0000".
        ESP_LOGI(TAG, "PIN requested -> replying 0000");
        if (param->pin_req.min_16_digit) {
            esp_bt_pin_code_t pin16 = {'0', '0', '0', '0', '0', '0', '0', '0',
                                       '0', '0', '0', '0', '0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin16);
        } else {
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        }
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        // Simple-Secure-Pairing numeric confirmation -> auto-accept (no keypad).
        ESP_LOGI(TAG, "SSP confirm (val=%u) -> accept", (unsigned)param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey: %06u", (unsigned)param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "SSP passkey requested -> replying 0");
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, true, 0);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            strncpy(s_peer_name, reinterpret_cast<const char *>(param->auth_cmpl.device_name),
                    sizeof(s_peer_name) - 1u);
            s_peer_name[sizeof(s_peer_name) - 1u] = '\0';
            ESP_LOGI(TAG, "paired with '%s'", s_peer_name);
            // Remember it so it auto-reconnects after a reboot.
            addSaved(param->auth_cmpl.bda, s_peer_name);
        } else {
            ESP_LOGW(TAG, "pairing failed, status=%d", param->auth_cmpl.stat);
        }
        break;
    default:
        break;
    }
}

void loadSaved()
{
    s_saved_count = 0;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_saved);
    if (nvs_get_blob(h, kNvsKeySaved, s_saved, &len) == ESP_OK) {
        s_saved_count = len / sizeof(BtDevice);
        if (s_saved_count > kMaxSaved) {
            s_saved_count = kMaxSaved;
        }
        // Clamp volumes from older/garbled blobs into the valid 0..15 range.
        for (size_t i = 0; i < s_saved_count; ++i) {
            if (s_saved[i].spk_volume > kSpkVolumeMax) {
                s_saved[i].spk_volume = kDefaultSpkVolume;
            }
        }
    }
    nvs_close(h);
}

void persistSaved()
{
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, kNvsKeySaved, s_saved, s_saved_count * sizeof(BtDevice));
    nvs_commit(h);
    nvs_close(h);
}

int findSavedIndex(const esp_bd_addr_t bda)
{
    for (size_t i = 0; i < s_saved_count; ++i) {
        if (memcmp(s_saved[i].bda, bda, sizeof(esp_bd_addr_t)) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Persist the connected headset's current speaker volume back to its saved entry.
void savePeerVolume()
{
    const int idx = findSavedIndex(s_peer_bda);
    if (idx >= 0 && s_saved[idx].spk_volume != s_peer_volume) {
        s_saved[idx].spk_volume = s_peer_volume;
        persistSaved();
    }
}

// Push the connected headset's speaker gain to it over the (RFCOMM) SLC link.
void applyPeerVolume()
{
    if (s_connected) {
        esp_hf_ag_volume_control(s_peer_bda, ESP_HF_VOLUME_CONTROL_TARGET_SPK, s_peer_volume);
    }
}

// Insert/refresh a paired device at the front of the saved list (most recent).
void addSaved(const esp_bd_addr_t bda, const char *name)
{
    // Carry the saved per-headset volume across a re-pair (default for new ones).
    uint8_t saved_volume = kDefaultSpkVolume;
    // Drop any existing entry for this address.
    for (size_t i = 0; i < s_saved_count; ++i) {
        if (memcmp(s_saved[i].bda, bda, sizeof(esp_bd_addr_t)) == 0) {
            saved_volume = s_saved[i].spk_volume;
            for (size_t j = i; j + 1 < s_saved_count; ++j) {
                s_saved[j] = s_saved[j + 1];
            }
            --s_saved_count;
            break;
        }
    }
    // Shift down and insert at front (cap at kMaxSaved).
    if (s_saved_count >= kMaxSaved) {
        s_saved_count = kMaxSaved - 1;
    }
    for (size_t j = s_saved_count; j > 0; --j) {
        s_saved[j] = s_saved[j - 1];
    }
    memcpy(s_saved[0].bda, bda, sizeof(esp_bd_addr_t));
    s_saved[0].spk_volume = saved_volume;
    if (name != nullptr && name[0] != '\0') {
        snprintf(s_saved[0].name, sizeof(s_saved[0].name), "%s", name);
    } else {
        snprintf(s_saved[0].name, sizeof(s_saved[0].name),
                 "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    }
    ++s_saved_count;
    persistSaved();
}

bool stackUp()
{
    if (s_stack_up) {
        return true;
    }

    // BR/EDR only: hand the BLE controller memory back to the heap once.
    if (!s_ble_mem_released) {
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        s_ble_mem_released = true;
    }

    // The controller-config default macro only sets a subset of this large
    // struct (the rest is intentionally zero-initialised); silence the project's
    // -Werror=missing-field-initializers just for it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#pragma GCC diagnostic pop
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(err));
        esp_bt_controller_deinit();
        return false;
    }
    if ((err = esp_bluedroid_init()) != ESP_OK ||
        (err = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid up: %s", esp_err_to_name(err));
        return false;
    }

    // Device name: NRL-<callsign> so it's recognisable in the headset's pair list.
    char name[32];
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg != nullptr && cfg->callsign[0] != '\0') {
        snprintf(name, sizeof(name), "NRL-%s", cfg->callsign);
    } else {
        snprintf(name, sizeof(name), "NRL-AUDIO");
    }
    esp_bt_gap_set_device_name(name);
    esp_bt_gap_register_callback(gapCb);

    esp_hf_ag_register_callback(hfAgCb);
    esp_hf_ag_init();
    // The SCO data-path callbacks are registered once the AG profile reports it
    // is up (ESP_HF_PROF_STATE_EVT), matching the IDF hfp_ag example.

    // Legacy pairing: variable PIN; we answer "0000" on a PIN request. (Match
    // the IDF example -- do NOT force an IO capability via set_security_param,
    // which broke SSP/pairing with some headsets.)
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 4, pin);

    // Be visible + connectable so a headset can pair/reconnect.
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // Let WiFi yield the shared radio between beacons. The app otherwise runs
    // WIFI_PS_NONE (WiFi never sleeps) for low-latency voice, which starves the
    // BT controller's coex scheduler (OLC sched failed) and the BT ACL gets no
    // slots -> 16 s supervision timeout. MIN_MODEM is required for stable coex.
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

    // Restore saved headsets and auto-reconnect to the most recent one.
    loadSaved();
    if (s_saved_count > 0u) {
        memcpy(s_peer_bda, s_saved[0].bda, sizeof(s_peer_bda));
        s_have_peer = true;
        s_last_reconnect_ms = 0;  // let Poll attempt promptly
        ESP_LOGI(TAG, "auto-reconnecting saved headset '%s'", s_saved[0].name);
    }

    // Reset the downlink jitter buffer (SCO TX is clocked from the RX callback).
    s_pcm_head = 0;
    s_pcm_tail = 0;
    s_pb_priming = true;

    s_stack_up = true;
    ESP_LOGI(TAG, "BT HFP AG up as '%s' (discoverable); saved=%u; free heap=%u",
             name, (unsigned)s_saved_count, (unsigned)esp_get_free_heap_size());
    return true;
}

void stackDown()
{
    if (!s_stack_up) {
        return;
    }
    if (s_discovering) {
        esp_bt_gap_cancel_discovery();
        s_discovering = false;
    }
    // Stop accepting new links, then gracefully drop any active one and WAIT for
    // it to finish before deinit. Tearing the stack down with a connection (and
    // its HCI disconnect command) still in flight crashes Bluedroid's HCI layer
    // (use-after-free in transmit_command).
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    s_have_peer = false;  // don't let Poll re-arm a reconnect during teardown
    if (s_connected || s_connecting) {
        esp_hf_ag_slc_disconnect(s_peer_bda);
        for (int i = 0; i < 50 && (s_connected || s_connecting); ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));  // up to ~1 s for the disconnect event
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // let any final HCI traffic drain
    esp_hf_ag_deinit();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    coexPreferBt(false);
    esp_wifi_set_ps(WIFI_PS_NONE);  // restore the app's low-latency WiFi setting
    s_stack_up = false;
    s_connected = false;
    s_connecting = false;
    s_audio_active = false;
    s_have_peer = false;
    s_device_count = 0;
    s_peer_name[0] = '\0';
    ESP_LOGI(TAG, "BT HFP AG down; free heap=%u", (unsigned)esp_get_free_heap_size());
}

// Bring the stack to match `enabled`. Slow (see stackUp/stackDown); only ever
// called from the BT task so it never blocks the UI or main loop.
void applyEnabled(bool enabled)
{
    if (enabled == s_enabled) {
        return;
    }
    s_enabled = enabled;
    if (enabled) {
        stackUp();
    } else {
        stackDown();
    }
}

} // namespace

// ---- Public API -------------------------------------------------------------

void NRL_BtHfp_Init(void)
{
    // State is statically initialised; nothing to bring up until SetEnabled(true).
    // Enable/disable requests are applied from NRL_BtHfp_Poll() on the main loop.
}

void NRL_BtHfp_SetEnabled(bool enabled)
{
    // Non-blocking: just record the desired state. The BT task brings the stack
    // up/down (slow), so the caller -- a UI touch callback or boot -- never
    // blocks. Rapid toggles coalesce to the most recent request.
    s_request_enabled = enabled;
    s_request_pending = true;
}

bool NRL_BtHfp_TogglePending(void) { return s_request_pending || s_transitioning; }

bool NRL_BtHfp_IsEnabled(void) { return s_enabled; }

void NRL_BtHfp_StartScan(void)
{
    if (!s_stack_up || s_discovering) {
        return;
    }
    s_device_count = 0;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, kInquiryLen, 0);
}

bool NRL_BtHfp_IsScanning(void) { return s_discovering; }

size_t NRL_BtHfp_GetDeviceCount(void) { return s_device_count; }

bool NRL_BtHfp_GetDeviceName(size_t index, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0u || index >= s_device_count) {
        return false;
    }
    snprintf(out, out_size, "%s", s_devices[index].name);
    return true;
}

void NRL_BtHfp_ConnectIndex(size_t index)
{
    if (!s_stack_up || index >= s_device_count) {
        return;
    }
    if (s_discovering) {
        esp_bt_gap_cancel_discovery();
    }
    memcpy(s_peer_bda, s_devices[index].bda, sizeof(s_peer_bda));
    s_have_peer = true;
    s_connecting = true;
    s_connect_started_ms = nowMs();
    s_last_reconnect_ms = nowMs();
    coexPreferBt(true);
    ESP_LOGI(TAG, "connecting to '%s'", s_devices[index].name);
    esp_hf_ag_slc_connect(s_peer_bda);
}

size_t NRL_BtHfp_GetSavedCount(void) { return s_saved_count; }

bool NRL_BtHfp_GetSavedName(size_t index, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0u || index >= s_saved_count) {
        return false;
    }
    snprintf(out, out_size, "%s", s_saved[index].name);
    return true;
}

void NRL_BtHfp_ConnectSaved(size_t index)
{
    if (!s_stack_up || index >= s_saved_count) {
        return;
    }
    if (s_discovering) {
        esp_bt_gap_cancel_discovery();
    }
    memcpy(s_peer_bda, s_saved[index].bda, sizeof(s_peer_bda));
    s_have_peer = true;
    s_connecting = true;
    s_connect_started_ms = nowMs();
    s_last_reconnect_ms = nowMs();
    coexPreferBt(true);
    ESP_LOGI(TAG, "connecting saved '%s'", s_saved[index].name);
    esp_hf_ag_slc_connect(s_peer_bda);
}

void NRL_BtHfp_RemoveSaved(size_t index)
{
    if (index >= s_saved_count) {
        return;
    }
    esp_bd_addr_t bda;
    memcpy(bda, s_saved[index].bda, sizeof(bda));
    // If we're forgetting the active/target headset, drop the link + target.
    if (s_have_peer && memcmp(s_peer_bda, bda, sizeof(bda)) == 0) {
        s_have_peer = false;
        if (s_connected) {
            esp_hf_ag_audio_disconnect(bda);
        }
    }
    for (size_t j = index; j + 1 < s_saved_count; ++j) {
        s_saved[j] = s_saved[j + 1];
    }
    --s_saved_count;
    persistSaved();
    if (s_stack_up) {
        esp_bt_gap_remove_bond_device(bda);
    }
    ESP_LOGI(TAG, "forgot saved headset");
}

bool NRL_BtHfp_IsConnected(void) { return s_connected; }

bool NRL_BtHfp_IsAudioActive(void) { return s_stack_up && s_audio_active; }

size_t NRL_BtHfp_GetPeerName(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0u) {
        return 0u;
    }
    const size_t n = strnlen(s_peer_name, sizeof(s_peer_name));
    const size_t copy = (n < out_size - 1u) ? n : out_size - 1u;
    memcpy(out, s_peer_name, copy);
    out[copy] = '\0';
    return copy;
}

int NRL_BtHfp_GetVolumePercent(void)
{
    if (!s_connected) {
        return -1;
    }
    return (static_cast<int>(s_peer_volume) * 100 + kSpkVolumeMax / 2) / kSpkVolumeMax;
}

void NRL_BtHfp_AdjustVolume(int direction)
{
    if (!s_connected || direction == 0) {
        return;
    }
    int v = static_cast<int>(s_peer_volume) + (direction > 0 ? 1 : -1);
    if (v < 0) {
        v = 0;
    } else if (v > kSpkVolumeMax) {
        v = kSpkVolumeMax;
    }
    if (static_cast<uint8_t>(v) == s_peer_volume) {
        return;  // already at the limit
    }
    s_peer_volume = static_cast<uint8_t>(v);
    applyPeerVolume();
    savePeerVolume();
    ESP_LOGI(TAG, "NRL volume key -> headset %u/15", (unsigned)s_peer_volume);
}

void NRL_BtHfp_SetVolumePercent(int percent)
{
    if (!s_connected) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    const uint8_t gain = static_cast<uint8_t>((percent * kSpkVolumeMax + 50) / 100);
    if (gain == s_peer_volume) {
        return;
    }
    s_peer_volume = gain;
    applyPeerVolume();
    savePeerVolume();
    ESP_LOGI(TAG, "NRL slider -> headset %u/15", (unsigned)s_peer_volume);
}

void NRL_BtHfp_PushPlayback(const int16_t *samples, size_t sample_count)
{
    if (samples == nullptr || sample_count == 0u) {
        return;
    }
    // Downlink network voice is arriving -> a voice burst is live. Record this
    // even when the SCO is not up yet, so Poll opens it on demand. Without this,
    // receive-only audio (no local PTT) would never trigger the SCO and the
    // headset would stay silent until PTT was pressed.
    s_last_voice_ms = nowMs();
    if (!s_audio_active) {
        // SCO not up yet; nothing to buffer (the jitter buffer re-primes when the
        // SCO connects, so anything queued now would just be discarded).
        return;
    }
    size_t head = s_pcm_head;
    const size_t tail = s_pcm_tail;  // snapshot (consumer side)
    for (size_t i = 0; i < sample_count; ++i) {
        const size_t next = (head + 1u) % kPcmCap;
        if (next == tail) {
            break;  // buffer full -> drop the rest (headset is behind)
        }
        s_pcm[head] = samples[i];
        head = next;
    }
    s_pcm_head = head;
}

void NRL_BtHfp_Poll(void)
{
    // Apply a pending enable/disable request first (this runs on the BT task, so
    // the slow stack up/down never blocks the UI or the main loop).
    if (s_request_pending) {
        s_request_pending = false;
        s_transitioning = true;
        applyEnabled(s_request_enabled);
        s_transitioning = false;
    }
    if (!s_enabled || !s_stack_up) {
        return;
    }
    // Apply a pending headset-button PTT toggle outside the BT callback context.
    if (s_headset_ptt_dirty) {
        s_headset_ptt_dirty = false;
        STATUS_IO_SetSoftPtt(s_headset_ptt);
    }
    // Persist a headset-reported volume change (NVS write kept out of the callback).
    if (s_peer_volume_dirty) {
        s_peer_volume_dirty = false;
        savePeerVolume();
    }
    const uint32_t now = nowMs();
    // Let a stuck in-flight attempt expire so a fresh retry is allowed.
    if (s_connecting && (now - s_connect_started_ms >= kConnectTimeoutMs)) {
        s_connecting = false;
    }
    if (!s_connected) {
        // Auto-reconnect only to the headset the user explicitly chose, and never
        // while a scan or another connect attempt is already in flight (a second
        // slc_connect mid-handshake collides and tears the link down).
        if (s_have_peer && !s_connecting && !s_discovering &&
            (now - s_last_reconnect_ms >= kReconnectIntervalMs)) {
            s_last_reconnect_ms = now;
            s_connecting = true;
            s_connect_started_ms = now;
            coexPreferBt(true);
            esp_hf_ag_slc_connect(s_peer_bda);
        }
    } else {
        // Linked. Open the SCO voice link on demand and tear it down when idle,
        // so the headset can sleep (sniff) between transmissions.
        if (STATUS_IO_IsSqlActive()) {
            s_last_voice_ms = now;  // local PTT held -> a voice burst is live
        }
        const bool call_wanted =
            (s_last_voice_ms != 0u) && (now - s_last_voice_ms < kVoiceHangoverMs);

        if (call_wanted) {
            if (!s_call_active) {
                // Voice burst starting: tell the headset a call is active (so it
                // keeps SCO up) and open the SCO now. We leave coex on BALANCE --
                // biasing to BT did not reduce the warble and only made the WiFi
                // UDP voice jittery.
                s_call_active = true;
                esp_hf_ag_ciev_report(s_peer_bda, ESP_HF_IND_TYPE_CALL, 1);
                esp_hf_ag_ciev_report(s_peer_bda, ESP_HF_IND_TYPE_CALLSETUP, 0);
                s_last_audio_retry_ms = now;
                esp_hf_ag_audio_connect(s_peer_bda);
            } else if (!s_audio_active && (now - s_last_audio_retry_ms >= kAudioRetryIntervalMs)) {
                s_last_audio_retry_ms = now;
                esp_hf_ag_audio_connect(s_peer_bda);
            }
        } else if (s_call_active) {
            // Idle past the hangover: end the call so the link can sniff/save power.
            s_call_active = false;
            if (s_audio_active) {
                esp_hf_ag_audio_disconnect(s_peer_bda);
            }
            esp_hf_ag_ciev_report(s_peer_bda, ESP_HF_IND_TYPE_CALL, 0);
        }
    }
}

#else // !CONFIG_BT_HFP_AG_ENABLE -- non-S31 boards: no Bluetooth HFP.

#include <string.h>

void NRL_BtHfp_Init(void) {}
void NRL_BtHfp_SetEnabled(bool) {}
bool NRL_BtHfp_IsEnabled(void) { return false; }
bool NRL_BtHfp_TogglePending(void) { return false; }
void NRL_BtHfp_Poll(void) {}
void NRL_BtHfp_StartScan(void) {}
bool NRL_BtHfp_IsScanning(void) { return false; }
size_t NRL_BtHfp_GetDeviceCount(void) { return 0u; }
bool NRL_BtHfp_GetDeviceName(size_t, char *out, size_t out_size)
{
    if (out != nullptr && out_size != 0u) {
        out[0] = '\0';
    }
    return false;
}
void NRL_BtHfp_ConnectIndex(size_t) {}
size_t NRL_BtHfp_GetSavedCount(void) { return 0u; }
bool NRL_BtHfp_GetSavedName(size_t, char *out, size_t out_size)
{
    if (out != nullptr && out_size != 0u) {
        out[0] = '\0';
    }
    return false;
}
void NRL_BtHfp_ConnectSaved(size_t) {}
void NRL_BtHfp_RemoveSaved(size_t) {}
bool NRL_BtHfp_IsConnected(void) { return false; }
bool NRL_BtHfp_IsAudioActive(void) { return false; }
size_t NRL_BtHfp_GetPeerName(char *out, size_t out_size)
{
    if (out != nullptr && out_size != 0u) {
        out[0] = '\0';
    }
    return 0u;
}
void NRL_BtHfp_PushPlayback(const int16_t *, size_t) {}
int NRL_BtHfp_GetVolumePercent(void) { return -1; }
void NRL_BtHfp_AdjustVolume(int) {}
void NRL_BtHfp_SetVolumePercent(int) {}

#endif // CONFIG_BT_HFP_AG_ENABLE
