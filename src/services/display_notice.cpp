#include "services/display_notice.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace {

portMUX_TYPE s_notice_lock = portMUX_INITIALIZER_UNLOCKED;
DisplayNoticeSnapshot s_notice = {};

void postNotice(const char *text, DisplayNoticeLevel level, uint32_t duration_ms,
                int8_t progress_percent)
{
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    const char *source = text != nullptr ? text : "";
    portENTER_CRITICAL(&s_notice_lock);
    uint32_t i = 0u;
    while (i + 1u < sizeof(s_notice.text) && source[i] != '\0') {
        s_notice.text[i] = source[i];
        ++i;
    }
    s_notice.text[i] = '\0';
    s_notice.level = level;
    s_notice.posted_ms = now;
    s_notice.duration_ms = duration_ms;
    s_notice.progress_percent = progress_percent;
    ++s_notice.sequence;
    portEXIT_CRITICAL(&s_notice_lock);
}

} // namespace

void DISPLAY_NOTICE_Post(const char *text, DisplayNoticeLevel level, uint32_t duration_ms)
{
    postNotice(text, level, duration_ms, -1);
}

void DISPLAY_NOTICE_PostProgress(const char *text, DisplayNoticeLevel level,
                                 uint32_t duration_ms, uint8_t progress_percent)
{
    if (progress_percent > 100u) progress_percent = 100u;
    postNotice(text, level, duration_ms, static_cast<int8_t>(progress_percent));
}

void DISPLAY_NOTICE_Get(DisplayNoticeSnapshot *out)
{
    if (out == nullptr) return;
    portENTER_CRITICAL(&s_notice_lock);
    *out = s_notice;
    portEXIT_CRITICAL(&s_notice_lock);
}
