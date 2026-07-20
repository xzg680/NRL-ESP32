#ifndef SRC_SERVICES_MUSIC_PLAYLIST_H
#define SRC_SERVICES_MUSIC_PLAYLIST_H

// Track list for the music player (docs/architecture.md 3.6): browses the
// music directories of every online mount (/sdcard/music, /usb/music, /smb),
// keeps the current directory's subdirectories and supported tracks in PSRAM
// and drives MUSIC_PlayFile with next/prev/auto-advance.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registers the track-end hook for auto-advance. Call once at startup
// (after MUSIC_Init).
void PLAYLIST_Init(void);

// (Re)scan the current music directory. Returns the number of tracks found.
// Called at startup, when storage mounts change, and when the UI refreshes.
size_t PLAYLIST_Scan(void);

// Web directory browsing uses asynchronous scans so a slow SMB directory
// cannot hold the HTTP request open until the NAS finishes enumeration.
bool PLAYLIST_ScanAsync(void);
bool PLAYLIST_EnterDirAsync(size_t index);
bool PLAYLIST_UpAsync(void);
bool PLAYLIST_IsScanning(void);
bool PLAYLIST_LastScanOk(void);

// Current directory browser state. At the virtual root, DirCount lists the
// available sources (SD music, USB music, SMB share); inside a source it lists
// direct child directories. Track APIs below always refer only to the current
// directory's direct music files.
const char *PLAYLIST_CurrentDir(void);
bool PLAYLIST_AtRoot(void);
size_t PLAYLIST_DirCount(void);
const char *PLAYLIST_GetDirName(size_t index);
const char *PLAYLIST_GetDirPath(size_t index);
bool PLAYLIST_EnterDir(size_t index);
bool PLAYLIST_Up(void);
bool PLAYLIST_InFavorites(void);

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

typedef enum {
    PLAYLIST_REPEAT_LIST = 0,
    PLAYLIST_REPEAT_ONE = 1,
} PlaylistRepeatMode;

PlaylistRepeatMode PLAYLIST_GetRepeatMode(void);
void PLAYLIST_SetRepeatMode(PlaylistRepeatMode mode);
PlaylistRepeatMode PLAYLIST_ToggleRepeatMode(void);

enum {
    PLAYLIST_FAV_MAX = 128,
};

size_t PLAYLIST_FavoriteCount(void);
bool PLAYLIST_IsFavorite(const char *path);
bool PLAYLIST_ToggleFavorite(const char *path);

// Auto-advance to the next track when one finishes naturally (default on).
void PLAYLIST_SetAutoAdvance(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_MUSIC_PLAYLIST_H
