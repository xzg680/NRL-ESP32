// ESP-Mosaico panel glue: 480x480 QSPI AMOLED (CO5300).
//
// Power sequence: VCC_PW (GPIO60, low active) switches the VCC_3V3 rail that
// feeds the LCD connector; LCD_RST and the CST9220 touch reset share GPIO42,
// so the panel reset pulse doubles as the touch reset. Brightness is the
// CO5300 0x51 command -- AMOLED pixels emit directly, there is no backlight.

#include "display_mosaico_panel.h"

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_ESP_MOSAICO

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_lcd_co5300.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

const char *kTag = "MOSAICO_PANEL";

// CO5300 command bytes used outside the driver init sequence.
constexpr uint8_t kCmdWriteBrightness = 0x51;

esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_panel_io_handle_t s_panel_io = nullptr;

void panelRailPowerOn()
{
    gpio_reset_pin((gpio_num_t)NRL_PIN_LCD_PWR_EN);
    gpio_set_direction((gpio_num_t)NRL_PIN_LCD_PWR_EN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)NRL_PIN_LCD_PWR_EN, NRL_PIN_LCD_PWR_EN_ACTIVE_LEVEL);
    // Give the rail and the panel's own power-up sequence time to settle.
    vTaskDelay(pdMS_TO_TICKS(50));
}

} // namespace

extern "C" esp_lcd_panel_handle_t MosaicoPanel_Init(void)
{
    if (s_panel != nullptr) {
        return s_panel;
    }

    panelRailPowerOn();

    // The CO5300_PANEL_*_CONFIG macros use C designated initializers that trip
    // -Werror=missing-field-initializers in C++; build the configs field by
    // field instead.
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = NRL_PIN_LCD_QSPI_CLK;
    bus_cfg.data0_io_num = NRL_PIN_LCD_QSPI_D0;
    bus_cfg.data1_io_num = NRL_PIN_LCD_QSPI_D1;
    bus_cfg.data2_io_num = NRL_PIN_LCD_QSPI_D2;
    bus_cfg.data3_io_num = NRL_PIN_LCD_QSPI_D3;
    bus_cfg.max_transfer_sz = NRL_DISPLAY_WIDTH * 80 * sizeof(uint16_t);
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "QSPI bus init failed: %s", esp_err_to_name(err));
        return nullptr;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = (gpio_num_t)NRL_PIN_LCD_QSPI_CS;
    io_cfg.dc_gpio_num = GPIO_NUM_NC;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 32;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.quad_mode = true;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg,
                                 &s_panel_io) != ESP_OK) {
        ESP_LOGE(kTag, "panel IO create failed");
        return nullptr;
    }

    co5300_vendor_config_t vendor_cfg = {};
    vendor_cfg.flags.use_qspi_interface = 1;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = (gpio_num_t)NRL_PIN_LCD_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config = &vendor_cfg;
    if (esp_lcd_new_panel_co5300(s_panel_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(kTag, "CO5300 create failed");
        return nullptr;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(kTag, "CO5300 init failed");
        return nullptr;
    }
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(kTag, "CO5300 QSPI AMOLED ready (%dx%d)", NRL_DISPLAY_WIDTH,
             NRL_DISPLAY_HEIGHT);
    return s_panel;
}

extern "C" bool MosaicoPanel_SetBrightness(const uint8_t brightness)
{
    if (s_panel_io == nullptr) {
        return false;
    }
    return esp_lcd_panel_io_tx_param(s_panel_io, kCmdWriteBrightness,
                                     &brightness, 1) == ESP_OK;
}

extern "C" bool MosaicoPanel_SetDisplayOn(const bool on)
{
    if (s_panel == nullptr) {
        return false;
    }
    return esp_lcd_panel_disp_on_off(s_panel, on) == ESP_OK;
}

#endif // NRL_BOARD == NRL_BOARD_ESP_MOSAICO
