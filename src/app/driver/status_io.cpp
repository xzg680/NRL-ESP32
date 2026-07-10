#include "status_io.h"

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_GEZIPAI || NRL_BOARD == NRL_BOARD_S31_KORVO || \
    NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
#include "external_radio.h"
#include "es8311.h"
#include "../../lib/nrl_bt_hfp.h"  // route the volume keys to a connected headset
#endif

#if NRL_BOARD == NRL_BOARD_S31_KORVO
#include "bsp/led.h"  // vendored ESP32-S31-Korvo BSP: WS2812 status LED (GPIO37)
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
#include <led_strip.h>
#endif

#ifdef ENABLE_OPENCV
#include "../opencv/Arduino.hpp"
#endif

#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if NRL_BOARD == NRL_BOARD_GEZIPAI || NRL_BOARD == NRL_BOARD_S31_KORVO || \
    NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
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
    if (pin < 0) {
        return;
    }
    gpio_set_level((gpio_num_t)pin, on ? 0 : 1);
}

static bool blinkPhase(const unsigned long now_ms, const unsigned long period_ms)
{
    return ((now_ms / period_ms) & 1UL) == 0UL;
}

} // namespace

#if NRL_BOARD == NRL_BOARD_GEZIPAI || NRL_BOARD == NRL_BOARD_S31_KORVO || \
    NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD

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
#if NRL_BOARD == NRL_BOARD_S31_KORVO
bool s_ws2812_ready = false;
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
led_strip_handle_t s_ws2812 = nullptr;
#endif
bool s_volume_dirty = false;
unsigned long s_volume_change_ms = 0UL;

// PTT transmit state machine.
bool s_tx_active = false;            // effective transmit gate (IsSqlActive)
bool s_tx_latched = false;           // latched on by a short press
bool s_tx_suppressed = false;        // timeout fired during the current hold
bool s_soft_ptt_held = false;        // on-screen "network" area pressed (hold-to-talk)
unsigned long s_ptt_press_ms = 0UL;  // press-down time of the current press
unsigned long s_tx_since_ms = 0UL;   // when transmit last switched on
// STATUS_IO_Poll() runs from both the main loop and the bridge task; this
// mutex keeps the two from racing inside the button / PTT state machine.
SemaphoreHandle_t s_poll_mutex = nullptr;

#if defined(NRL_HAS_ADC_BUTTONS) && NRL_HAS_ADC_BUTTONS
adc_oneshot_unit_handle_t s_button_adc = nullptr;
adc_cali_handle_t s_button_adc_cali = nullptr;
bool s_button_adc_ready = false;
bool s_button_adc_cali_ready = false;

#if NRL_BOARD == NRL_BOARD_S31_KORVO
// The S31 has no IDF-supported adc_cali scheme (curve/line fitting are both
// disabled in esp_adc/esp32s31). Use the vendored BSP's per-bit-weight
// calibration (vendor/esp32_s31_korvo/esp32_s31_adc_calibration.c) so raw ADC
// counts become true millivolts that match the resistor-ladder thresholds in
// board_pins.h. Without it the code compared raw counts against mV thresholds,
// so the buttons were mismapped (VOL+ acted as PTT, SET did nothing).
extern "C" esp_err_t bsp_s31_adc_calibration_init(adc_oneshot_unit_handle_t handle,
                                                  adc_unit_t unit, adc_channel_t channel);
extern "C" esp_err_t bsp_s31_adc_calibration_raw_to_mv(adc_unit_t unit, int raw,
                                                       int *voltage_mv);
#endif

static bool adcInRange(const int value, const int low, const int high)
{
    return value >= low && value <= high;
}

static int readAdcButtonLevel(const int pin)
{
    if (!s_button_adc_ready || s_button_adc == nullptr) {
        return 1;
    }
    int raw_sum = 0;
    int samples = 0;
    for (int i = 0; i < 4; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_button_adc, NRL_ADC_BUTTON_CHANNEL, &raw) == ESP_OK) {
            raw_sum += raw;
            ++samples;
        }
    }
    if (samples == 0) {
        return 1;
    }
    int value = raw_sum / samples;
#if NRL_BOARD == NRL_BOARD_S31_KORVO
    if (s_button_adc_cali_ready) {
        int voltage_mv = 0;
        if (bsp_s31_adc_calibration_raw_to_mv(ADC_UNIT_1, value, &voltage_mv) == ESP_OK) {
            value = voltage_mv;
        }
    }
#else
    if (s_button_adc_cali_ready && s_button_adc_cali != nullptr) {
        int voltage_mv = 0;
        if (adc_cali_raw_to_voltage(s_button_adc_cali, value, &voltage_mv) == ESP_OK) {
            value = voltage_mv;
        }
    }
#endif
    bool pressed = false;
    switch (pin) {
        case NRL_PIN_BTN_VOL_UP:
            pressed = adcInRange(value, NRL_ADC_BTN_VOL_UP_LOW, NRL_ADC_BTN_VOL_UP_HIGH);
            break;
        case NRL_PIN_BTN_VOL_DOWN:
            pressed = adcInRange(value, NRL_ADC_BTN_VOL_DN_LOW, NRL_ADC_BTN_VOL_DN_HIGH);
            break;
        case NRL_PIN_BTN_PTT:
            pressed = adcInRange(value, NRL_ADC_BTN_SET_LOW, NRL_ADC_BTN_SET_HIGH);
            break;
        default:
            break;
    }
    return pressed ? 0 : 1;
}

static void initAdcButtons()
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit_cfg, &s_button_adc) != ESP_OK) {
        ESP_LOGI(TAG, "ADC button unit init failed");
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_0;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(s_button_adc, NRL_ADC_BUTTON_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGI(TAG, "ADC button channel config failed");
        return;
    }

#if NRL_BOARD == NRL_BOARD_S31_KORVO
    // S31: use the vendored BSP's software calibration (the only correct raw->mV
    // path on this chip). The standard adc_cali schemes below are unsupported.
    if (bsp_s31_adc_calibration_init(s_button_adc, ADC_UNIT_1, NRL_ADC_BUTTON_CHANNEL) == ESP_OK) {
        s_button_adc_cali_ready = true;
    } else {
        ESP_LOGW(TAG, "S31 ADC button calibration failed");
    }
#endif
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.chan = NRL_ADC_BUTTON_CHANNEL;
    cali_cfg.atten = ADC_ATTEN_DB_0;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_button_adc_cali) == ESP_OK) {
        s_button_adc_cali_ready = true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!s_button_adc_cali_ready) {
        adc_cali_line_fitting_config_t cali_cfg = {};
        cali_cfg.unit_id = ADC_UNIT_1;
        cali_cfg.atten = ADC_ATTEN_DB_0;
        cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_button_adc_cali) == ESP_OK) {
            s_button_adc_cali_ready = true;
        }
    }
#endif
    s_button_adc_ready = true;
}
#endif

// Updates one debounced button; returns true once on each release->press edge.
static bool pollButtonPressEdge(DebouncedButton &btn, const unsigned long now)
{
#if defined(NRL_HAS_ADC_BUTTONS) && NRL_HAS_ADC_BUTTONS
    const int level = readAdcButtonLevel(btn.pin);
#else
    const int level = gpio_get_level((gpio_num_t)btn.pin);
#endif
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
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
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
#endif
}

// Adjust the ES8311 speaker volume by `pct_delta` percentage points. The
// volume is stored as 0..255, but the user (and the LCD readout) think in
// 0..100 %, so each key press moves it exactly one percent.
static void adjustLineOutVolume(const int pct_delta)
{
    // While a Bluetooth headset is connected, the physical volume keys drive the
    // headset's own speaker volume (one HFP step per press) rather than the
    // onboard codec line-out. The LCD top-bar readout follows the headset value.
    if (NRL_BtHfp_IsConnected()) {
        NRL_BtHfp_AdjustVolume(pct_delta);
        return;
    }
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
#if !defined(NRL_HAS_USER_BUTTONS) || NRL_HAS_USER_BUTTONS
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
#else
    const bool is_pressed = false;
#endif

    // Effective transmit: the held PTT button or the on-screen soft-PTT hold
    // (unless the timeout suppressed it) or the latch left on by a short press.
    bool tx = ((is_pressed || s_soft_ptt_held) && !s_tx_suppressed) || s_tx_latched;

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

#if defined(NRL_HAS_ADC_BUTTONS) && NRL_HAS_ADC_BUTTONS
    initAdcButtons();
#elif !defined(NRL_HAS_USER_BUTTONS) || NRL_HAS_USER_BUTTONS
    gpio_reset_pin((gpio_num_t)NRL_PIN_BTN_VOL_UP);
    gpio_set_direction((gpio_num_t)NRL_PIN_BTN_VOL_UP, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)NRL_PIN_BTN_VOL_UP, GPIO_PULLUP_ONLY);
    gpio_reset_pin((gpio_num_t)NRL_PIN_BTN_VOL_DOWN);
    gpio_set_direction((gpio_num_t)NRL_PIN_BTN_VOL_DOWN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)NRL_PIN_BTN_VOL_DOWN, GPIO_PULLUP_ONLY);
    gpio_reset_pin((gpio_num_t)NRL_PIN_BTN_PTT);
    gpio_set_direction((gpio_num_t)NRL_PIN_BTN_PTT, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)NRL_PIN_BTN_PTT, GPIO_PULLUP_ONLY);
#endif

    if (NRL_PIN_LED_PTT >= 0) {
    gpio_reset_pin((gpio_num_t)NRL_PIN_LED_PTT);
    gpio_set_direction((gpio_num_t)NRL_PIN_LED_PTT, GPIO_MODE_OUTPUT);
    }
    if (NRL_PIN_LED_AUDIO >= 0) {
    gpio_reset_pin((gpio_num_t)NRL_PIN_LED_AUDIO);
    gpio_set_direction((gpio_num_t)NRL_PIN_LED_AUDIO, GPIO_MODE_OUTPUT);
    }
    if (NRL_PIN_LED_NET >= 0) {
    gpio_reset_pin((gpio_num_t)NRL_PIN_LED_NET);
    gpio_set_direction((gpio_num_t)NRL_PIN_LED_NET, GPIO_MODE_OUTPUT);
    }
    writeLed(NRL_PIN_LED_PTT,   false);
    writeLed(NRL_PIN_LED_AUDIO, false);
    writeLed(NRL_PIN_LED_NET,   false);

#if NRL_BOARD == NRL_BOARD_S31_KORVO
    // The S31 has a single addressable WS2812 on GPIO37 instead of the three
    // discrete LEDs above. Drive it via the vendored BSP and clear it.
    if (bsp_led_init() == ESP_OK) {
        s_ws2812_ready = true;
        bsp_led_clear();
    } else {
        ESP_LOGW(TAG, "WS2812 status LED init failed");
    }
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
    const led_strip_config_t strip_config = {
        .strip_gpio_num = NRL_PIN_WS2812_STATUS,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = NRL_WS2812_INVERT_OUT,
        },
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_ws2812) == ESP_OK) {
        (void)led_strip_clear(s_ws2812);
    } else {
        s_ws2812 = nullptr;
        ESP_LOGW(TAG, "Function CoreBoard WS2812 init failed");
    }
#endif
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

// Hold-to-talk from the LCD "network" panel: press = transmit, release = stop.
// The PTT timeout still applies; releasing clears any timeout suppression so the
// next hold can key up again.
extern "C" void STATUS_IO_SetSoftPtt(const bool held)
{
    s_soft_ptt_held = held;
    if (!held) {
        s_tx_suppressed = false;
    }
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

#if !defined(NRL_HAS_USER_BUTTONS) || NRL_HAS_USER_BUTTONS
    pollVolumeButton(s_btn_vol_up,   s_btn_vol_up_repeat,   +1, now);
    pollVolumeButton(s_btn_vol_down, s_btn_vol_down_repeat, -1, now);
#endif
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

#if NRL_BOARD == NRL_BOARD_S31_KORVO
    // Fold the three discrete-LED meanings into the single RGB pixel:
    //   R = transmitting (mic uplink), G = inbound network audio playing,
    //   B = server link (solid when the heartbeat is alive, slow blink while it
    //   is missing). Kept dim so it is a soft indicator, not a flashlight.
    if (s_ws2812_ready) {
        const uint8_t r = s_tx_active ? 60 : 0;
        const uint8_t g = s_net_audio_active ? 60 : 0;
        const uint8_t b = heartbeat_ok ? 30 : (blinkPhase(now, kSlowBlinkMs) ? 30 : 0);
        bsp_led_set_rgb(BSP_LED_STATUS, r, g, b);
    }
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
    if (s_ws2812 != nullptr) {
        const uint8_t r = s_tx_active ? 60 : 0;
        const uint8_t g = s_net_audio_active ? 60 : 0;
        const uint8_t b = heartbeat_ok ? 30 : (blinkPhase(now, kSlowBlinkMs) ? 30 : 0);
        (void)led_strip_set_pixel(s_ws2812, 0, r, g, b);
        (void)led_strip_refresh(s_ws2812);
    }
#endif

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
