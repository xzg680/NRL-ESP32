#ifndef SRC_AUDIO_AUDIO_FOCUS_H
#define SRC_AUDIO_AUDIO_FOCUS_H

// Minimal audio-focus notification between the voice path and media
// playback (docs/architecture.md "域冲突策略", interrupt policy). The NRL
// bridge announces "voice is starting"; whoever holds the hi-fi speaker
// (music player) registers a callback and yields it. Grows into the App
// manager's arbitration in a later phase.

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*AudioFocusVoiceCb_t)(void);

// Register (or clear with NULL) the callback fired when network voice
// playback starts. Single listener is enough for the interrupt policy.
void AudioFocus_RegisterVoiceStart(AudioFocusVoiceCb_t callback);

// Register (or clear with NULL) the callback fired after every overlapping
// voice/signalling focus owner has released audio focus.
void AudioFocus_RegisterVoiceEnd(AudioFocusVoiceCb_t callback);

// Called by the NRL bridge right before downlink voice playback begins.
// Must be cheap and non-blocking; the callback only flags the media task
// to stop, it does not wait for it.
void AudioFocus_NotifyVoiceStart(void);

// Balances AudioFocus_NotifyVoiceStart(). Nested focus owners are counted, so
// the end callback fires only when the final owner releases focus.
void AudioFocus_NotifyVoiceEnd(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_AUDIO_AUDIO_FOCUS_H
