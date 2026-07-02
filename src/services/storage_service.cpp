#include "services/storage_service.h"

#include "driver/board_pins.h"

#include <esp_log.h>

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
    if (err != ESP_OK) {
        // No card inserted is a normal condition; the music player simply
        // has nothing to index. Hot-insert requires a reboot for now.
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
#if (defined(NRL_HAS_SDCARD) && NRL_HAS_SDCARD) || (defined(NRL_HAS_USB_HOST) && NRL_HAS_USB_HOST)
    return STORAGE_SdMounted() || STORAGE_UsbMounted();
#else
    ESP_LOGD(TAG, "no removable storage on this board");
    return false;
#endif
}
