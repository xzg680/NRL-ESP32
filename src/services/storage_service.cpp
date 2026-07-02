#include "services/storage_service.h"

#include "driver/board_pins.h"

#include <esp_log.h>

static const char *TAG = "STORAGE";

#if defined(NRL_HAS_SDCARD) && NRL_HAS_SDCARD

#include "bsp/sdcard.h"

namespace {
static bool s_sd_mounted = false;
}

extern "C" bool STORAGE_Init(void)
{
    if (s_sd_mounted) {
        return true;
    }

    sdmmc_card_t *card = nullptr;
    const esp_err_t err = bsp_sdcard_mount(nullptr, &card);
    if (err != ESP_OK) {
        // No card inserted is a normal condition; the music player simply
        // has nothing to index. Hot-insert requires a reboot for now.
        ESP_LOGW(TAG, "TF card mount failed: %s (no card inserted?)", esp_err_to_name(err));
        return false;
    }

    s_sd_mounted = true;
    ESP_LOGI(TAG, "TF card mounted at %s: %s %lluMB",
             bsp_sdcard_get_mount_point(),
             (card != nullptr) ? card->cid.name : "?",
             (card != nullptr)
                 ? (static_cast<unsigned long long>(card->csd.capacity) * card->csd.sector_size) / (1024ULL * 1024ULL)
                 : 0ULL);
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

extern "C" bool STORAGE_Init(void)
{
    ESP_LOGD(TAG, "no removable storage on this board");
    return false;
}

extern "C" bool STORAGE_SdMounted(void)
{
    return false;
}

extern "C" const char *STORAGE_SdMountPoint(void)
{
    return nullptr;
}

#endif // NRL_HAS_SDCARD
