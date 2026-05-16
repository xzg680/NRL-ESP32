#include "status_io.h"

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_GEZIPAI
#include "external_radio.h"
#include "es8311.h"
#endif

#ifndef ENABLE_OPENCV
#include <Arduino.h>
#else
#include "../opencv/Arduino.hpp"
#endif

namespace {

constexpr unsigned long kSlowBlinkMs = 500UL;
constexpr unsigned long kHeartbeatMissedBlinkMs = 6500UL;

// Indicator LEDs are wired active-low (GPIO sinks current): driving the pin
// LOW lights the LED, HIGH turns it off.
static void writeLed(const int pin, const bool on)
{
    digitalWrite(pin, on ? LOW : HIGH);
}

static bool blinkPhase(const unsigned long now_ms, const unsigned long period_ms)
{
    return ((now_ms / period_ms) & 1UL) == 0UL;
}

} // namespace

#if NRL_BOARD == NRL_BOARD_GEZIPAI

// ============================================================
// 格子派: 3 push buttons (volume +/-, physical PTT) + 3 LEDs.
//   - Physical PTT gates microphone capture/streaming (replaces radio SQL).
//   - Yellow LED (NRL_PIN_LED_PTT)  : lit while PTT is held / mic is captured.
//   - Green  LED (NRL_PIN_LED_AUDIO): lit while inbound network audio plays.
//   - White  LED (NRL_PIN_LED_NET)  : network/server link status -- solid when
//                                     the server heartbeat is alive, slow
//                                     blink while it is missing.
//   - Volume +/- adjust the ES8311 line-out (speaker) volume.
// ============================================================

namespace {

constexpr unsigned long kButtonDebounceMs = 30UL;
constexpr unsigned long kVolumeSaveDelayMs = 2000UL;
constexpr int kVolumeStep = 16;
constexpr int kVolumeMax = 255;

struct DebouncedButton {
    int pin;
    int raw_level;
    bool pressed;
    unsigned long changed_ms;
};

DebouncedButton s_btn_vol_up   = {NRL_PIN_BTN_VOL_UP,   HIGH, false, 0UL};
DebouncedButton s_btn_vol_down = {NRL_PIN_BTN_VOL_DOWN, HIGH, false, 0UL};
DebouncedButton s_btn_ptt      = {NRL_PIN_BTN_PTT,      HIGH, false, 0UL};

bool s_net_audio_active = false;
unsigned long s_last_heartbeat_rx_ms = 0UL;
bool s_volume_dirty = false;
unsigned long s_volume_change_ms = 0UL;

// Updates one debounced button; returns true once on each release->press edge.
static bool pollButtonPressEdge(DebouncedButton &btn, const unsigned long now)
{
    const int level = digitalRead(btn.pin);
    if (level != btn.raw_level) {
        btn.raw_level = level;
        btn.changed_ms = now;
        return false;
    }
    const bool stable_pressed = (level == LOW); // active-low, INPUT_PULLUP
    if (stable_pressed != btn.pressed && (now - btn.changed_ms) >= kButtonDebounceMs) {
        btn.pressed = stable_pressed;
        return stable_pressed;
    }
    return false;
}

// Push the current config volumes (and tone/EQ) back into the ES8311 so a
// volume change made via the buttons takes effect immediately.
static void reapplyEs8311Volume()
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) {
        return;
    }
    ES8311_ApplyAudioConfig(cfg->mic_volume,
                            cfg->line_out_volume,
                            cfg->hp_drive_enabled,
                            cfg->drc_enabled,
                            cfg->drc_winsize,
                            cfg->drc_maxlevel,
                            cfg->drc_minlevel,
                            cfg->dac_ramprate,
                            cfg->dac_eq_bypass,
                            cfg->daceq_b0,
                            cfg->daceq_b1,
                            cfg->daceq_a1);
}

static void adjustLineOutVolume(const int delta)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) {
        return;
    }
    int volume = static_cast<int>(cfg->line_out_volume) + delta;
    if (volume < 0) {
        volume = 0;
    } else if (volume > kVolumeMax) {
        volume = kVolumeMax;
    }
    if (static_cast<uint8_t>(volume) == cfg->line_out_volume) {
        return; // already at the limit
    }
    EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), false);
    reapplyEs8311Volume();
    s_volume_dirty = true;
    s_volume_change_ms = millis();
    Serial.printf("[IO] line_out_volume=%d\n", volume);
}

} // namespace

extern "C" bool STATUS_IO_IsSqlActive(void)
{
    // 格子派 has no radio squelch: the physical PTT button decides when the
    // microphone is captured and streamed, mirroring the SQL-active gate.
    return s_btn_ptt.pressed;
}

extern "C" bool STATUS_IO_IsPttActive(void)
{
    return s_net_audio_active;
}

extern "C" void STATUS_IO_Init(void)
{
    pinMode(NRL_PIN_BTN_VOL_UP,   INPUT_PULLUP);
    pinMode(NRL_PIN_BTN_VOL_DOWN, INPUT_PULLUP);
    pinMode(NRL_PIN_BTN_PTT,      INPUT_PULLUP);

    pinMode(NRL_PIN_LED_PTT,   OUTPUT);
    pinMode(NRL_PIN_LED_AUDIO, OUTPUT);
    pinMode(NRL_PIN_LED_NET,   OUTPUT);
    writeLed(NRL_PIN_LED_PTT,   false);
    writeLed(NRL_PIN_LED_AUDIO, false);
    writeLed(NRL_PIN_LED_NET,   false);
}

extern "C" void STATUS_IO_SetPttActive(const bool active)
{
    // On 格子派 "PTT active" means inbound network voice is being played out.
    if (s_net_audio_active != active) {
        Serial.printf("[IO] net_audio=%u\n", active ? 1u : 0u);
    }
    s_net_audio_active = active;
    writeLed(NRL_PIN_LED_AUDIO, active);
}

extern "C" void STATUS_IO_NotifyHeartbeatReceived(void)
{
    s_last_heartbeat_rx_ms = millis();
}

extern "C" void STATUS_IO_Poll(void)
{
    const unsigned long now = millis();

    if (pollButtonPressEdge(s_btn_vol_up, now)) {
        adjustLineOutVolume(+kVolumeStep);
    }
    if (pollButtonPressEdge(s_btn_vol_down, now)) {
        adjustLineOutVolume(-kVolumeStep);
    }
    pollButtonPressEdge(s_btn_ptt, now);

    // Yellow LED follows the physical PTT button (mic capture in progress).
    writeLed(NRL_PIN_LED_PTT, s_btn_ptt.pressed);
    // Green LED is refreshed from the latched network-audio state.
    writeLed(NRL_PIN_LED_AUDIO, s_net_audio_active);
    // White LED: solid while the server heartbeat is alive, slow blink while
    // it is missing (no link yet / lost).
    const bool heartbeat_ok =
        s_last_heartbeat_rx_ms != 0UL && (now - s_last_heartbeat_rx_ms) <= kHeartbeatMissedBlinkMs;
    writeLed(NRL_PIN_LED_NET, heartbeat_ok ? true : blinkPhase(now, kSlowBlinkMs));

    // Persist the volume only after the user stops adjusting, so a burst of
    // button taps does not hammer the EEPROM.
    if (s_volume_dirty && (now - s_volume_change_ms) >= kVolumeSaveDelayMs) {
        s_volume_dirty = false;
        EXTERNAL_RADIO_SaveConfig();
        Serial.println("[IO] line_out_volume saved");
    }
}

#else // NRL_BOARD_BH4TDV

// ============================================================
// BH4TDV 3188 NRL: radio-facing status I/O (PTT out, squelch in, LEDs).
// ============================================================

namespace {

bool s_ptt_active = false;
unsigned long s_last_heartbeat_rx_ms = 0UL;
int s_last_sql1_level = -1;
int s_last_sql2_level = -1;

static bool sql1Active()
{
    return digitalRead(NRL_PIN_SQL1) == HIGH;
}

static bool sql2Active()
{
    return digitalRead(NRL_PIN_SQL2) == LOW;
}

} // namespace

extern "C" bool STATUS_IO_IsSqlActive(void)
{
    return sql1Active() || sql2Active();
}

extern "C" bool STATUS_IO_IsPttActive(void)
{
    return s_ptt_active;
}

extern "C" void STATUS_IO_Init(void)
{
    pinMode(NRL_PIN_PTT_OUT, OUTPUT);
    pinMode(NRL_PIN_STATUS_PTT_LED, OUTPUT);
    pinMode(NRL_PIN_STATUS_IO1, OUTPUT);
    pinMode(NRL_PIN_STATUS_IO2, OUTPUT);

    pinMode(NRL_PIN_SQL1, INPUT_PULLDOWN);
    pinMode(NRL_PIN_SQL2, INPUT_PULLUP);

    digitalWrite(NRL_PIN_PTT_OUT, LOW);
    writeLed(NRL_PIN_STATUS_PTT_LED, false);
    writeLed(NRL_PIN_STATUS_IO1, false);
    writeLed(NRL_PIN_STATUS_IO2, false);
}

extern "C" void STATUS_IO_SetPttActive(const bool active)
{
    if (s_ptt_active != active) {
        Serial.printf("[IO] ptt_out=%u\n", active ? 1u : 0u);
    }
    s_ptt_active = active;
    digitalWrite(NRL_PIN_PTT_OUT, active ? HIGH : LOW);
    writeLed(NRL_PIN_STATUS_PTT_LED, active);
}

extern "C" void STATUS_IO_NotifyHeartbeatReceived(void)
{
    s_last_heartbeat_rx_ms = millis();
}

extern "C" void STATUS_IO_Poll(void)
{
    const unsigned long now = millis();
    const int sql1_level = digitalRead(NRL_PIN_SQL1);
    const int sql2_level = digitalRead(NRL_PIN_SQL2);
    if (sql1_level != s_last_sql1_level || sql2_level != s_last_sql2_level) {
        Serial.printf("[IO] sql1=%u sql2=%u active=%u\n",
                      static_cast<unsigned>(sql1_level == HIGH ? 1u : 0u),
                      static_cast<unsigned>(sql2_level == HIGH ? 1u : 0u),
                      static_cast<unsigned>((sql1_level == HIGH || sql2_level == LOW) ? 1u : 0u));
        s_last_sql1_level = sql1_level;
        s_last_sql2_level = sql2_level;
    }

    const bool sql_active = (sql1_level == HIGH) || (sql2_level == LOW);
    const bool heartbeat_ok =
        s_last_heartbeat_rx_ms != 0UL && (now - s_last_heartbeat_rx_ms) <= kHeartbeatMissedBlinkMs;

    // IO1 is the blue status LED, used for server-alive indication.
    writeLed(NRL_PIN_STATUS_IO1, heartbeat_ok ? true : blinkPhase(now, kSlowBlinkMs));
    // IO2 is the green status LED, used for radio SQL indication.
    writeLed(NRL_PIN_STATUS_IO2, sql_active);

    // Keep these two outputs refreshed even if callers only update the latch state.
    digitalWrite(NRL_PIN_PTT_OUT, s_ptt_active ? HIGH : LOW);
    writeLed(NRL_PIN_STATUS_PTT_LED, s_ptt_active);
}

#endif // NRL_BOARD
