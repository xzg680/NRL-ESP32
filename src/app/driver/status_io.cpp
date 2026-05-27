#include "status_io.h"

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_GEZIPAI
#include "external_radio.h"
#include "es8311.h"
#endif

#ifdef ENABLE_OPENCV
#include "../opencv/Arduino.hpp"
#endif

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if NRL_BOARD == NRL_BOARD_GEZIPAI
#include <freertos/semphr.h>
#endif

static const char *TAG = "IO";

namespace {

constexpr unsigned long kSlowBlinkMs = 500UL;
constexpr unsigned long kHeartbeatMissedBlinkMs = 6500UL;

inline unsigned long nrl_millis_now() { return (unsigned long)(esp_timer_get_time() / 1000ULL); }

// Indicator LEDs are wired active-low (GPIO sinks current): driving the pin
// LOW lights the LED, HIGH turns it off.
static void writeLed(const int pin, const bool on)
{
    gpio_set_level((gpio_num_t)pin, on ? 0 : 1);
}

static bool blinkPhase(const unsigned long now_ms, const unsigned long period_ms)
{
    return ((now_ms / period_ms) & 1UL) == 0UL;
}

} // namespace

#if NRL_BOARD == NRL_BOARD_GEZIPAI

// ============================================================
// 格子派: 3 push buttons (volume +/-, physical PTT) + 3 LEDs.
//   - PTT button: a short press toggles transmit on/off; a long press is
//     momentary (transmit only while held). Transmit is force-stopped once it
//     has been on for the configurable PTT timeout (AT+PTT_TIMEOUT / web).
//   - Yellow LED (NRL_PIN_LED_PTT)  : lit while transmitting (mic uplink).
//   - Green  LED (NRL_PIN_LED_AUDIO): lit while inbound network audio plays.
//   - White  LED (NRL_PIN_LED_NET)  : network/server link status -- solid when
//                                     the server heartbeat is alive, slow
//                                     blink while it is missing.
//   - Volume +/- adjust the ES8311 line-out (speaker) volume.
// ============================================================

namespace {

constexpr unsigned long kButtonDebounceMs = 30UL;
constexpr unsigned long kVolumeSaveDelayMs = 2000UL;
// Volume +/- auto-repeat: after the first press, wait this long before the
// held button starts firing again, then step at kVolumeRepeatIntervalMs.
constexpr unsigned long kVolumeRepeatDelayMs = 400UL;
constexpr unsigned long kVolumeRepeatIntervalMs = 80UL;
// A press shorter than this toggles the transmit latch; a longer press is a
// momentary (push-to-talk) hold that ends the moment the button is released.
constexpr unsigned long kPttLongPressMs = 500UL;
// Fallback transmit timeout, used only if the stored config cannot be read.
constexpr unsigned long kDefaultPttTimeoutMs = 300000UL;

struct DebouncedButton {
    int pin;
    int raw_level;
    bool pressed;
    unsigned long changed_ms;
};

struct AutoRepeat {
    bool active;
    unsigned long next_fire_ms;
};

DebouncedButton s_btn_vol_up   = {NRL_PIN_BTN_VOL_UP,   1, false, 0UL};
DebouncedButton s_btn_vol_down = {NRL_PIN_BTN_VOL_DOWN, 1, false, 0UL};
DebouncedButton s_btn_ptt      = {NRL_PIN_BTN_PTT,      1, false, 0UL};

AutoRepeat s_btn_vol_up_repeat   = {false, 0UL};
AutoRepeat s_btn_vol_down_repeat = {false, 0UL};

bool s_net_audio_active = false;
unsigned long s_last_heartbeat_rx_ms = 0UL;
bool s_volume_dirty = false;
unsigned long s_volume_change_ms = 0UL;

// PTT transmit state machine.
bool s_tx_active = false;            // effective transmit gate (IsSqlActive)
bool s_tx_latched = false;           // latched on by a short press
bool s_tx_suppressed = false;        // timeout fired during the current hold
unsigned long s_ptt_press_ms = 0UL;  // press-down time of the current press
unsigned long s_tx_since_ms = 0UL;   // when transmit last switched on
// STATUS_IO_Poll() runs from both the main loop and the bridge task; this
// mutex keeps the two from racing inside the button / PTT state machine.
SemaphoreHandle_t s_poll_mutex = nullptr;

// Updates one debounced button; returns true once on each release->press edge.
static bool pollButtonPressEdge(DebouncedButton &btn, const unsigned long now)
{
    const int level = gpio_get_level((gpio_num_t)btn.pin);
    if (level != btn.raw_level) {
        btn.raw_level = level;
        btn.changed_ms = now;
        return false;
    }
    const bool stable_pressed = (level == 0); // active-low, INPUT_PULLUP
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
                            cfg->daceq_a1,
                            cfg->adc_dmic_enabled,
                            cfg->adc_linsel,
                            cfg->adc_pga_gain,
                            cfg->adc_ramprate,
                            cfg->adc_dmic_sense,
                            cfg->adc_sync,
                            cfg->adc_inv,
                            cfg->adc_ramclr,
                            cfg->adc_scale,
                            cfg->alc_enabled,
                            cfg->adc_automute_enabled,
                            cfg->alc_winsize,
                            cfg->alc_maxlevel,
                            cfg->alc_minlevel,
                            cfg->adc_automute_winsize,
                            cfg->adc_automute_noise_gate,
                            cfg->adc_automute_volume,
                            cfg->adc_hpfs1,
                            cfg->adc_eq_bypass,
                            cfg->adc_hpf,
                            cfg->adc_hpfs2,
                            cfg->adceq_b0,
                            cfg->adceq_a1,
                            cfg->adceq_a2,
                            cfg->adceq_b1,
                            cfg->adceq_b2);
}

// Adjust the ES8311 speaker volume by `pct_delta` percentage points. The
// volume is stored as 0..255, but the user (and the LCD readout) think in
// 0..100 %, so each key press moves it exactly one percent.
static void adjustLineOutVolume(const int pct_delta)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) {
        return;
    }
    int pct = (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255;
    pct += pct_delta;
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    const int volume = (pct * 255 + 50) / 100;
    if (static_cast<uint8_t>(volume) == cfg->line_out_volume) {
        return; // already at the limit
    }
    EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), false);
    reapplyEs8311Volume();
    s_volume_dirty = true;
    s_volume_change_ms = nrl_millis_now();
    ESP_LOGI(TAG, "line_out_volume=%d (%d%%)", volume, pct);
}

// Step the speaker volume on the initial press, and again at a steady cadence
// while the button is held so the user can sweep up/down by holding.
static void pollVolumeButton(DebouncedButton &btn, AutoRepeat &rep,
                             const int pct_delta, const unsigned long now)
{
    if (pollButtonPressEdge(btn, now)) {
        adjustLineOutVolume(pct_delta);
        rep.active = true;
        rep.next_fire_ms = now + kVolumeRepeatDelayMs;
        return;
    }
    if (!btn.pressed) {
        rep.active = false;
        return;
    }
    if (rep.active && (long)(now - rep.next_fire_ms) >= 0) {
        adjustLineOutVolume(pct_delta);
        rep.next_fire_ms = now + kVolumeRepeatIntervalMs;
    }
}

// Effective PTT auto-off timeout in milliseconds, from the persisted config.
static unsigned long pttTimeoutMs(void)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg != nullptr && cfg->ptt_timeout_s > 0u) {
        return static_cast<unsigned long>(cfg->ptt_timeout_s) * 1000UL;
    }
    return kDefaultPttTimeoutMs;
}

// Runs the PTT button state machine and updates s_tx_active. A short press
// toggles the transmit latch; a long press is momentary transmit while held;
// either way transmit is forced off once it has been on for the PTT timeout.
static void updatePtt(const unsigned long now)
{
    const bool was_pressed = s_btn_ptt.pressed;
    pollButtonPressEdge(s_btn_ptt, now);  // refreshes s_btn_ptt.pressed
    const bool is_pressed = s_btn_ptt.pressed;

    if (is_pressed && !was_pressed) {
        // press down
        s_ptt_press_ms = now;
        s_tx_suppressed = false;
    } else if (!is_pressed && was_pressed) {
        // release
        const unsigned long held = now - s_ptt_press_ms;
        if (!s_tx_suppressed && held < kPttLongPressMs) {
            s_tx_latched = !s_tx_latched;  // short press toggles the latch
        } else {
            s_tx_latched = false;  // a long press always ends transmit
        }
        s_tx_suppressed = false;
    }

    // Effective transmit: the held button (unless the timeout suppressed it)
    // or the latch left on by a short press.
    bool tx = (is_pressed && !s_tx_suppressed) || s_tx_latched;

    if (tx && !s_tx_active) {
        s_tx_since_ms = now;  // transmit just switched on -> start the timer
    }
    if (tx && (now - s_tx_since_ms) >= pttTimeoutMs()) {
        // Timeout: force transmit off. s_tx_suppressed keeps a still-held
        // button from immediately re-keying until it is released.
        s_tx_latched = false;
        s_tx_suppressed = true;
        tx = false;
        ESP_LOGI(TAG, "PTT timeout, transmit forced off");
    }

    if (tx != s_tx_active) {
        ESP_LOGI(TAG, "transmit %s", tx ? "ON" : "OFF");
    }
    s_tx_active = tx;
}

} // namespace

extern "C" bool STATUS_IO_IsSqlActive(void)
{
    // 格子派 has no radio squelch: the PTT button state machine (short-press
    // latch / long-press momentary / timeout) decides when the microphone is
    // captured and streamed, mirroring the SQL-active gate.
    return s_tx_active;
}

extern "C" bool STATUS_IO_IsPttActive(void)
{
    return s_net_audio_active;
}

extern "C" void STATUS_IO_Init(void)
{
    if (s_poll_mutex == nullptr) {
        s_poll_mutex = xSemaphoreCreateMutex();
    }

    gpio_reset_pin((gpio_num_t)NRL_PIN_BTN_VOL_UP);
    gpio_set_direction((gpio_num_t)NRL_PIN_BTN_VOL_UP, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)NRL_PIN_BTN_VOL_UP, GPIO_PULLUP_ONLY);
    gpio_reset_pin((gpio_num_t)NRL_PIN_BTN_VOL_DOWN);
    gpio_set_direction((gpio_num_t)NRL_PIN_BTN_VOL_DOWN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)NRL_PIN_BTN_VOL_DOWN, GPIO_PULLUP_ONLY);
    gpio_reset_pin((gpio_num_t)NRL_PIN_BTN_PTT);
    gpio_set_direction((gpio_num_t)NRL_PIN_BTN_PTT, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)NRL_PIN_BTN_PTT, GPIO_PULLUP_ONLY);

    gpio_reset_pin((gpio_num_t)NRL_PIN_LED_PTT);
    gpio_set_direction((gpio_num_t)NRL_PIN_LED_PTT, GPIO_MODE_OUTPUT);
    gpio_reset_pin((gpio_num_t)NRL_PIN_LED_AUDIO);
    gpio_set_direction((gpio_num_t)NRL_PIN_LED_AUDIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin((gpio_num_t)NRL_PIN_LED_NET);
    gpio_set_direction((gpio_num_t)NRL_PIN_LED_NET, GPIO_MODE_OUTPUT);
    writeLed(NRL_PIN_LED_PTT,   false);
    writeLed(NRL_PIN_LED_AUDIO, false);
    writeLed(NRL_PIN_LED_NET,   false);
}

extern "C" void STATUS_IO_SetPttActive(const bool active)
{
    // On Gezipai, "PTT active" means inbound network voice is being played out.
    s_net_audio_active = active;
    writeLed(NRL_PIN_LED_AUDIO, active);
}

extern "C" void STATUS_IO_NotifyHeartbeatReceived(void)
{
    s_last_heartbeat_rx_ms = nrl_millis_now();
}

extern "C" void STATUS_IO_Poll(void)
{
    // STATUS_IO_Poll() is called from both the main loop and the bridge task;
    // run the body single-threaded so the button / PTT state machine, and the
    // debounced inputs it reads, cannot be processed concurrently.
    if (s_poll_mutex != nullptr && xSemaphoreTake(s_poll_mutex, 0) != pdTRUE) {
        return;
    }

    const unsigned long now = nrl_millis_now();

    pollVolumeButton(s_btn_vol_up,   s_btn_vol_up_repeat,   +1, now);
    pollVolumeButton(s_btn_vol_down, s_btn_vol_down_repeat, -1, now);
    updatePtt(now);

    // Yellow LED follows the transmit state (mic uplink in progress).
    writeLed(NRL_PIN_LED_PTT, s_tx_active);
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
        ESP_LOGI(TAG, "line_out_volume saved");
    }

    if (s_poll_mutex != nullptr) {
        xSemaphoreGive(s_poll_mutex);
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
    return gpio_get_level((gpio_num_t)NRL_PIN_SQL1) == 1;
}

static bool sql2Active()
{
    return gpio_get_level((gpio_num_t)NRL_PIN_SQL2) == 0;
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
    gpio_reset_pin((gpio_num_t)NRL_PIN_PTT_OUT);
    gpio_set_direction((gpio_num_t)NRL_PIN_PTT_OUT, GPIO_MODE_OUTPUT);
    gpio_reset_pin((gpio_num_t)NRL_PIN_STATUS_PTT_LED);
    gpio_set_direction((gpio_num_t)NRL_PIN_STATUS_PTT_LED, GPIO_MODE_OUTPUT);
    gpio_reset_pin((gpio_num_t)NRL_PIN_STATUS_IO1);
    gpio_set_direction((gpio_num_t)NRL_PIN_STATUS_IO1, GPIO_MODE_OUTPUT);
    gpio_reset_pin((gpio_num_t)NRL_PIN_STATUS_IO2);
    gpio_set_direction((gpio_num_t)NRL_PIN_STATUS_IO2, GPIO_MODE_OUTPUT);

    gpio_reset_pin((gpio_num_t)NRL_PIN_SQL1);
    gpio_set_direction((gpio_num_t)NRL_PIN_SQL1, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)NRL_PIN_SQL1, GPIO_PULLDOWN_ONLY);
    gpio_reset_pin((gpio_num_t)NRL_PIN_SQL2);
    gpio_set_direction((gpio_num_t)NRL_PIN_SQL2, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)NRL_PIN_SQL2, GPIO_PULLUP_ONLY);

    gpio_set_level((gpio_num_t)NRL_PIN_PTT_OUT, 0);
    writeLed(NRL_PIN_STATUS_PTT_LED, false);
    writeLed(NRL_PIN_STATUS_IO1, false);
    writeLed(NRL_PIN_STATUS_IO2, false);
}

extern "C" void STATUS_IO_SetPttActive(const bool active)
{
    if (s_ptt_active != active) {
        ESP_LOGI(TAG, "ptt_out=%u", active ? 1u : 0u);
    }
    s_ptt_active = active;
    gpio_set_level((gpio_num_t)NRL_PIN_PTT_OUT, active ? 1 : 0);
    writeLed(NRL_PIN_STATUS_PTT_LED, active);
}

extern "C" void STATUS_IO_NotifyHeartbeatReceived(void)
{
    s_last_heartbeat_rx_ms = nrl_millis_now();
}

extern "C" void STATUS_IO_Poll(void)
{
    const unsigned long now = nrl_millis_now();
    const int sql1_level = gpio_get_level((gpio_num_t)NRL_PIN_SQL1);
    const int sql2_level = gpio_get_level((gpio_num_t)NRL_PIN_SQL2);
    if (sql1_level != s_last_sql1_level || sql2_level != s_last_sql2_level) {
        ESP_LOGI(TAG, "sql1=%u sql2=%u active=%u",
                 static_cast<unsigned>(sql1_level == 1 ? 1u : 0u),
                 static_cast<unsigned>(sql2_level == 1 ? 1u : 0u),
                 static_cast<unsigned>((sql1_level == 1 || sql2_level == 0) ? 1u : 0u));
        s_last_sql1_level = sql1_level;
        s_last_sql2_level = sql2_level;
    }

    const bool sql_active = (sql1_level == 1) || (sql2_level == 0);
    const bool heartbeat_ok =
        s_last_heartbeat_rx_ms != 0UL && (now - s_last_heartbeat_rx_ms) <= kHeartbeatMissedBlinkMs;

    // IO1 is the blue status LED, used for server-alive indication.
    writeLed(NRL_PIN_STATUS_IO1, heartbeat_ok ? true : blinkPhase(now, kSlowBlinkMs));
    // IO2 is the green status LED, used for radio SQL indication.
    writeLed(NRL_PIN_STATUS_IO2, sql_active);

    // Keep these two outputs refreshed even if callers only update the latch state.
    gpio_set_level((gpio_num_t)NRL_PIN_PTT_OUT, s_ptt_active ? 1 : 0);
    writeLed(NRL_PIN_STATUS_PTT_LED, s_ptt_active);
}

#endif // NRL_BOARD
