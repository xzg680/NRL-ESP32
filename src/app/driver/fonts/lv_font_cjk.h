#ifndef DRIVER_FONTS_LV_FONT_CJK_H
#define DRIVER_FONTS_LV_FONT_CJK_H

// Generated CJK bitmap fonts (scripts/gen_cjk_font.py): GB2312 level-1
// hanzi + CJK punctuation, Noto Sans SC. Compiled only for S31 (the sole
// board with the 800x480 LVGL panel); used as fallback of the Montserrat
// fonts so Chinese track tags render on the LCD.

#include "driver/board_pins.h"

#if NRL_BOARD == NRL_BOARD_S31_KORVO

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t lv_font_cjk_16;
extern const lv_font_t lv_font_cjk_20;

#ifdef __cplusplus
}
#endif

#endif // NRL_BOARD_S31_KORVO

#endif // DRIVER_FONTS_LV_FONT_CJK_H
