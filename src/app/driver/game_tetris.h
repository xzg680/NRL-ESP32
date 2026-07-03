#ifndef DRIVER_GAME_TETRIS_H
#define DRIVER_GAME_TETRIS_H

// Tetris for the S31 800x480 touch panel (docs/architecture.md 小游戏).
// Self-contained LVGL page: display_s31 hands over a cleared screen and an
// exit callback; the game owns its widgets and an lv_timer for gravity.
// Teardown is idempotent and must run when the page is left (display_s31
// calls it from clearScreen()).

#include "board_pins.h"

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && NRL_BOARD == NRL_BOARD_S31_KORVO

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build the game UI on `screen` (already cleared). `exit_cb` is invoked when
// the user taps Back.
void GAME_TETRIS_Build(lv_obj_t *screen, void (*exit_cb)(void));

// Stop the gravity timer and forget widget pointers. Safe to call anytime.
void GAME_TETRIS_Teardown(void);

#ifdef __cplusplus
}
#endif

#endif // S31

#endif // DRIVER_GAME_TETRIS_H
