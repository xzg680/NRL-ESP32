#ifndef DRIVER_DISPLAY_BI4UMD_H
#define DRIVER_DISPLAY_BI4UMD_H

#include <stddef.h>
#include <stdint.h>

#include <esp_lcd_panel_io.h>

// BI4UMD-specific ILI9341V panel driver. The shared display module owns LVGL;
// this module owns the panel bus, controller commands and backlight.
bool BI4UMD_Display_Init(esp_lcd_panel_io_handle_t *panel_io);
bool BI4UMD_Display_Flush(esp_lcd_panel_io_handle_t panel_io,
                         int x1, int y1, int x2, int y2,
                         uint8_t *pixels, size_t pixel_count);
void BI4UMD_Display_SetBacklight(bool enabled);

#endif // DRIVER_DISPLAY_BI4UMD_H
