#include "display.h"

#include "board_pins.h"

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && NRL_BOARD == NRL_BOARD_S31_KORVO

#include "../../lib/nrl_audio_bridge.h"
#include "../../lib/nrl_net_compat.h"
#include "external_radio.h"
#include "s31_i2c.h"
#include "status_io.h"

#include <stdint.h>
#include <stdio.h>
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
#include <lvgl.h>

static const char *TAG = "LCD_S31";

namespace {

constexpr int kWidth = NRL_DISPLAY_WIDTH;
constexpr int kHeight = NRL_DISPLAY_HEIGHT;
constexpr int kBufLines = 80;
constexpr uint32_t kRefreshIntervalMs = 500u;

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
};

bool s_ready = false;
Page s_page = Page::Home;
uint32_t s_last_refresh_ms = 0u;
bool s_time_sync_started = false;

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
lv_obj_t *s_lbl_wifi = nullptr;
lv_obj_t *s_lbl_vol = nullptr;
lv_obj_t *s_lbl_ip = nullptr;
lv_obj_t *s_lbl_detail = nullptr;

char s_shown_caption[24] = {};
char s_shown_callsign[16] = {};
char s_shown_ssid[16] = {};
char s_shown_time[16] = {};
char s_shown_wifi[32] = {};
char s_shown_vol[16] = {};
char s_shown_ip[96] = {};

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
    lv_obj_add_event_cb(btn, action, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(id)));

    lv_obj_t *txt = label(btn, &lv_font_montserrat_20, kColorText);
    lv_obj_center(txt);
    lv_label_set_text(txt, text);
    return btn;
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
    memset(s_shown_wifi, 0, sizeof(s_shown_wifi));
    memset(s_shown_vol, 0, sizeof(s_shown_vol));
    memset(s_shown_ip, 0, sizeof(s_shown_ip));
    s_lbl_caption = nullptr;
    s_lbl_callsign = nullptr;
    s_lbl_ssid = nullptr;
    s_lbl_time = nullptr;
    s_lbl_wifi = nullptr;
    s_lbl_vol = nullptr;
    s_lbl_ip = nullptr;
    s_lbl_detail = nullptr;
}

void topBar(lv_obj_t *scr, const char *title)
{
    lv_obj_t *bar = panel(scr, 0, 0, kWidth, 56);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A111B), 0);

    lv_obj_t *name = label(bar, &lv_font_montserrat_20, kColorText);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 20, 0);
    lv_label_set_text(name, title);

    s_lbl_time = label(bar, &lv_font_montserrat_28, kColorTime);
    lv_obj_center(s_lbl_time);
    lv_label_set_text(s_lbl_time, "--:--:--");

    s_lbl_wifi = label(bar, &lv_font_montserrat_16, kColorSub);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI " --");
}

void buildHome()
{
    clearScreen();
    s_page = Page::Home;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr, "NRL ESP32-S31");

    lv_obj_t *left = panel(scr, 22, 78, 458, 270);
    s_lbl_caption = label(left, &lv_font_montserrat_20, kColorDim);
    lv_obj_align(s_lbl_caption, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_lbl_caption, "STANDBY");

    s_lbl_callsign = label(left, &lv_font_montserrat_48, kColorText);
    lv_obj_set_width(s_lbl_callsign, 430);
    lv_obj_align(s_lbl_callsign, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_label_set_text(s_lbl_callsign, "----");

    s_lbl_ssid = label(left, &lv_font_montserrat_28, kColorSub);
    lv_obj_align(s_lbl_ssid, LV_ALIGN_TOP_LEFT, 2, 124);
    lv_label_set_text(s_lbl_ssid, "SSID -");

    lv_obj_t *hint = label(left, &lv_font_montserrat_16, kColorSub);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(hint, "Touch CONFIG for network, station and audio settings");

    lv_obj_t *right = panel(scr, 502, 78, 276, 270);
    lv_obj_t *net = label(right, &lv_font_montserrat_16, kColorDim);
    lv_obj_align(net, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(net, "NETWORK");
    s_lbl_ip = label(right, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_width(s_lbl_ip, 248);
    lv_obj_align(s_lbl_ip, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_ip, "---");

    lv_obj_t *audio = label(right, &lv_font_montserrat_16, kColorDim);
    lv_obj_align(audio, LV_ALIGN_TOP_LEFT, 0, 104);
    lv_label_set_text(audio, "AUDIO");
    s_lbl_vol = label(right, &lv_font_montserrat_20, kColorSub);
    lv_obj_align(s_lbl_vol, LV_ALIGN_TOP_LEFT, 0, 138);
    lv_label_set_text(s_lbl_vol, LV_SYMBOL_VOLUME_MID " --");

    button(scr, 22, 372, 180, 78, "VOL-", Action::VolumeDown);
    button(scr, 222, 372, 356, 78, "CONFIG", Action::Config);
    button(scr, 598, 372, 180, 78, "VOL+", Action::VolumeUp);
}

void buildConfig()
{
    clearScreen();
    s_page = Page::Config;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr, "CONFIG");
    button(scr, 24, 84, 230, 132, "WiFi", Action::Wifi);
    button(scr, 284, 84, 230, 132, "Station", Action::Station);
    button(scr, 544, 84, 230, 132, "Audio", Action::Audio);
    button(scr, 24, 372, 230, 76, "Back", Action::Home);

    lv_obj_t *info = label(scr, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_width(info, 720);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 34, 250);
    lv_label_set_text(info, "Use the touch panels for quick changes. Full text editing remains available through the WiFi/BLE config portal.");
}

void buildWifi()
{
    clearScreen();
    s_page = Page::Wifi;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr, "WiFi");
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);
    s_lbl_detail = label(box, &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_lbl_detail, 710);
    lv_obj_align(s_lbl_detail, LV_ALIGN_TOP_LEFT, 0, 0);
    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 544, 372, 230, 76, "Reset WiFi", Action::ResetWifi);
}

void buildStation()
{
    clearScreen();
    s_page = Page::Station;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr, "Station");
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);
    s_lbl_detail = label(box, &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_lbl_detail, 710);
    lv_obj_align(s_lbl_detail, LV_ALIGN_TOP_LEFT, 0, 0);
    button(scr, 24, 372, 170, 76, "Back", Action::Config);
    button(scr, 238, 372, 170, 76, "SSID-", Action::CallsignSsidDown);
    button(scr, 432, 372, 170, 76, "SSID+", Action::CallsignSsidUp);
}

void buildAudio()
{
    clearScreen();
    s_page = Page::Audio;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr, "Audio");
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);
    s_lbl_detail = label(box, &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_lbl_detail, 710);
    lv_obj_align(s_lbl_detail, LV_ALIGN_TOP_LEFT, 0, 0);
    button(scr, 24, 372, 170, 76, "Back", Action::Config);
    button(scr, 238, 372, 170, 76, "VOL-", Action::VolumeDown);
    button(scr, 432, 372, 170, 76, "VOL+", Action::VolumeUp);
}

void setVolumeDelta(int delta)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) {
        return;
    }
    int volume = static_cast<int>(cfg->line_out_volume) + delta;
    if (volume < 0) {
        volume = 0;
    } else if (volume > 255) {
        volume = 255;
    }
    if (volume != static_cast<int>(cfg->line_out_volume)) {
        EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), true);
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
    } else if (ssid > 15) {
        ssid = 15;
    }
    if (ssid != static_cast<int>(cfg->callsign_ssid)) {
        EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(ssid), true);
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
        case Action::VolumeDown: setVolumeDelta(-16); break;
        case Action::VolumeUp: setVolumeDelta(16); break;
        case Action::CallsignSsidDown: setSsidDelta(-1); break;
        case Action::CallsignSsidUp: setSsidDelta(1); break;
        case Action::ResetWifi: EXTERNAL_RADIO_ResetNetworkConfig(); break;
    }
    s_last_refresh_ms = 0;
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

void refreshHome()
{
    char voice_call[8] = {};
    unsigned voice_ssid = 0;
    const bool rx = NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid);
    const bool tx = STATUS_IO_IsSqlActive();

    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    char call[16] = "----";
    char ssid[16] = "SSID -";
    if (rx && voice_call[0] != '\0') {
        snprintf(call, sizeof(call), "%s", voice_call);
        snprintf(ssid, sizeof(ssid), "SSID %u", voice_ssid);
    } else if (cfg != nullptr) {
        snprintf(call, sizeof(call), "%s", cfg->callsign[0] ? cfg->callsign : "----");
        snprintf(ssid, sizeof(ssid), "SSID %u", static_cast<unsigned>(cfg->callsign_ssid));
    }
    setLabel(s_lbl_callsign, s_shown_callsign, sizeof(s_shown_callsign), call);
    setLabel(s_lbl_ssid, s_shown_ssid, sizeof(s_shown_ssid), ssid);

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
    }
    if (setLabel(s_lbl_caption, s_shown_caption, sizeof(s_shown_caption), caption)) {
        lv_obj_set_style_text_color(s_lbl_caption, lv_color_hex(color), 0);
        lv_obj_set_style_text_color(s_lbl_callsign, lv_color_hex(call_color), 0);
    }

    char ip[96];
    uint32_t ip_color = kColorAccent;
    if (tx || rx) {
        snprintf(ip, sizeof(ip), "%s", (cfg && cfg->server_host[0]) ? cfg->server_host : "---");
        ip_color = tx ? kColorTx : kColorAccent;
    } else if (nrlWifiStaConnected()) {
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
                 "Callsign: %s\nSSID: %u\nServer: %s:%u\n\nUse SSID buttons for quick APRS SSID changes. Edit callsign text in the web/BLE portal.",
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
    if (s_last_refresh_ms == 0u || (now - s_last_refresh_ms) >= kRefreshIntervalMs) {
        s_last_refresh_ms = now;
        refresh();
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
