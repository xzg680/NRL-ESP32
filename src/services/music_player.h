#ifndef SRC_SERVICES_MUSIC_PLAYER_H
#define SRC_SERVICES_MUSIC_PLAYER_H

// Music player service (docs/architecture.md Phase 1). Plays audio files
// from mounted storage (/sdcard, /usb) through the hi-fi speaker path at
// their native sample rate. Voice interrupts music: when NRL downlink
// playback starts, the current track is stopped and the voice path restored
// (interrupt policy; ducking/mixing come later).
//
// Currently WAV/PCM 16-bit only; MP3/FLAC/APE decoders arrive as plugins.
// On boards without the ES8389 hi-fi path MUSIC_PlayFile simply fails.

#include <stdbool.h>

#include "media/media_metadata.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the voice-interrupt hook. Safe to call on every board.
void MUSIC_Init(void);

// Start playing `path` (e.g. "/sdcard/music/song.wav"). Stops any current
// track first. Returns false if the file is missing/unsupported or the
// hi-fi path is unavailable.
bool MUSIC_PlayFile(const char *path);

// Request stop; returns immediately, the player task winds down and
// restores the voice path within one buffer duration.
void MUSIC_Stop(void);

bool MUSIC_IsPlaying(void);

// Path of the current (or last) track; empty string when none.
const char *MUSIC_CurrentPath(void);

// Tags/cover of the current track (parsed when playback starts). The
// returned pointer -- including cover_data -- stays valid until the next
// MUSIC_PlayFile; UI consumers should render promptly after a track change.
const MediaTrackInfo *MUSIC_GetTrackInfo(void);

// Called when a track finishes decoding to the end (NOT on MUSIC_Stop,
// voice interrupt, or decode error). Fired from the player task after it
// has released the codec, so the callback may start the next track.
typedef void (*MusicTrackEndCb_t)(void);
void MUSIC_SetTrackEndCallback(MusicTrackEndCb_t callback);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_MUSIC_PLAYER_H
