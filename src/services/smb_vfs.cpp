#include "services/smb_vfs.h"

#include "driver/board_pins.h"

#if NRL_BOARD == NRL_BOARD_S31_KORVO

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

// libsmb2's context is not thread safe; the player task, playlist scan and
// metadata parser all issue calls, so every operation takes this lock.
static SemaphoreHandle_t s_lock = nullptr;
static struct smb2_context *s_smb = nullptr;
static struct smb2fh *s_files[kMaxOpenFiles] = {};

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

static int vfs_open(const char *path, const int flags, int)
{
    if ((flags & (O_WRONLY | O_RDWR | O_CREAT)) != 0) {
        errno = EROFS; // read-only backend
        return -1;
    }
    LockGuard lock;
    if (s_smb == nullptr) {
        errno = ENODEV;
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
        ESP_LOGW(TAG, "open %s: %s", path, smb2_get_error(s_smb));
        errno = ENOENT;
        return -1;
    }
    s_files[fd] = fh;
    return fd;
}

static ssize_t vfs_read(const int fd, void *dst, const size_t size)
{
    LockGuard lock;
    if (s_smb == nullptr || fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
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

static off_t vfs_lseek(const int fd, const off_t offset, const int whence)
{
    LockGuard lock;
    if (s_smb == nullptr || fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
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

static int vfs_close(const int fd)
{
    LockGuard lock;
    if (fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (s_smb != nullptr) {
        (void)smb2_close(s_smb, s_files[fd]);
    }
    s_files[fd] = nullptr;
    return 0;
}

static int vfs_fstat(const int fd, struct stat *st)
{
    LockGuard lock;
    if (s_smb == nullptr || fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    struct smb2_stat_64 smb_st = {};
    if (smb2_fstat(s_smb, s_files[fd], &smb_st) < 0) {
        errno = EIO;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = static_cast<off_t>(smb_st.smb2_size);
    st->st_mode = S_IFREG | 0444;
    return 0;
}

static int vfs_stat(const char *path, struct stat *st)
{
    LockGuard lock;
    if (s_smb == nullptr) {
        errno = ENODEV;
        return -1;
    }
    struct smb2_stat_64 smb_st = {};
    if (smb2_stat(s_smb, smb_path(path), &smb_st) < 0) {
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = static_cast<off_t>(smb_st.smb2_size);
    st->st_mode = (smb_st.smb2_type == SMB2_TYPE_DIRECTORY) ? (S_IFDIR | 0555)
                                                            : (S_IFREG | 0444);
    return 0;
}

static DIR *vfs_opendir(const char *path)
{
    LockGuard lock;
    if (s_smb == nullptr) {
        errno = ENODEV;
        return nullptr;
    }
    struct smb2dir *smb_dir = smb2_opendir(s_smb, smb_path(path));
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
    return &dir->dir;
}

static struct dirent *vfs_readdir(DIR *pdir)
{
    LockGuard lock;
    SmbVfsDir *dir = reinterpret_cast<SmbVfsDir *>(pdir);
    if (s_smb == nullptr || dir == nullptr || dir->smb_dir == nullptr) {
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

static int vfs_closedir(DIR *pdir)
{
    LockGuard lock;
    SmbVfsDir *dir = reinterpret_cast<SmbVfsDir *>(pdir);
    if (dir == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (s_smb != nullptr && dir->smb_dir != nullptr) {
        smb2_closedir(s_smb, dir->smb_dir);
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
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.open = vfs_open;
    vfs.read = vfs_read;
    vfs.lseek = vfs_lseek;
    vfs.close = vfs_close;
    vfs.fstat = vfs_fstat;
    vfs.stat = vfs_stat;
    vfs.opendir = vfs_opendir;
    vfs.readdir = vfs_readdir;
    vfs.closedir = vfs_closedir;
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

    LockGuard lock;
    s_smb = smb2_init_context();
    if (s_smb == nullptr) {
        return false;
    }
    smb2_set_timeout(s_smb, kSmbTimeoutSeconds);
    smb2_set_security_mode(s_smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (password != nullptr && password[0] != '\0') {
        smb2_set_password(s_smb, password);
    }
    const char *login = (user != nullptr && user[0] != '\0') ? user : "guest";
    if (smb2_connect_share(s_smb, server, share, login) < 0) {
        ESP_LOGE(TAG, "connect //%s/%s failed: %s", server, share, smb2_get_error(s_smb));
        smb2_destroy_context(s_smb);
        s_smb = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "//%s/%s mounted at %s (user=%s)", server, share, SMB_VFS_MOUNT_POINT, login);
    return true;
}

extern "C" void SMB_VFS_Unmount(void)
{
    if (s_lock == nullptr || s_smb == nullptr) {
        return;
    }
    LockGuard lock;
    for (int i = 0; i < kMaxOpenFiles; ++i) {
        if (s_files[i] != nullptr) {
            (void)smb2_close(s_smb, s_files[i]);
            s_files[i] = nullptr;
        }
    }
    (void)smb2_disconnect_share(s_smb);
    smb2_destroy_context(s_smb);
    s_smb = nullptr;
    ESP_LOGI(TAG, "unmounted");
}

extern "C" bool SMB_VFS_Mounted(void)
{
    return s_smb != nullptr;
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
