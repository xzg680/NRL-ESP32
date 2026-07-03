#include "display.h"

#include "board_pins.h"

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && NRL_BOARD == NRL_BOARD_S31_KORVO

#include "../../lib/nrl_audio_bridge.h"
#include "../../lib/nrl_bt_hfp.h"
#include "../../lib/nrl_net_compat.h"
#include "../../lib/nrl_wifi.h"
#include "../../media/cover_decoder.h"
#include "../../services/config_notify.h"
#include "../../services/espnow_link.h"
#include "../../services/music_player.h"
#include "../../services/music_playlist.h"
#include "../../services/nanny.h"
#include "../../services/storage_service.h"
#include "external_radio.h"
#include "fonts/lv_font_cjk.h"
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
    Apps,
    Wifi,
    Station,
    Audio,
    Bt,
    Music,
    Radio,
    Nanny,
    Smb,
    EspNow,
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
    Music,
    MusicToggle,
    MusicNext,
    MusicPrev,
    MusicRescan,
    Radio,
    Nanny,
    Smb,
    Apps,
    EspNow,
    SaveSmb,
    ClearSmb,
    SaveNanny,
    BeaconOff,
    SaveRadio,
    RadioPlay,
    RadioStop,
};

enum class AudioControl : intptr_t {
    Speaker = 1,
    Mic,
    Aec,
    Noise,
    MicHpf,
    EspNow,
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

// Media / nanny config pages (SMB share, beacon scheduler, net radio).
lv_obj_t *s_ta_smb_server = nullptr;
lv_obj_t *s_ta_smb_share = nullptr;
lv_obj_t *s_ta_smb_user = nullptr;
lv_obj_t *s_ta_smb_pass = nullptr;
lv_obj_t *s_ta_beacon_path = nullptr;
lv_obj_t *s_ta_beacon_interval = nullptr;
lv_obj_t *s_ta_radio_url = nullptr;
lv_obj_t *s_dd_music_target = nullptr;
lv_obj_t *s_sw_espnow = nullptr;
lv_obj_t *s_lbl_espnow_status = nullptr;
char s_shown_espnow_status[128] = {};
// Whether the Home page was built with the split (NRL + ESP-NOW) PTT; when
// the ESP-NOW enable state changes, refreshHome rebuilds the page.
bool s_home_espnow_split = false;
// Config generation this UI last rendered/produced. When another config
// writer (web portal, AT console) bumps CONFIG_NOTIFY past this, refresh()
// rebuilds the visible form page so its widgets show the new values.
uint32_t s_cfg_gen_seen = 0u;

// Music player page widgets. The list is rebuilt on page entry / rescan;
// the now-playing labels update from refresh().
lv_obj_t *s_lbl_music_title = nullptr;
lv_obj_t *s_lbl_music_artist = nullptr;
lv_obj_t *s_lbl_music_album = nullptr;
lv_obj_t *s_lbl_music_state = nullptr;
lv_obj_t *s_btn_music_toggle_label = nullptr;
lv_obj_t *s_list_music = nullptr;
lv_obj_t *s_img_music_cover = nullptr;
lv_obj_t *s_lbl_music_icon = nullptr;
char s_shown_music_path[128] = {};
bool s_shown_music_playing = false;

// Decoded album art for the now-playing card. The bitmap and its LVGL
// descriptor outlive page rebuilds (the widget re-attaches to them);
// replaced on track change in refreshMusic.
CoverBitmap s_music_cover_bmp = {};
lv_image_dsc_t s_music_cover_dsc = {};
constexpr uint16_t kMusicCoverDim = 152;

// Montserrat with a CJK font as fallback: ASCII keeps the Latin design,
// Chinese tags/filenames render from the CJK engine. Initialised in
// Display_Init (lv_font_montserrat_* are const, so mutable copies).
// The fallback is switchable at runtime between the built-in bitmap subset
// (lv_font_cjk_*) and FreeType vector rendering from a TTF on the TF card,
// for side-by-side comparison (Display_SetCjkFontEngine).
lv_font_t s_font_music_16;
lv_font_t s_font_music_20;
int s_cjk_font_engine = DISPLAY_CJK_FONT_BITMAP;
#if LV_USE_FREETYPE
constexpr const char *kCjkTtfPath = "/sdcard/fonts/cjk.ttf";
bool s_freetype_inited = false;
lv_font_t *s_ft_font_16 = nullptr;
lv_font_t *s_ft_font_20 = nullptr;
#endif

char s_shown_caption[24] = {};
char s_shown_callsign[16] = {};
char s_shown_ssid[16] = {};
char s_shown_time[16] = {};
char s_shown_local_station[24] = {};
char s_shown_wifi[32] = {};
char s_shown_vol[16] = {};
char s_shown_remote_station[160] = {};
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
void espnowPttEvent(lv_event_t *event);
void textAreaEvent(lv_event_t *event);
void wifiOptionEvent(lv_event_t *event);
void audioSliderEvent(lv_event_t *event);
void audioSwitchEvent(lv_event_t *event);
void musicTargetEvent(lv_event_t *event);
void updateAudioValueLabels();
void refresh();
void rebuildCurrentPage();
lv_obj_t *styledDropdown(lv_obj_t *parent, int x, int y, int w);

// Set the per-page form status line (no-op when the page has none).
void formStatus(const char *text, uint32_t color)
{
    if (s_lbl_form_status != nullptr) {
        lv_label_set_text(s_lbl_form_status, text);
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(color), 0);
    }
}

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
    // A page (re)build renders current config; consume any pending change.
    s_cfg_gen_seen = CONFIG_NOTIFY_Generation();
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
    s_ta_smb_server = nullptr;
    s_ta_smb_share = nullptr;
    s_ta_smb_user = nullptr;
    s_ta_smb_pass = nullptr;
    s_ta_beacon_path = nullptr;
    s_ta_beacon_interval = nullptr;
    s_ta_radio_url = nullptr;
    s_dd_music_target = nullptr;
    s_sw_espnow = nullptr;
    s_lbl_espnow_status = nullptr;
    memset(s_shown_espnow_status, 0, sizeof(s_shown_espnow_status));
    s_lbl_music_title = nullptr;
    s_lbl_music_artist = nullptr;
    s_lbl_music_album = nullptr;
    s_lbl_music_state = nullptr;
    s_btn_music_toggle_label = nullptr;
    s_list_music = nullptr;
    s_img_music_cover = nullptr;
    s_lbl_music_icon = nullptr;
    s_shown_music_path[0] = '\0';
    s_shown_music_playing = false;
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

    // Music font (CJK fallback): when idle this badge doubles as the
    // now-playing ticker, which must render Chinese titles.
    s_lbl_remote_station = label(bar, &s_font_music_20, kColorDim);
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

    // Right: hold-to-talk PTT. With ESP-NOW enabled the area splits into two
    // stacked PTTs -- top keys the NRL network, bottom is the dedicated
    // ESP-NOW intercom PTT (purple when pressed to tell them apart).
    const bool espnow_split = ESPNOW_LINK_IsEnabled();
    s_home_espnow_split = espnow_split;
    auto make_ptt = [&scr](int y, int h, void (*cb)(lv_event_t *), uint32_t pressed_color) {
        lv_obj_t *ptt = lv_button_create(scr);
        lv_obj_set_pos(ptt, 502, y);
        lv_obj_set_size(ptt, 276, h);
        lv_obj_set_style_radius(ptt, 12, 0);
        lv_obj_set_style_bg_color(ptt, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_bg_color(ptt, lv_color_hex(pressed_color), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(ptt, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_border_width(ptt, 1, 0);
        lv_obj_add_event_cb(ptt, cb, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(ptt, cb, LV_EVENT_RELEASED, nullptr);
        lv_obj_add_event_cb(ptt, cb, LV_EVENT_PRESS_LOST, nullptr);
        return ptt;
    };
    if (!espnow_split) {
        lv_obj_t *ptt = make_ptt(78, 270, softPttEvent, 0x7A1F1F);
        lv_obj_t *ptt_lbl = label(ptt, &lv_font_montserrat_48, kColorText);
        lv_obj_center(ptt_lbl);
        lv_label_set_text(ptt_lbl, "PTT");
    } else {
        lv_obj_t *ptt = make_ptt(78, 128, softPttEvent, 0x7A1F1F);
        lv_obj_t *ptt_lbl = label(ptt, &lv_font_montserrat_48, kColorText);
        lv_obj_center(ptt_lbl);
        lv_label_set_text(ptt_lbl, "PTT");

        lv_obj_t *eptt = make_ptt(220, 128, espnowPttEvent, 0x4C1D95);
        lv_obj_t *eptt_lbl = label(eptt, &lv_font_montserrat_20, kColorDuplex);
        lv_obj_center(eptt_lbl);
        lv_obj_set_style_text_align(eptt_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(eptt_lbl, "ESP-NOW\nPTT");
    }

    button(scr, 22, 372, 170, 78, "VOL-", Action::VolumeDown);
    button(scr, 208, 372, 184, 78, "Config", Action::Config);
    button(scr, 408, 372, 184, 78, "Apps", Action::Apps);
    button(scr, 608, 372, 170, 78, "VOL+", Action::VolumeUp);
}

void buildApps()
{
    clearScreen();
    s_page = Page::Apps;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    button(scr, 24, 84, 240, 120, "Music", Action::Music);
    button(scr, 280, 84, 240, 120, "Radio", Action::Radio);
    button(scr, 536, 84, 238, 120, "ESP-NOW", Action::EspNow);

    // Shared playback target: one setting for everything the music player
    // outputs (music / nanny beacon / net radio), so it lives here next to
    // those features instead of inside any single one's settings page.
    lv_obj_t *box = panel(scr, 24, 220, 750, 132);
    fieldLabel(box, 0, 0, "Playback Target (music / beacon / radio)");
    s_dd_music_target = styledDropdown(box, 0, 24, 340);
    lv_dropdown_set_options(s_dd_music_target, "Local speaker\nNRL network\nLocal + network");
    const int target = MUSIC_GetTarget();
    lv_dropdown_set_selected(s_dd_music_target,
                             (target >= MUSIC_TARGET_LOCAL && target <= MUSIC_TARGET_BOTH)
                                 ? static_cast<uint32_t>(target)
                                 : 0u);
    lv_obj_add_event_cb(s_dd_music_target, musicTargetEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_lbl_form_status, 370, 34);
    lv_obj_set_width(s_lbl_form_status, 350);
    lv_label_set_text(s_lbl_form_status, "Applies from the next track.");

    button(scr, 24, 372, 230, 76, "Back", Action::Home);
}

void buildConfig()
{
    clearScreen();
    s_page = Page::Config;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    // Settings entries only; the feature/app entries live on the Apps page.
    button(scr, 24, 84, 240, 120, "WiFi", Action::Wifi);
    button(scr, 280, 84, 240, 120, "Station", Action::Station);
    button(scr, 536, 84, 238, 120, "Audio", Action::Audio);
    button(scr, 24, 220, 240, 120, "BT", Action::Bt);
    button(scr, 280, 220, 240, 120, "Nanny", Action::Nanny);
    button(scr, 536, 220, 238, 120, "NAS", Action::Smb);
    button(scr, 24, 372, 230, 76, "Back", Action::Home);
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

// Dark-theme dropdown matching the WiFi page's (shared styling for the
// playback-target selector).
lv_obj_t *styledDropdown(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_pos(dd, x, y);
    lv_obj_set_size(dd, w, 42);
    lv_obj_set_style_radius(dd, 6, 0);
    lv_obj_set_style_bg_color(dd, lv_color_hex(kColorPanel2), 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(kColorText), 0);
    lv_obj_t *dd_list = lv_dropdown_get_list(dd);
    if (dd_list != nullptr) {
        lv_obj_set_style_bg_color(dd_list, lv_color_hex(kColorPanel), 0);
        lv_obj_set_style_text_color(dd_list, lv_color_hex(kColorText), 0);
        lv_obj_set_style_border_color(dd_list, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_max_height(dd_list, 220, 0);
    }
    return dd;
}

void buildSmb()
{
    clearScreen();
    s_page = Page::Smb;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);

    char server[64] = {};
    char share[64] = {};
    char user[32] = {};
    char pass[64] = {};
    (void)STORAGE_SmbGetConfig(server, sizeof(server), share, sizeof(share),
                               user, sizeof(user), pass, sizeof(pass));

    fieldLabel(box, 0, 0, "Server (NAS / PC)");
    s_ta_smb_server = textArea(box, 0, 24, 340, "192.168.1.10", server, 63, false, nullptr, false);
    fieldLabel(box, 370, 0, "Share Name");
    s_ta_smb_share = textArea(box, 370, 24, 340, "music", share, 63, false, nullptr, false);
    fieldLabel(box, 0, 82, "Username (empty = guest)");
    s_ta_smb_user = textArea(box, 0, 106, 340, "guest", user, 31, false, nullptr, false);
    fieldLabel(box, 370, 82, "Password");
    s_ta_smb_pass = textArea(box, 370, 106, 340, "Password", pass, 63, true, nullptr, false);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 174);
    char status[96] = {};
    STORAGE_SmbDescribe(status, sizeof(status));
    char text[128];
    snprintf(text, sizeof(text), "SMB: %s", status);
    lv_label_set_text(s_lbl_form_status, text);

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 284, 372, 230, 76, "Clear", Action::ClearSmb);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveSmb);
    createKeyboard(scr);
}

void buildNanny()
{
    clearScreen();
    s_page = Page::Nanny;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);

    char path[128] = {};
    uint32_t interval = 0;
    const bool armed = NANNY_GetBeacon(path, sizeof(path), &interval);

    fieldLabel(box, 0, 0, "Beacon File Path");
    s_ta_beacon_path = textArea(box, 0, 24, 460, "/sdcard/beacon/id.wav", path, 127, false, nullptr, false);
    fieldLabel(box, 490, 0, "Interval (min)");
    char interval_text[8] = {};
    if (armed) {
        snprintf(interval_text, sizeof(interval_text), "%lu", static_cast<unsigned long>(interval));
    }
    s_ta_beacon_interval = textArea(box, 490, 24, 140, "30", interval_text, 4, false, "0123456789", true);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 100);
    lv_label_set_text(s_lbl_form_status,
                      armed ? "Beacon armed. Save applies changes; Beacon Off disarms."
                            : "Beacon off. Set file path + minutes, then Save.");

    button(scr, 24, 372, 230, 76, "Back", Action::Config);
    button(scr, 284, 372, 230, 76, "Beacon Off", Action::BeaconOff);
    button(scr, 544, 372, 230, 76, "Save", Action::SaveNanny);
    createKeyboard(scr);
}

void buildRadio()
{
    clearScreen();
    s_page = Page::Radio;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);

    char url[256] = {};
    MUSIC_GetRadioUrl(url, sizeof(url));

    fieldLabel(box, 0, 0, "Stream URL (http:// or https://)");
    s_ta_radio_url = textArea(box, 0, 24, 710, "http://...", url, 200, false, nullptr, false);

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 100);
    const char *playing_path = MUSIC_CurrentPath();
    if (MUSIC_IsPlaying() && strncmp(playing_path, "http", 4) == 0) {
        char text[192];
        snprintf(text, sizeof(text), "Playing: %s", playing_path);
        lv_label_set_text(s_lbl_form_status, text);
        lv_obj_set_style_text_color(s_lbl_form_status, lv_color_hex(kColorGood), 0);
    } else {
        lv_label_set_text(s_lbl_form_status, "Save the station URL, then tap Play.");
    }

    button(scr, 24, 372, 190, 76, "Back", Action::Apps);
    button(scr, 230, 372, 170, 76, "Save", Action::SaveRadio);
    button(scr, 416, 372, 170, 76, LV_SYMBOL_PLAY, Action::RadioPlay);
    button(scr, 602, 372, 172, 76, LV_SYMBOL_STOP, Action::RadioStop);
    createKeyboard(scr);
}

void refreshEspNowPage()
{
    if (s_lbl_espnow_status == nullptr) {
        return;
    }
    char peer[16] = {};
    ESPNOW_LINK_GetLastPeer(peer, sizeof(peer));
    char text[128];
    snprintf(text, sizeof(text), "State: %s\nLast peer heard: %s%s",
             ESPNOW_LINK_IsEnabled() ? "ON" : "OFF",
             peer[0] != '\0' ? peer : "(none)",
             ESPNOW_LINK_IsReceiving() ? "  " LV_SYMBOL_VOLUME_MAX " receiving" : "");
    if (setLabel(s_lbl_espnow_status, s_shown_espnow_status,
                 sizeof(s_shown_espnow_status), text)) {
        lv_obj_set_style_text_color(s_lbl_espnow_status,
                                    lv_color_hex(ESPNOW_LINK_IsReceiving() ? kColorDuplex : kColorText), 0);
    }
}

void buildEspNow()
{
    clearScreen();
    s_page = Page::EspNow;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);
    lv_obj_t *box = panel(scr, 24, 86, 750, 250);

    s_sw_espnow = switchControl(box, 0, 8, "ESP-NOW Intercom",
                                ESPNOW_LINK_IsEnabled(), AudioControl::EspNow);

    s_lbl_espnow_status = label(box, &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_lbl_espnow_status, 710);
    lv_obj_set_pos(s_lbl_espnow_status, 0, 70);
    refreshEspNowPage();

    s_lbl_form_status = label(box, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_form_status, 710);
    lv_obj_set_pos(s_lbl_form_status, 0, 174);
    lv_label_set_text(s_lbl_form_status,
                      "Off-grid intercom with nearby devices. When on, the home screen "
                      "gains a dedicated ESP-NOW PTT below the network PTT.");

    button(scr, 24, 372, 230, 76, "Back", Action::Apps);
}

const char *musicBasename(const char *path)
{
    if (path == nullptr) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return (slash != nullptr) ? slash + 1 : path;
}

void musicListEvent(lv_event_t *event)
{
    const size_t index = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    (void)PLAYLIST_PlayIndex(index);
    s_last_refresh_ms = 0;
}

void populateMusicList()
{
    if (s_list_music == nullptr) {
        return;
    }
    lv_obj_clean(s_list_music);
    const size_t count = PLAYLIST_Count();
    if (count == 0u) {
        lv_obj_t *empty = lv_list_add_text(s_list_music,
                                           STORAGE_SdMounted()
                                               ? "No tracks. Put files in /sdcard/music and Rescan."
                                               : "No TF card mounted.");
        lv_obj_set_style_text_color(empty, lv_color_hex(kColorSub), 0);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *btn = lv_list_add_button(s_list_music, LV_SYMBOL_AUDIO,
                                           musicBasename(PLAYLIST_GetPath(i)));
        lv_obj_set_style_bg_color(btn, lv_color_hex(kColorPanel2), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(kColorText), 0);
        lv_obj_add_event_cb(btn, musicListEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }
}

void refreshMusicCover(const MediaTrackInfo *track)
{
    if (s_img_music_cover == nullptr || s_lbl_music_icon == nullptr) {
        return;
    }

    // Detach the widget before releasing the old bitmap it may reference.
    lv_image_set_src(s_img_music_cover, nullptr);
    lv_obj_add_flag(s_img_music_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_lbl_music_icon, LV_OBJ_FLAG_HIDDEN);
    COVER_Free(&s_music_cover_bmp);

    if (track == nullptr || track->cover_type != MEDIA_COVER_JPEG ||
        track->cover_data == nullptr || track->cover_size == 0u) {
        return; // PNG or no art: keep the placeholder icon
    }
    if (!COVER_DecodeJpeg(track->cover_data, track->cover_size, kMusicCoverDim, &s_music_cover_bmp)) {
        return;
    }

    memset(&s_music_cover_dsc, 0, sizeof(s_music_cover_dsc));
    s_music_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_music_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_music_cover_dsc.header.w = s_music_cover_bmp.width;
    s_music_cover_dsc.header.h = s_music_cover_bmp.height;
    s_music_cover_dsc.header.stride = static_cast<uint32_t>(s_music_cover_bmp.width) * 2u;
    s_music_cover_dsc.data = s_music_cover_bmp.rgb565;
    s_music_cover_dsc.data_size = s_music_cover_bmp.bytes;

    lv_image_set_src(s_img_music_cover, &s_music_cover_dsc);
    lv_obj_remove_flag(s_img_music_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_music_icon, LV_OBJ_FLAG_HIDDEN);
}

void refreshMusic()
{
    const bool playing = MUSIC_IsPlaying();
    const char *path = MUSIC_CurrentPath();
    const bool track_changed = strncmp(path, s_shown_music_path, sizeof(s_shown_music_path)) != 0;
    if (!track_changed && playing == s_shown_music_playing) {
        return;
    }
    strncpy(s_shown_music_path, path, sizeof(s_shown_music_path) - 1u);
    s_shown_music_path[sizeof(s_shown_music_path) - 1u] = '\0';
    s_shown_music_playing = playing;

    const MediaTrackInfo *track = MUSIC_GetTrackInfo();
    if (track_changed) {
        refreshMusicCover(track);
    }
    if (s_lbl_music_title != nullptr) {
        const char *title = (track != nullptr && track->title[0] != '\0')
                                ? track->title
                                : musicBasename(path);
        lv_label_set_text(s_lbl_music_title, (title[0] != '\0') ? title : "--");
    }
    if (s_lbl_music_artist != nullptr) {
        lv_label_set_text(s_lbl_music_artist,
                          (track != nullptr && track->artist[0] != '\0') ? track->artist : "");
    }
    if (s_lbl_music_album != nullptr) {
        lv_label_set_text(s_lbl_music_album,
                          (track != nullptr && track->album[0] != '\0') ? track->album : "");
    }
    if (s_lbl_music_state != nullptr) {
        lv_label_set_text(s_lbl_music_state, playing ? "Playing" : "Stopped");
        lv_obj_set_style_text_color(s_lbl_music_state,
                                    lv_color_hex(playing ? kColorGood : kColorSub), 0);
    }
    if (s_btn_music_toggle_label != nullptr) {
        lv_label_set_text(s_btn_music_toggle_label, playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    }
}

void buildMusic()
{
    clearScreen();
    s_page = Page::Music;
    lv_obj_t *scr = lv_screen_active();
    topBar(scr);

    // Left: now-playing card -- cover art (or placeholder icon) on top,
    // track labels underneath.
    lv_obj_t *card = panel(scr, 24, 78, 300, 274);
    s_lbl_music_icon = label(card, &lv_font_montserrat_48, kColorDim);
    lv_obj_align(s_lbl_music_icon, LV_ALIGN_TOP_MID, 0, 48);
    lv_label_set_text(s_lbl_music_icon, LV_SYMBOL_AUDIO);

    s_img_music_cover = lv_image_create(card);
    lv_obj_align(s_img_music_cover, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_add_flag(s_img_music_cover, LV_OBJ_FLAG_HIDDEN);
    if (s_music_cover_bmp.rgb565 != nullptr) {
        // Returning to the page mid-track: re-attach the existing bitmap.
        lv_image_set_src(s_img_music_cover, &s_music_cover_dsc);
        lv_obj_remove_flag(s_img_music_cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_music_icon, LV_OBJ_FLAG_HIDDEN);
    }

    s_lbl_music_title = label(card, &s_font_music_20, kColorText);
    lv_obj_set_width(s_lbl_music_title, 270);
    lv_obj_align(s_lbl_music_title, LV_ALIGN_TOP_LEFT, 0, 162);
    lv_label_set_long_mode(s_lbl_music_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_music_title, "--");

    s_lbl_music_artist = label(card, &s_font_music_16, kColorSub);
    lv_obj_set_width(s_lbl_music_artist, 270);
    lv_obj_align(s_lbl_music_artist, LV_ALIGN_TOP_LEFT, 0, 196);
    lv_label_set_long_mode(s_lbl_music_artist, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_music_artist, "");

    s_lbl_music_album = label(card, &s_font_music_16, kColorDim);
    lv_obj_set_width(s_lbl_music_album, 270);
    lv_obj_align(s_lbl_music_album, LV_ALIGN_TOP_LEFT, 0, 222);
    lv_label_set_long_mode(s_lbl_music_album, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_music_album, "");

    s_lbl_music_state = label(card, &lv_font_montserrat_16, kColorSub);
    lv_obj_align(s_lbl_music_state, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    lv_label_set_text(s_lbl_music_state, "Stopped");

    // Right: scrollable track list.
    s_list_music = lv_list_create(scr);
    lv_obj_set_pos(s_list_music, 340, 78);
    lv_obj_set_size(s_list_music, 436, 274);
    lv_obj_set_style_bg_color(s_list_music, lv_color_hex(kColorPanel), 0);
    lv_obj_set_style_border_color(s_list_music, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_text_font(s_list_music, &s_font_music_16, 0);
    populateMusicList();

    button(scr, 24, 372, 150, 76, "Back", Action::Apps);
    button(scr, 190, 372, 120, 76, LV_SYMBOL_PREV, Action::MusicPrev);
    lv_obj_t *toggle = button(scr, 326, 372, 120, 76,
                              MUSIC_IsPlaying() ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY,
                              Action::MusicToggle);
    s_btn_music_toggle_label = lv_obj_get_child(toggle, 0);
    button(scr, 462, 372, 120, 76, LV_SYMBOL_NEXT, Action::MusicNext);
    button(scr, 598, 372, 178, 76, "Rescan", Action::MusicRescan);

    // Prime the now-playing card.
    s_shown_music_path[0] = '\1'; // force refreshMusic to repaint
    refreshMusic();
}

void musicToggle()
{
    if (MUSIC_IsPlaying()) {
        MUSIC_Stop();
        return;
    }
    const int current = PLAYLIST_CurrentIndex();
    if (current >= 0) {
        (void)PLAYLIST_PlayIndex(static_cast<size_t>(current));
    } else {
        (void)PLAYLIST_Next();
    }
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

void saveSmbForm()
{
    const char *server = (s_ta_smb_server != nullptr) ? lv_textarea_get_text(s_ta_smb_server) : "";
    const char *share = (s_ta_smb_share != nullptr) ? lv_textarea_get_text(s_ta_smb_share) : "";
    const char *user = (s_ta_smb_user != nullptr) ? lv_textarea_get_text(s_ta_smb_user) : "";
    const char *pass = (s_ta_smb_pass != nullptr) ? lv_textarea_get_text(s_ta_smb_pass) : "";
    if (server[0] == '\0' && share[0] == '\0') {
        STORAGE_SmbClear();
        formStatus("SMB config cleared.", kColorGood);
        return;
    }
    if (STORAGE_SmbConfigure(server, share, user, pass)) {
        formStatus("SMB saved. Mounting in background...", kColorGood);
    } else {
        formStatus("Save failed: server and share are required.", kColorBad);
    }
}

void clearSmbForm()
{
    STORAGE_SmbClear();
    if (s_ta_smb_server != nullptr) lv_textarea_set_text(s_ta_smb_server, "");
    if (s_ta_smb_share != nullptr) lv_textarea_set_text(s_ta_smb_share, "");
    if (s_ta_smb_user != nullptr) lv_textarea_set_text(s_ta_smb_user, "");
    if (s_ta_smb_pass != nullptr) lv_textarea_set_text(s_ta_smb_pass, "");
    formStatus("SMB config cleared.", kColorGood);
}

void saveNannyForm()
{
    const char *path = (s_ta_beacon_path != nullptr) ? lv_textarea_get_text(s_ta_beacon_path) : "";
    const char *interval_text =
        (s_ta_beacon_interval != nullptr) ? lv_textarea_get_text(s_ta_beacon_interval) : "";
    char *end = nullptr;
    const unsigned long minutes = strtoul(interval_text, &end, 10);
    const bool interval_ok = interval_text[0] != '\0' && end != interval_text && *end == '\0' &&
                             minutes >= 1ul && minutes <= 1440ul;
    if (!interval_ok) {
        formStatus("Save failed: interval must be 1-1440 minutes.", kColorBad);
        return;
    }
    if (NANNY_SetBeacon(path, static_cast<uint32_t>(minutes))) {
        formStatus("Beacon armed.", kColorGood);
    } else {
        formStatus("Save failed: set the beacon file path.", kColorBad);
    }
}

void beaconOffForm()
{
    NANNY_DisableBeacon();
    if (s_ta_beacon_interval != nullptr) {
        lv_textarea_set_text(s_ta_beacon_interval, "");
    }
    formStatus("Beacon disabled.", kColorGood);
}

// Save the station URL; with `play` also tune in right away.
void saveRadioForm(const bool play)
{
    const char *url = (s_ta_radio_url != nullptr) ? lv_textarea_get_text(s_ta_radio_url) : "";
    if (!MUSIC_SetRadioUrl(url)) {
        formStatus("Save failed: URL must start with http:// or https://.", kColorBad);
        return;
    }
    if (!play) {
        formStatus("Station URL saved.", kColorGood);
        return;
    }
    if (url[0] == '\0') {
        formStatus("Set a station URL first.", kColorBad);
        return;
    }
    if (MUSIC_PlayFile(url)) {
        formStatus("Tuning in...", kColorGood);
    } else {
        formStatus("Play failed.", kColorBad);
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
        case Action::Music: buildMusic(); break;
        case Action::MusicToggle: musicToggle(); break;
        case Action::MusicNext: (void)PLAYLIST_Next(); break;
        case Action::MusicPrev: (void)PLAYLIST_Prev(); break;
        case Action::MusicRescan:
            (void)PLAYLIST_Scan();
            populateMusicList();
            break;
        case Action::Radio: buildRadio(); break;
        case Action::Nanny: buildNanny(); break;
        case Action::Smb: buildSmb(); break;
        case Action::Apps: buildApps(); break;
        case Action::EspNow: buildEspNow(); break;
        case Action::SaveSmb: saveSmbForm(); break;
        case Action::ClearSmb: clearSmbForm(); break;
        case Action::SaveNanny: saveNannyForm(); break;
        case Action::BeaconOff: beaconOffForm(); break;
        case Action::SaveRadio: saveRadioForm(false); break;
        case Action::RadioPlay: saveRadioForm(true); break;
        case Action::RadioStop:
            MUSIC_Stop();
            formStatus("Stopped.", kColorSub);
            break;
    }
    // Changes made from this UI are already on screen (with their status
    // message); consume the bump so refresh() doesn't rebuild over them.
    s_cfg_gen_seen = CONFIG_NOTIFY_Generation();
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
        case AudioControl::EspNow:
            // Persists immediately (own NVS entry), no Save step. Enabling
            // fails while WiFi is down -- revert the switch so it shows truth.
            if (ESPNOW_LINK_SetEnabled(checked)) {
                formStatus(checked ? "ESP-NOW intercom on." : "ESP-NOW intercom off.", kColorGood);
            } else {
                if (checked) {
                    lv_obj_remove_state(obj, LV_STATE_CHECKED);
                }
                formStatus("ESP-NOW enable failed (WiFi not started).", kColorBad);
            }
            s_cfg_gen_seen = CONFIG_NOTIFY_Generation(); // own change
            return;
        default:
            break;
    }
    markAudioChanged();
}

// Playback-target dropdown (Apps page): applies + persists immediately.
void musicTargetEvent(lv_event_t *event)
{
    lv_obj_t *dd = lv_event_get_target_obj(event);
    MUSIC_SetTarget(static_cast<int>(lv_dropdown_get_selected(dd)));
    formStatus("Playback target saved (applies from the next track).", kColorGood);
    s_cfg_gen_seen = CONFIG_NOTIFY_Generation(); // own change
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

// Dedicated ESP-NOW hold-to-talk (bottom half of the split Home PTT).
void espnowPttEvent(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            ESPNOW_LINK_SetPtt(true);
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            ESPNOW_LINK_SetPtt(false);
            break;
        default:
            return;
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

// UTF-8 helpers for the now-playing ticker (titles are valid UTF-8 after the
// metadata GBK conversion).
size_t utf8GlyphCount(const char *s)
{
    size_t n = 0;
    for (size_t i = 0; s[i] != '\0'; ++i) {
        if ((static_cast<uint8_t>(s[i]) & 0xC0u) != 0x80u) {
            ++n;
        }
    }
    return n;
}

size_t utf8NextOffset(const char *s, size_t off)
{
    if (s[off] == '\0') {
        return 0;
    }
    ++off;
    while (s[off] != '\0' && (static_cast<uint8_t>(s[off]) & 0xC0u) == 0x80u) {
        ++off;
    }
    return (s[off] == '\0') ? 0 : off;
}

// Copy `glyphs` UTF-8 characters starting at byte `start`, wrapping around.
void tickerWindow(const char *src, size_t start, const size_t glyphs, char *out, const size_t cap)
{
    size_t pos = 0;
    size_t off = start;
    for (size_t g = 0; g < glyphs; ++g) {
        if (src[off] == '\0') {
            off = 0;
            if (src[0] == '\0') {
                break;
            }
        }
        const uint8_t b = static_cast<uint8_t>(src[off]);
        size_t clen = 1;
        if ((b & 0xE0u) == 0xC0u) clen = 2;
        else if ((b & 0xF0u) == 0xE0u) clen = 3;
        else if ((b & 0xF8u) == 0xF0u) clen = 4;
        if (pos + clen + 1u > cap) {
            break;
        }
        for (size_t k = 0; k < clen && src[off] != '\0'; ++k) {
            out[pos++] = src[off++];
        }
    }
    out[pos] = '\0';
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
    char remote[160] = {};
    bool now_playing = false;
    if (rx && voice_call[0] != '\0') {
        // A caller always wins the badge.
        formatStationBadge(remote, sizeof(remote), voice_call, voice_ssid);
    } else if (MUSIC_IsPlaying()) {
        // Idle badge doubles as a now-playing ticker (song / net radio /
        // nanny beacon). Manual one-glyph-per-tick scroll: an LVGL circular
        // scroll label would re-render the whole screen at animation rate in
        // FULL mode.
        constexpr size_t kTickerGlyphs = 7;
        const MediaTrackInfo *track = MUSIC_GetTrackInfo();
        const char *title = (track != nullptr && track->title[0] != '\0')
                                ? track->title
                                : musicBasename(MUSIC_CurrentPath());
        static char s_ticker_src[136] = {};
        static size_t s_ticker_off = 0;
        char padded[136];
        snprintf(padded, sizeof(padded), "%.100s | ", title);
        if (strcmp(padded, s_ticker_src) != 0) {
            snprintf(s_ticker_src, sizeof(s_ticker_src), "%s", padded);
            s_ticker_off = 0;
        }
        if (utf8GlyphCount(title) <= kTickerGlyphs) {
            snprintf(remote, sizeof(remote), LV_SYMBOL_AUDIO " %.100s", title);
        } else {
            char window[48];
            tickerWindow(s_ticker_src, s_ticker_off, kTickerGlyphs, window, sizeof(window));
            s_ticker_off = utf8NextOffset(s_ticker_src, s_ticker_off);
            snprintf(remote, sizeof(remote), "%s", window);
        }
        now_playing = true;
    } else {
        formatStationBadge(remote, sizeof(remote), nullptr, 0);
    }
    setLabel(s_lbl_remote_station, s_shown_remote_station, sizeof(s_shown_remote_station), remote);
    setLabelColor(s_lbl_remote_station, s_clr_remote_station,
                  rx ? kColorAccent : (now_playing ? kColorSub : kColorDim));
}

void refreshHome()
{
    // ESP-NOW got toggled while sitting on Home (config page, web, AT, or the
    // deferred boot restore): rebuild so the PTT area matches (single/split).
    if (ESPNOW_LINK_IsEnabled() != s_home_espnow_split) {
        buildHome();
        return;
    }

    char voice_call[8] = {};
    unsigned voice_ssid = 0;
    const bool rx = NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid);
    const bool tx = STATUS_IO_IsSqlActive();
    const bool espnow_tx = ESPNOW_LINK_PttActive();
    const bool espnow_rx = ESPNOW_LINK_IsReceiving();
    char espnow_peer[16] = {};
    if (espnow_rx) {
        ESPNOW_LINK_GetLastPeer(espnow_peer, sizeof(espnow_peer));
    }

    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    // Main panel shows only the remote caller (NRL stream first, else the
    // ESP-NOW peer). Local station lives in the top bar.
    const bool has_caller = (rx && voice_call[0] != '\0');
    const bool has_espnow_caller = (!has_caller && espnow_rx && espnow_peer[0] != '\0');
    char call[24] = {};
    if (has_espnow_caller) {
        snprintf(call, sizeof(call), "%s", espnow_peer);
    } else {
        formatStationBadge(call, sizeof(call), has_caller ? voice_call : nullptr, voice_ssid);
    }
    setLabel(s_lbl_callsign, s_shown_callsign, sizeof(s_shown_callsign), call);
    if (s_lbl_callsign != nullptr) {
        // Spread only the placeholder dashes so they read bigger; keep a real
        // callsign at normal spacing. Only re-apply on change (avoids a full
        // re-render every tick).
        const int letter_space = (has_caller || has_espnow_caller) ? 0 : 12;
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
    } else if (espnow_tx) {
        // Dedicated ESP-NOW PTT held: purple, to match its button.
        caption = "ESP-NOW TX";
        color = kColorDuplex;
    } else if (rx) {
        caption = "RECEIVING";
        color = kColorAccent;
        call_color = kColorAccent;
    } else if (espnow_rx) {
        // Voice arriving over the off-grid ESP-NOW link (not the NRL server).
        caption = "ESP-NOW RX";
        color = kColorDuplex;
        call_color = kColorDuplex;
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
    // Config changed underneath us (web portal / AT console): rebuild the
    // visible form page so its widgets show the new values. Pages without
    // config-backed widgets just consume the bump; while the on-screen
    // keyboard is open the rebuild is deferred so the user's edit survives.
    const uint32_t cfg_gen = CONFIG_NOTIFY_Generation();
    if (cfg_gen != s_cfg_gen_seen) {
        const bool form_page = s_page == Page::Wifi || s_page == Page::Station ||
                               s_page == Page::Audio || s_page == Page::Apps ||
                               s_page == Page::Nanny || s_page == Page::Smb ||
                               s_page == Page::Radio || s_page == Page::EspNow;
        const bool keyboard_open =
            s_keyboard != nullptr && !lv_obj_has_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        if (!form_page) {
            s_cfg_gen_seen = cfg_gen;
        } else if (!keyboard_open) {
            rebuildCurrentPage(); // syncs s_cfg_gen_seen via clearScreen
            formStatus("Settings updated from web/serial.", kColorWarn);
        }
    }

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
    if (s_page == Page::Music) {
        refreshMusic();
    }
    if (s_page == Page::EspNow) {
        refreshEspNowPage();
    }
}

// Rebuild the active page so a font-engine switch takes effect on every
// label immediately (LVGL caches glyph layout per label).
void rebuildCurrentPage()
{
    switch (s_page) {
        case Page::Home: buildHome(); break;
        case Page::Config: buildConfig(); break;
        case Page::Wifi: buildWifi(); break;
        case Page::Station: buildStation(); break;
        case Page::Audio: buildAudio(); break;
        case Page::Bt: buildBt(); break;
        case Page::Music: buildMusic(); break;
        case Page::Radio: buildRadio(); break;
        case Page::Nanny: buildNanny(); break;
        case Page::Smb: buildSmb(); break;
        case Page::Apps: buildApps(); break;
        case Page::EspNow: buildEspNow(); break;
    }
    s_last_refresh_ms = 0;
}

#if LV_USE_FREETYPE
// Lazily bring up FreeType and create the two vector font sizes from the
// TTF on the TF card. Kept out of Display_Init so boots without the file
// (or without a card) pay nothing.
bool ensureFreetypeFonts()
{
    if (s_ft_font_16 != nullptr && s_ft_font_20 != nullptr) {
        return true;
    }
    FILE *probe = fopen(kCjkTtfPath, "rb");
    if (probe == nullptr) {
        ESP_LOGW(TAG, "no TTF at %s", kCjkTtfPath);
        return false;
    }
    fclose(probe);

    if (!s_freetype_inited) {
        if (lv_freetype_init(LV_FREETYPE_CACHE_FT_GLYPH_CNT) != LV_RESULT_OK) {
            ESP_LOGE(TAG, "FreeType init failed");
            return false;
        }
        s_freetype_inited = true;
    }
    if (s_ft_font_16 == nullptr) {
        s_ft_font_16 = lv_freetype_font_create(kCjkTtfPath, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                               16, LV_FREETYPE_FONT_STYLE_NORMAL);
    }
    if (s_ft_font_20 == nullptr) {
        s_ft_font_20 = lv_freetype_font_create(kCjkTtfPath, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                               20, LV_FREETYPE_FONT_STYLE_NORMAL);
    }
    if (s_ft_font_16 == nullptr || s_ft_font_20 == nullptr) {
        ESP_LOGE(TAG, "FreeType font create failed (%s)", kCjkTtfPath);
        return false;
    }
    ESP_LOGI(TAG, "FreeType fonts ready from %s", kCjkTtfPath);
    return true;
}
#endif

} // namespace

extern "C" void Display_Init(void)
{
    if (s_ready) {
        return;
    }
    if (!initPanel() || !initLvgl()) {
        return;
    }
    // Music-page fonts: Montserrat primary with the generated CJK fallback
    // so Chinese track tags render (lv_font_montserrat_* are const).
    s_font_music_16 = lv_font_montserrat_16;
    s_font_music_16.fallback = &lv_font_cjk_16;
    s_font_music_20 = lv_font_montserrat_20;
    s_font_music_20.fallback = &lv_font_cjk_20;
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
        // Deferred save of our own volume change: consume the bump so the
        // page isn't pointlessly rebuilt half a second later.
        s_cfg_gen_seen = CONFIG_NOTIFY_Generation();
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

extern "C" bool Display_SetCjkFontEngine(const int engine)
{
    if (!s_ready) {
        return false;
    }
    if (engine == DISPLAY_CJK_FONT_BITMAP) {
        s_font_music_16.fallback = &lv_font_cjk_16;
        s_font_music_20.fallback = &lv_font_cjk_20;
        s_cjk_font_engine = DISPLAY_CJK_FONT_BITMAP;
        rebuildCurrentPage();
        return true;
    }
    if (engine == DISPLAY_CJK_FONT_FREETYPE) {
#if LV_USE_FREETYPE
        if (!ensureFreetypeFonts()) {
            return false;
        }
        s_font_music_16.fallback = s_ft_font_16;
        s_font_music_20.fallback = s_ft_font_20;
        s_cjk_font_engine = DISPLAY_CJK_FONT_FREETYPE;
        rebuildCurrentPage();
        return true;
#else
        return false;
#endif
    }
    return false;
}

extern "C" int Display_GetCjkFontEngine(void)
{
    return s_cjk_font_engine;
}

#endif
