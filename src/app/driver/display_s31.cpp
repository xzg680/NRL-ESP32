#include "display.h"

#include "board_pins.h"

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && NRL_BOARD == NRL_BOARD_S31_KORVO

#include "../../lib/nrl_audio_bridge.h"
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
constexpr int kBufLines = 80;
constexpr int kBounceLines = 10;
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
constexpr size_t kWifiOptionCount = 4u;

enum class Page : uint8_t {
    Home,
    Config,
    Wifi,
    Station,
    Audio,
};

enum class Action : intptr_t {
    Config = 1,
    Home,
    Wifi,
    Station,
    Audio,
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
uint8_t *s_draw_buf = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
lv_indev_t *s_touch_indev = nullptr;

lv_obj_t *s_lbl_caption = nullptr;
lv_obj_t *s_lbl_callsign = nullptr;
lv_obj_t *s_lbl_ssid = nullptr;
lv_obj_t *s_lbl_time = nullptr;
lv_obj_t *s_lbl_local_station = nullptr;
lv_obj_t *s_lbl_wifi = nullptr;
lv_obj_t *s_lbl_vol = nullptr;
lv_obj_t *s_lbl_remote_station = nullptr;
lv_obj_t *s_lbl_ip = nullptr;
lv_obj_t *s_lbl_server = nullptr;
lv_obj_t *s_lbl_detail = nullptr;
lv_obj_t *s_lbl_form_status = nullptr;
lv_obj_t *s_btn_wifi_options[kWifiOptionCount] = {};
lv_obj_t *s_lbl_wifi_options[kWifiOptionCount] = {};
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
char s_wifi_option_ssids[kWifiOptionCount][33] = {};

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
    // Single framebuffer: LVGL renders in PARTIAL mode and we copy each dirty
    // area straight into the one framebuffer being scanned out. With num_fbs=2
    // esp_lcd_panel_draw_bitmap copies the partial area into the back buffer and
    // then swaps the *whole* frame, so untouched regions flash 2-frame-old
    // content (the "ghosting/drifting" artefacts). For a UI that refreshes a few
    // small labels twice a second, a single buffer is coherent and the only cost
    // is negligible tearing inside the small area being written.
    panel_cfg.num_fbs = 1;
    panel_cfg.dma_burst_size = 128;
    // Bounce-buffer mode: the framebuffer lives in PSRAM, but the RGB DMA reads
    // it one chunk at a time through this small internal-SRAM buffer. PSRAM
    // bandwidth jitter (CPU, WiFi, and especially the AFE/AEC audio buffers that
    // also sit in PSRAM) was starving the line FIFO, so the image rolled/drifted
    // and looked oversized. Pulling each line through internal SRAM keeps the DMA
    // fed in time and stops the drift, without touching PSRAM speed/XIP (which
    // boot-loops on this board). ~10 lines = 800*10*2 = 16 KB of internal RAM.
    panel_cfg.bounce_buffer_size_px = static_cast<size_t>(kWidth) * kBounceLines;
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
    esp_lcd_panel_handle_t panel =
        static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

bool initLvgl()
{
    lv_init();
    const size_t buf_bytes = static_cast<size_t>(kWidth) * kBufLines * 2u;
    s_draw_buf = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_draw_buf == nullptr) {
        ESP_LOGE(TAG, "draw buffer alloc failed");
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
    lv_display_set_buffers(s_disp, s_draw_buf, nullptr, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
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
    s_lbl_caption = nullptr;
    s_lbl_callsign = nullptr;
    s_lbl_ssid = nullptr;
    s_lbl_time = nullptr;
    s_lbl_local_station = nullptr;
    s_lbl_wifi = nullptr;
    s_lbl_vol = nullptr;
    s_lbl_remote_station = nullptr;
    s_lbl_ip = nullptr;
    s_lbl_server = nullptr;
    s_lbl_detail = nullptr;
    s_lbl_form_status = nullptr;
    for (size_t i = 0; i < kWifiOptionCount; ++i) {
        s_btn_wifi_options[i] = nullptr;
        s_lbl_wifi_options[i] = nullptr;
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
    s_keyboard = nullptr;
}

void topBar(lv_obj_t *scr)
{
    constexpr int kTopLeft = 20;
    constexpr int kTopGap = 22;
    constexpr int kTopTimeW = 128;
    constexpr int kTopStationW = 150;
    constexpr int kTopVolW = 94;
    constexpr int kTopWifiW = 150;
    constexpr int kTopTimeX = kTopLeft;
    constexpr int kTopLocalX = kTopTimeX + kTopTimeW + kTopGap;
    constexpr int kTopVolX = kTopLocalX + kTopStationW + kTopGap;
    constexpr int kTopRemoteX = kTopVolX + kTopVolW + kTopGap;
    constexpr int kTopWifiX = kTopRemoteX + kTopStationW + kTopGap;

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

    // Callsign with the SSID appended as "CALL-SSID" (no separate SSID line).
    s_lbl_callsign = label(left, &lv_font_montserrat_48, kColorText);
    lv_obj_set_width(s_lbl_callsign, 430);
    lv_obj_align(s_lbl_callsign, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_label_set_text(s_lbl_callsign, "----");

    s_lbl_ip = label(left, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_width(s_lbl_ip, 430);
    lv_obj_align(s_lbl_ip, LV_ALIGN_TOP_LEFT, 0, 138);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_ip, LV_SYMBOL_WIFI " ---");

    s_lbl_server = label(left, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_width(s_lbl_server, 430);
    lv_obj_align(s_lbl_server, LV_ALIGN_TOP_LEFT, 0, 176);
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
    button(scr, 24, 84, 230, 132, "WiFi", Action::Wifi);
    button(scr, 284, 84, 230, 132, "Station", Action::Station);
    button(scr, 544, 84, 230, 132, "Audio", Action::Audio);
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

    for (size_t i = 0; i < kWifiOptionCount; ++i) {
        if (s_lbl_wifi_options[i] == nullptr || s_btn_wifi_options[i] == nullptr) {
            continue;
        }
        const bool has_ssid = s_wifi_option_ssids[i][0] != '\0';
        lv_label_set_text(s_lbl_wifi_options[i], has_ssid ? s_wifi_option_ssids[i] : "--");
        lv_obj_set_style_text_color(s_lbl_wifi_options[i],
                                    lv_color_hex(has_ssid ? kColorText : kColorDim), 0);
        if (has_ssid) {
            lv_obj_clear_state(s_btn_wifi_options[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_btn_wifi_options[i], LV_STATE_DISABLED);
        }
    }
    ESP_LOGI(TAG, "WiFi inline options updated: scanned=%u shown=%u",
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
    for (size_t i = 0; i < kWifiOptionCount; ++i) {
        lv_obj_t *btn = lv_button_create(box);
        lv_obj_set_pos(btn, static_cast<int>(i) * 176, 24);
        lv_obj_set_size(btn, (i == kWifiOptionCount - 1u) ? 182 : 164, 42);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D4E63), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kColorPanel), LV_STATE_DISABLED);
        lv_obj_set_style_border_color(btn, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_add_event_cb(btn, wifiOptionEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        s_btn_wifi_options[i] = btn;

        lv_obj_t *txt = label(btn, &lv_font_montserrat_16, kColorText);
        lv_obj_set_width(txt, (i == kWifiOptionCount - 1u) ? 162 : 144);
        lv_obj_center(txt);
        lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_DOT);
        lv_label_set_text(txt, "--");
        s_lbl_wifi_options[i] = txt;
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
    const int speaker_pct = cfg ? (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255 : 0;

    fieldLabel(box, 0, 0, "Speaker");
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

void action(lv_event_t *event)
{
    const Action id = static_cast<Action>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    switch (id) {
        case Action::Config: buildConfig(); break;
        case Action::Home: buildHome(); break;
        case Action::Wifi: buildWifi(); break;
        case Action::Station: buildStation(); break;
        case Action::Audio: buildAudio(); break;
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
    const size_t index = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
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
    if (s_lbl_remote_station != nullptr) {
        lv_obj_set_style_text_color(s_lbl_remote_station,
                                    lv_color_hex(rx ? kColorAccent : kColorDim), 0);
    }
}

void refreshHome()
{
    char voice_call[8] = {};
    unsigned voice_ssid = 0;
    const bool rx = NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid);
    const bool tx = STATUS_IO_IsSqlActive();

    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    // Main panel shows only the remote caller. Local station lives in the top bar.
    char call[24] = {};
    formatStationBadge(call, sizeof(call), (rx && voice_call[0] != '\0') ? voice_call : nullptr,
                       voice_ssid);
    setLabel(s_lbl_callsign, s_shown_callsign, sizeof(s_shown_callsign), call);

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
    if (s_lbl_callsign != nullptr) {
        lv_obj_set_style_text_color(s_lbl_callsign, lv_color_hex(call_color), 0);
    }

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
    if (s_lbl_server != nullptr) {
        lv_obj_set_style_text_color(s_lbl_server, lv_color_hex(server_color), 0);
    }
}

// Volume now lives in the shared top bar, so refresh it on every page.
void refreshVolume()
{
    if (s_lbl_vol == nullptr) {
        return;
    }
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    char vol[16];
    const int pct = cfg ? (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255 : 0;
    snprintf(vol, sizeof(vol), LV_SYMBOL_VOLUME_MID " %d%%", pct);
    setLabel(s_lbl_vol, s_shown_vol, sizeof(s_shown_vol), vol);
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
    lv_label_set_text(s_lbl_detail, text);
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
