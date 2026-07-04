#ifndef SRC_SERVICES_RADIO_FAVORITES_H
#define SRC_SERVICES_RADIO_FAVORITES_H

// Net-radio favorite station list (name + stream URL), persisted in NVS.
// One shared list edited from three fronts: the web portal Media page, the
// LCD Radio page, and AT commands (AT+RADIOLIST / RADIOADD / RADIODEL /
// RADIOPLAY / RADIONEXT / RADIOPREV). Tuning a favorite also updates the
// single saved station (MUSIC_SetRadioUrl) so the legacy URL field follows.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RADIO_FAV_MAX = 16,       // list capacity
    RADIO_FAV_NAME_SIZE = 48, // station name buffer size (bytes, incl. NUL)
    RADIO_FAV_URL_SIZE = 200, // stream URL buffer size (bytes, incl. NUL)
};

// Load the list from NVS. Call once at startup (after nvs_flash_init).
void RADIO_FAV_Init(void);

size_t RADIO_FAV_Count(void);

// Copy entry `index` out; either destination may be NULL. Returns false when
// the index is out of range.
bool RADIO_FAV_Get(size_t index, char *name, size_t name_size, char *url, size_t url_size);

// Append (index < 0) or overwrite (0 <= index < count) an entry and persist.
// The URL must start with http:// or https://; an empty name falls back to
// the URL text. out_index (optional) receives the stored slot.
bool RADIO_FAV_Set(int index, const char *name, const char *url, int *out_index);

bool RADIO_FAV_Remove(size_t index);

// Index of the favorite last tuned via RADIO_FAV_PlayIndex (persisted
// across reboots), or -1 when none.
int RADIO_FAV_CurrentIndex(void);

// Index of the first favorite whose URL equals `url`, or -1.
int RADIO_FAV_IndexOfUrl(const char *url);

// Tune favorite `index`: saves it as the current station and starts
// streaming. Returns false when out of range or playback fails to start.
bool RADIO_FAV_PlayIndex(size_t index);

// Switch to the next/previous favorite (wraps around; starts from the
// beginning when nothing was tuned yet). No-ops on an empty list.
bool RADIO_FAV_Next(void);
bool RADIO_FAV_Prev(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_RADIO_FAVORITES_H
