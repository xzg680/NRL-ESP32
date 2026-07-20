#pragma once

#include <stdint.h>

enum DisplayNoticeLevel : uint8_t {
    DISPLAY_NOTICE_INFO = 0,
    DISPLAY_NOTICE_SUCCESS,
    DISPLAY_NOTICE_WARNING,
    DISPLAY_NOTICE_ERROR,
};

struct DisplayNoticeSnapshot {
    char text[96];
    DisplayNoticeLevel level;
    uint32_t posted_ms;
    uint32_t duration_ms; // 0 keeps the notice visible until another is posted.
    uint32_t sequence;
    int8_t progress_percent; // -1 hides the optional progress bar.
};

// Safe to call from worker tasks. LCD drivers consume the copied snapshot from
// Display_Poll(), so callers never touch LVGL outside the display task.
void DISPLAY_NOTICE_Post(const char *text, DisplayNoticeLevel level, uint32_t duration_ms);
void DISPLAY_NOTICE_PostProgress(const char *text, DisplayNoticeLevel level,
                                 uint32_t duration_ms, uint8_t progress_percent);
void DISPLAY_NOTICE_Get(DisplayNoticeSnapshot *out);
