set(app_sources
    app/main.cpp
    app/aec/aec_processor.cpp
    app/aec/esp_sr_probe.cpp
    audio/audio_focus.cpp
    audio/audio_router.cpp
    media/wav_reader.cpp
    services/music_player.cpp
    services/storage_service.cpp
    app/driver/audio_passthrough.cpp
    app/driver/eeprom.cpp
    app/driver/display.cpp
    app/driver/display_s31.cpp
    app/driver/es7210.cpp
    app/driver/es8311.cpp
    app/driver/es8389.cpp
    app/driver/external_radio.cpp
    app/driver/i2c.cpp
    app/driver/s31_i2c.cpp
    app/driver/sci_serial.cpp
    app/driver/status_io.cpp
    lib/ble_config.cpp
    lib/nrl_bt_hfp.cpp
    lib/nrl_at_commands.cpp
    lib/nrl_audio_bridge.cpp
    lib/nrl_captive_dns.cpp
    lib/nrl_g711.cpp
    lib/nrl_usb_console.cpp
    lib/nrl_wifi.cpp
    lib/wifi_config_portal.cpp
    lib/wifi_config_portal_view.cpp
)

# LVGL is vendored under components/lvgl/ only for the gezipai board (the
# fetch_lvgl.py extra_script runs only in [env:gezipai]). On bh4tdv, the
# directory is absent and display.cpp's `#include <lvgl.h>` is itself gated
# behind `#if NRL_HAS_DISPLAY`, so we must not declare the dependency.
set(app_priv_requires
    app_update
    bt
    driver
    efuse
    esp_adc
    espressif__esp_codec_dev
    esp_coex
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_spi
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_event
    esp_http_server
    esp_lcd
    esp_netif
    esp_ringbuf
    esp_partition
    esp_psram
    esp_timer
    esp_wifi
    espressif__esp-sr
    lwip
    nvs_flash
    spi_flash
)
# BLE provisioning (ble_config.cpp) uses the NimBLE host via esp-nimble-cpp.
# S31 instead uses the Bluedroid host (Classic BT + HFP for a BT headset); the
# two hosts are mutually exclusive, so only pull esp-nimble-cpp on non-S31.
if(NOT IDF_TARGET STREQUAL "esp32s31")
    list(APPEND app_priv_requires h2zero__esp-nimble-cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../components/lvgl/lvgl.h")
    list(APPEND app_priv_requires lvgl)
endif()
if(IDF_TARGET STREQUAL "esp32s31" AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../vendor/esp32_s31_korvo/CMakeLists.txt")
    list(APPEND app_priv_requires esp32_s31_korvo)
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        .
        app
        app/driver
    PRIV_REQUIRES ${app_priv_requires}
    # WHOLE_ARCHIVE keeps app_main (and other referenced-by-startup symbols) from
    # being dropped at link time. Under PlatformIO src is the main component
    # (already whole-archived); for the native idf.py build (build-s31 job) src is
    # registered as a plain component via EXTRA_COMPONENT_DIRS, so it needs this
    # explicitly or the link fails with "undefined reference to app_main".
    WHOLE_ARCHIVE
)
