#include "services/smb_vfs.h"

#include "driver/board_pins.h"

#if NRL_BOARD == NRL_BOARD_S31_KORVO || NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD

#include <esp_log.h>
#include <esp_vfs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

static const char *TAG = "SMBVFS";

namespace {

constexpr int kMaxOpenFiles = 8;
constexpr int kSmbTimeoutSeconds = 10;
constexpr size_t kMaxSmbText = 128;

// libsmb2's context is not thread safe; the player task, playlist scan and
// metadata parser all issue calls, so every operation takes this lock.
static SemaphoreHandle_t s_lock = nullptr;
static struct smb2_context *s_smb = nullptr;
static struct smb2fh *s_files[kMaxOpenFiles] = {};
static int s_open_dirs = 0;
static bool s_smb_bad = false;
static char s_server[kMaxSmbText] = {};
static char s_share[kMaxSmbText] = {};
static char s_user[kMaxSmbText] = {};
static char s_password[kMaxSmbText] = {};

// DIR wrapper handed back through the VFS: newlib requires the first member
// to be the plain DIR the VFS layer fills in.
struct SmbVfsDir {
    DIR dir;
    struct smb2dir *smb_dir;
    struct dirent entry;
};

static inline const char *smb_path(const char *vfs_path)
{
    // VFS hands paths relative to the mount point ("" or "/sub/file");
    // libsmb2 wants share-relative paths without the leading slash.
    return (vfs_path != nullptr && vfs_path[0] == '/') ? vfs_path + 1 : vfs_path;
}

class LockGuard {
public:
    LockGuard() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~LockGuard() { xSemaphoreGive(s_lock); }
};

static bool smb_error_is_connection_loss(const char *error)
{
    return error != nullptr &&
           (strstr(error, "smb2_service failed") != nullptr ||
            strstr(error, "SMB2_STATUS_IO_TIMEOUT") != nullptr ||
            strstr(error, "Not Connected") != nullptr ||
            strstr(error, "POLLHUP") != nullptr ||
            strstr(error, "socket error") != nullptr);
}

static bool smb_has_open_handles_locked(void)
{
    if (s_open_dirs > 0) {
        return true;
    }
    for (int i = 0; i < kMaxOpenFiles; ++i) {
        if (s_files[i] != nullptr) {
            return true;
        }
    }
    return false;
}

static void smb_destroy_context_locked(void)
{
    if (s_smb == nullptr) {
        return;
    }
    for (int i = 0; i < kMaxOpenFiles; ++i) {
        s_files[i] = nullptr;
    }
    smb2_destroy_context(s_smb);
    s_smb = nullptr;
}

static bool smb_connect_locked(void)
{
    if (smb_has_open_handles_locked()) {
        ESP_LOGW(TAG, "defer reconnect while SMB handles are open");
        return false;
    }
    smb_destroy_context_locked();
    s_smb_bad = false;

    s_smb = smb2_init_context();
    if (s_smb == nullptr) {
        return false;
    }
    smb2_set_timeout(s_smb, kSmbTimeoutSeconds);
    smb2_set_security_mode(s_smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (s_password[0] != '\0') {
        smb2_set_password(s_smb, s_password);
    }

    const char *login = (s_user[0] != '\0') ? s_user : "guest";
    if (smb2_connect_share(s_smb, s_server, s_share, login) < 0) {
        ESP_LOGE(TAG, "connect //%s/%s failed: %s", s_server, s_share, smb2_get_error(s_smb));
        smb_destroy_context_locked();
        return false;
    }
    return true;
}

static bool smb_ensure_connected_locked(void)
{
    if (s_smb != nullptr && !s_smb_bad) {
        return true;
    }
    if (smb_has_open_handles_locked()) {
        errno = EIO;
        return false;
    }
    if (s_server[0] == '\0' || s_share[0] == '\0') {
        errno = ENODEV;
        return false;
    }
    ESP_LOGW(TAG, "SMB connection stale, reconnecting //%s/%s", s_server, s_share);
    if (!smb_connect_locked()) {
        errno = ENODEV;
        return false;
    }
    return true;
}

static int vfs_open(void *, const char *path, const int flags, int)
{
    if ((flags & (O_WRONLY | O_RDWR | O_CREAT)) != 0) {
        errno = EROFS; // read-only backend
        return -1;
    }
    LockGuard lock;
    if (!smb_ensure_connected_locked()) {
        return -1;
    }
    int fd = -1;
    for (int i = 0; i < kMaxOpenFiles; ++i) {
        if (s_files[i] == nullptr) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        errno = ENFILE;
        return -1;
    }
    struct smb2fh *fh = smb2_open(s_smb, smb_path(path), O_RDONLY);
    if (fh == nullptr) {
        const char *error = smb2_get_error(s_smb);
        if (smb_error_is_connection_loss(error)) {
            ESP_LOGW(TAG, "open %s failed, reconnecting: %s", path, error);
            if (!smb_has_open_handles_locked() && smb_connect_locked()) {
                fh = smb2_open(s_smb, smb_path(path), O_RDONLY);
            } else {
                s_smb_bad = true;
            }
        }
    }
    if (fh == nullptr) {
        ESP_LOGW(TAG, "open %s: %s", path, (s_smb != nullptr) ? smb2_get_error(s_smb) : "not connected");
        errno = ENOENT;
        return -1;
    }
    s_files[fd] = fh;
    return fd;
}

static ssize_t vfs_read(void *, const int fd, void *dst, const size_t size)
{
    LockGuard lock;
    if (s_smb == nullptr || s_smb_bad || fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    // libsmb2's sync smb2_read never returns on this port once a single
    // request spans multiple TCP segments (the media player's 8 KB reads
    // hang forever while metadata-sized reads always complete). Issue
    // MTU-sized requests instead so every SMB2 READ response fits in one
    // segment, and reassemble here.
    constexpr size_t kChunk = 1200;
    uint8_t *out = static_cast<uint8_t *>(dst);
    size_t done = 0;
    while (done < size) {
        const size_t want = ((size - done) < kChunk) ? (size - done) : kChunk;
        const int got = smb2_read(s_smb, s_files[fd], out + done,
                                  static_cast<uint32_t>(want));
        if (got < 0) {
            const char *error = smb2_get_error(s_smb);
            if (smb_error_is_connection_loss(error)) {
                s_smb_bad = true;
            }
            if (done > 0) {
                break; // hand back what we already have
            }
            errno = EIO;
            return -1;
        }
        done += static_cast<size_t>(got);
        if (got == 0 || static_cast<size_t>(got) < want) {
            break; // EOF or short read
        }
    }
    return static_cast<ssize_t>(done);
}

static off_t vfs_lseek(void *, const int fd, const off_t offset, const int whence)
{
    LockGuard lock;
    if (s_smb == nullptr || s_smb_bad || fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    uint64_t current = 0;
    const int64_t pos = smb2_lseek(s_smb, s_files[fd], offset, whence, &current);
    if (pos < 0) {
        errno = EINVAL;
        return -1;
    }
    return static_cast<off_t>(pos);
}

static int vfs_close(void *, const int fd)
{
    LockGuard lock;
    if (fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (s_smb != nullptr && !s_smb_bad) {
        (void)smb2_close(s_smb, s_files[fd]);
    }
    s_files[fd] = nullptr;
    return 0;
}

static int vfs_fstat(void *, const int fd, struct stat *st)
{
    LockGuard lock;
    if (s_smb == nullptr || s_smb_bad || fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    struct smb2_stat_64 smb_st = {};
    if (smb2_fstat(s_smb, s_files[fd], &smb_st) < 0) {
        if (smb_error_is_connection_loss(smb2_get_error(s_smb))) {
            s_smb_bad = true;
        }
        errno = EIO;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = static_cast<off_t>(smb_st.smb2_size);
    st->st_mode = S_IFREG | 0444;
    return 0;
}

static int vfs_stat(void *, const char *path, struct stat *st)
{
    LockGuard lock;
    if (!smb_ensure_connected_locked()) {
        return -1;
    }
    struct smb2_stat_64 smb_st = {};
    if (smb2_stat(s_smb, smb_path(path), &smb_st) < 0) {
        if (smb_error_is_connection_loss(smb2_get_error(s_smb))) {
            s_smb_bad = true;
        }
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = static_cast<off_t>(smb_st.smb2_size);
    st->st_mode = (smb_st.smb2_type == SMB2_TYPE_DIRECTORY) ? (S_IFDIR | 0555)
                                                            : (S_IFREG | 0444);
    return 0;
}

static DIR *vfs_opendir(void *, const char *path)
{
    LockGuard lock;
    if (!smb_ensure_connected_locked()) {
        return nullptr;
    }
    struct smb2dir *smb_dir = smb2_opendir(s_smb, smb_path(path));
    if (smb_dir == nullptr) {
        const char *error = smb2_get_error(s_smb);
        if (smb_error_is_connection_loss(error)) {
            ESP_LOGW(TAG, "opendir %s failed, reconnecting: %s", path, error);
            if (!smb_has_open_handles_locked() && smb_connect_locked()) {
                smb_dir = smb2_opendir(s_smb, smb_path(path));
            } else {
                s_smb_bad = true;
            }
        }
    }
    if (smb_dir == nullptr) {
        errno = ENOENT;
        return nullptr;
    }
    SmbVfsDir *dir = static_cast<SmbVfsDir *>(calloc(1, sizeof(SmbVfsDir)));
    if (dir == nullptr) {
        smb2_closedir(s_smb, smb_dir);
        errno = ENOMEM;
        return nullptr;
    }
    dir->smb_dir = smb_dir;
    ++s_open_dirs;
    return &dir->dir;
}

static struct dirent *vfs_readdir(void *, DIR *pdir)
{
    LockGuard lock;
    SmbVfsDir *dir = reinterpret_cast<SmbVfsDir *>(pdir);
    if (s_smb == nullptr || s_smb_bad || dir == nullptr || dir->smb_dir == nullptr) {
        return nullptr;
    }
    const struct smb2dirent *ent;
    while ((ent = smb2_readdir(s_smb, dir->smb_dir)) != nullptr) {
        if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
            continue;
        }
        memset(&dir->entry, 0, sizeof(dir->entry));
        snprintf(dir->entry.d_name, sizeof(dir->entry.d_name), "%s", ent->name);
        dir->entry.d_type = (ent->st.smb2_type == SMB2_TYPE_DIRECTORY) ? DT_DIR : DT_REG;
        return &dir->entry;
    }
    return nullptr;
}

static int vfs_closedir(void *, DIR *pdir)
{
    LockGuard lock;
    SmbVfsDir *dir = reinterpret_cast<SmbVfsDir *>(pdir);
    if (dir == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (s_smb != nullptr && !s_smb_bad && dir->smb_dir != nullptr) {
        smb2_closedir(s_smb, dir->smb_dir);
    }
    if (s_open_dirs > 0) {
        --s_open_dirs;
    }
    free(dir);
    return 0;
}

static bool s_registered = false;

static bool register_vfs(void)
{
    if (s_registered) {
        return true;
    }
    esp_vfs_t vfs = {};
    vfs.flags = ESP_VFS_FLAG_CONTEXT_PTR;
    vfs.open_p = vfs_open;
    vfs.read_p = vfs_read;
    vfs.lseek_p = vfs_lseek;
    vfs.close_p = vfs_close;
    vfs.fstat_p = vfs_fstat;
    vfs.stat_p = vfs_stat;
    vfs.opendir_p = vfs_opendir;
    vfs.readdir_p = vfs_readdir;
    vfs.closedir_p = vfs_closedir;
    if (esp_vfs_register(SMB_VFS_MOUNT_POINT, &vfs, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "vfs register failed");
        return false;
    }
    s_registered = true;
    return true;
}

} // namespace

extern "C" bool SMB_VFS_Mount(const char *server, const char *share,
                              const char *user, const char *password)
{
    if (server == nullptr || server[0] == '\0' || share == nullptr || share[0] == '\0') {
        return false;
    }
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == nullptr) {
            return false;
        }
    }
    SMB_VFS_Unmount();
    if (!register_vfs()) {
        return false;
    }

    snprintf(s_server, sizeof(s_server), "%s", server);
    snprintf(s_share, sizeof(s_share), "%s", share);
    snprintf(s_user, sizeof(s_user), "%s", (user != nullptr) ? user : "");
    snprintf(s_password, sizeof(s_password), "%s", (password != nullptr) ? password : "");

    LockGuard lock;
    if (!smb_connect_locked()) {
        return false;
    }
    ESP_LOGI(TAG, "//%s/%s mounted at %s (user=%s)", s_server, s_share, SMB_VFS_MOUNT_POINT,
             (s_user[0] != '\0') ? s_user : "guest");
    return true;
}

extern "C" void SMB_VFS_Unmount(void)
{
    if (s_lock == nullptr) {
        return;
    }
    LockGuard lock;
    if (smb_has_open_handles_locked()) {
        ESP_LOGW(TAG, "unmount deferred: SMB handles still open");
        s_smb_bad = true;
        return;
    }
    if (s_smb != nullptr && !s_smb_bad) {
        for (int i = 0; i < kMaxOpenFiles; ++i) {
            if (s_files[i] != nullptr) {
                (void)smb2_close(s_smb, s_files[i]);
                s_files[i] = nullptr;
            }
        }
        (void)smb2_disconnect_share(s_smb);
    }
    smb_destroy_context_locked();
    s_smb_bad = false;
    ESP_LOGI(TAG, "unmounted");
}

extern "C" bool SMB_VFS_Mounted(void)
{
    return s_smb != nullptr && !s_smb_bad;
}

#else // !S31

extern "C" bool SMB_VFS_Mount(const char *, const char *, const char *, const char *)
{
    return false;
}

extern "C" void SMB_VFS_Unmount(void) {}

extern "C" bool SMB_VFS_Mounted(void)
{
    return false;
}

#endif
