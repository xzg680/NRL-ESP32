#ifndef SRC_SERVICES_SMB_STREAM_H
#define SRC_SERVICES_SMB_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SmbStream SmbStream;

// Dedicated SMB media connection. This deliberately bypasses the generic
// /smb VFS context so directory browsing and audio prefetch never share a
// socket, libsmb2 context, or mutex.
SmbStream *SMB_STREAM_Open(const char *vfs_path,
                           const volatile bool *stop_requested);

// High-level async libsmb2 read serviced by the calling media-fill task.
// Returns bytes read, 0 for EOF, or -1 on connection/read failure.
int SMB_STREAM_Read(SmbStream *stream, void *dst, size_t size);

bool SMB_STREAM_Seek(SmbStream *stream, int64_t offset, int whence);

void SMB_STREAM_Close(SmbStream *stream);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_SMB_STREAM_H
