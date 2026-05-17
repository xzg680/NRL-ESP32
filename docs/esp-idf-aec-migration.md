# ESP-IDF and Gezipai AEC Migration Plan

## Goal

Migrate the project from PlatformIO Arduino to ESP-IDF and add full-duplex
device-side AEC for the Gezipai board only.

The BH4TDV / NRL 3188 board must remain on the current non-AEC audio path.
Board-specific behavior should continue to be selected with `NRL_BOARD`.

## Hardware Assumptions

### Gezipai

- MCU: ESP32-S3 N16R8
- Flash: 16 MB
- PSRAM: 8 MB, required for ESP-SR AFE/AEC
- Codec output: ES8311 DAC
- Codec input: ES7210 ADC
- ES7210 MIC1: near-end voice microphone
- ES7210 MIC2: far-end reference, wired from ES8311 output

### BH4TDV / NRL 3188

- Keep the existing ES8311-only receive/transmit bridge behavior.
- Do not enable ESP-SR AEC.
- Do not require ES7210 or PSRAM.

## Reference Project

Use the local Xiaozhi 2.0.2 ESP-IDF project provided by the user as the main
ESP-IDF reference.

Important files in the reference project:

- `main/idf_component.yml`
  - Uses ESP-IDF component manager.
  - Requires `idf >=5.4.0`.
  - Uses `espressif/esp-sr` and `espressif/esp_codec_dev`.
- `sdkconfig.defaults.esp32s3`
  - Enables PSRAM for ESP32-S3.
- `main/audio/processors/afe_audio_processor.cc`
  - Shows ESP-SR AFE setup and device-side AEC switching.
- `main/audio/codecs/box_audio_codec.cc`
  - Shows ES8311 output plus ES7210 input using ESP-IDF I2S and
    `esp_codec_dev`.
- `main/boards/waveshare-s3-touch-lcd-3.49/config.json`
  - Enables `CONFIG_USE_DEVICE_AEC=y` for an ES8311 + ES7210 style board.

Do not copy the whole Xiaozhi application. Reuse the audio architecture and
ESP-IDF component choices, while keeping this project's NRL protocol,
configuration model, SCI transport, and board abstraction.

## Proposed Audio Architecture

### Gezipai Full-Duplex Path

```text
UDP downlink A-law
    -> PCM decode
    -> ES8311 DAC output
    -> hardware reference into ES7210 MIC2

ES7210 MIC1 near-end voice
ES7210 MIC2 far-end reference
    -> interleaved MR frames
    -> ESP-SR AFE/AEC
    -> cleaned near-end PCM
    -> optional 16 kHz to 8 kHz downsample
    -> A-law encode
    -> UDP uplink
```

Use the ESP-SR AFE input format `MR`:

- `M`: ES7210 MIC1, near-end voice
- `R`: ES7210 MIC2, far-end reference

The hardware reference path is preferred over a software reference queue
because it includes the real DAC/output analog path and timing.

### BH4TDV / NRL 3188 Path

Keep the current single-board audio bridge behavior:

```text
ES8311 ADC -> A-law encode -> UDP uplink
UDP downlink -> A-law decode -> ES8311 DAC
```

No ESP-SR AEC should be compiled or initialized for this board.

## ESP-IDF Component Direction

Target ESP-IDF version:

```text
ESP-IDF 5.4 or newer
```

Core dependencies:

```yaml
dependencies:
  espressif/esp-sr: "~2.1.5"
  espressif/esp_codec_dev: "~1.4.0"
  idf:
    version: ">=5.4.0"
```

## PlatformIO Status

The project has been moved to the pioarduino `platform-espressif32` fork:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/54.03.21/platform-espressif32.zip
```

The local installed platform reports:

```text
platform-espressif32 54.03.21
framework-arduinoespressif32 3.2.1
framework-arduinoespressif32-libs 5.4.0
framework-espidf package URL: esp-idf-v5.4.2.zip
```

This satisfies the ESP-IDF 5.4+ version requirement for the Xiaozhi 2.0.2 AEC
reference path. The current project still builds as `framework = arduino`; the
ESP-IDF framework package is not installed until an `espidf` or mixed
`arduino, espidf` environment is added and built.

Recommended migration route:

```text
Keep the existing Arduino gezipai/bh4tdv environments intact.
Add a new Gezipai-only AEC migration environment using ESP-IDF 5.4.2.
Do not add AEC code to the BH4TDV build.
```

If upgrading to a newer `esp-sr` version, retest AFE config names and memory
usage because AFE/AEC APIs have changed across ESP-SR releases.

## Partition Direction

Current PlatformIO app partitions are about 1.625 MB each, which is too tight
for ESP-IDF plus ESP-SR.

For the ESP32-S3 N16R8 Gezipai board, use a 16 MB partition layout with larger
OTA slots. A starting point can follow the Xiaozhi 16 MB layout style:

```text
nvs
otadata
phy_init
ota_0
ota_1
assets / shared data
```

The existing `shared` data region must be preserved or migrated deliberately
because current configuration persistence uses a shared flash/EEPROM-style
area.

## Implementation Phases

### Phase 1: ESP-IDF Skeleton

- Add top-level ESP-IDF `CMakeLists.txt`.
- Add `main/` or component layout.
- Add `idf_component.yml`.
- Add `sdkconfig.defaults` and `sdkconfig.defaults.esp32s3`.
- Add 16 MB partition table for Gezipai.
- Keep PlatformIO files until ESP-IDF reaches feature parity.

### Phase 2: Board and HAL Port

- Preserve `NRL_BOARD_GEZIPAI` and `NRL_BOARD_BH4TDV`.
- Replace Arduino GPIO/time/serial calls with ESP-IDF APIs or a small local
  compatibility layer.
- Keep `board_pins.h` as the board pin authority.

### Phase 3: Codec and I2S Port

- Port ES8311 / ES7210 to ESP-IDF.
- For Gezipai, configure ES7210 for two-channel capture:
  - MIC1 = near-end voice
  - MIC2 = reference
- Validate channel order with serial logs and test tones before enabling AEC.
- Consider using `esp_codec_dev` for codec control if it matches the board
  well; otherwise keep the existing proven register sequences and move only
  I2S to ESP-IDF APIs.

### Phase 4: ESP-SR AEC

- Add a Gezipai-only AFE processor.
- Feed interleaved `MR` frames into ESP-SR.
- Start with a VOIP AEC mode because the application is a network voice bridge.
- Keep AEC disabled at compile time for BH4TDV.
- Add debug counters for:
  - mic peak / RMS
  - reference peak / RMS
  - AEC output peak / RMS
  - dropped frames
  - AFE feed/fetch sizes

### Phase 5: NRL Bridge Integration

- Keep the existing packet format and A-law encode/decode behavior.
- Prefer internal 16 kHz audio for ESP-SR.
- Downsample AEC output to 8 kHz before A-law encoding if the NRL protocol
  remains 8 kHz.
- Decode downlink A-law and play through ES8311 continuously during full-duplex
  sessions.

### Phase 6: Network, Config, OTA, BLE

- Port WiFi station/AP logic to ESP-IDF WiFi APIs.
- Port UDP to lwIP sockets or `esp_netif` compatible sockets.
- Port web configuration to `esp_http_server`.
- Port OTA to `esp_ota_ops`.
- Port BLE config to NimBLE or ESP-IDF GATT.

## Debug Checklist

Before judging AEC quality, verify:

- ES7210 MIC1 contains near-end microphone signal.
- ES7210 MIC2 contains ES8311 output reference signal.
- MIC1 and MIC2 are not swapped.
- MIC2 reference level is not clipped.
- MIC2 reference level is not too low compared with MIC1.
- AEC feed chunk size matches the amount of interleaved `MR` samples.
- Downlink playback continues while uplink capture is active.
- PSRAM is enabled and ESP-SR allocations are using PSRAM where expected.

## Estimated Schedule

With the confirmed Gezipai ESP32-S3 N16R8 hardware and MIC2 reference wiring:

- Gezipai ESP-IDF + AEC first working build: 7-10 working days
- Both boards organized and stable: 10-15 working days
- Extra acoustic tuning margin: 3-5 working days

The largest remaining risk is not memory. It is acoustic tuning: reference
level, channel order, frame alignment, and whether the internal audio chain
should run at 16 kHz while the NRL protocol remains 8 kHz.
