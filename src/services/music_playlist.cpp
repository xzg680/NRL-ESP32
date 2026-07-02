#include "services/music_playlist.h"

#include "services/music_player.h"
#include "services/storage_service.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "PLAYLIST";

namespace {

constexpr size_t kMaxTracks = 512;
constexpr size_t kMaxPathLen = 128;
constexpr const char *kMusicSubdir = "music";

static char (*s_paths)[kMaxPathLen] = nullptr; // PSRAM array of paths
static size_t s_count = 0;
static volatile int s_current = -1;
static volatile bool s_auto_advance = true;

static bool has_supported_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == nullptr) {
        return false;
    }
    return strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0 ||
           strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".m4a") == 0 ||
           strcasecmp(dot, ".aac") == 0;
}

static void scan_dir(const char *mount_point)
{
    if (mount_point == nullptr) {
        return;
    }
    char dir_path[kMaxPathLen];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", mount_point, kMusicSubdir);

    DIR *dir = opendir(dir_path);
    if (dir == nullptr) {
        ESP_LOGI(TAG, "no %s directory", dir_path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr && s_count < kMaxTracks) {
        if (entry->d_type == DT_DIR || !has_supported_extension(entry->d_name)) {
            continue;
        }
        const int written = snprintf(s_paths[s_count], kMaxPathLen, "%s/%s", dir_path, entry->d_name);
        if (written > 0 && static_cast<size_t>(written) < kMaxPathLen) {
            ++s_count;
        }
    }
    closedir(dir);
}

static int compare_paths(const void *a, const void *b)
{
    return strcasecmp(static_cast<const char *>(a), static_cast<const char *>(b));
}

static void on_track_end(void)
{
    if (s_auto_advance) {
        (void)PLAYLIST_Next();
    }
}

} // namespace

extern "C" void PLAYLIST_Init(void)
{
    MUSIC_SetTrackEndCallback(on_track_end);
}

extern "C" size_t PLAYLIST_Scan(void)
{
    if (s_paths == nullptr) {
        s_paths = static_cast<char(*)[kMaxPathLen]>(
            heap_caps_malloc(kMaxTracks * kMaxPathLen, MALLOC_CAP_SPIRAM));
        if (s_paths == nullptr) {
            ESP_LOGE(TAG, "path table alloc failed");
            return 0;
        }
    }

    s_count = 0;
    s_current = -1;
    scan_dir(STORAGE_SdMountPoint());
    scan_dir(STORAGE_UsbMountPoint());

    if (s_count > 1u) {
        qsort(s_paths, s_count, kMaxPathLen, compare_paths);
    }
    ESP_LOGI(TAG, "%u tracks indexed", static_cast<unsigned>(s_count));
    return s_count;
}

extern "C" size_t PLAYLIST_Count(void)
{
    return s_count;
}

extern "C" const char *PLAYLIST_GetPath(const size_t index)
{
    if (s_paths == nullptr || index >= s_count) {
        return nullptr;
    }
    return s_paths[index];
}

extern "C" int PLAYLIST_CurrentIndex(void)
{
    return s_current;
}

extern "C" bool PLAYLIST_PlayIndex(const size_t index)
{
    const char *path = PLAYLIST_GetPath(index);
    if (path == nullptr) {
        return false;
    }
    if (!MUSIC_PlayFile(path)) {
        return false;
    }
    s_current = static_cast<int>(index);
    return true;
}

extern "C" bool PLAYLIST_Next(void)
{
    if (s_count == 0u) {
        return false;
    }
    const size_t next = (s_current < 0) ? 0u : (static_cast<size_t>(s_current) + 1u) % s_count;
    return PLAYLIST_PlayIndex(next);
}

extern "C" bool PLAYLIST_Prev(void)
{
    if (s_count == 0u) {
        return false;
    }
    const size_t prev = (s_current <= 0) ? (s_count - 1u) : (static_cast<size_t>(s_current) - 1u);
    return PLAYLIST_PlayIndex(prev);
}

extern "C" void PLAYLIST_SetAutoAdvance(const bool enabled)
{
    s_auto_advance = enabled;
}
