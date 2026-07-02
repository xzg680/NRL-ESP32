#ifndef SRC_SERVICES_STORAGE_SERVICE_H
#define SRC_SERVICES_STORAGE_SERVICE_H

// Storage service (docs/architecture.md 3.5): mounts removable media into
// VFS/FatFS so the media player and playlists access files by path without
// caring about the medium. Backends:
//   - TF card via SDMMC (S31-Korvo only, vendored BSP) -> /sdcard
//   - USB flash drive via USB Host MSC -> /usb        (planned, Phase 1)
// On boards without the hardware every call is a cheap no-op returning false.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount whatever storage backends the board has. Failure to mount (e.g. no
// card inserted) is logged but not fatal; returns true if at least one
// backend mounted.
bool STORAGE_Init(void);

// TF card state.
bool STORAGE_SdMounted(void);

// Mount point of the TF card ("/sdcard"), or NULL when not mounted.
const char *STORAGE_SdMountPoint(void);

// USB flash drive state (USB-OTG Host MSC, hot-pluggable). On insertion the
// drive is mounted at /usb and the playlist rescans; on removal any track
// playing from it is stopped first.
bool STORAGE_UsbMounted(void);

// Mount point of the USB drive ("/usb"), or NULL when not mounted.
const char *STORAGE_UsbMountPoint(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_STORAGE_SERVICE_H
