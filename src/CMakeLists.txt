set(app_sources
    app/main.cpp
    app/aec/aec_processor.cpp
    app/aec/esp_sr_probe.cpp
    app/driver/eeprom.cpp
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
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        app
        app/driver
)
