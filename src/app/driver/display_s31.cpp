#include "display.h"

#include "board_pins.h"

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && NRL_BOARD == NRL_BOARD_S31_KORVO

#include "../../lib/nrl_audio_bridge.h"
#include "../../lib/nrl_bt_hfp.h"
#include "../../lib/nrl_net_compat.h"
#include "../../lib/nrl_wifi.h"
#include "external_radio.h"
#include "s31_i2c.h"
#include "status_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_gt1151.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

static const char *TAG = "LCD_S31";

namespace {

constexpr int kWidth = NRL_DISPLAY_WIDTH;
constexpr int kHeight = NRL_DISPLAY_HEIGHT;
constexpr uint32_t kRefreshIntervalMs = 500u;
constexpr uint32_t kVolumeSaveDelayMs = 2000u;
constexpr uint16_t kVolumeLongPressRepeatMs = 80u;

constexpr uint32_t kColorBg = 0x070B11;
constexpr uint32_t kColorPanel = 0x0B1220;
constexpr uint32_t kColorPanel2 = 0x101A2A;
constexpr uint32_t kColorBorder = 0x24364D;
constexpr uint32_t kColorText = 0xE6EDF3;
constexpr uint32_t kColorSub = 0x7D95AA;
constexpr uint32_t kColorDim = 0x4C6378;
constexpr uint32_t kColorTime = 0xBFE9F5;
constexpr uint32_t kColorAccent = 0x22D3EE;
constexpr uint32_t kColorGood = 0x4ADE80;
constexpr uint32_t kColorWarn = 0xF5B453;
constexpr uint32_t kColorBad = 0xF87171;
constexpr uint32_t kColorTx = 0xFF6B6B;
constexpr uint32_t kColorDuplex = 0xA78BFA;
constexpr const char *kCallsignPlaceholder = "----------";
constexpr size_t kStationFieldChars = 10u;
constexpr size_t kWifiOptionCount = 12u;  // max scanned SSIDs listed in the dropdown

enum class Page : uint8_t {
    Home,
    Config,
    Wifi,
    Station,
    Audio,
    Bt,
};

enum class Action : intptr_t {
    Config = 1,
    Home,
    Wifi,
    Station,
    Audio,
    Bt,
    ScanBt,
    VolumeDown,
    VolumeUp,
    CallsignSsidDown,
    CallsignSsidUp,
    ResetWifi,
    ScanWifi,
    SaveWifi,
    SaveStation,
    SaveAudio,
    ResetAudio,
};

enum class AudioControl : intptr_t {
    Speaker = 1,
    Mic,
    Aec,
    Noise,
    MicHpf,
};

bool s_ready = false;
Page s_page = Page::Home;
uint32_t s_last_refresh_ms = 0u;
uint32_t s_volume_change_ms = 0u;
bool s_time_sync_started = false;
bool s_volume_dirty = false;
volatile bool s_wifi_scan_running = false;
volatile bool s_wifi_scan_complete = false;
volatile bool s_wifi_scan_ok = false;
TaskHandle_t s_wifi_scan_task = nullptr;

esp_lcd_panel_handle_t s_panel = nullptr;
lv_display_t *s_disp = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
lv_indev_t *s_touch_indev = nullptr;

lv_obj_t *s_lbl_caption = nullptr;
lv_obj_t *s_lbl_callsign = nullptr;
lv_obj_t *s_lbl_ssid = nullptr;
lv_obj_t *s_lbl_time = nullptr;
lv_obj_t *s_lbl_local_station = nullptr;
lv_obj_t *s_lbl_wifi = nullptr;
lv_obj_t *s_lbl_bt_top = nullptr;
lv_obj_t *s_list_bt = nullptr;
lv_obj_t *s_lbl_vol = nullptr;
lv_obj_t *s_lbl_remote_station = nullptr;
lv_obj_t *s_lbl_ip = nullptr;
lv_obj_t *s_lbl_server = nullptr;
lv_obj_t *s_lbl_detail = nullptr;
lv_obj_t *s_lbl_form_status = nullptr;
lv_obj_t *s_dd_wifi = nullptr;
lv_obj_t *s_ta_wifi_ssid = nullptr;
lv_obj_t *s_ta_wifi_pass = nullptr;
lv_obj_t *s_ta_callsign = nullptr;
lv_obj_t *s_ta_callsign_ssid = nullptr;
lv_obj_t *s_ta_server_host = nullptr;
lv_obj_t *s_ta_server_port = nullptr;
lv_obj_t *s_slider_speaker = nullptr;
lv_obj_t *s_slider_mic = nullptr;
lv_obj_t *s_lbl_speaker_value = nullptr;
lv_obj_t *s_lbl_mic_value = nullptr;
lv_obj_t *s_sw_aec = nullptr;
lv_obj_t *s_sw_noise = nullptr;
lv_obj_t *s_sw_mic_hpf = nullptr;
lv_obj_t *s_sw_bt = nullptr;
lv_obj_t *s_lbl_bt_status = nullptr;
lv_obj_t *s_keyboard = nullptr;

char s_shown_caption[24] = {};
char s_shown_callsign[16] = {};
char s_shown_ssid[16] = {};
char s_shown_time[16] = {};
char s_shown_local_station[24] = {};
char s_shown_wifi[32] = {};
char s_shown_vol[16] = {};
char s_shown_remote_station[24] = {};
char s_shown_ip[96] = {};
char s_shown_server[96] = {};
char s_shown_detail[512] = {};
char s_shown_bt_top[24] = {};
char s_wifi_option_ssids[kWifiOptionCount][33] = {};

// Cached text colours / styles. In LVGL FULL render mode any invalidation
// re-renders the whole 800x480 framebuffer (~80-106 ms), so re-applying a colour
// or style every refresh tick -- even when unchanged -- forces a costly full
// redraw and makes the UI feel laggy. These caches let the refresh helpers skip
// the setter (and the invalidation) when the value hasn't changed. Sentinel
// 0xFFFFFFFF / -1 means "unknown" and is reset on every page rebuild.
constexpr uint32_t kColorUnset = 0xFFFFFFFFu;
uint32_t s_clr_callsign = kColorUnset;
uint32_t s_clr_server = kColorUnset;
uint32_t s_clr_remote_station = kColorUnset;
uint32_t s_clr_bt_top = kColorUnset;
int s_ls_callsign = -1;  // callsign letter-spacing

uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool initPanel()
{
    esp_lcd_rgb_panel_config_t panel_cfg = {};
    panel_cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    panel_cfg.timings.pclk_hz = 26 * 1000 * 1000;
    panel_cfg.timings.h_res = kWidth;
    panel_cfg.timings.v_res = kHeight;
    panel_cfg.timings.hsync_pulse_width = 1;
    panel_cfg.timings.hsync_back_porch = 40;
    panel_cfg.timings.hsync_front_porch = 20;
    panel_cfg.timings.vsync_pulse_width = 1;
    panel_cfg.timings.vsync_back_porch = 10;
    panel_cfg.timings.vsync_front_porch = 5;
    panel_cfg.timings.flags.pclk_active_neg = true;
    panel_cfg.data_width = 16;
    panel_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    panel_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
    // Two full framebuffers in PSRAM, double-buffered with LVGL FULL-refresh (see
    // initLvgl). The GDMA always streams one complete framebuffer straight from
    // PSRAM while LVGL renders the whole next frame into the other, so nothing
    // writes the buffer being scanned and there is NO bounce buffer / CPU refill
    // ISR for a codec I2C write to starve. This is the example's multi-framebuffer
    // technique and is what finally stops the volume-change drift.
    panel_cfg.num_fbs = 2;
    panel_cfg.dma_burst_size = 128;
    panel_cfg.hsync_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_HSYNC);
    panel_cfg.vsync_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_VSYNC);
    panel_cfg.de_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_DE);
    panel_cfg.pclk_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_PCLK);
    panel_cfg.disp_gpio_num = GPIO_NUM_NC;
    panel_cfg.data_gpio_nums[0] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA0);
    panel_cfg.data_gpio_nums[1] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA1);
    panel_cfg.data_gpio_nums[2] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA2);
    panel_cfg.data_gpio_nums[3] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA3);
    panel_cfg.data_gpio_nums[4] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA4);
    panel_cfg.data_gpio_nums[5] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA5);
    panel_cfg.data_gpio_nums[6] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA6);
    panel_cfg.data_gpio_nums[7] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA7);
    panel_cfg.data_gpio_nums[8] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA8);
    panel_cfg.data_gpio_nums[9] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA9);
    panel_cfg.data_gpio_nums[10] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA10);
    panel_cfg.data_gpio_nums[11] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA11);
    panel_cfg.data_gpio_nums[12] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA12);
    panel_cfg.data_gpio_nums[13] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA13);
    panel_cfg.data_gpio_nums[14] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA14);
    panel_cfg.data_gpio_nums[15] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA15);
    panel_cfg.flags.fb_in_psram = true;

    if (esp_lcd_new_rgb_panel(&panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel create failed");
        return false;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel init failed");
        return false;
    }
    esp_lcd_panel_invert_color(s_panel, false);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return true;
}

uint32_t lvglTick()
{
    return millis();
}

void lvglFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    esp_lcd_panel_handle_t panel =
        static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));
    // DIRECT render mode: LVGL renders only the changed areas, but straight into
    // one of the panel's two PSRAM framebuffers (px_map is that whole buffer).
    // Present it once per frame -- on the last flush -- by handing the framebuffer
    // back to the RGB panel, which flips to it at VSYNC (no copy, since px_map is a
    // known framebuffer). LVGL keeps both buffers in sync by also re-rendering the
    // previous frame's dirty areas. Rendering touches only the back buffer, so the
    // partial-write-into-the-live-framebuffer drift FULL mode avoided stays avoided.
    if (lv_display_flush_is_last(disp)) {
        esp_lcd_panel_draw_bitmap(panel, 0, 0, kWidth, kHeight, px_map);
    }
    lv_display_flush_ready(disp);
}

bool initLvgl()
{
    lv_init();

    // Use the RGB panel's own two framebuffers and let LVGL double-buffer in
    // DIRECT mode: LVGL renders only the changed areas into the off-screen
    // framebuffer, the flush hands that whole buffer to the panel (which swaps it
    // in at VSYNC), and the GDMA scans it straight from PSRAM. No bounce buffer and
    // no partial writes into the live framebuffer, so a codec I2C write can no
    // longer starve a refill and drift the image. DIRECT (vs FULL) avoids
    // re-rendering the entire 800x480 screen on every small change (e.g. the clock
    // tick), which in FULL mode cost ~80-106 ms per refresh and made the UI laggy.
    void *fb0 = nullptr;
    void *fb1 = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1) != ESP_OK) {
        ESP_LOGE(TAG, "get frame buffers failed");
        return false;
    }

    s_disp = lv_display_create(kWidth, kHeight);
    if (s_disp == nullptr) {
        ESP_LOGE(TAG, "display create failed");
        return false;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_flush_cb(s_disp, lvglFlush);
    const size_t fb_bytes = static_cast<size_t>(kWidth) * kHeight * 2u;
    lv_display_set_buffers(s_disp, fb0, fb1, fb_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_tick_set_cb(lvglTick);
    return true;
}

void touchRead(lv_indev_t *, lv_indev_data_t *data)
{
    if (s_touch == nullptr || data == nullptr) {
        return;
    }
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t strength = 0;
    uint8_t count = 0;
    esp_lcd_touch_read_data(s_touch);
    const bool pressed = esp_lcd_touch_get_coordinates(s_touch, &x, &y, &strength, &count, 1);
    if (pressed && count > 0) {
        data->point.x = static_cast<int16_t>(x);
        data->point.y = static_cast<int16_t>(y);
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bool initTouch()
{
    i2c_master_bus_handle_t bus = nullptr;
    if (!S31_I2C_GetBus(&bus)) {
        ESP_LOGW(TAG, "touch I2C unavailable");
        return false;
    }

    esp_lcd_touch_config_t touch_cfg = {};
    touch_cfg.x_max = kWidth;
    touch_cfg.y_max = kHeight;
    touch_cfg.rst_gpio_num = GPIO_NUM_NC;
    touch_cfg.int_gpio_num = GPIO_NUM_NC;

    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT1151_ADDRESS;
    io_cfg.scl_speed_hz = 400000;
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset = 0;
    io_cfg.lcd_cmd_bits = 16;
    io_cfg.lcd_param_bits = 0;
    io_cfg.flags.disable_control_phase = 1;
    io_cfg.transaction_timeout_ms = 100;

    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &s_touch_io) != ESP_OK) {
        ESP_LOGW(TAG, "touch IO create failed");
        return false;
    }
    if (esp_lcd_touch_new_i2c_gt1151(s_touch_io, &touch_cfg, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "GT1151 create failed");
        return false;
    }

    s_touch_indev = lv_indev_create();
    if (s_touch_indev == nullptr) {
        return false;
    }
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_touch_indev, s_disp);
    lv_indev_set_read_cb(s_touch_indev, touchRead);
    lv_indev_set_long_press_repeat_time(s_touch_indev, kVolumeLongPressRepeatMs);
    ESP_LOGI(TAG, "GT1151 touch ready");
    return true;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
    return obj;
}

lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kColorPanel), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_pad_all(obj, 14, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

void action(lv_event_t *event);
void volumeEvent(lv_event_t *event);
void softPttEvent(lv_event_t *event);
void textAreaEvent(lv_event_t *event);
void wifiOptionEvent(lv_event_t *event);
void audioSliderEvent(lv_event_t *event);
void audioSwitchEvent(lv_event_t *event);
void updateAudioValueLabels();
void refresh();

lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, Action id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D4E63), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    void *user_data = reinterpret_cast<void *>(static_cast<intptr_t>(id));
    if (id == Action::VolumeDown || id == Action::VolumeUp) {
        lv_obj_add_event_cb(btn, volumeEvent, LV_EVENT_PRESSED, user_data);
        lv_obj_add_event_cb(btn, volumeEvent, LV_EVENT_LONG_PRESSED_REPEAT, user_data);
    } else {
        lv_obj_add_event_cb(btn, action, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *txt = label(btn, &lv_font_montserrat_20, kColorText);
    lv_obj_center(txt);
    lv_label_set_text(txt, text);
    return btn;
}

lv_obj_t *textArea(lv_obj_t *parent,
                   int x,
                   int y,
                   int w,
                   const char *placeholder,
                   const char *text,
                   uint32_t max_len,
                   bool password,
                   const char *accepted_chars,
                   bool number_keyboard)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, 46);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kColorText), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, text ? text : "");
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_password_mode(ta, password);
    if (accepted_chars != nullptr) {
        lv_textarea_set_accepted_chars(ta, accepted_chars);
    }
    lv_obj_add_event_cb(ta, textAreaEvent, LV_EVENT_ALL,
                        reinterpret_cast<void *>(static_cast<intptr_t>(number_keyboard ? 1 : 0)));
    return ta;
}

void fieldLabel(lv_obj_t *parent, int x, int y, const char *text)
{
    lv_obj_t *obj = label(parent, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(obj, x, y);
    lv_label_set_text(obj, text);
}

lv_obj_t *valueLabel(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *obj = label(parent, &lv_font_montserrat_16, kColorAccent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(obj, "--");
    return obj;
}

lv_obj_t *slider(lv_obj_t *parent, int x, int y, int w, int min, int max, int value, AudioControl id)
{
    lv_obj_t *obj = lv_slider_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, 16);
    lv_slider_set_range(obj, min, max);
    lv_slider_set_value(obj, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kColorDim), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kColorText), LV_PART_KNOB);
    lv_obj_add_event_cb(obj, audioSliderEvent, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(id)));
    return obj;
}

lv_obj_t *switchControl(lv_obj_t *parent, int x, int y, const char *text, bool checked, AudioControl id)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_size(sw, 64, 34);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, audioSwitchEvent, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(id)));

    lv_obj_t *txt = label(parent, &lv_font_montserrat_20, kColorText);
    lv_obj_set_pos(txt, x + 78, y + 6);
    lv_label_set_text(txt, text);
    return sw;
}

void createKeyboard(lv_obj_t *scr)
{
    s_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(s_keyboard, kWidth, 184);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

bool setLabel(lv_obj_t *obj, char *cache, size_t cache_size, const char *text)
{
    if (obj == nullptr || strncmp(cache, text, cache_size) == 0) {
        return false;
    }
    strncpy(cache, text, cache_size - 1u);
    cache[cache_size - 1u] = '\0';
    lv_label_set_text(obj, text);
    return true;
}

// Apply a text colour only when it differs from the cached value, to avoid the
// invalidation (and full-screen re-render) that lv_obj_set_style_text_color
// triggers every time it is called.
void setLabelColor(lv_obj_t *obj, uint32_t &cache, uint32_t color)
{
    if (obj == nullptr || cache == color) {
        return;
    }
    cache = color;
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

void formatStationBadge(char *out, size_t out_size, const char *call, unsigned ssid)
{
    if (out == nullptr || out_size == 0u) {
        return;
    }
    const size_t field_chars = (out_size - 1u < kStationFieldChars) ? out_size - 1u : kStationFieldChars;
    if (call == nullptr || call[0] == '\0') {
        snprintf(out, out_size, "%s", kCallsignPlaceholder);
        out[field_chars] = '\0';
        return;
    }

    char raw[24] = {};
    snprintf(raw, sizeof(raw), "%s-%u", call, ssid);
    size_t i = 0u;
    for (; i < field_chars && raw[i] != '\0'; ++i) {
        out[i] = raw[i];
    }
    for (; i < field_chars; ++i) {
        out[i] = ' ';
    }
    out[field_chars] = '\0';
}

void clearScreen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    memset(s_shown_caption, 0, sizeof(s_shown_caption));
    memset(s_shown_callsign, 0, sizeof(s_shown_callsign));
    memset(s_shown_ssid, 0, sizeof(s_shown_ssid));
    memset(s_shown_time, 0, sizeof(s_shown_time));
    memset(s_shown_local_station, 0, sizeof(s_shown_local_station));
    memset(s_shown_wifi, 0, sizeof(s_shown_wifi));
    memset(s_shown_vol, 0, sizeof(s_shown_vol));
    memset(s_shown_remote_station, 0, sizeof(s_shown_remote_station));
    memset(s_shown_ip, 0, sizeof(s_shown_ip));
    memset(s_shown_server, 0, sizeof(s_shown_server));
    memset(s_shown_detail, 0, sizeof(s_shown_detail));
    memset(s_shown_bt_top, 0, sizeof(s_shown_bt_top));
    // Labels are recreated on rebuild, so drop the colour/style caches too.
    s_clr_callsign = kColorUnset;
    s_clr_server = kColorUnset;
    s_clr_remote_station = kColorUnset;
    s_clr_bt_top = kColorUnset;
    s_ls_callsign = -1;
    s_lbl_caption = nullptr;
    s_lbl_callsign = nullptr;
    s_lbl_ssid = nullptr;
    s_lbl_time = nullptr;
    s_lbl_local_station = nullptr;
    s_lbl_wifi = nullptr;
    s_lbl_bt_top = nullptr;
    s_list_bt = nullptr;
    s_lbl_vol = nullptr;
    s_lbl_remote_station = nullptr;
    s_lbl_ip = nullptr;
    s_lbl_server = nullptr;
    s_lbl_detail = nullptr;
    s_lbl_form_status = nullptr;
    s_dd_wifi = nullptr;
    for (size_t i = 0; i < kWifiOptionCount; ++i) {
        s_wifi_option_ssids[i][0] = '\0';
    }
    s_ta_wifi_ssid = nullptr;
    s_ta_wifi_pass = nullptr;
    s_ta_callsign = nullptr;
    s_ta_callsign_ssid = nullptr;
    s_ta_server_host = nullptr;
    s_ta_server_port = nullptr;
    s_slider_speaker = nullptr;
    s_slider_mic = nullptr;
    s_lbl_speaker_value = nullptr;
    s_lbl_mic_value = nullptr;
    s_sw_aec = nullptr;
    s_sw_noise = nullptr;
    s_sw_mic_hpf = nullptr;
    s_sw_bt = nullptr;
    s_lbl_bt_status = nullptr;
    s_keyboard = nullptr;
}

void topBar(lv_obj_t *scr)
{
    constexpr int kTopLeft = 20;
    constexpr int kTopGap = 22;
    constexpr int kTopTimeW = 128;
    constexpr int kTopStationW = 150;
    constexpr int kTopVolW = 94;
    constexpr int kTopWifiW = 104;
    constexpr int kTopBtW = 52;
    // Order, left to right: local callsign-SSID, time, incoming caller, volume,
    // WiFi signal, Bluetooth.
    constexpr int kTopLocalX = kTopLeft;
    constexpr int kTopTimeX = kTopLocalX + kTopStationW + kTopGap;
    constexpr int kTopRemoteX = kTopTimeX + kTopTimeW + kTopGap;
    constexpr int kTopVolX = kTopRemoteX + kTopStationW + kTopGap;
    constexpr int kTopWifiX = kTopVolX + kTopVolW + kTopGap;
    constexpr int kTopBtX = kTopWifiX + kTopWifiW + 8;

    lv_obj_t *bar = panel(scr, 0, 0, kWidth, 56);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A111B), 0);

    s_lbl_time = label(bar, &lv_font_montserrat_20, kColorTime);
    lv_obj_set_width(s_lbl_time, kTopTimeW);
    lv_obj_align(s_lbl_time, LV_ALIGN_LEFT_MID, kTopTimeX, 0);
    lv_obj_set_style_text_align(s_lbl_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_time, "--:--:--");

    s_lbl_local_station = label(bar, &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_lbl_local_station, kTopStationW);
    lv_obj_align(s_lbl_local_station, LV_ALIGN_LEFT_MID, kTopLocalX, 0);
    lv_obj_set_style_text_align(s_lbl_local_station, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_local_station, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_local_station, kCallsignPlaceholder);

    s_lbl_vol = label(bar, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_width(s_lbl_vol, kTopVolW);
    lv_obj_align(s_lbl_vol, LV_ALIGN_LEFT_MID, kTopVolX, 0);
    lv_obj_set_style_text_align(s_lbl_vol, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_vol, LV_SYMBOL_VOLUME_MID " --");

    s_lbl_remote_station = label(bar, &lv_font_montserrat_20, kColorDim);
    lv_obj_set_width(s_lbl_remote_station, kTopStationW);
    lv_obj_align(s_lbl_remote_station, LV_ALIGN_LEFT_MID, kTopRemoteX, 0);
    lv_obj_set_style_text_align(s_lbl_remote_station, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_remote_station, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_remote_station, kCallsignPlaceholder);

    s_lbl_wifi = label(bar, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_wifi, kTopWifiW);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_LEFT_MID, kTopWifiX, 0);
    lv_obj_set_style_text_align(s_lbl_wifi, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI " --");

    // Bluetooth state lives next to WiFi: hidden when off, grey when on but
    // unlinked, green + headset glyph once a headset is linked. Updated in
    // refresh() (starts empty -- Bluetooth defaults off).
    s_lbl_bt_top = label(bar, &lv_font_montserrat_16, kColorDim);
    lv_obj_set_width(s_lbl_bt_top, kTopBtW);
    lv_obj_align(s_lbl_bt_top, LV_ALIGN_LEFT_MID, kTopBtX, 0);
    lv_obj_set_style_text_align(s_lbl_bt_top, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_bt_top, "");
}

void buildHome()
{
    clearScreen();
    s_page = Page::Home;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    // Left: callsign / status, with the network IP underneath.
    lv_obj_t *left = panel(scr, 22, 78, 458, 270);
    s_lbl_caption = label(left, &lv_font_montserrat_20, kColorDim);
    lv_obj_align(s_lbl_caption, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_lbl_caption, "STANDBY");

    // Incoming caller's callsign-SSID at the largest crisp built-in font (48px),
    // centred in the freed space so it reads as the focal element.
    s_lbl_callsign = label(left, &lv_font_montserrat_48, kColorText);
    lv_obj_set_width(s_lbl_callsign, 430);
    lv_obj_align(s_lbl_callsign, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(s_lbl_callsign, kCallsignPlaceholder);

    // Bottom row of the main panel: local IP on the left, server IP on the right.
    s_lbl_ip = label(left, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_width(s_lbl_ip, 215);
    lv_obj_align(s_lbl_ip, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_ip, LV_SYMBOL_WIFI " ---");

    s_lbl_server = label(left, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_width(s_lbl_server, 215);
    lv_obj_align(s_lbl_server, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_text_align(s_lbl_server, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_lbl_server, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_server, "---");

    // Right: the whole panel is one big hold-to-talk PTT button.
    lv_obj_t *ptt = lv_button_create(scr);
    lv_obj_set_pos(ptt, 502, 78);
    lv_obj_set_size(ptt, 276, 270);
    lv_obj_set_style_radius(ptt, 12, 0);
    lv_obj_set_style_bg_color(ptt, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_bg_color(ptt, lv_color_hex(0x7A1F1F), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(ptt, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(ptt, 1, 0);
    lv_obj_add_event_cb(ptt, softPttEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(ptt, softPttEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(ptt, softPttEvent, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_t *ptt_lbl = label(ptt, &lv_font_montserrat_48, kColorText);
    lv_obj_center(ptt_lbl);
    lv_label_set_text(ptt_lbl, "PTT");

    button(scr, 22, 372, 180, 78, "VOL-", Action::VolumeDown);
    button(scr, 222, 372, 356, 78, "CONFIG", Action::Config);
    button(scr, 598, 372, 180, 78, "VOL+", Action::VolumeUp);
}

void buildConfig()
{
    clearScreen();
    s_page = Page::Config;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    button(scr, 24, 84, 172, 132, "WiFi", Action::Wifi);
    button(scr, 214, 84, 172, 132, "Station", Action::Station);
    button(scr, 404, 84, 172, 132, "Audio", Action::Audio);
    button(scr, 594, 84, 172, 132, "Bluetooth", Action::Bt);
    button(scr, 24, 372, 230, 76, "Back", Action::Home);

    lv_obj_t *info = label(scr, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_width(info, 720);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 34, 250);
    lv_label_set_text(info, "Use the touch panels for quick changes. Full text editing remains available through the WiFi/BLE config portal.");
}

void sanitizeOptionText(const char *src, char *dst, size_t dst_size)
{
    if (dst == nullptr || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == nullptr) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size; ++i) {
        const char ch = src[i];
        dst[out++] = (ch == '\r' || ch == '\n') ? ' ' : ch;
    }
    dst[out] = '\0';
}

void setWifiDropdownOptionsFromCache()
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    NrlWifiScanResult results[16] = {};
    const size_t got = nrlWifiScanGetCache(results, sizeof(results) / sizeof(results[0]));

    for (size_t i = 0; i < kWifiOptionCount; ++i) {
        s_wifi_option_ssids[i][0] = '\0';
    }

    size_t used = 0;
    if (cfg != nullptr && cfg->wifi_ssid[0] != '\0') {
        sanitizeOptionText(cfg->wifi_ssid, s_wifi_option_ssids[used], sizeof(s_wifi_option_ssids[used]));
        ++used;
    }

    for (size_t i = 0; i < got && used < kWifiOptionCount; ++i) {
        if (results[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < used; ++j) {
            if (strncmp(results[i].ssid, s_wifi_option_ssids[j], sizeof(results[i].ssid)) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        sanitizeOptionText(results[i].ssid, s_wifi_option_ssids[used], sizeof(s_wifi_option_ssids[used]));
        ++used;
    }

    if (s_dd_wifi != nullptr) {
        // Build a newline-separated option list for the dropdown (scrollable, so
        // it can show every scanned network, not just the first few).
        char options[kWifiOptionCount * 34] = {};
        size_t pos = 0;
        for (size_t i = 0; i < used; ++i) {
            if (i > 0 && pos < sizeof(options) - 1) {
                options[pos++] = '\n';
            }
            const int n = snprintf(options + pos, sizeof(options) - pos, "%s",
                                   s_wifi_option_ssids[i]);
            if (n > 0) {
                pos += static_cast<size_t>(n);
            }
        }
        if (used == 0) {
            snprintf(options, sizeof(options), "%s", "(no networks)");
        }
        lv_dropdown_set_options(s_dd_wifi, options);
    }
    ESP_LOGI(TAG, "WiFi dropdown updated: scanned=%u shown=%u",
             static_cast<unsigned>(got), static_cast<unsigned>(used));
}

void buildWifi()
{
    clearScreen();
    s_page = Page::Wifi;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();

    fieldLabel(box, 0, 0, "Nearby WiFi");
    // Scrollable dropdown lists every scanned network; picking one fills the SSID
    // field below.
    s_dd_wifi = lv_dropdown_create(box);
    lv_obj_set_pos(s_dd_wifi, 0, 24);
    lv_obj_set_size(s_dd_wifi, 710, 42);
    lv_obj_set_style_radius(s_dd_wifi, 6, 0);
    lv_obj_set_style_bg_color(s_dd_wifi, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_border_color(s_dd_wifi, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(s_dd_wifi, 1, 0);
    lv_obj_set_style_text_color(s_dd_wifi, lv_color_hex(kColorText), 0);
    lv_dropdown_set_text(s_dd_wifi, "Select WiFi...");
    lv_dropdown_set_options(s_dd_wifi, "(scanning)");
    lv_obj_add_event_cb(s_dd_wifi, wifiOptionEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    // Style the popped-up option list so it is readable on the dark theme.
    lv_obj_t *dd_list = lv_dropdown_get_list(s_dd_wifi);
    if (dd_list != nullptr) {
        lv_obj_set_style_bg_color(dd_list, lv_color_hex(kColorPanel), 0);
        lv_obj_set_style_text_color(dd_list, lv_color_hex(kColorText), 0);
        lv_obj_set_style_border_color(dd_list, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_max_height(dd_list, 220, 0);
    }
    setWifiDropdownOptionsFromCache();

    fieldLabel(box, 0, 82, "WiFi SSID");
    s_ta_wifi_ssid = textArea(box, 0, 106, 340, "SSID", cfg ? cfg->wifi_ssid : "", 32, false, nullptr, false);
    fieldLabel(box, 370, 82, "Password");
    s_ta_wifi_pass = textArea(box, 370, 106, 340, "Password", cfg ? cfg->wifi_password : "", 64, true, nullptr, false);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 174);
    lv_label_set_text(s_lbl_form_status, "Scan, select or type SSID, then save.");

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 278, 372, 146, 76, "Scan", Action::ScanWifi);
    button(scr, 442, 372, 146, 76, "Save", Action::SaveWifi);
    button(scr, 606, 372, 168, 76, "Reset", Action::ResetWifi);
    createKeyboard(scr);
}

void buildStation()
{
    clearScreen();
    s_page = Page::Station;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();

    fieldLabel(box, 0, 0, "Callsign");
    s_ta_callsign = textArea(box, 0, 24, 250, "CALL", cfg ? cfg->callsign : "", 6, false,
                             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", false);
    fieldLabel(box, 280, 0, "SSID");
    char ssid[8] = {};
    snprintf(ssid, sizeof(ssid), "%u", cfg ? static_cast<unsigned>(cfg->callsign_ssid) : 0);
    s_ta_callsign_ssid = textArea(box, 280, 24, 110, "0", ssid, 3, false, "0123456789", true);

    fieldLabel(box, 0, 82, "Server Host / IP");
    s_ta_server_host = textArea(box, 0, 106, 430, "server host", cfg ? cfg->server_host : "", 64, false,
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-_", false);
    fieldLabel(box, 460, 82, "Port");
    char port[8] = {};
    snprintf(port, sizeof(port), "%u", cfg ? static_cast<unsigned>(cfg->server_port) : 0);
    s_ta_server_port = textArea(box, 460, 106, 120, "0", port, 5, false, "0123456789", true);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 176);
    lv_label_set_text(s_lbl_form_status, "Edit station and server settings, then save.");

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveStation);
    createKeyboard(scr);
}

void buildAudio()
{
    clearScreen();
    s_page = Page::Audio;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    // When a headset is online the Speaker slider tracks/controls its volume;
    // otherwise it controls the onboard codec line-out.
    const int bt_pct = NRL_BtHfp_IsConnected() ? NRL_BtHfp_GetVolumePercent() : -1;
    const int speaker_pct = (bt_pct >= 0)
                                ? bt_pct
                                : (cfg ? (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255 : 0);

    fieldLabel(box, 0, 0, (bt_pct >= 0) ? "Speaker (headset)" : "Speaker");
    s_lbl_speaker_value = valueLabel(box, 620, 0, 90);
    s_slider_speaker = slider(box, 0, 36, 710, 0, 100, speaker_pct, AudioControl::Speaker);

    fieldLabel(box, 0, 70, "Mic");
    s_lbl_mic_value = valueLabel(box, 620, 70, 90);
    s_slider_mic = slider(box, 0, 106, 710, 0, 255, cfg ? cfg->mic_volume : 0, AudioControl::Mic);

    s_sw_aec = switchControl(box, 0, 152, "AEC", cfg && cfg->aec_enabled, AudioControl::Aec);
    s_sw_noise = switchControl(box, 228, 152, "AI Noise", cfg && cfg->ai_noise_enabled, AudioControl::Noise);
    s_sw_mic_hpf = switchControl(box, 500, 152, "Mic HPF", cfg && cfg->mic_hpf_enabled, AudioControl::MicHpf);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 210);
    lv_label_set_text(s_lbl_form_status, "Adjust audio settings, then save.");
    updateAudioValueLabels();

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 284, 372, 230, 76, "Reset", Action::ResetAudio);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveAudio);
}

void updateAudioValueLabels()
{
    if (s_slider_speaker != nullptr && s_lbl_speaker_value != nullptr) {
        char text[16] = {};
        snprintf(text, sizeof(text), "%ld%%", static_cast<long>(lv_slider_get_value(s_slider_speaker)));
        lv_label_set_text(s_lbl_speaker_value, text);
    }
    if (s_slider_mic != nullptr && s_lbl_mic_value != nullptr) {
        char text[16] = {};
        snprintf(text, sizeof(text), "%ld", static_cast<long>(lv_slider_get_value(s_slider_mic)));
        lv_label_set_text(s_lbl_mic_value, text);
    }
}

void markAudioChanged()
{
    updateAudioValueLabels();
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, "Changed. Tap Save to persist.");
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorWarn), 0);
    }
    s_last_refresh_ms = 0;
}

void setVolumePercentDelta(int pct_delta)
{
    // While a Bluetooth headset is connected, the volume keys drive the headset's
    // own speaker volume (one HFP step per press), not the onboard codec. The
    // top-bar readout (refreshVolume) follows the headset value.
    if (NRL_BtHfp_IsConnected()) {
        NRL_BtHfp_AdjustVolume(pct_delta);
        s_last_refresh_ms = 0;
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
    if (volume != static_cast<int>(cfg->line_out_volume)) {
        EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), false);
        s_volume_dirty = true;
        s_volume_change_ms = millis();
        s_last_refresh_ms = 0;
    }
}

void setSsidDelta(int delta)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) {
        return;
    }
    int ssid = static_cast<int>(cfg->callsign_ssid) + delta;
    if (ssid < 0) {
        ssid = 0;
    } else if (ssid > 255) {
        ssid = 255;
    }
    if (ssid != static_cast<int>(cfg->callsign_ssid)) {
        EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(ssid), true);
        if (s_ta_callsign_ssid != nullptr) {
            char text[8] = {};
            snprintf(text, sizeof(text), "%d", ssid);
            lv_textarea_set_text(s_ta_callsign_ssid, text);
        }
    }
}

void wifiScanTask(void *)
{
    const bool ok = nrlWifiScanStartBlocking(6000u);
    s_wifi_scan_ok = ok;
    s_wifi_scan_complete = true;
    s_wifi_scan_running = false;
    s_wifi_scan_task = nullptr;
    vTaskDelete(nullptr);
}

void scanWifiForDropdown()
{
    if (s_wifi_scan_running) {
        if (s_lbl_form_status != nullptr) {
            lv_label_set_text(s_lbl_form_status, "Scanning WiFi...");
            lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorWarn), 0);
        }
        return;
    }

    s_wifi_scan_complete = false;
    s_wifi_scan_ok = false;
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, "Scanning WiFi...");
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorWarn), 0);
    }
    lv_timer_handler();

    s_wifi_scan_running = true;
    const BaseType_t created = xTaskCreatePinnedToCore(wifiScanTask, "wifi_scan_ui", 8192, nullptr, 4,
                                                       &s_wifi_scan_task, 0);
    if (created != pdPASS) {
        s_wifi_scan_task = nullptr;
        s_wifi_scan_running = false;
        s_wifi_scan_complete = false;
        if (s_lbl_form_status != nullptr) {
            lv_label_set_text(s_lbl_form_status, "Scan failed: task create failed.");
            lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorBad), 0);
        }
    }
}

void pollWifiScan()
{
    if (!s_wifi_scan_complete) {
        return;
    }
    s_wifi_scan_complete = false;

    setWifiDropdownOptionsFromCache();
    if (s_lbl_form_status != nullptr) {
        if (s_wifi_scan_ok) {
            NrlWifiScanResult results[16] = {};
            const size_t got = nrlWifiScanGetCache(results, sizeof(results) / sizeof(results[0]));
            if (got > 0u && s_ta_wifi_ssid != nullptr && s_wifi_option_ssids[0][0] != '\0') {
                lv_textarea_set_text(s_ta_wifi_ssid, s_wifi_option_ssids[0]);
            }
            char text[48] = {};
            snprintf(text, sizeof(text), "Found %u WiFi networks.", static_cast<unsigned>(got));
            lv_label_set_text(s_lbl_form_status, text);
            lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorGood), 0);
        } else {
            lv_label_set_text(s_lbl_form_status, "Scan failed.");
            lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorBad), 0);
        }
    }
    s_last_refresh_ms = 0;
}

void saveWifiForm()
{
    const char *ssid = (s_ta_wifi_ssid != nullptr) ? lv_textarea_get_text(s_ta_wifi_ssid) : "";
    const char *pass = (s_ta_wifi_pass != nullptr) ? lv_textarea_get_text(s_ta_wifi_pass) : "";
    bool ok = EXTERNAL_RADIO_SetWifiSsid(ssid, false) &&
              EXTERNAL_RADIO_SetWifiPassword(pass, false) &&
              EXTERNAL_RADIO_SaveConfig();
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, ok ? "WiFi config saved." : "Save failed: SSID is required.");
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(ok ? kColorGood : kColorBad), 0);
    }
}

void saveStationForm()
{
    const char *call = (s_ta_callsign != nullptr) ? lv_textarea_get_text(s_ta_callsign) : "";
    const char *ssid_text = (s_ta_callsign_ssid != nullptr) ? lv_textarea_get_text(s_ta_callsign_ssid) : "";
    const char *host = (s_ta_server_host != nullptr) ? lv_textarea_get_text(s_ta_server_host) : "";
    const char *port_text = (s_ta_server_port != nullptr) ? lv_textarea_get_text(s_ta_server_port) : "";
    char *end = nullptr;
    const unsigned long ssid = strtoul(ssid_text, &end, 10);
    const bool ssid_ok = ssid_text[0] != '\0' && end != ssid_text && *end == '\0' && ssid <= 255ul;
    end = nullptr;
    const unsigned long port = strtoul(port_text, &end, 10);
    const bool port_ok = port_text[0] != '\0' && end != port_text && *end == '\0' &&
                         port > 0ul && port <= 65535ul;
    bool ok = ssid_ok && port_ok &&
              EXTERNAL_RADIO_SetCallsign(call, false) &&
              EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(ssid), false) &&
              EXTERNAL_RADIO_SetServerHost(host, false) &&
              EXTERNAL_RADIO_SetServerPort(static_cast<uint16_t>(port), false) &&
              EXTERNAL_RADIO_SaveConfig();
    if (s_lbl_form_status != nullptr) {
        const char *msg = ok ? "Station config saved."
                             : (!ssid_ok ? "Save failed: SSID must be 0-255."
                                         : (!port_ok ? "Save failed: port must be 1-65535."
                                                     : "Save failed: check callsign and server."));
        lv_label_set_text(s_lbl_form_status, msg);
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(ok ? kColorGood : kColorBad), 0);
    }
    s_last_refresh_ms = 0;
}

void saveAudioForm()
{
    const bool ok = EXTERNAL_RADIO_SaveConfig();
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, ok ? "Audio config saved." : "Save failed.");
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(ok ? kColorGood : kColorBad), 0);
    }
}

void resetAudioForm()
{
    const bool ok = EXTERNAL_RADIO_ResetAudioConfig(true);
    if (ok) {
        buildAudio();
    } else if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, "Reset failed.");
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorBad), 0);
    }
}

// ---- Bluetooth headset page ------------------------------------------------

void btEnableEvent(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    const bool checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
    // Request the change (non-blocking -- the BT task does the slow stack work)
    // and persist the preference. Give immediate on-screen feedback so the user
    // knows the tap registered while the stack comes up/down in the background.
    EXTERNAL_RADIO_SetBtEnabled(checked, true);
    if (s_lbl_bt_status != nullptr) {
        lv_label_set_text(s_lbl_bt_status, checked ? "Turning Bluetooth on..."
                                                   : "Turning Bluetooth off...");
        lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorWarn), 0);
    }
    s_last_refresh_ms = 0;
}

// Row user-data encodes which list a row belongs to: saved rows are offset by
// kBtSavedTag, scanned rows carry their raw index.
constexpr intptr_t kBtSavedTag = 0x10000;

void btRowConnect(lv_event_t *event)
{
    const intptr_t v = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (v >= kBtSavedTag) {
        NRL_BtHfp_ConnectSaved(static_cast<size_t>(v - kBtSavedTag));
    } else {
        NRL_BtHfp_ConnectIndex(static_cast<size_t>(v));
    }
    if (s_lbl_bt_status != nullptr) {
        lv_label_set_text(s_lbl_bt_status, "Connecting...");
        lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorWarn), 0);
    }
    s_last_refresh_ms = 0;
}

void btRowForget(lv_event_t *event)
{
    const intptr_t v = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (v < kBtSavedTag) {
        return;  // only saved devices can be forgotten
    }
    NRL_BtHfp_RemoveSaved(static_cast<size_t>(v - kBtSavedTag));
    if (s_lbl_bt_status != nullptr) {
        lv_label_set_text(s_lbl_bt_status, "Removed saved headset.");
        lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorSub), 0);
    }
    // Don't rebuild the list from inside this row's own event; let refreshBtPage
    // pick up the count change on the next tick.
    s_last_refresh_ms = 0;
}

lv_obj_t *addBtRow(const char *icon, const char *text, intptr_t tag, bool saved)
{
    lv_obj_t *row = lv_list_add_button(s_list_bt, icon, text);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x274060), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(row, lv_color_hex(kColorText), 0);
    // Short click connects; long-press forgets (saved rows only).
    lv_obj_add_event_cb(row, btRowConnect, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void *>(tag));
    if (saved) {
        lv_obj_add_event_cb(row, btRowForget, LV_EVENT_LONG_PRESSED,
                            reinterpret_cast<void *>(tag));
    }
    return row;
}

// Rebuild the BT list: saved headsets first (tap = connect, long-press =
// forget), then freshly scanned devices not already saved. Tapping connects.
// (Dropdowns proved unreliable on this touch panel.)
void setBtDeviceList()
{
    if (s_list_bt == nullptr) {
        return;
    }
    lv_obj_clean(s_list_bt);
    const size_t saved = NRL_BtHfp_GetSavedCount();
    for (size_t i = 0; i < saved; ++i) {
        char name[32] = {};
        NRL_BtHfp_GetSavedName(i, name, sizeof(name));
        char row_txt[56];
        snprintf(row_txt, sizeof(row_txt), "%s  (saved)", name);
        addBtRow(LV_SYMBOL_BLUETOOTH, row_txt, kBtSavedTag + static_cast<intptr_t>(i), true);
    }
    const size_t n = NRL_BtHfp_GetDeviceCount();
    size_t scanned_shown = 0;
    for (size_t i = 0; i < n; ++i) {
        char name[32] = {};
        NRL_BtHfp_GetDeviceName(i, name, sizeof(name));
        // Skip ones already in the saved section (best-effort, by name).
        bool dup = false;
        for (size_t s = 0; s < saved; ++s) {
            char sname[32] = {};
            NRL_BtHfp_GetSavedName(s, sname, sizeof(sname));
            if (strcmp(sname, name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        addBtRow(LV_SYMBOL_BLUETOOTH, name, static_cast<intptr_t>(i), false);
        ++scanned_shown;
    }
    if (saved == 0u && scanned_shown == 0u) {
        lv_obj_t *row = lv_list_add_button(s_list_bt, nullptr,
                                           NRL_BtHfp_IsScanning() ? "Scanning..."
                                                                  : "No headsets -- tap Scan");
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_text_color(row, lv_color_hex(kColorSub), 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
}

void buildBt()
{
    clearScreen();
    s_page = Page::Bt;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();

    fieldLabel(box, 0, 6, "Bluetooth headset");
    s_sw_bt = lv_switch_create(box);
    lv_obj_set_pos(s_sw_bt, 280, 0);
    lv_obj_set_size(s_sw_bt, 64, 34);
    if (cfg != nullptr && cfg->bt_enabled) {
        lv_obj_add_state(s_sw_bt, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(s_sw_bt, lv_color_hex(kColorDim), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sw_bt, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sw_bt, lv_color_hex(kColorText), LV_PART_KNOB);
    lv_obj_add_event_cb(s_sw_bt, btEnableEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    fieldLabel(box, 0, 58, "Headsets: tap to connect, long-press a saved one to delete");
    s_list_bt = lv_list_create(box);
    lv_obj_set_pos(s_list_bt, 0, 82);
    lv_obj_set_size(s_list_bt, 710, 132);
    lv_obj_set_style_radius(s_list_bt, 6, 0);
    lv_obj_set_style_bg_color(s_list_bt, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_border_color(s_list_bt, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(s_list_bt, 1, 0);
    setBtDeviceList();

    s_lbl_bt_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_bt_status, 710);
    lv_obj_set_pos(s_lbl_bt_status, 0, 222);
    lv_label_set_text(s_lbl_bt_status, "Turn on, Scan, then tap a headset. Saved ones reconnect automatically.");

    button(scr, 24, 372, 360, 76, "Back", Action::Config);
    button(scr, 414, 372, 360, 76, "Scan", Action::ScanBt);
}

void scanBtForDropdown()
{
    if (!NRL_BtHfp_IsEnabled()) {
        if (s_lbl_bt_status != nullptr) {
            lv_label_set_text(s_lbl_bt_status, "Turn Bluetooth on first.");
            lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorWarn), 0);
        }
        return;
    }
    NRL_BtHfp_StartScan();
    if (s_lbl_bt_status != nullptr) {
        lv_label_set_text(s_lbl_bt_status, "Scanning for headsets...");
        lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(kColorWarn), 0);
    }
}

// Top-bar Bluetooth indicator: hidden when Bluetooth is off, grey when enabled
// but no headset is linked, and green with a headset glyph once a headset is
// connected (no checkmark).
void refreshBtTop()
{
    if (s_lbl_bt_top == nullptr) {
        return;
    }
    uint32_t color;
    const char *txt;
    if (!NRL_BtHfp_IsEnabled()) {
        color = kColorDim;
        txt = "";  // Bluetooth off: no icon at all.
    } else if (NRL_BtHfp_IsConnected()) {
        // Linked to a headset: green BT glyph plus a headset glyph.
        color = kColorGood;
        txt = LV_SYMBOL_BLUETOOTH " " LV_SYMBOL_AUDIO;
    } else {
        // Enabled but no headset linked yet: grey BT glyph only.
        color = kColorDim;
        txt = LV_SYMBOL_BLUETOOTH;
    }
    setLabel(s_lbl_bt_top, s_shown_bt_top, sizeof(s_shown_bt_top), txt);
    setLabelColor(s_lbl_bt_top, s_clr_bt_top, color);
}

void refreshBtPage()
{
    // Rebuild the row list when the saved/scanned counts or the scanning state
    // change (not every tick, so taps aren't disrupted mid-press).
    static size_t s_bt_last_key = SIZE_MAX;
    static bool s_bt_was_scanning = false;
    const size_t key = NRL_BtHfp_GetSavedCount() * 1000u + NRL_BtHfp_GetDeviceCount();
    const bool scanning = NRL_BtHfp_IsScanning();
    if (key != s_bt_last_key || scanning != s_bt_was_scanning) {
        s_bt_last_key = key;
        s_bt_was_scanning = scanning;
        setBtDeviceList();
    }
    if (s_lbl_bt_status == nullptr) {
        return;
    }
    char bt[96];
    uint32_t color = kColorSub;
    char peer[32] = {};
    if (NRL_BtHfp_TogglePending()) {
        // Mid-transition: the BT task is bringing the stack up/down.
        snprintf(bt, sizeof(bt), "Switching Bluetooth...");
        color = kColorWarn;
    } else if (!NRL_BtHfp_IsEnabled()) {
        snprintf(bt, sizeof(bt), "Bluetooth off.");
    } else if (NRL_BtHfp_IsAudioActive()) {
        NRL_BtHfp_GetPeerName(peer, sizeof(peer));
        snprintf(bt, sizeof(bt), "Voice on headset%s%s.", peer[0] ? ": " : "", peer);
        color = kColorGood;
    } else if (NRL_BtHfp_IsConnected()) {
        NRL_BtHfp_GetPeerName(peer, sizeof(peer));
        snprintf(bt, sizeof(bt), "Connected%s%s (opening audio)...", peer[0] ? ": " : "", peer);
        color = kColorGood;
    } else if (NRL_BtHfp_IsScanning()) {
        snprintf(bt, sizeof(bt), "Scanning for headsets...");
        color = kColorWarn;
    } else {
        snprintf(bt, sizeof(bt), "Scan, pick a headset, then Connect.");
    }
    lv_label_set_text(s_lbl_bt_status, bt);
    lv_obj_set_style_text_color(s_lbl_bt_status, lv_color_hex(color), 0);
}

void action(lv_event_t *event)
{
    const Action id = static_cast<Action>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    switch (id) {
        case Action::Config: buildConfig(); break;
        case Action::Home: buildHome(); break;
        case Action::Wifi: buildWifi(); break;
        case Action::Station: buildStation(); break;
        case Action::Audio: buildAudio(); break;
        case Action::Bt: buildBt(); break;
        case Action::ScanBt: scanBtForDropdown(); break;
        case Action::VolumeDown: setVolumePercentDelta(-1); break;
        case Action::VolumeUp: setVolumePercentDelta(1); break;
        case Action::CallsignSsidDown: setSsidDelta(-1); break;
        case Action::CallsignSsidUp: setSsidDelta(1); break;
        case Action::ResetWifi: EXTERNAL_RADIO_ResetNetworkConfig(); break;
        case Action::ScanWifi: scanWifiForDropdown(); break;
        case Action::SaveWifi: saveWifiForm(); break;
        case Action::SaveStation: saveStationForm(); break;
        case Action::SaveAudio: saveAudioForm(); break;
        case Action::ResetAudio: resetAudioForm(); break;
    }
    s_last_refresh_ms = 0;
}

void volumeEvent(lv_event_t *event)
{
    const Action id = static_cast<Action>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const int delta = (id == Action::VolumeUp) ? 1 : -1;
    setVolumePercentDelta(delta);
}

void textAreaEvent(lv_event_t *event)
{
    if (s_keyboard == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *ta = lv_event_get_target_obj(event);
    const bool number_keyboard = reinterpret_cast<intptr_t>(lv_event_get_user_data(event)) != 0;
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_mode(s_keyboard, number_keyboard ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(s_keyboard, ta);
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_keyboard_set_textarea(s_keyboard, nullptr);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void wifiOptionEvent(lv_event_t *event)
{
    if (s_ta_wifi_ssid == nullptr) {
        return;
    }
    lv_obj_t *dd = lv_event_get_target_obj(event);
    const uint16_t index = lv_dropdown_get_selected(dd);
    if (index < kWifiOptionCount && s_wifi_option_ssids[index][0] != '\0') {
        lv_textarea_set_text(s_ta_wifi_ssid, s_wifi_option_ssids[index]);
    }
}

void audioSliderEvent(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    const AudioControl id =
        static_cast<AudioControl>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const int value = static_cast<int>(lv_slider_get_value(obj));
    if (id == AudioControl::Speaker) {
        if (NRL_BtHfp_IsConnected()) {
            // Headset online: the Speaker slider drives its volume, saved live by
            // the BT module (no separate Save needed), so don't flag the form.
            NRL_BtHfp_SetVolumePercent(value);
            updateAudioValueLabels();
            return;
        }
        const int volume = (value * 255 + 50) / 100;
        EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), false);
    } else if (id == AudioControl::Mic) {
        EXTERNAL_RADIO_SetMicVolume(static_cast<uint8_t>(value), false);
    }
    markAudioChanged();
}

void audioSwitchEvent(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    const bool checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
    const AudioControl id =
        static_cast<AudioControl>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    switch (id) {
        case AudioControl::Aec:
            EXTERNAL_RADIO_SetAecEnabled(checked, false);
            break;
        case AudioControl::Noise:
            EXTERNAL_RADIO_SetAiNoiseEnabled(checked, false);
            break;
        case AudioControl::MicHpf:
            EXTERNAL_RADIO_SetMicHpfEnabled(checked, false);
            break;
        default:
            break;
    }
    markAudioChanged();
}

// The Home "network" panel doubles as a hold-to-talk soft PTT: pressing anywhere
// in it keys up; releasing (or sliding a finger off) stops transmit.
void softPttEvent(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            STATUS_IO_SetSoftPtt(true);
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            STATUS_IO_SetSoftPtt(false);
            break;
        default:
            return;
    }
    s_last_refresh_ms = 0;  // reflect the TX state on screen immediately
}

void refreshClock()
{
    if (!s_time_sync_started && nrlWifiStaConnected()) {
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "ntp.ntsc.ac.cn");
        esp_sntp_setservername(2, "pool.ntp.org");
        esp_sntp_init();
        s_time_sync_started = true;
    }

    char text[16];
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 >= 2024) {
        snprintf(text, sizeof(text), "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    } else {
        snprintf(text, sizeof(text), "--:--:--");
    }
    setLabel(s_lbl_time, s_shown_time, sizeof(s_shown_time), text);
}

void refreshWifiBadge()
{
    char text[32];
    uint32_t color = kColorSub;
    if (nrlWifiStaConnected()) {
        wifi_ap_record_t ap = {};
        const int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
        snprintf(text, sizeof(text), LV_SYMBOL_WIFI " %ddB", rssi);
        color = (rssi >= -65) ? kColorGood : ((rssi >= -78) ? kColorWarn : kColorBad);
    } else {
        snprintf(text, sizeof(text), LV_SYMBOL_WIFI " AP");
        color = kColorWarn;
    }
    if (setLabel(s_lbl_wifi, s_shown_wifi, sizeof(s_shown_wifi), text)) {
        lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(color), 0);
    }
}

void refreshStationBadges()
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();

    char local[24] = {};
    formatStationBadge(local, sizeof(local), cfg ? cfg->callsign : nullptr,
                       cfg ? static_cast<unsigned>(cfg->callsign_ssid) : 0u);
    setLabel(s_lbl_local_station, s_shown_local_station, sizeof(s_shown_local_station), local);

    char voice_call[8] = {};
    unsigned voice_ssid = 0;
    const bool rx = NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid);
    char remote[24] = {};
    formatStationBadge(remote, sizeof(remote), (rx && voice_call[0] != '\0') ? voice_call : nullptr,
                       voice_ssid);
    setLabel(s_lbl_remote_station, s_shown_remote_station, sizeof(s_shown_remote_station), remote);
    setLabelColor(s_lbl_remote_station, s_clr_remote_station, rx ? kColorAccent : kColorDim);
}

void refreshHome()
{
    char voice_call[8] = {};
    unsigned voice_ssid = 0;
    const bool rx = NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid);
    const bool tx = STATUS_IO_IsSqlActive();

    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    // Main panel shows only the remote caller. Local station lives in the top bar.
    const bool has_caller = (rx && voice_call[0] != '\0');
    char call[24] = {};
    formatStationBadge(call, sizeof(call), has_caller ? voice_call : nullptr, voice_ssid);
    setLabel(s_lbl_callsign, s_shown_callsign, sizeof(s_shown_callsign), call);
    if (s_lbl_callsign != nullptr) {
        // Spread only the placeholder dashes so they read bigger; keep a real
        // callsign at normal spacing. Only re-apply on change (avoids a full
        // re-render every tick).
        const int letter_space = has_caller ? 0 : 12;
        if (letter_space != s_ls_callsign) {
            s_ls_callsign = letter_space;
            lv_obj_set_style_text_letter_space(s_lbl_callsign, letter_space, 0);
        }
    }

    const char *caption = "STANDBY";
    uint32_t color = kColorDim;
    uint32_t call_color = kColorText;
    if (tx && rx) {
        caption = "FULL DUPLEX";
        color = kColorDuplex;
        call_color = kColorAccent;
    } else if (tx) {
        caption = "TRANSMITTING";
        color = kColorTx;
    } else if (rx) {
        caption = "RECEIVING";
        color = kColorAccent;
        call_color = kColorAccent;
    } else {
        call_color = kColorDim;
    }
    if (setLabel(s_lbl_caption, s_shown_caption, sizeof(s_shown_caption), caption)) {
        lv_obj_set_style_text_color(s_lbl_caption, lv_color_hex(color), 0);
    }
    setLabelColor(s_lbl_callsign, s_clr_callsign, call_color);

    char ip[96];
    uint32_t ip_color = kColorAccent;
    if (nrlWifiStaConnected()) {
        char buf[16] = {};
        nrlIpToString(nrlWifiStaIp(), buf, sizeof(buf));
        snprintf(ip, sizeof(ip), "%s", buf);
    } else {
        char buf[16] = {};
        nrlIpToString(nrlWifiApIp(), buf, sizeof(buf));
        snprintf(ip, sizeof(ip), "%s", buf[0] ? buf : "---");
        ip_color = kColorWarn;
    }
    if (setLabel(s_lbl_ip, s_shown_ip, sizeof(s_shown_ip), ip)) {
        lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(ip_color), 0);
    }

    char server[96];
    const char *host = (cfg && cfg->server_host[0]) ? cfg->server_host : "---";
    snprintf(server, sizeof(server), "%s", host);
    const uint32_t server_color = tx ? kColorTx : (rx ? kColorAccent : kColorSub);
    setLabel(s_lbl_server, s_shown_server, sizeof(s_shown_server), server);
    setLabelColor(s_lbl_server, s_clr_server, server_color);
}

// Volume now lives in the shared top bar, so refresh it on every page.
void refreshVolume()
{
    if (s_lbl_vol == nullptr) {
        return;
    }
    char vol[24];
    uint32_t color = kColorSub;
    const int bt_pct = NRL_BtHfp_IsConnected() ? NRL_BtHfp_GetVolumePercent() : -1;
    if (bt_pct >= 0) {
        // Headset connected: show its speaker volume (green, with the BT glyph).
        snprintf(vol, sizeof(vol), LV_SYMBOL_BLUETOOTH " %d%%", bt_pct);
        color = kColorGood;
    } else {
        const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
        const int pct = cfg ? (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255 : 0;
        snprintf(vol, sizeof(vol), LV_SYMBOL_VOLUME_MID " %d%%", pct);
    }
    if (setLabel(s_lbl_vol, s_shown_vol, sizeof(s_shown_vol), vol)) {
        lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(color), 0);
    }
}

void refreshDetailPage()
{
    if (s_lbl_detail == nullptr) {
        return;
    }
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    char text[512];
    if (s_page == Page::Wifi) {
        char sta[16] = {};
        char ap[16] = {};
        nrlIpToString(nrlWifiStaIp(), sta, sizeof(sta));
        nrlIpToString(nrlWifiApIp(), ap, sizeof(ap));
        snprintf(text, sizeof(text),
                 "SSID: %s\nMode: %s\nSTA IP: %s\nConfig AP: %s\n\nReset WiFi clears saved network credentials.",
                 (cfg && cfg->wifi_ssid[0]) ? cfg->wifi_ssid : "(not set)",
                 nrlWifiStaConnected() ? "STA connected" : "AP/config",
                 sta[0] ? sta : "---",
                 ap[0] ? ap : "192.168.4.1");
    } else if (s_page == Page::Station) {
        snprintf(text, sizeof(text),
                 "Callsign: %s\nSSID: %u\nServer: %s:%u\n\nEdit station and server settings on the Station page.",
                 (cfg && cfg->callsign[0]) ? cfg->callsign : "----",
                 cfg ? static_cast<unsigned>(cfg->callsign_ssid) : 0,
                 (cfg && cfg->server_host[0]) ? cfg->server_host : "---",
                 cfg ? static_cast<unsigned>(cfg->server_port) : 0);
    } else if (s_page == Page::Audio) {
        const int pct = cfg ? (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255 : 0;
        snprintf(text, sizeof(text),
                 "Speaker volume: %d%%\nMic volume: %u\nAEC: %s\nNoise reduction: %s",
                 pct,
                 cfg ? static_cast<unsigned>(cfg->mic_volume) : 0,
                 (cfg && cfg->aec_enabled) ? "on" : "off",
                 (cfg && cfg->ai_noise_enabled) ? "on" : "off");
    } else {
        return;
    }
    setLabel(s_lbl_detail, s_shown_detail, sizeof(s_shown_detail), text);
}

void refresh()
{
    refreshClock();
    refreshWifiBadge();
    refreshVolume();
    refreshStationBadges();
    if (s_page == Page::Home) {
        refreshHome();
    } else {
        refreshDetailPage();
    }
    refreshBtTop();
    if (s_page == Page::Bt) {
        refreshBtPage();
    }
}

} // namespace

extern "C" void Display_Init(void)
{
    if (s_ready) {
        return;
    }
    if (!initPanel() || !initLvgl()) {
        return;
    }
    initTouch();
    buildHome();
    refresh();
    lv_refr_now(nullptr);
    s_ready = true;
    ESP_LOGI(TAG, "display ready");
}

extern "C" void Display_Poll(void)
{
    if (!s_ready) {
        return;
    }
    const uint32_t now = millis();
    pollWifiScan();
    // Update the top-bar volume every poll (~20 ms) so a physical volume key held
    // down shows each 1% step live, matching the soft buttons. setLabel no-ops
    // when the value is unchanged, so this is cheap. The full refresh below stays
    // on the slower cadence.
    refreshVolume();
    if (s_last_refresh_ms == 0u || (now - s_last_refresh_ms) >= kRefreshIntervalMs) {
        s_last_refresh_ms = now;
        refresh();
    }
    if (s_volume_dirty && (now - s_volume_change_ms) >= kVolumeSaveDelayMs) {
        s_volume_dirty = !EXTERNAL_RADIO_SaveConfig();
    }
    lv_timer_handler();
}

extern "C" int Display_GetBatteryRawMv(void)
{
    return 0;
}

extern "C" int Display_GetBatteryCalibratedMv(void)
{
    return 0;
}

#endif
