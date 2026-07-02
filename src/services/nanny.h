#ifndef SRC_SERVICES_NANNY_H
#define SRC_SERVICES_NANNY_H

// Nanny beacon scheduler (docs/architecture.md 功能2): plays a beacon audio
// file (station ID / announcement) every N minutes through the music player,
// honouring the configured playback target (local / NRL network / both).
// A beacon interrupts the current track; when it finishes the playlist
// auto-advance resumes normal rotation. Beacons are postponed while an NRL
// voice stream is playing. Configuration persists in NVS.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load persisted config and start the scheduler tick. Call once at startup
// (after MUSIC_Init / PLAYLIST_Init).
void NANNY_Init(void);

// Arm the beacon: play `path` every `interval_min` minutes (1-1440).
// Persists and takes effect immediately.
bool NANNY_SetBeacon(const char *path, uint32_t interval_min);

// Disarm and erase the persisted config.
void NANNY_DisableBeacon(void);

// Current config; returns false when disarmed.
bool NANNY_GetBeacon(char *path_out, size_t path_size, uint32_t *interval_min);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_NANNY_H
