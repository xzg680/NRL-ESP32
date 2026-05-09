# NRL ESP32 Radio Bridge

This project is an ESP32-S3 + ES8311 firmware for an NRL network radio bridge. It connects radio audio, PTT, SQL, channel selection, SCI serial passthrough, and network configuration in one embedded application. The current pin map targets the Moto3188/NRL hardware branch.

## Features

- NRL UDP network audio bridge
  - When radio SQL is active, the firmware captures ES8311 ADC audio and sends it to the NRL server over UDP.
  - When downlink voice is received from the server, the firmware enables PTT and plays decoded audio through the ES8311 DAC to the radio audio input.
  - Audio uses A-law encoding/decoding at the default 8 kHz sample rate.

- WiFi and server configuration portal
  - The device starts a configuration AP and web page.
  - Default portal IP: `192.168.4.1`.
  - Supports WiFi scanning and configuration of WiFi SSID/password, server host, server port, channel, callsign, audio volume, and ES8311 output mode.
  - Holding the BOOT button for 5 seconds resets network-related settings.

- Radio control IO
  - PTT output controls radio transmit.
  - SQL1/SQL2 inputs detect radio receive/squelch state.
  - Three channel-selection outputs provide 8 channels, `0..7`.
  - Status LEDs show network heartbeat, SQL state, and PTT state.

- ES8311 audio codec support
  - Drives ES8311 in I2S master mode.
  - Supports microphone input gain, line-out volume, and HP Drive output mode.
  - Provides a downlink playback queue while the codec remains in receive mode.

- SCI serial passthrough
  - Forwards SCI serial data through NRL packets.
  - Default SCI configuration: `9600,8,N,1`.
  - SCI parameters can be changed through remote AT commands.

- Remote AT command configuration
  - Supports querying and changing channel, server settings, callsign, SSID, volume, SCI parameters, and output mode.
  - Supports remote reboot.
  - Common commands: `AT+CH`, `AT+D_IP`, `AT+D_PORT`, `AT+CALL`, `AT+SSID`, `AT+MIC_GAIN`, `AT+VOLUME`, `AT+HP_DRIVE`, `AT+SCI`, `AT+REBOOT`.

- Persistent configuration
  - Radio and network configuration is saved to the shared Flash/EEPROM area.
  - On first boot, default settings are written. Later boots restore the persisted configuration.

## Default Configuration

Defaults are defined in `src/lib/nrl_audio_config.h`.

| Item | Default |
| --- | --- |
| WiFi SSID | `NRL-ESP32` |
| WiFi password | `12345678` |
| Server host | `110.42.107.105` |
| Server port | `60050` |
| Local port | `60050` |
| Callsign | `NOCALL` |
| Callsign SSID | `0` |
| Device mode | `55` |
| Downlink voice timeout | `120 ms` |
| Heartbeat interval | `2000 ms` |

## Main Pins

Pins are centralized in `src/app/driver/board_pins.h`.

| Function | GPIO |
| --- | --- |
| PTT output | `8` |
| SQL1 input | `17` |
| SQL2 input | `18` |
| Network/heartbeat LED | `1` |
| SQL LED | `2` |
| PTT red LED | `42` |
| Channel bit0 | `40` |
| Channel bit1 | `39` |
| Channel bit2 | `38` |
| SCI RX | `6` |
| SCI TX | `7` |
| I2C SCL | `14` |
| I2C SDA | `21` |
| PA EN | `46` |
| I2S MCLK | `9` |
| I2S BCLK | `10` |
| I2S DOUT | `13` |
| I2S LRCLK | `12` |
| I2S DIN | `11` |
| BOOT button | `0` |

Channel outputs use 3-bit binary encoding. For example, channel `0` outputs `000`, and channel `7` outputs `111`.

## Startup Flow

The firmware entry point is `src/app/main.cpp`.

1. Initialize serial logging.
2. Initialize external radio configuration and channel-selection IO.
3. Apply saved audio configuration.
4. Initialize PTT, SQL, and status LED IO.
5. Start the WiFi configuration portal.
6. Initialize the ES8311 audio codec and enter receive mode.
7. Start the NRL audio bridge task.

When downlink network voice is received, the firmware enables PTT and starts feeding audio to the radio. When voice packets time out, PTT is released.

## Build and Flash

This project uses PlatformIO. The default environment is `app0_main`.

```powershell
platformio run -e app0_main
platformio run -e app0_main -t upload
```

Serial monitor:

```powershell
platformio device monitor -b 115200
```

The partition table is `part.csv`, and the current application partition is `app0`.

## Project Layout

```text
src/app/main.cpp                  Firmware entry point
src/app/driver/board_pins.h       Board pin mapping
src/app/driver/external_radio.*   Radio configuration, channel, persistence
src/app/driver/status_io.*        PTT, SQL, status LEDs
src/app/driver/es8311.*           ES8311/I2S audio driver
src/app/driver/sci_serial.*       SCI serial driver
src/lib/nrl_audio_bridge.*        NRL UDP audio bridge
src/lib/nrl_at_commands.*         Remote AT commands
src/lib/wifi_config_portal.*      Web configuration portal
src/lib/nrl_audio_config.h        Default network and audio settings
scripts/                          Build helper scripts
```

## License

This project is licensed under the MIT License. See `LICENSE` for details.

