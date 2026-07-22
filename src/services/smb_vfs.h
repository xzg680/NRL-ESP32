#ifndef SRC_SERVICES_SMB_VFS_H
#define SRC_SERVICES_SMB_VFS_H

// SMB2/3 network share mounted into the ESP VFS at /smb (libsmb2 +
// esp_vfs adapter). Read-only: exactly what the media player needs --
// fopen/fread/fseek and opendir/readdir work on /smb/... paths, so the
// decoder, metadata parser and playlist scan all play network folders with
// zero changes. The backend is shared by all four boards.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMB_VFS_MOUNT_POINT "/smb"

// Connect to \\server\share and register the VFS. user/pass may be empty
// for guest shares. Blocking (DNS + TCP + SMB negotiate); call from a task,
// not from a UI callback.
bool SMB_VFS_Mount(const char *server, const char *share,
                   const char *user, const char *password);

// Disconnect and unregister. Any open files are force-closed.
void SMB_VFS_Unmount(void);

bool SMB_VFS_Mounted(void);

// Interrupt an in-progress directory enumeration. Used by Web navigation so
// entering another folder does not wait for the previous large folder.
void SMB_VFS_CancelDirectoryScan(void);

// Interrupt an in-progress SMB file read. The current connection is retired
// safely and reconnects on the next open; used to stop media immediately.
void SMB_VFS_CancelFileRead(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_SMB_VFS_H
