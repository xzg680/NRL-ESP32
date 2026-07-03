// On-device LCD UI for the 格子派 board.
//
// Hardware: ST7789 240x240 SPI panel, identical wiring to the 小智 (xiaozhi)
// 格子派 board (see src/app/driver/board_pins.h). The panel is an IDF esp_lcd
// ST7789 device; rendering uses LVGL. LVGL is vendored as a local component
// (components/lvgl) and driven directly here -- no esp_lvgl_port -- so the
// build never has to download anything from the component registry.
//
// Layout (clean, dark "tech" theme):
//   +------------------------------------------+
//   | -72dB            75%             4.05V   |  <- status bar
//   |------------------------------------------|
//   |                RECEIVING                 |  <- caption (TX/RX state)
//   |               B G 7 X Y Z                 |  <- callsign (large)
//   |                 SSID 5                    |  <- callsign SSID (smaller)
//   |                10:24:37                   |  <- current time
//   |                                           |
//   |               192.168.1.50               |  <- address bar
//   +------------------------------------------+
//
// The callsign area shows the remote caller while a voice stream is being
// received, and this device's own callsign/SSID otherwise. The caption tracks
// the live state: STANDBY / RECEIVING / TRANSMITTING / FULL DUPLEX. Heartbeat
// packets never count as "receiving".
//
// The address bar normally shows the station IP; while transmitting OR
// receiving voice it shows the NRL server host instead (red for TX, cyan for
// RX). When WiFi cannot join a router and the device falls back to its config
// AP, the bar shows the AP hotspot address (192.168.4.1) in amber.

#include "display.h"

#include "board_pins.h"
#include "s31_i2c.h"

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && NRL_BOARD == NRL_BOARD_GEZIPAI

#include "../../lib/nrl_audio_bridge.h"
#include "external_radio.h"
#include "status_io.h"

#include "../../lib/nrl_net_compat.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

static const char *TAG = "LCD";
#include <esp_adc/adc_oneshot.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#if NRL_DISPLAY_BUS_RGB
// Touch is only wired on the RGB-panel path (all esp_lcd_touch* usage below is
// under NRL_DISPLAY_BUS_RGB). Keeping the include out of the ST7789 path avoids
// pulling the esp_lcd_touch component on gezipai, which has no touch.
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_gt1151.h>
#endif
#include <esp_lcd_panel_vendor.h>

#include <lvgl.h>

namespace {

constexpr int kWidth = NRL_DISPLAY_WIDTH;
constexpr int kHeight = NRL_DISPLAY_HEIGHT;

#ifndef NRL_DISPLAY_BUS_ST7789
#define NRL_DISPLAY_BUS_ST7789 0
#endif
#ifndef NRL_DISPLAY_BUS_RGB
#define NRL_DISPLAY_BUS_RGB 0
#endif

// LVGL partial render buffer height, in scan lines.
constexpr int kBufLines = 60;

// ---- Tech-style palette (0xRRGGBB) ----
constexpr uint32_t kColorBg       = 0x070B11;  // near-black screen background
constexpr uint32_t kColorBar      = 0x0E1622;  // status/IP bar fill
constexpr uint32_t kColorAccent   = 0x22D3EE;  // cyan accent line
constexpr uint32_t kColorCallIdle = 0xE6EDF3;  // callsign while idle
constexpr uint32_t kColorCallLive = 0x22D3EE;  // callsign while receiving
constexpr uint32_t kColorSub      = 0x6F8BA0;  // muted blue-gray (SSID, bars)
constexpr uint32_t kColorTime     = 0xBFE9F5;  // clock
constexpr uint32_t kColorCaption  = 0x46627A;  // dim caption above callsign
constexpr uint32_t kColorIp       = 0x46D6E6;  // IP address (STA mode)
constexpr uint32_t kColorApWarn   = 0xF5B453;  // amber: config / AP mode
constexpr uint32_t kColorGood     = 0x4ADE80;  // strong signal
constexpr uint32_t kColorWeak     = 0xF87171;  // weak signal / low battery
constexpr uint32_t kColorTx       = 0xFF6B6B;  // transmitting (PTT held)
constexpr uint32_t kColorDuplex   = 0xA78BFA;  // transmitting + receiving

// Battery pack assumed to be a single Li-ion cell (3.0 V .. 4.2 V).
constexpr int kBatteryMinMv = 3000;
constexpr int kBatteryMaxMv = 4200;

constexpr uint32_t kRefreshIntervalMs = 500u;
constexpr uint32_t kBatteryIntervalMs = 10000u;

bool s_ready = false;

esp_lcd_panel_io_handle_t s_panel_io = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;

lv_display_t *s_disp = nullptr;
uint8_t *s_draw_buf = nullptr;
#if NRL_DISPLAY_BUS_RGB
esp_lcd_touch_handle_t s_touch = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
lv_indev_t *s_touch_indev = nullptr;
#endif

lv_obj_t *s_lbl_caption = nullptr;
lv_obj_t *s_lbl_callsign = nullptr;
lv_obj_t *s_lbl_ssid = nullptr;
lv_obj_t *s_lbl_time = nullptr;
lv_obj_t *s_lbl_wifi = nullptr;
lv_obj_t *s_lbl_vol = nullptr;
lv_obj_t *s_lbl_batt = nullptr;
lv_obj_t *s_lbl_ip = nullptr;
lv_obj_t *s_lbl_hint = nullptr;

adc_oneshot_unit_handle_t s_adc = nullptr;
adc_cali_handle_t s_adc_cali = nullptr;
bool s_adc_ready = false;

uint32_t s_last_refresh_ms = 0u;
uint32_t s_last_battery_ms = 0u;
int s_battery_mv = 0;
bool s_time_sync_started = false;

// Cached on-screen text, so labels are only rewritten when a value changes.
char s_shown_callsign[16] = {};
char s_shown_ssid[16] = {};
char s_shown_time[16] = {};
char s_shown_wifi[28] = {};
char s_shown_vol[16] = {};
char s_shown_batt[20] = {};
char s_shown_ip[96] = {};
int s_shown_state = -1;  // caption: -1 unset, 0 standby, 1 last heard, 2 rx, 3 tx

void refreshVolume();

//============================ Panel bring-up =================================

bool initPanel()
{
#if NRL_PIN_DISPLAY_BL >= 0
    // Backlight off until the first frame is drawn, to avoid a white flash.
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << NRL_PIN_DISPLAY_BL;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl_cfg);
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_DISPLAY_BL), 0);
#endif

#if NRL_DISPLAY_BUS_RGB
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
    panel_cfg.disp_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_DISP);
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
        ESP_LOGI(TAG,"[LCD] RGB panel init failed");
        return false;
    }
#else
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = NRL_PIN_DISPLAY_SCLK;
    bus_cfg.mosi_io_num = NRL_PIN_DISPLAY_MOSI;
    bus_cfg.miso_io_num = -1;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = kWidth * kBufLines * static_cast<int>(sizeof(uint16_t));
    if (spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] SPI bus init failed");
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = static_cast<gpio_num_t>(NRL_PIN_DISPLAY_CS);
    io_cfg.dc_gpio_num = static_cast<gpio_num_t>(NRL_PIN_DISPLAY_DC);
    io_cfg.spi_mode = 3;
    io_cfg.pclk_hz = 80 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi(SPI3_HOST, &io_cfg, &s_panel_io) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] panel IO init failed");
        return false;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(NRL_PIN_DISPLAY_RST);
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] ST7789 init failed");
        return false;
    }
#endif

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
#if NRL_DISPLAY_BUS_RGB
    esp_lcd_panel_invert_color(s_panel, false);
#else
    esp_lcd_panel_invert_color(s_panel, true);
#endif
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return true;
}

//============================ LVGL <-> esp_lcd ===============================

// LVGL time base: millis() is a 32-bit millisecond counter, exactly what LVGL
// wants from its tick callback.
uint32_t lvglTick()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

// esp_lcd finished pushing a chunk over SPI -> let LVGL reuse the buffer.
// Runs in the SPI ISR; lv_display_flush_ready() only flips a flag, so this is
// safe here.
bool onColorTransDone(esp_lcd_panel_io_handle_t /*io*/,
                      esp_lcd_panel_io_event_data_t * /*edata*/,
                      void *user_ctx)
{
    lv_display_flush_ready(static_cast<lv_display_t *>(user_ctx));
    return false;
}

// LVGL hands us a rendered chunk; byte-swap it (ST7789-over-SPI is big-endian
// RGB565, LVGL renders little-endian) and DMA it to the panel.
void lvglFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel =
        static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));

#if NRL_DISPLAY_BUS_ST7789
    const int32_t count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    uint16_t *pixels = reinterpret_cast<uint16_t *>(px_map);
    for (int32_t i = 0; i < count; ++i) {
        pixels[i] = static_cast<uint16_t>((pixels[i] >> 8) | (pixels[i] << 8));
    }
#endif

    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
#if NRL_DISPLAY_BUS_RGB
    lv_display_flush_ready(disp);
#endif
}

bool initLvgl()
{
    lv_init();

    const size_t buf_bytes = static_cast<size_t>(kWidth) * kBufLines * 2u;
#if NRL_DISPLAY_BUS_RGB
    s_draw_buf = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    s_draw_buf = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA));
#endif
    if (s_draw_buf == nullptr) {
        ESP_LOGI(TAG,"[LCD] draw buffer alloc failed");
        return false;
    }

    s_disp = lv_display_create(kWidth, kHeight);
    if (s_disp == nullptr) {
        ESP_LOGI(TAG,"[LCD] lv_display_create failed");
        return false;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_flush_cb(s_disp, lvglFlush);
    lv_display_set_buffers(s_disp, s_draw_buf, nullptr, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_tick_set_cb(lvglTick);

#if NRL_DISPLAY_BUS_ST7789
    esp_lcd_panel_io_callbacks_t io_cbs = {};
    io_cbs.on_color_trans_done = onColorTransDone;
    esp_lcd_panel_io_register_event_callbacks(s_panel_io, &io_cbs, s_disp);
#endif
    return true;
}

#if NRL_DISPLAY_BUS_RGB
void touchRead(lv_indev_t * /*indev*/, lv_indev_data_t *data)
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
    i2c_master_bus_handle_t i2c_bus = nullptr;
    if (!S31_I2C_GetBus(&i2c_bus)) {
        ESP_LOGW(TAG, "[LCD] touch I2C unavailable");
        return false;
    }

    esp_lcd_touch_config_t touch_cfg = {};
    touch_cfg.x_max = kWidth;
    touch_cfg.y_max = kHeight;
    touch_cfg.rst_gpio_num = GPIO_NUM_NC;
    touch_cfg.int_gpio_num = GPIO_NUM_NC;
    touch_cfg.levels.reset = 0;
    touch_cfg.levels.interrupt = 0;

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &s_touch_io) != ESP_OK) {
        ESP_LOGW(TAG, "[LCD] touch IO init failed");
        return false;
    }
    if (esp_lcd_touch_new_i2c_gt1151(s_touch_io, &touch_cfg, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "[LCD] GT1151 init failed");
        return false;
    }

    s_touch_indev = lv_indev_create();
    if (s_touch_indev == nullptr) {
        ESP_LOGW(TAG, "[LCD] touch LVGL indev init failed");
        return false;
    }
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_touch_indev, s_disp);
    lv_indev_set_read_cb(s_touch_indev, touchRead);
    ESP_LOGI(TAG, "[LCD] GT1151 touch ready");
    return true;
}

void adjustTouchVolume(const int delta)
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
    if (volume == static_cast<int>(cfg->line_out_volume)) {
        return;
    }
    EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), true);
    refreshVolume();
}

void onTouchButton(lv_event_t *event)
{
    const intptr_t id = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (id < 0) {
        adjustTouchVolume(-16);
    } else if (id > 0) {
        adjustTouchVolume(16);
    } else {
        ESP_LOGI(TAG, "[LCD] config touch action");
    }
}
#endif

//============================ ADC battery sense ==============================

void initBatteryAdc()
{
#if defined(NRL_HAS_BATTERY_ADC) && NRL_HAS_BATTERY_ADC
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] battery ADC unit init failed");
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(s_adc, NRL_BATTERY_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] battery ADC channel config failed");
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.chan = NRL_BATTERY_ADC_CHANNEL;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
        s_adc_ready = true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!s_adc_ready) {
        adc_cali_line_fitting_config_t cali_cfg = {};
        cali_cfg.unit_id = ADC_UNIT_1;
        cali_cfg.atten = ADC_ATTEN_DB_12;
        cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
            s_adc_ready = true;
        }
    }
#endif
    if (!s_adc_ready) {
        ESP_LOGI(TAG,"[LCD] battery ADC not calibrated (eFuse missing) -- voltage hidden");
    }
#else
    ESP_LOGI(TAG,"[LCD] battery ADC not present on this board");
#endif
}

// Reads the raw (uncalibrated) battery voltage in millivolts, or 0 if
// unavailable. The sense pin sits behind a 1:2 divider, so the ADC reading is
// multiplied by 3 to match the 小智 格子派 board's measurement.
int readBatteryRawMv()
{
#if defined(NRL_HAS_BATTERY_ADC) && NRL_HAS_BATTERY_ADC
    if (!s_adc_ready || s_adc == nullptr || s_adc_cali == nullptr) {
        return 0;
    }

    long sum = 0;
    int samples = 0;
    for (int i = 0; i < 16; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, NRL_BATTERY_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
            ++samples;
        }
    }
    if (samples == 0) {
        return 0;
    }

    int voltage = 0;
    if (adc_cali_raw_to_voltage(s_adc_cali, static_cast<int>(sum / samples), &voltage) != ESP_OK) {
        return 0;
    }
    return voltage * 3;
#else
    return 0;
#endif
}

// Applies the persisted calibration multiplier (battery_cal_milli / 1000) to
// the raw voltage. Falls back to the raw value if the config cannot be read.
int applyBatteryCalibration(const int raw_mv)
{
    if (raw_mv <= 0) {
        return 0;
    }
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    const unsigned scale = (cfg != nullptr && cfg->battery_cal_milli != 0u)
                               ? cfg->battery_cal_milli
                               : 1000u;
    return static_cast<int>((static_cast<long>(raw_mv) * static_cast<long>(scale) + 500L) / 1000L);
}

int readBatteryMv()
{
    return applyBatteryCalibration(readBatteryRawMv());
}

//================================ UI build ===================================

lv_obj_t *makeBar(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kWidth, height);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorBar), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    return label;
}

#if NRL_DISPLAY_BUS_RGB
lv_obj_t *makePanel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x1C2B3D), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

lv_obj_t *makeTouchButton(lv_obj_t *parent, int x, int y, int w, int h,
                          const char *text, intptr_t id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x142033), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D4E63), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x29445E), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_add_event_cb(btn, onTouchButton, LV_EVENT_CLICKED, reinterpret_cast<void *>(id));

    lv_obj_t *label = makeLabel(btn, &lv_font_montserrat_20, kColorCallIdle);
    lv_obj_center(label);
    lv_label_set_text(label, text);
    return btn;
}

void buildWideUi()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = makeBar(scr, 0, 52);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x0A111B), 0);

    s_lbl_wifi = makeLabel(top, &lv_font_montserrat_20, kColorSub);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_LEFT_MID, 22, 0);
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI "  --");

    s_lbl_time = makeLabel(top, &lv_font_montserrat_28, kColorTime);
    lv_obj_center(s_lbl_time);
    lv_label_set_text(s_lbl_time, "--:--:--");

    s_lbl_vol = makeLabel(top, &lv_font_montserrat_20, kColorSub);
    lv_obj_align(s_lbl_vol, LV_ALIGN_RIGHT_MID, -22, 0);
    lv_label_set_text(s_lbl_vol, LV_SYMBOL_VOLUME_MID " --");

    lv_obj_t *accent = lv_obj_create(scr);
    lv_obj_remove_style_all(accent);
    lv_obj_set_pos(accent, 0, 52);
    lv_obj_set_size(accent, kWidth, 2);
    lv_obj_set_style_bg_color(accent, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);

    lv_obj_t *left = makePanel(scr, 22, 76, 456, 260);
    s_lbl_caption = makeLabel(left, &lv_font_montserrat_20, kColorCaption);
    lv_obj_align(s_lbl_caption, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_lbl_caption, "STANDBY");

    s_lbl_callsign = makeLabel(left, &lv_font_montserrat_48, kColorCallIdle);
    lv_obj_set_width(s_lbl_callsign, 428);
    lv_obj_set_style_text_align(s_lbl_callsign, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_lbl_callsign, LV_ALIGN_TOP_LEFT, 0, 46);
    lv_label_set_text(s_lbl_callsign, "----");

    s_lbl_ssid = makeLabel(left, &lv_font_montserrat_28, kColorSub);
    lv_obj_align(s_lbl_ssid, LV_ALIGN_TOP_LEFT, 2, 118);
    lv_label_set_text(s_lbl_ssid, "SSID -");

    s_lbl_hint = makeLabel(left, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_hint, 428);
    lv_obj_align(s_lbl_hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(s_lbl_hint, "NRL voice bridge");

    lv_obj_t *right = makePanel(scr, 500, 76, 278, 260);
    lv_obj_t *net_title = makeLabel(right, &lv_font_montserrat_16, kColorCaption);
    lv_obj_align(net_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(net_title, "NETWORK");

    s_lbl_ip = makeLabel(right, &lv_font_montserrat_20, kColorIp);
    lv_obj_set_width(s_lbl_ip, 250);
    lv_obj_align(s_lbl_ip, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_ip, "---");

    lv_obj_t *audio_title = makeLabel(right, &lv_font_montserrat_16, kColorCaption);
    lv_obj_align(audio_title, LV_ALIGN_TOP_LEFT, 0, 104);
    lv_label_set_text(audio_title, "POWER");

    s_lbl_batt = makeLabel(right, &lv_font_montserrat_20, kColorSub);
    lv_obj_align(s_lbl_batt, LV_ALIGN_TOP_LEFT, 0, 138);
    lv_label_set_text(s_lbl_batt, "--  " LV_SYMBOL_BATTERY_EMPTY);

    makeTouchButton(scr, 22, 362, 180, 78, "VOL-", -1);
    makeTouchButton(scr, 222, 362, 356, 78, "CONFIG", 0);
    makeTouchButton(scr, 598, 362, 180, 78, "VOL+", 1);
}
#endif

void buildUi()
{
#if NRL_DISPLAY_BUS_RGB
    buildWideUi();
    return;
#endif

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Top status bar ----
    lv_obj_t *top = makeBar(scr, 0, 34);

    lv_obj_t *accent = lv_obj_create(scr);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, kWidth, 2);
    lv_obj_set_pos(accent, 0, 34);
    lv_obj_set_style_bg_color(accent, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);

    s_lbl_wifi = makeLabel(top, &lv_font_montserrat_14, kColorSub);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI "  --");

    s_lbl_vol = makeLabel(top, &lv_font_montserrat_14, kColorSub);
    lv_obj_align(s_lbl_vol, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_lbl_vol, LV_SYMBOL_VOLUME_MID " --");

    s_lbl_batt = makeLabel(top, &lv_font_montserrat_14, kColorSub);
    lv_obj_align(s_lbl_batt, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_label_set_text(s_lbl_batt, "--  " LV_SYMBOL_BATTERY_EMPTY);

    // ---- Centre content ----
    s_lbl_caption = makeLabel(scr, &lv_font_montserrat_14, kColorCaption);
    lv_obj_align(s_lbl_caption, LV_ALIGN_TOP_MID, 0, 50);
    lv_label_set_text(s_lbl_caption, "STANDBY");

    s_lbl_callsign = makeLabel(scr, &lv_font_montserrat_48, kColorCallIdle);
    lv_obj_set_width(s_lbl_callsign, kWidth);
    lv_obj_set_style_text_align(s_lbl_callsign, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_callsign, LV_ALIGN_TOP_MID, 0, 68);
    lv_label_set_text(s_lbl_callsign, "----");

    s_lbl_ssid = makeLabel(scr, &lv_font_montserrat_20, kColorSub);
    lv_obj_align(s_lbl_ssid, LV_ALIGN_TOP_MID, 0, 130);
    lv_label_set_text(s_lbl_ssid, "SSID -");

    s_lbl_time = makeLabel(scr, &lv_font_montserrat_28, kColorTime);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_MID, 0, 164);
    lv_label_set_text(s_lbl_time, "--:--:--");

    // ---- Bottom IP bar ----
    lv_obj_t *bottom = makeBar(scr, kHeight - 34, 34);
    s_lbl_ip = makeLabel(bottom, &lv_font_montserrat_16, kColorIp);
    lv_obj_center(s_lbl_ip);
    lv_label_set_text(s_lbl_ip, "---");
}

//================================ Refresh ====================================

// Updates a label only when its text actually changed (avoids redraw churn).
bool setLabel(lv_obj_t *label, char *cache, size_t cache_size, const char *text)
{
    if (strncmp(cache, text, cache_size) == 0) {
        return false;
    }
    snprintf(cache, cache_size, "%.*s", static_cast<int>(cache_size - 1u), text);
    lv_label_set_text(label, text);
    return true;
}

void refreshCaller()
{
    char voice_call[8] = {};
    unsigned voice_ssid = 0u;
    const bool rx = NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid);
    const bool tx = STATUS_IO_IsSqlActive();

    // While a voice stream is actually being received, the main area shows the
    // remote caller. Otherwise it shows this device's own callsign/SSID, read
    // straight from the local config -- heartbeats never feed this.
    char call_text[16];
    char ssid_text[16];
    if (rx && voice_call[0] != '\0') {
        snprintf(call_text, sizeof(call_text), "%s", voice_call);
        snprintf(ssid_text, sizeof(ssid_text), "SSID %u", voice_ssid);
    } else {
        const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
        if (cfg != nullptr && cfg->callsign[0] != '\0') {
            snprintf(call_text, sizeof(call_text), "%s", cfg->callsign);
            snprintf(ssid_text, sizeof(ssid_text), "SSID %u",
                     static_cast<unsigned>(cfg->callsign_ssid));
        } else {
            snprintf(call_text, sizeof(call_text), "----");
            snprintf(ssid_text, sizeof(ssid_text), "SSID -");
        }
    }

    setLabel(s_lbl_callsign, s_shown_callsign, sizeof(s_shown_callsign), call_text);
    setLabel(s_lbl_ssid, s_shown_ssid, sizeof(s_shown_ssid), ssid_text);

    // Status caption above the callsign. Transmitting while also receiving is
    // shown as full duplex; otherwise TX wins over RX wins over standby.
    int state;
    if (tx && rx) {
        state = 4;  // full duplex
    } else if (tx) {
        state = 3;  // transmitting
    } else if (rx) {
        state = 2;  // receiving
    } else {
        state = 0;  // standby
    }

    if (state != s_shown_state) {
        s_shown_state = state;
        const char *caption;
        uint32_t caption_color;
        uint32_t call_color;
        switch (state) {
            case 4:
                caption = "FULL DUPLEX";  caption_color = kColorDuplex;
                call_color = kColorCallLive;
                break;
            case 3:
                caption = "TRANSMITTING"; caption_color = kColorTx;
                call_color = kColorCallIdle;
                break;
            case 2:
                caption = "RECEIVING";    caption_color = kColorAccent;
                call_color = kColorCallLive;
                break;
            default:
                caption = "STANDBY";      caption_color = kColorCaption;
                call_color = kColorCallIdle;
                break;
        }
        lv_label_set_text(s_lbl_caption, caption);
        lv_obj_set_style_text_color(s_lbl_caption, lv_color_hex(caption_color), 0);
        lv_obj_set_style_text_color(s_lbl_callsign, lv_color_hex(call_color), 0);
    }
}

void refreshClock()
{
    if (!s_time_sync_started && nrlWifiStaConnected()) {
        // CST-8 == UTC+8 (China). NTP servers are tried in order.
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "ntp.ntsc.ac.cn");
        esp_sntp_setservername(2, "pool.ntp.org");
        esp_sntp_init();
        s_time_sync_started = true;
    }

    char time_text[16];
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 >= 2024) {
        snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d",
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    } else {
        snprintf(time_text, sizeof(time_text), "--:--:--");
    }
    setLabel(s_lbl_time, s_shown_time, sizeof(s_shown_time), time_text);
}

void refreshWifi()
{
    char wifi_text[28];
    uint32_t color = kColorSub;
    if (nrlWifiStaConnected()) {
        wifi_ap_record_t ap_info = {};
        const int rssi = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ? ap_info.rssi : 0;
        snprintf(wifi_text, sizeof(wifi_text), LV_SYMBOL_WIFI "  %ddB", rssi);
        if (rssi >= -65) {
            color = kColorGood;
        } else if (rssi >= -78) {
            color = kColorApWarn;
        } else {
            color = kColorWeak;
        }
    } else {
        snprintf(wifi_text, sizeof(wifi_text), LV_SYMBOL_WIFI "  AP");
        color = kColorApWarn;
    }
    if (setLabel(s_lbl_wifi, s_shown_wifi, sizeof(s_shown_wifi), wifi_text)) {
        lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(color), 0);
    }
}

void refreshVolume()
{
    char vol_text[16];
    uint32_t color = kColorSub;
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg != nullptr) {
        // line_out_volume is the ES8311 speaker volume, stored 0..255.
        const int pct = (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255;
        const char *symbol = LV_SYMBOL_VOLUME_MAX;
        if (pct == 0) {
            symbol = LV_SYMBOL_MUTE;
            color = kColorWeak;
        } else if (pct < 55) {
            symbol = LV_SYMBOL_VOLUME_MID;
        }
        snprintf(vol_text, sizeof(vol_text), "%s %d%%", symbol, pct);
    } else {
        snprintf(vol_text, sizeof(vol_text), LV_SYMBOL_VOLUME_MID " --");
    }
    if (setLabel(s_lbl_vol, s_shown_vol, sizeof(s_shown_vol), vol_text)) {
        lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(color), 0);
    }
}

void refreshBattery()
{
    char batt_text[20];
    uint32_t color = kColorSub;
    if (s_adc_ready && s_battery_mv > 0) {
        int pct = (s_battery_mv - kBatteryMinMv) * 100 / (kBatteryMaxMv - kBatteryMinMv);
        if (pct < 0) {
            pct = 0;
        } else if (pct > 100) {
            pct = 100;
        }
        const char *symbol = LV_SYMBOL_BATTERY_EMPTY;
        if (pct >= 80) {
            symbol = LV_SYMBOL_BATTERY_FULL;
        } else if (pct >= 55) {
            symbol = LV_SYMBOL_BATTERY_3;
        } else if (pct >= 30) {
            symbol = LV_SYMBOL_BATTERY_2;
        } else if (pct >= 12) {
            symbol = LV_SYMBOL_BATTERY_1;
        }
        snprintf(batt_text, sizeof(batt_text), "%d.%02dV  %s",
                 s_battery_mv / 1000, (s_battery_mv % 1000) / 10, symbol);
        color = (pct <= 15) ? kColorWeak : kColorSub;
    } else {
        snprintf(batt_text, sizeof(batt_text), "--  %s", LV_SYMBOL_BATTERY_EMPTY);
    }
    if (setLabel(s_lbl_batt, s_shown_batt, sizeof(s_shown_batt), batt_text)) {
        lv_obj_set_style_text_color(s_lbl_batt, lv_color_hex(color), 0);
    }
}

void refreshIp()
{
    char ip_text[96];
    uint32_t color;
    // The bar shows the bare address only -- no icon, no prefix label -- so a
    // long host/IP still fits. The text colour alone marks the state:
    // red = transmitting, cyan = receiving voice, blue = STA IP, amber = AP.
    char rx_call[8];
    unsigned rx_ssid = 0u;
    const bool rx = NRLAudioBridge_GetRemoteCaller(rx_call, sizeof(rx_call), &rx_ssid);
    const bool tx = STATUS_IO_IsSqlActive();
    if (tx || rx) {
        // Transmitting or receiving voice -> show the configured NRL server
        // host (the host string as configured, not a resolved IP address).
        const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
        const char *host = (cfg != nullptr && cfg->server_host[0] != '\0')
                               ? cfg->server_host
                               : "---";
        snprintf(ip_text, sizeof(ip_text), "%s", host);
        color = tx ? kColorTx : kColorAccent;
    } else if (nrlWifiStaConnected()) {
        char sta_buf[16] = {};
        nrlIpToString(nrlWifiStaIp(), sta_buf, sizeof(sta_buf));
        snprintf(ip_text, sizeof(ip_text), "%s", sta_buf);
        color = kColorIp;
    } else {
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
            // WiFi join failed -> config AP is up; show the hotspot address.
            char ap_buf[16] = {};
            nrlIpToString(nrlWifiApIp(), ap_buf, sizeof(ap_buf));
            snprintf(ip_text, sizeof(ip_text), "%s", ap_buf);
            color = kColorApWarn;
        } else {
            snprintf(ip_text, sizeof(ip_text), "---");
            color = kColorSub;
        }
    }
    if (setLabel(s_lbl_ip, s_shown_ip, sizeof(s_shown_ip), ip_text)) {
        lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(color), 0);
    }
}

} // namespace

//================================ Public API =================================

extern "C" void Display_Init(void)
{
    if (s_ready) {
        return;
    }
    if (!initPanel()) {
        return;
    }
    if (!initLvgl()) {
        return;
    }
#if NRL_DISPLAY_BUS_RGB
    initTouch();
#endif

    initBatteryAdc();
    s_battery_mv = readBatteryMv();

    buildUi();
    lv_refr_now(nullptr);  // paint the first frame before the backlight is lit

#if NRL_PIN_DISPLAY_BL >= 0
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_DISPLAY_BL), 1);
#endif
    s_ready = true;
    ESP_LOGI(TAG,"[LCD] display ready");
}

extern "C" void Display_Poll(void)
{
    if (!s_ready) {
        return;
    }

    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (s_last_battery_ms == 0u || (now - s_last_battery_ms) >= kBatteryIntervalMs) {
        s_last_battery_ms = now;
        s_battery_mv = readBatteryMv();
    }

    // The caller caption, IP bar and volume readout react to PTT / button
    // presses, so refresh them every poll for snappy feedback. setLabel()
    // still only redraws when the text actually changed.
    refreshCaller();
    refreshIp();
    refreshVolume();

    if (s_last_refresh_ms == 0u || (now - s_last_refresh_ms) >= kRefreshIntervalMs) {
        s_last_refresh_ms = now;
        refreshClock();
        refreshWifi();
        refreshBattery();
    }

    lv_timer_handler();
}

extern "C" int Display_GetBatteryRawMv(void)
{
    return readBatteryRawMv();
}

extern "C" int Display_GetBatteryCalibratedMv(void)
{
    return applyBatteryCalibration(readBatteryRawMv());
}

// CJK font engine switching exists only on the S31 800x480 panel.
extern "C" bool Display_SetCjkFontEngine(int) { return false; }
extern "C" int Display_GetCjkFontEngine(void) { return DISPLAY_CJK_FONT_BITMAP; }

// Framebuffer benchmark exists only on the S31 RGB panel.
extern "C" long Display_FramebufferBenchMBps(void) { return -1; }

#elif NRL_BOARD != NRL_BOARD_S31_KORVO

extern "C" void Display_Init(void) {}
extern "C" void Display_Poll(void) {}
extern "C" int Display_GetBatteryRawMv(void) { return 0; }
extern "C" int Display_GetBatteryCalibratedMv(void) { return 0; }
extern "C" bool Display_SetCjkFontEngine(int) { return false; }
extern "C" int Display_GetCjkFontEngine(void) { return DISPLAY_CJK_FONT_BITMAP; }
extern "C" long Display_FramebufferBenchMBps(void) { return -1; }

#endif
