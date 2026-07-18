#include "audio/audio_focus.h"

#include <freertos/FreeRTOS.h>

#include <stddef.h>

static volatile AudioFocusVoiceCb_t s_voice_start_cb = NULL;
static volatile AudioFocusVoiceCb_t s_voice_end_cb = NULL;
static portMUX_TYPE s_focus_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_focus_owners = 0u;

extern "C" void AudioFocus_RegisterVoiceStart(AudioFocusVoiceCb_t callback)
{
    bool already_active = false;
    portENTER_CRITICAL(&s_focus_mux);
    s_voice_start_cb = callback;
    already_active = s_focus_owners > 0u;
    portEXIT_CRITICAL(&s_focus_mux);
    if (already_active && callback != NULL) {
        callback();
    }
}

extern "C" void AudioFocus_RegisterVoiceEnd(AudioFocusVoiceCb_t callback)
{
    s_voice_end_cb = callback;
}

extern "C" void AudioFocus_NotifyVoiceStart(void)
{
    bool first_owner = false;
    portENTER_CRITICAL(&s_focus_mux);
    first_owner = (s_focus_owners++ == 0u);
    portEXIT_CRITICAL(&s_focus_mux);
    const AudioFocusVoiceCb_t cb = s_voice_start_cb;
    if (first_owner && cb != NULL) {
        cb();
    }
}

extern "C" void AudioFocus_NotifyVoiceEnd(void)
{
    bool final_owner = false;
    portENTER_CRITICAL(&s_focus_mux);
    if (s_focus_owners > 0u) {
        --s_focus_owners;
        final_owner = (s_focus_owners == 0u);
    }
    portEXIT_CRITICAL(&s_focus_mux);
    const AudioFocusVoiceCb_t cb = s_voice_end_cb;
    if (final_owner && cb != NULL) {
        cb();
    }
}
