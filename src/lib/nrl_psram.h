#pragma once

#include "sdkconfig.h"

#if !defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) || \
    !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "NRL static buffers require CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y"
#endif

#include <esp_attr.h>

#define NRL_PSRAM_BSS EXT_RAM_BSS_ATTR
