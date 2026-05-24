set(app_sources
    app/main.cpp
    app/aec/aec_processor.cpp
    app/aec/esp_sr_probe.cpp
    app/driver/eeprom.cpp
    app/driver/display.cpp
    app/driver/es7210.cpp
    app/driver/es8311.cpp
    app/driver/external_radio.cpp
    app/driver/i2c.cpp
    app/driver/sci_serial.cpp
    app/driver/status_io.cpp
    lib/ble_config.cpp
    lib/nrl_at_commands.cpp
    lib/nrl_audio_bridge.cpp
    lib/wifi_config_portal.cpp
    lib/wifi_config_portal_view.cpp
)

# LVGL is vendored under components/lvgl/ only for the gezipai board (the
# fetch_lvgl.py extra_script runs only in [env:gezipai]). On bh4tdv, the
# directory is absent and display.cpp's `#include <lvgl.h>` is itself gated
# behind `#if NRL_HAS_DISPLAY`, so we must not declare the dependency.
set(app_priv_requires "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../components/lvgl/lvgl.h")
    list(APPEND app_priv_requires lvgl)
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        app
        app/driver
    PRIV_REQUIRES ${app_priv_requires}
)
