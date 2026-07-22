#include "services/music_playlist.h"

#include "services/music_player.h"
#include "services/smb_vfs.h"
#include "services/storage_service.h"
#include "lib/nrl_psram.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <atomic>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "PLAYLIST";

namespace {

constexpr size_t kMaxTracks = 512;
constexpr size_t kMaxDirs = 128;
constexpr size_t kMaxScannedEntries = kMaxTracks + kMaxDirs;
// Matches music_player's limit; UTF-8 Chinese paths (3 bytes per CJK char
// plus artist/album directories) overflow the old 128 quickly.
constexpr size_t kMaxPathLen = 256;
constexpr size_t kMaxNameLen = 96;
constexpr const char *kMusicSubdir = "music";

struct PlaylistDir {
    char path[kMaxPathLen];
    char name[kMaxNameLen];
};

NRL_PSRAM_BSS static char s_path_storage[kMaxTracks][kMaxPathLen];
NRL_PSRAM_BSS static PlaylistDir s_dir_storage[kMaxDirs];
NRL_PSRAM_BSS static char s_fav_storage[PLAYLIST_FAV_MAX][kMaxPathLen];
static char (*s_paths)[kMaxPathLen] = s_path_storage;
static PlaylistDir *s_dirs = s_dir_storage;
static std::atomic_size_t s_count{0};
static std::atomic_size_t s_dir_count{0};
static volatile int s_current = -1;
static volatile bool s_auto_advance = true;
static char s_current_dir[kMaxPathLen] = {};   // empty string means virtual source root
static char (*s_favs)[kMaxPathLen] = s_fav_storage;
static size_t s_fav_count = 0;
static PlaylistRepeatMode s_repeat_mode = PLAYLIST_REPEAT_LIST;
static volatile bool s_scanning = false;
static volatile bool s_async_scan_active = false;
static volatile bool s_scan_entries_visible = true;
static volatile bool s_last_scan_ok = true;
static std::atomic_bool s_cancel_scan{false};
static portMUX_TYPE s_scan_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_scan_queued = false;
static char s_queued_dir[kMaxPathLen] = {};

constexpr const char *kNvsNamespace = "playlist";
constexpr const char *kFavoritesDir = "@favorites";
constexpr const char *kFavoritesFile = "/sdcard/.nrl_music_favorites.txt";

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

static const char *basename_of(const char *path)
{
    if (path == nullptr) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return (slash != nullptr) ? slash + 1 : path;
}

static bool ensure_tables()
{
    return true;
}

static void reset_entries()
{
    s_count = 0;
    s_dir_count = 0;
    s_current = -1;
}

static bool add_dir(const char *path, const char *name)
{
    if (path == nullptr || path[0] == '\0' || s_dir_count >= kMaxDirs) {
        return false;
    }
    const int p = snprintf(s_dirs[s_dir_count].path, kMaxPathLen, "%s", path);
    const int n = snprintf(s_dirs[s_dir_count].name, kMaxNameLen, "%s",
                           (name != nullptr && name[0] != '\0') ? name : basename_of(path));
    if (p <= 0 || n <= 0 || static_cast<size_t>(p) >= kMaxPathLen ||
        static_cast<size_t>(n) >= kMaxNameLen) {
        return false;
    }
    ++s_dir_count;
    return true;
}

static bool add_track(const char *dir_path, const char *name)
{
    if (dir_path == nullptr || name == nullptr || s_count >= kMaxTracks) {
        return false;
    }
    const int written = snprintf(s_paths[s_count], kMaxPathLen, "%s/%s", dir_path, name);
    if (written <= 0 || static_cast<size_t>(written) >= kMaxPathLen) {
        return false;
    }
    ++s_count;
    return true;
}

static int compare_paths(const void *a, const void *b)
{
    return strcasecmp(static_cast<const char *>(a), static_cast<const char *>(b));
}

static int compare_dirs(const void *a, const void *b)
{
    const PlaylistDir *da = static_cast<const PlaylistDir *>(a);
    const PlaylistDir *db = static_cast<const PlaylistDir *>(b);
    return strcasecmp(da->name, db->name);
}

static bool source_root_path(const char *mount_point, const char *subdir,
                             char *out, const size_t out_size)
{
    if (mount_point == nullptr || mount_point[0] == '\0' || out == nullptr || out_size == 0u) {
        return false;
    }
    int written = 0;
    if (subdir != nullptr && subdir[0] != '\0') {
        written = snprintf(out, out_size, "%s/%s", mount_point, subdir);
    } else {
        written = snprintf(out, out_size, "%s", mount_point);
    }
    return written > 0 && static_cast<size_t>(written) < out_size;
}

static bool is_source_root(const char *path)
{
    char root[kMaxPathLen];
    if (source_root_path(STORAGE_SdMountPoint(), kMusicSubdir, root, sizeof(root)) &&
        strcmp(path, root) == 0) {
        return true;
    }
    if (source_root_path(STORAGE_UsbMountPoint(), kMusicSubdir, root, sizeof(root)) &&
        strcmp(path, root) == 0) {
        return true;
    }
    if (source_root_path(STORAGE_SmbMountPoint(), nullptr, root, sizeof(root)) &&
        strcmp(path, root) == 0) {
        return true;
    }
    return false;
}

static void scan_virtual_root()
{
    char path[kMaxPathLen];
    if (STORAGE_SdMounted()) {
        add_dir(kFavoritesDir, "Favorites");
    }
    if (source_root_path(STORAGE_SmbMountPoint(), nullptr, path, sizeof(path))) {
        add_dir(path, "SMB");
    }
    if (source_root_path(STORAGE_SdMountPoint(), kMusicSubdir, path, sizeof(path))) {
        add_dir(path, "SD");
    }
    if (source_root_path(STORAGE_UsbMountPoint(), kMusicSubdir, path, sizeof(path))) {
        add_dir(path, "USB");
    }
}

static void scan_favorites()
{
    if (s_favs == nullptr) {
        return;
    }
    for (size_t i = 0; i < s_fav_count && s_count < kMaxTracks; ++i) {
        memcpy(s_paths[s_count], s_favs[i], kMaxPathLen);
        s_paths[s_count][kMaxPathLen - 1u] = '\0';
        ++s_count;
    }
    if (!s_async_scan_active && s_count > 1u) {
        qsort(s_paths, s_count, kMaxPathLen, compare_paths);
    }
}

static bool scan_current_dir()
{
    DIR *dir = opendir(s_current_dir);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "open dir failed: %s", s_current_dir);
        return false;
    }

    struct dirent *entry;
    size_t inspected = 0;
    while (inspected < kMaxScannedEntries && !s_cancel_scan.load() &&
           (entry = readdir(dir)) != nullptr) {
        ++inspected;
        if (entry->d_name[0] == '.') {
            continue; // hidden entries + "."/".."
        }
        if (entry->d_type == DT_DIR) {
            if (s_dir_count < kMaxDirs) {
                char sub_path[kMaxPathLen];
                const int written = snprintf(sub_path, sizeof(sub_path), "%s/%s",
                                             s_current_dir, entry->d_name);
                if (written > 0 && static_cast<size_t>(written) < sizeof(sub_path)) {
                    add_dir(sub_path, entry->d_name);
                }
            }
            continue;
        }
        if (s_count < kMaxTracks && has_supported_extension(entry->d_name)) {
            add_track(s_current_dir, entry->d_name);
        }
    }
    closedir(dir);

    if (!s_async_scan_active && s_dir_count > 1u) {
        qsort(s_dirs, s_dir_count, sizeof(PlaylistDir), compare_dirs);
    }
    if (!s_async_scan_active && s_count > 1u) {
        qsort(s_paths, s_count, kMaxPathLen, compare_paths);
    }
    return true;
}

static bool set_current_dir(const char *path)
{
    if (path == nullptr) {
        s_current_dir[0] = '\0';
        return true;
    }
    const int written = snprintf(s_current_dir, sizeof(s_current_dir), "%s", path);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(s_current_dir)) {
        return false;
    }
    return true;
}

static void async_scan_task(void *)
{
    while (true) {
        (void)PLAYLIST_Scan();

        char next[kMaxPathLen] = {};
        bool scan_again = false;
        portENTER_CRITICAL(&s_scan_state_lock);
        if (s_scan_queued) {
            memcpy(next, s_queued_dir, sizeof(next));
            s_scan_queued = false;
            s_scan_entries_visible = false;
            s_last_scan_ok = true;
            scan_again = true;
        } else {
            s_async_scan_active = false;
            s_scan_entries_visible = true;
            s_scanning = false;
        }
        portEXIT_CRITICAL(&s_scan_state_lock);

        if (!scan_again) {
            break;
        }
        (void)set_current_dir(next);
        s_cancel_scan.store(false);
        ESP_LOGI(TAG, "switching directory scan to %s", next[0] ? next : "<sources>");
    }
    vTaskDelete(nullptr);
}

static bool schedule_scan(const char *path)
{
    if (path == nullptr) {
        return false;
    }
    char target[kMaxPathLen] = {};
    char previous[kMaxPathLen] = {};
    const int written = snprintf(target, sizeof(target), "%s", path);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(target)) {
        return false;
    }
    portENTER_CRITICAL(&s_scan_state_lock);
    if (s_scanning) {
        memcpy(s_queued_dir, target, sizeof(s_queued_dir));
        s_scan_queued = true;
        s_last_scan_ok = true;
        portEXIT_CRITICAL(&s_scan_state_lock);

        // readdir() may currently be waiting for an SMB QUERY_DIRECTORY reply.
        // Wake that wait promptly; the scan task will reconnect and open the
        // most recently queued directory after discarding the partial result.
        s_cancel_scan.store(true);
        SMB_VFS_CancelDirectoryScan();
        ESP_LOGI(TAG, "directory switch queued: %s", target[0] ? target : "<sources>");
        return true;
    }
    memcpy(previous, s_current_dir, sizeof(previous));
    memcpy(s_current_dir, target, sizeof(s_current_dir));
    s_scanning = true;
    s_async_scan_active = true;
    s_scan_entries_visible = false;
    s_last_scan_ok = true;
    s_scan_queued = false;
    s_cancel_scan.store(false);
    portEXIT_CRITICAL(&s_scan_state_lock);

    if (xTaskCreate(async_scan_task, "playlist_scan", 6144, nullptr, 3, nullptr) != pdPASS) {
        portENTER_CRITICAL(&s_scan_state_lock);
        s_scanning = false;
        s_async_scan_active = false;
        s_scan_entries_visible = true;
        s_last_scan_ok = false;
        memcpy(s_current_dir, previous, sizeof(s_current_dir));
        portEXIT_CRITICAL(&s_scan_state_lock);
        ESP_LOGE(TAG, "background scan task create failed");
        return false;
    }
    return true;
}

static void save_settings()
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "settings persist failed");
        return;
    }
    (void)nvs_set_u8(nvs, "repeat", static_cast<uint8_t>(s_repeat_mode));
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static void save_favorites_to_sd()
{
    if (!STORAGE_SdMounted() || s_favs == nullptr) {
        return;
    }
    FILE *file = fopen(kFavoritesFile, "w");
    if (file == nullptr) {
        ESP_LOGW(TAG, "favorite file save failed: %s", kFavoritesFile);
        return;
    }
    for (size_t i = 0; i < s_fav_count; ++i) {
        fprintf(file, "%s\n", s_favs[i]);
    }
    fclose(file);
}

static void load_favorites_from_sd()
{
    if (!ensure_tables()) {
        return;
    }
    s_fav_count = 0;
    if (!STORAGE_SdMounted()) {
        return;
    }
    FILE *file = fopen(kFavoritesFile, "r");
    if (file == nullptr) {
        return;
    }
    char line[kMaxPathLen + 8];
    while (fgets(line, sizeof(line), file) != nullptr && s_fav_count < PLAYLIST_FAV_MAX) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }
        *end = '\0';
        if (line[0] == '\0' || strlen(line) >= kMaxPathLen) {
            continue;
        }
        // Precision bound keeps GCC's -Werror=format-truncation happy: line is
        // sized kMaxPathLen+8, but the guard above already ensures it fits.
        snprintf(s_favs[s_fav_count], kMaxPathLen, "%.*s", static_cast<int>(kMaxPathLen - 1), line);
        ++s_fav_count;
    }
    fclose(file);
}

static void load_settings()
{
    if (!ensure_tables()) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t repeat = 0;
    if (nvs_get_u8(nvs, "repeat", &repeat) == ESP_OK && repeat <= PLAYLIST_REPEAT_ONE) {
        s_repeat_mode = static_cast<PlaylistRepeatMode>(repeat);
    }
    nvs_close(nvs);
    load_favorites_from_sd();
    ESP_LOGI(TAG, "%u favorite tracks loaded from SD", static_cast<unsigned>(s_fav_count));
}

static void on_track_end(void)
{
    if (!s_auto_advance) {
        return;
    }
    if (s_repeat_mode == PLAYLIST_REPEAT_ONE && s_current >= 0) {
        (void)PLAYLIST_PlayIndex(static_cast<size_t>(s_current));
    } else {
        (void)PLAYLIST_Next();
    }
}

} // namespace

extern "C" void PLAYLIST_Init(void)
{
    load_settings();
    MUSIC_SetTrackEndCallback(on_track_end);
}

extern "C" size_t PLAYLIST_Scan(void)
{
    if (!ensure_tables()) {
        s_last_scan_ok = false;
        return 0;
    }
    load_favorites_from_sd();

    char current_path[kMaxPathLen] = {};
    if (s_current >= 0 && static_cast<size_t>(s_current) < s_count) {
        snprintf(current_path, sizeof(current_path), "%s", s_paths[s_current]);
    }

    reset_entries();
    if (s_async_scan_active) {
        s_scan_entries_visible = true;
    }
    bool scan_ok = true;
    if (strcmp(s_current_dir, kFavoritesDir) == 0) {
        scan_favorites();
    } else if (s_current_dir[0] == '\0') {
        scan_virtual_root();
    } else if (!scan_current_dir()) {
        // A network directory can fail transiently while SMB reconnects. Keep
        // the requested directory selected so the web/display UI can retry it
        // in place instead of unexpectedly jumping back to the source root.
        reset_entries();
        scan_ok = false;
    }

    if (current_path[0] != '\0') {
        for (size_t i = 0; i < s_count; ++i) {
            if (strcmp(s_paths[i], current_path) == 0) {
                s_current = static_cast<int>(i);
                break;
            }
        }
    }

    ESP_LOGI(TAG, "%u dirs, %u tracks indexed in %s",
             static_cast<unsigned>(s_dir_count),
             static_cast<unsigned>(s_count),
             s_current_dir[0] ? s_current_dir : "<sources>");
    s_last_scan_ok = scan_ok;
    return s_count;
}

extern "C" bool PLAYLIST_ScanAsync(void)
{
    return schedule_scan(s_current_dir);
}

extern "C" bool PLAYLIST_EnterDirAsync(const size_t index)
{
    const char *path = PLAYLIST_GetDirPath(index);
    if (path == nullptr ||
        (strcmp(path, kFavoritesDir) == 0 && !STORAGE_SdMounted())) {
        return false;
    }
    char target[kMaxPathLen];
    const int written = snprintf(target, sizeof(target), "%s", path);
    return written >= 0 && static_cast<size_t>(written) < sizeof(target) &&
           schedule_scan(target);
}

extern "C" bool PLAYLIST_UpAsync(void)
{
    if (s_current_dir[0] == '\0') {
        return false;
    }
    char target[kMaxPathLen];
    snprintf(target, sizeof(target), "%s", s_current_dir);
    if (strcmp(target, kFavoritesDir) == 0 || is_source_root(target)) {
        target[0] = '\0';
    } else {
        char *slash = strrchr(target, '/');
        if (slash == nullptr || slash == target) {
            target[0] = '\0';
        } else {
            *slash = '\0';
        }
    }
    return schedule_scan(target);
}

extern "C" bool PLAYLIST_IsScanning(void)
{
    return s_scanning;
}

extern "C" bool PLAYLIST_LastScanOk(void)
{
    return s_last_scan_ok;
}

extern "C" const char *PLAYLIST_CurrentDir(void)
{
    return s_current_dir;
}

extern "C" bool PLAYLIST_AtRoot(void)
{
    return s_current_dir[0] == '\0';
}

extern "C" bool PLAYLIST_InFavorites(void)
{
    return strcmp(s_current_dir, kFavoritesDir) == 0;
}

extern "C" size_t PLAYLIST_DirCount(void)
{
    return s_scan_entries_visible ? s_dir_count.load() : 0u;
}

extern "C" const char *PLAYLIST_GetDirName(const size_t index)
{
    if (!s_scan_entries_visible || s_dirs == nullptr || index >= s_dir_count) {
        return nullptr;
    }
    return s_dirs[index].name;
}

extern "C" const char *PLAYLIST_GetDirPath(const size_t index)
{
    if (!s_scan_entries_visible || s_dirs == nullptr || index >= s_dir_count) {
        return nullptr;
    }
    return s_dirs[index].path;
}

extern "C" bool PLAYLIST_EnterDir(const size_t index)
{
    if (s_scanning) {
        return false;
    }
    const char *path = PLAYLIST_GetDirPath(index);
    if (path != nullptr && strcmp(path, kFavoritesDir) == 0 && !STORAGE_SdMounted()) {
        return false;
    }
    if (path == nullptr || !set_current_dir(path)) {
        return false;
    }
    (void)PLAYLIST_Scan();
    return true;
}

extern "C" bool PLAYLIST_Up(void)
{
    if (s_scanning || s_current_dir[0] == '\0') {
        return false;
    }
    if (strcmp(s_current_dir, kFavoritesDir) == 0) {
        s_current_dir[0] = '\0';
        (void)PLAYLIST_Scan();
        return true;
    }
    if (is_source_root(s_current_dir)) {
        s_current_dir[0] = '\0';
        (void)PLAYLIST_Scan();
        return true;
    }
    char *slash = strrchr(s_current_dir, '/');
    if (slash == nullptr || slash == s_current_dir) {
        s_current_dir[0] = '\0';
    } else {
        *slash = '\0';
    }
    (void)PLAYLIST_Scan();
    return true;
}

extern "C" size_t PLAYLIST_Count(void)
{
    return s_scan_entries_visible ? s_count.load() : 0u;
}

extern "C" const char *PLAYLIST_GetPath(const size_t index)
{
    if (!s_scan_entries_visible || s_paths == nullptr || index >= s_count) {
        return nullptr;
    }
    return s_paths[index];
}

extern "C" int PLAYLIST_CurrentIndex(void)
{
    return s_scanning ? -1 : s_current;
}

extern "C" bool PLAYLIST_PlayIndex(const size_t index)
{
    if (s_scanning) {
        return false;
    }
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
    if (s_scanning || s_count == 0u) {
        return false;
    }
    const size_t next = (s_current < 0) ? 0u : (static_cast<size_t>(s_current) + 1u) % s_count;
    return PLAYLIST_PlayIndex(next);
}

extern "C" bool PLAYLIST_Prev(void)
{
    if (s_scanning || s_count == 0u) {
        return false;
    }
    const size_t prev = (s_current <= 0) ? (s_count - 1u) : (static_cast<size_t>(s_current) - 1u);
    return PLAYLIST_PlayIndex(prev);
}

extern "C" PlaylistRepeatMode PLAYLIST_GetRepeatMode(void)
{
    return s_repeat_mode;
}

extern "C" void PLAYLIST_SetRepeatMode(const PlaylistRepeatMode mode)
{
    s_repeat_mode = (mode == PLAYLIST_REPEAT_ONE) ? PLAYLIST_REPEAT_ONE : PLAYLIST_REPEAT_LIST;
    save_settings();
}

extern "C" PlaylistRepeatMode PLAYLIST_ToggleRepeatMode(void)
{
    PLAYLIST_SetRepeatMode(s_repeat_mode == PLAYLIST_REPEAT_ONE
                               ? PLAYLIST_REPEAT_LIST
                               : PLAYLIST_REPEAT_ONE);
    return s_repeat_mode;
}

extern "C" size_t PLAYLIST_FavoriteCount(void)
{
    return s_fav_count;
}

extern "C" bool PLAYLIST_IsFavorite(const char *path)
{
    if (path == nullptr || path[0] == '\0' || s_favs == nullptr) {
        return false;
    }
    for (size_t i = 0; i < s_fav_count; ++i) {
        if (strcmp(s_favs[i], path) == 0) {
            return true;
        }
    }
    return false;
}

extern "C" bool PLAYLIST_ToggleFavorite(const char *path)
{
    if (!STORAGE_SdMounted() || path == nullptr || path[0] == '\0' || !ensure_tables()) {
        return false;
    }
    for (size_t i = 0; i < s_fav_count; ++i) {
        if (strcmp(s_favs[i], path) == 0) {
            memmove(&s_favs[i], &s_favs[i + 1], (s_fav_count - i - 1u) * kMaxPathLen);
            --s_fav_count;
            save_favorites_to_sd();
            if (strcmp(s_current_dir, kFavoritesDir) == 0) {
                (void)PLAYLIST_Scan();
            }
            return true;
        }
    }
    if (s_fav_count >= PLAYLIST_FAV_MAX) {
        return false;
    }
    const int written = snprintf(s_favs[s_fav_count], kMaxPathLen, "%s", path);
    if (written <= 0 || static_cast<size_t>(written) >= kMaxPathLen) {
        return false;
    }
    ++s_fav_count;
    save_favorites_to_sd();
    return true;
}

extern "C" void PLAYLIST_SetAutoAdvance(const bool enabled)
{
    s_auto_advance = enabled;
}
