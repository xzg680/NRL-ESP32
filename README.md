# NRL ESP32 Radio Bridge

[English manual](README.en.md)

本项目是基于 ESP32-S3 + ES8311 的 NRL 网络语音电台桥接固件，用于把电台音频、PTT、SQL、频道选择、串口透明传输和网络配置集中到一个嵌入式应用中。当前工程主要面向 Moto3188/NRL 分支硬件。

## 支持功能

- NRL UDP 网络语音桥接
  - 电台 SQL 有效时，采集 ES8311 ADC 音频并通过 UDP 发送到 NRL 服务器。
  - 收到服务器下行语音时，拉起 PTT 输出，并通过 ES8311 DAC 播放到电台音频输入。
  - 使用 A-law 编解码，默认采样率 8 kHz。

- WiFi 和服务器配置门户
  - 设备启动后提供配置 AP 和 Web 页面。
  - 默认配置入口 IP 为 `192.168.4.1`。
  - 支持扫描附近 WiFi、配置 WiFi SSID/密码、服务器地址、端口、频道、呼号、音量等参数。
  - 长按 BOOT 键 5 秒可重置网络相关配置。

- 电台控制 IO
  - PTT 输出控制电台发射。
  - SQL1/SQL2 输入检测电台接收状态。
  - 三位频道选择输出，支持 `0..7` 共 8 个频道。
  - 状态灯显示网络心跳、SQL 状态和 PTT 状态。

- ES8311 音频编解码
  - I2S 主模式驱动 ES8311。
  - 支持麦克风输入增益、线路输出音量、HP Drive 输出模式配置。
  - 支持接收模式下的下行音频播放队列。

- SCI 串口透明传输
  - 支持通过 NRL 数据包转发 SCI 串口数据。
  - 默认 SCI 配置为 `9600,8,N,1`。
  - SCI 参数可通过 AT 命令更新。

- 远程 AT 命令配置
  - 支持查询和设置频道、服务器、呼号、SSID、音量、SCI 参数等。
  - 支持远程重启命令。
  - 常用命令包括 `AT+CH`、`AT+D_IP`、`AT+D_PORT`、`AT+CALL`、`AT+SSID`、`AT+MIC_GAIN`、`AT+VOLUME`、`AT+HP_DRIVE`、`AT+SCI`、`AT+REBOOT`。

- 参数持久化
  - 电台配置保存到共享 Flash/EEPROM 区域。
  - 首次启动会写入默认配置，之后从持久化配置恢复。

## 默认配置

默认参数定义在 `src/lib/nrl_audio_config.h`：

| 项目 | 默认值 |
| --- | --- |
| WiFi SSID | `NRL-ESP32` |
| WiFi 密码 | `12345678` |
| 服务器地址 | `110.42.107.105` |
| 服务器端口 | `60050` |
| 本地端口 | `60050` |
| 呼号 | `NOCALL` |
| 呼号 SSID | `0` |
| 设备模式 | `55` |
| 下行语音超时 | `120 ms` |
| 心跳间隔 | `2000 ms` |

## 主要管脚

管脚集中定义在 `src/app/driver/board_pins.h`。

| 功能 | GPIO |
| --- | --- |
| PTT 输出 | `8` |
| SQL1 输入 | `17` |
| SQL2 输入 | `18` |
| 网络/心跳状态灯 | `1` |
| SQL 状态灯 | `2` |
| PTT 红灯 | `42` |
| 频道 bit0 | `40` |
| 频道 bit1 | `39` |
| 频道 bit2 | `38` |
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
| BOOT 按键 | `0` |

频道输出为三位二进制编码，例如频道 `0` 输出 `000`，频道 `7` 输出 `111`。

## 启动流程

固件入口在 `src/app/main.cpp`：

1. 初始化串口日志。
2. 初始化外部电台配置和频道选择 IO。
3. 应用已保存的音频配置。
4. 初始化 PTT、SQL 和状态灯 IO。
5. 启动 WiFi 配置门户。
6. 初始化 ES8311 音频编解码器并进入接收模式。
7. 启动 NRL 音频桥接任务。

收到网络下行语音时，固件会拉高 PTT 并开始向电台输出音频；语音包超时后释放 PTT。

## 构建和烧录

工程使用 PlatformIO，默认环境是 `app0_main`。

```powershell
platformio run -e app0_main
platformio run -e app0_main -t upload
```

串口监视：

```powershell
platformio device monitor -b 115200
```

分区表使用 `part.csv`，当前应用分区为 `app0`。

## 目录结构

```text
src/app/main.cpp                  固件入口
src/app/driver/board_pins.h       板级管脚定义
src/app/driver/external_radio.*   电台配置、频道和持久化
src/app/driver/status_io.*        PTT、SQL、状态灯
src/app/driver/es8311.*           ES8311/I2S 音频驱动
src/app/driver/sci_serial.*       SCI 串口
src/lib/nrl_audio_bridge.*        NRL UDP 音频桥接
src/lib/nrl_at_commands.*         远程 AT 命令
src/lib/wifi_config_portal.*      Web 配置门户
src/lib/nrl_audio_config.h        默认网络和音频参数
scripts/                          构建辅助脚本
```

## 许可证

本项目使用 MIT License，详见 `LICENSE`。
