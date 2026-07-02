#ifndef SRC_SERVICES_MUSIC_PLAYLIST_H
#define SRC_SERVICES_MUSIC_PLAYLIST_H

// Track list for the music player (docs/architecture.md 3.6): scans the
// music directories of every online mount (/sdcard/music, /usb/music) for
// supported extensions, keeps an alphabetically sorted path list in PSRAM
// and drives MUSIC_PlayFile with next/prev/auto-advance. The nanny service
// reuses this list for its playback rotation.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registers the track-end hook for auto-advance. Call once at startup
// (after MUSIC_Init).
void PLAYLIST_Init(void);

// (Re)scan the music directories. Returns the number of tracks found.
// Called at startup and when storage mounts change.
size_t PLAYLIST_Scan(void);

size_t PLAYLIST_Count(void);

// Path of entry `index` (NULL when out of range).
const char *PLAYLIST_GetPath(size_t index);

// Index of the entry currently playing, or -1 when idle.
int PLAYLIST_CurrentIndex(void);

// Start playing entry `index`; wraps the current index for next/prev.
bool PLAYLIST_PlayIndex(size_t index);

// Relative navigation (wraps around). No-ops on an empty list.
bool PLAYLIST_Next(void);
bool PLAYLIST_Prev(void);

// Auto-advance to the next track when one finishes naturally (default on).
void PLAYLIST_SetAutoAdvance(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_MUSIC_PLAYLIST_H
