#include "audio/audio_focus.h"

#include <stddef.h>

static volatile AudioFocusVoiceStartCb_t s_voice_start_cb = NULL;

extern "C" void AudioFocus_RegisterVoiceStart(AudioFocusVoiceStartCb_t callback)
{
    s_voice_start_cb = callback;
}

extern "C" void AudioFocus_NotifyVoiceStart(void)
{
    const AudioFocusVoiceStartCb_t cb = s_voice_start_cb;
    if (cb != NULL) {
        cb();
    }
}
