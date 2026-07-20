#include "services/storage_service.h"

#include "driver/board_pins.h"

#include <esp_log.h>
#include <stdio.h>

static const char *TAG = "STORAGE";

// ---------------------------------------------------------------------------
// TF card backend (SDMMC via the vendored BSP)
// ---------------------------------------------------------------------------
#if defined(NRL_HAS_SDCARD) && NRL_HAS_SDCARD

#include "bsp/sdcard.h"

namespace {
static bool s_sd_mounted = false;

static void storage_mount_sdcard(void)
{
    sdmmc_card_t *card = nullptr;
    const esp_err_t err = bsp_sdcard_mount(nullptr, &card);
    if (err == ESP_FAIL) {
        // The SDMMC probe answered but FatFs found no FAT volume (FatFs error
        // FR_NO_FILESYSTEM surfaces as ESP_FAIL): typically a >32 GB card
        // still in its factory exFAT format, which this FatFs build cannot
        // read (IDF hardcodes FF_FS_EXFAT=0).
        ESP_LOGW(TAG, "TF card detected but has no FAT filesystem (exFAT/NTFS "
                      "unsupported): reformat it as FAT32 on a PC, or erase + "
                      "format in-device with AT+SDFORMAT=YES");
        return;
    }
    if (err != ESP_OK) {
        // No card inserted is a normal condition; the music player simply
        // has nothing to index. AT+SDMOUNT=1 retries after a hot insert.
        ESP_LOGW(TAG, "TF card mount failed: %s (no card inserted?)", esp_err_to_name(err));
        return;
    }
    s_sd_mounted = true;
    ESP_LOGI(TAG, "TF card mounted at %s: %s %lluMB",
             bsp_sdcard_get_mount_point(),
             (card != nullptr) ? card->cid.name : "?",
             (card != nullptr)
                 ? (static_cast<unsigned long long>(card->csd.capacity) * card->csd.sector_size) / (1024ULL * 1024ULL)
                 : 0ULL);
}
} // namespace

extern "C" bool STORAGE_SdMountRetry(void)
{
    if (!s_sd_mounted) {
        storage_mount_sdcard();
    }
    return s_sd_mounted;
}

extern "C" bool STORAGE_SdFormat(void)
{
    if (s_sd_mounted) {
        // Card already works; refuse to erase it from here.
        return false;
    }
    bsp_sdcard_config_t cfg = BSP_SDCARD_DEFAULT_CONFIG();
    cfg.format_if_mount_failed = true;
    sdmmc_card_t *card = nullptr;
    const esp_err_t err = bsp_sdcard_mount(&cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TF card format failed: %s", esp_err_to_name(err));
        return false;
    }
    s_sd_mounted = true;
    ESP_LOGI(TAG, "TF card formatted and mounted at %s", bsp_sdcard_get_mount_point());
    return true;
}

extern "C" bool STORAGE_SdMounted(void)
{
    return s_sd_mounted;
}

extern "C" const char *STORAGE_SdMountPoint(void)
{
    return s_sd_mounted ? bsp_sdcard_get_mount_point() : nullptr;
}

#else // !NRL_HAS_SDCARD

extern "C" bool STORAGE_SdMounted(void)
{
    return false;
}

extern "C" const char *STORAGE_SdMountPoint(void)
{
    return nullptr;
}

extern "C" bool STORAGE_SdMountRetry(void)
{
    return false;
}

extern "C" bool STORAGE_SdFormat(void)
{
    return false;
}

#endif // NRL_HAS_SDCARD

// ---------------------------------------------------------------------------
// USB flash drive backend (USB-OTG Host MSC, hot-pluggable)
// ---------------------------------------------------------------------------
#if defined(NRL_HAS_USB_HOST) && NRL_HAS_USB_HOST

#include "services/music_player.h"
#include "services/music_playlist.h"

#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <usb/msc_host.h>
#include <usb/msc_host_vfs.h>
#include <usb/usb_host.h>

#include <string.h>

namespace {

constexpr const char *kUsbMountPoint = "/usb";

static volatile bool s_usb_mounted = false;
static msc_host_device_handle_t s_msc_device = nullptr;
static msc_host_vfs_handle_t s_msc_vfs = nullptr;
static QueueHandle_t s_msc_event_queue = nullptr;

// The USB Host Library needs a dedicated task pumping its events (device
// enumeration, cleanup) independent of the MSC class driver's own task.
static void usb_host_lib_task(void *)
{
    while (true) {
        uint32_t flags = 0;
        (void)usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            (void)usb_host_device_free_all();
        }
    }
}

static void msc_event_callback(const msc_host_event_t *event, void *)
{
    // Runs in the MSC driver task: just forward, blocking calls like
    // msc_host_install_device must not run in this context.
    if (s_msc_event_queue != nullptr) {
        (void)xQueueSend(s_msc_event_queue, event, 0);
    }
}

static void msc_mount(const uint8_t address)
{
    if (s_usb_mounted) {
        return; // single drive supported
    }
    if (msc_host_install_device(address, &s_msc_device) != ESP_OK) {
        ESP_LOGE(TAG, "USB drive install failed (addr=%u)", static_cast<unsigned>(address));
        s_msc_device = nullptr;
        return;
    }
    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;
    if (msc_host_vfs_register(s_msc_device, kUsbMountPoint, &mount_config, &s_msc_vfs) != ESP_OK) {
        ESP_LOGE(TAG, "USB drive mount failed (unsupported filesystem?)");
        (void)msc_host_uninstall_device(s_msc_device);
        s_msc_device = nullptr;
        return;
    }
    s_usb_mounted = true;
    ESP_LOGI(TAG, "USB drive mounted at %s", kUsbMountPoint);
    (void)PLAYLIST_Scan();
}

static void msc_unmount(void)
{
    if (!s_usb_mounted) {
        return;
    }
    // Stop playback that reads from the vanished drive before tearing the
    // filesystem down under it.
    if (MUSIC_IsPlaying() &&
        strncmp(MUSIC_CurrentPath(), kUsbMountPoint, strlen(kUsbMountPoint)) == 0) {
        MUSIC_Stop();
        vTaskDelay(pdMS_TO_TICKS(100)); // let the player task release the file
    }
    s_usb_mounted = false;
    if (s_msc_vfs != nullptr) {
        (void)msc_host_vfs_unregister(s_msc_vfs);
        s_msc_vfs = nullptr;
    }
    if (s_msc_device != nullptr) {
        (void)msc_host_uninstall_device(s_msc_device);
        s_msc_device = nullptr;
    }
    ESP_LOGI(TAG, "USB drive removed");
    (void)PLAYLIST_Scan();
}

static void msc_event_task(void *)
{
    msc_host_event_t event = {};
    while (true) {
        if (xQueueReceive(s_msc_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // The event enum is unnamed inside the struct, so C++ scopes its
        // enumerators to msc_host_event_t.
        if (event.event == msc_host_event_t::MSC_DEVICE_CONNECTED) {
            msc_mount(event.device.address);
        } else if (event.event == msc_host_event_t::MSC_DEVICE_DISCONNECTED) {
            msc_unmount();
        }
    }
}

static void storage_start_usb_host(void)
{
    s_msc_event_queue = xQueueCreate(4, sizeof(msc_host_event_t));
    if (s_msc_event_queue == nullptr) {
        return;
    }

    usb_host_config_t host_config = {};
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreatePinnedToCore(usb_host_lib_task, "usb_events", 4096, nullptr, 2, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "usb events task create failed");
        return;
    }

    msc_host_driver_config_t msc_config = {};
    msc_config.create_backround_task = true;
    msc_config.task_priority = 5;
    msc_config.stack_size = 4096;
    msc_config.callback = msc_event_callback;
    err = msc_host_install(&msc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install failed: %s", esp_err_to_name(err));
        return;
    }
    // FatFS mount work happens here (not in the driver task), so give it a
    // roomier stack.
    if (xTaskCreatePinnedToCore(msc_event_task, "usb_msc", 6144, nullptr, 4, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "msc event task create failed");
        return;
    }
    ESP_LOGI(TAG, "USB host ready (drives mount at %s)", kUsbMountPoint);
}

} // namespace

extern "C" bool STORAGE_UsbMounted(void)
{
    return s_usb_mounted;
}

extern "C" const char *STORAGE_UsbMountPoint(void)
{
    return s_usb_mounted ? kUsbMountPoint : nullptr;
}

#else // !NRL_HAS_USB_HOST

extern "C" bool STORAGE_UsbMounted(void)
{
    return false;
}

extern "C" const char *STORAGE_UsbMountPoint(void)
{
    return nullptr;
}

#endif // NRL_HAS_USB_HOST

// ---------------------------------------------------------------------------
// SMB network share backend (libsmb2 via services/smb_vfs, all boards)
// ---------------------------------------------------------------------------
#include "lib/nrl_net_compat.h"
#include "services/config_notify.h"
#include "services/music_playlist.h"
#include "services/smb_vfs.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *kSmbNvsNamespace = "smb";
constexpr uint32_t kSmbRetryMs = 30000u;

static char s_smb_server[64] = {};
static char s_smb_share[64] = {};
static char s_smb_user[32] = {};
static char s_smb_pass[64] = {};
static TaskHandle_t s_smb_task = nullptr;
static volatile bool s_smb_task_restart = false;

static bool smb_load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kSmbNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(s_smb_server);
    const bool have_server = nvs_get_str(nvs, "server", s_smb_server, &len) == ESP_OK;
    len = sizeof(s_smb_share);
    const bool have_share = nvs_get_str(nvs, "share", s_smb_share, &len) == ESP_OK;
    len = sizeof(s_smb_user);
    (void)nvs_get_str(nvs, "user", s_smb_user, &len);
    len = sizeof(s_smb_pass);
    (void)nvs_get_str(nvs, "pass", s_smb_pass, &len);
    nvs_close(nvs);
    return have_server && have_share && s_smb_server[0] != '\0' && s_smb_share[0] != '\0';
}

static bool smb_save_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kSmbNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_str(nvs, "server", s_smb_server) == ESP_OK &&
                    nvs_set_str(nvs, "share", s_smb_share) == ESP_OK &&
                    nvs_set_str(nvs, "user", s_smb_user) == ESP_OK &&
                    nvs_set_str(nvs, "pass", s_smb_pass) == ESP_OK &&
                    nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

// Waits for WiFi, then mounts; keeps retrying while unreachable. Restarted
// (via s_smb_task_restart) when the configuration changes.
static void smb_mount_task(void *)
{
    while (true) {
        s_smb_task_restart = false;
        while (!nrlNetworkConnected() && !s_smb_task_restart) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (!s_smb_task_restart) {
            if (SMB_VFS_Mount(s_smb_server, s_smb_share, s_smb_user, s_smb_pass)) {
                (void)PLAYLIST_Scan();
                break; // mounted; a config change spawns a fresh task
            }
            // Unreachable (NAS down, wrong credentials...): retry later.
            for (uint32_t waited = 0; waited < kSmbRetryMs && !s_smb_task_restart; waited += 1000u) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if (s_smb_task_restart) {
            continue;
        }
    }
    s_smb_task = nullptr;
    vTaskDelete(nullptr);
}

static void smb_start_mount_task(void)
{
    if (s_smb_task != nullptr) {
        s_smb_task_restart = true;
        return;
    }
    if (xTaskCreatePinnedToCore(smb_mount_task, "smb_mount", 6144, nullptr, 3, &s_smb_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "smb mount task create failed");
        s_smb_task = nullptr;
    }
}

} // namespace

extern "C" bool STORAGE_SmbConfigure(const char *server, const char *share,
                                     const char *user, const char *password)
{
    if (server == nullptr || server[0] == '\0' || share == nullptr || share[0] == '\0') {
        return false;
    }
    snprintf(s_smb_server, sizeof(s_smb_server), "%s", server);
    snprintf(s_smb_share, sizeof(s_smb_share), "%s", share);
    snprintf(s_smb_user, sizeof(s_smb_user), "%s", (user != nullptr) ? user : "");
    snprintf(s_smb_pass, sizeof(s_smb_pass), "%s", (password != nullptr) ? password : "");
    if (!smb_save_config()) {
        ESP_LOGW(TAG, "smb config persist failed (mount continues)");
    }
    SMB_VFS_Unmount();
    smb_start_mount_task();
    CONFIG_NOTIFY_Bump();
    return true;
}

extern "C" void STORAGE_SmbClear(void)
{
    SMB_VFS_Unmount();
    s_smb_server[0] = '\0';
    s_smb_share[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(kSmbNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_erase_all(nvs);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    (void)PLAYLIST_Scan();
    CONFIG_NOTIFY_Bump();
}

extern "C" bool STORAGE_SmbMounted(void)
{
    return SMB_VFS_Mounted();
}

extern "C" const char *STORAGE_SmbMountPoint(void)
{
    return SMB_VFS_Mounted() ? SMB_VFS_MOUNT_POINT : nullptr;
}

extern "C" void STORAGE_SmbDescribe(char *out, const size_t out_size)
{
    if (out == nullptr || out_size == 0u) {
        return;
    }
    if (s_smb_server[0] == '\0') {
        snprintf(out, out_size, "(not configured)");
        return;
    }
    snprintf(out, out_size, "//%s/%s (%s)", s_smb_server, s_smb_share,
             SMB_VFS_Mounted() ? "mounted" : "connecting");
}

extern "C" bool STORAGE_SmbGetConfig(char *server, const size_t server_size,
                                     char *share, const size_t share_size,
                                     char *user, const size_t user_size,
                                     char *password, const size_t password_size)
{
    if (server != nullptr && server_size > 0u) {
        snprintf(server, server_size, "%s", s_smb_server);
    }
    if (share != nullptr && share_size > 0u) {
        snprintf(share, share_size, "%s", s_smb_share);
    }
    if (user != nullptr && user_size > 0u) {
        snprintf(user, user_size, "%s", s_smb_user);
    }
    if (password != nullptr && password_size > 0u) {
        snprintf(password, password_size, "%s", s_smb_pass);
    }
    return s_smb_server[0] != '\0' && s_smb_share[0] != '\0';
}

// ---------------------------------------------------------------------------

extern "C" bool STORAGE_Init(void)
{
#if defined(NRL_HAS_SDCARD) && NRL_HAS_SDCARD
    if (!s_sd_mounted) {
        storage_mount_sdcard();
    }
#endif
#if defined(NRL_HAS_USB_HOST) && NRL_HAS_USB_HOST
    static bool usb_started = false;
    if (!usb_started) {
        usb_started = true;
        storage_start_usb_host();
    }
#endif
    // SMB share configured earlier: mount once either network is up.
    static bool smb_started = false;
    if (!smb_started && smb_load_config()) {
        smb_started = true;
        smb_start_mount_task();
    }
    return STORAGE_SdMounted() || STORAGE_UsbMounted() || STORAGE_SmbMounted();
}
