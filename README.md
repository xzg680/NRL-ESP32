# NRL ESP32 Radio Bridge

[English manual](README.en.md)

当前固件版本：`0.1.6`

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
  - 支持在 `/update` 页面通过 WiFi 上传 `firmware.bin` 进行 OTA 刷机。
  - 长按 BOOT 键 5 秒可重置网络相关配置。

- BLE 蓝牙配置
  - 设备启动后广播 BLE 设备名 `NRL-ESP32-CFG`。
  - 使用 Nordic UART 风格的 BLE 服务，通过手机或电脑 BLE 工具写入文本命令。
  - 支持配置 WiFi SSID/密码、服务器地址、端口、频道和呼号。
  - 配置保存后会自动重启 WiFi/UDP 连接。

- 电台控制 IO
  - PTT 输出控制电台发射。
  - SQL1/SQL2 输入检测电台接收状态。
  - 三位频道选择输出，支持 `0..7` 共 8 个频道。
  - 状态灯显示网络心跳、SQL 状态和 PTT 状态。

- PTT 按键发射控制（格子派）
  - 短按 PTT 键开启发射，再短按一次关闭发射（锁定式）。
  - 长按 PTT 键为即时发射（按住发射），松开按键立即停止发送。
  - 发射超时：发射持续超过设定时间后自动强制关闭（短按锁定和长按均受此限制）。
  - 超时时间默认 5 分钟，范围 `5..3600` 秒，可通过 `AT+PTT_TIMEOUT` 或 Web 配置页调节。

- ES8311 音频编解码
  - I2S 主模式驱动 ES8311。
  - 支持麦克风输入增益、线路输出音量、HP Drive 输出模式配置。
  - 支持接收模式下的下行音频播放队列。

- 尾音消除（防回波激发）
  - ESP32 把网络语音播放给电台结束后，会在一段时间内丢弃电台返回的语音，不再转发到网络。
  - 用于抑制电台所接中继台的应答回波，避免网络上 2 台以上设备相互激发。
  - 默认 `0`（不抑制），范围 `0..5000` 毫秒，可通过 `AT+TAIL_SUPPRESS` 或 Web 配置页调节。

- 屏幕显示（仅格子派）
  - 板载 ST7789 240x240 SPI 彩屏，使用 LVGL 渲染，简洁现代科技风格深色界面。
  - 中间主区域显示呼号（接收语音时为呼叫方，待机/发送时为本机配置的呼号），呼号下方是较小的 SSID，再下面一行是当前时间。
  - 顶部状态栏左侧显示 WiFi 信号强度（dB 值），右侧显示电池电压。
  - 底部显示设备获取到的局域网 IP 地址；WiFi 连接失败进入配网状态时，改为显示配置热点的 IP 地址。
  - 时间通过 NTP 同步（CST-8 时区），未联网时显示 `--:--:--`。

- SCI 串口透明传输
  - 支持通过 NRL 数据包转发 SCI 串口数据。
  - 默认 SCI 配置为 `9600,8,N,1`。
  - SCI 参数可通过 AT 命令更新。

- 远程 AT 命令配置
  - 支持查询和设置频道、服务器、呼号、SSID、音量、SCI 参数等。
  - 支持远程重启命令。
  - 常用命令包括 `AT+CH`、`AT+D_IP`、`AT+D_PORT`、`AT+CALL`、`AT+SSID`、`AT+PTT_TIMEOUT`、`AT+TAIL_SUPPRESS`、`AT+MIC_GAIN`、`AT+VOLUME`、`AT+HP_DRIVE`、`AT+SCI`、`AT+REBOOT`。

- 参数持久化
  - 电台配置保存到共享 Flash/EEPROM 区域。
  - 首次启动会写入默认配置，之后从持久化配置恢复。

## 默认配置

默认参数定义在 `src/lib/nrl_audio_config.h`：

| 项目 | 默认值 |
| --- | --- |
| WiFi SSID | `NRL-ESP32` |
| WiFi 密码 | `12345678` |
| 服务器地址 | `101.133.166.204` |
| 服务器端口 | `60050` |
| 本地端口 | `60050` |
| 呼号 | `NOCALL` |
| 呼号 SSID | `0` |
| 设备模式 | `22` |
| 下行语音超时 | `120 ms` |
| 心跳间隔 | `2000 ms` |

## BLE 配置命令

可使用 nRF Connect、LightBlue 等 BLE 工具连接 `NRL-ESP32-CFG`。

| 项目 | UUID |
| --- | --- |
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| RX Write | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| TX Notify | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

向 RX 特征写入以换行结尾的文本命令：

```text
HELP
GET
SET WIFI_SSID=your_ssid
SET WIFI_PASS=your_password
SET SERVER_HOST=101.133.166.204
SET SERVER_PORT=60050
SET CHANNEL=0
SET CALLSIGN=NOCALL
SAVE
APPLY
RESET_NET
REBOOT
```

`SET` 命令会立即保存配置；如果改动了 WiFi 或服务器参数，会自动让网络桥重新连接。`GET` 会通过 TX Notify 返回当前主要配置。

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

## 屏幕显示（格子派）

格子派板载一块 ST7789 240x240 SPI 彩屏，管脚与小智（xiaozhi）格子派板一致。屏幕驱动位于 `src/app/driver/display.cpp`，面板用 IDF 内置的 `esp_lcd` ST7789 驱动，界面用 LVGL 渲染。

| 功能 | GPIO |
| --- | --- |
| LCD SCLK | `7` |
| LCD MOSI | `6` |
| LCD DC | `16` |
| LCD CS | `15` |
| LCD RST | `5` |
| LCD 背光 | `4` |
| 电池电压 ADC | `3`（ADC1 通道 2，1:2 分压） |

由于 LCD 占用 GPIO 4-7/15/16，格子派的 SCI 串口已从默认的 GPIO 4/5 移到 GPIO 9（RX）/ 8（TX），以避让 LCD 背光（4）与复位（5）。BH4TDV 板没有屏幕，SCI 仍为 GPIO 4/5。

界面布局：

- 顶部状态栏：左侧 WiFi 信号强度（dB），中间输出音量（百分比），右侧电池电压。
- 中间主区域：呼号上方的英文状态标题、呼号（大号字体）、SSID（小号）、当前时间。接收语音时显示呼叫方的呼号/SSID，待机或发送时显示本机配置的呼号/SSID（仅语音包参与判断，心跳包不影响）。
- 状态标题随收发实时变化：`STANDBY`（待机）、`RECEIVING`（接收中）、`TRANSMITTING`（发送中）、`FULL DUPLEX`（同时收发）。
- 底部：局域网 IP；进入配网（AP）状态时显示配置热点 IP（琥珀色）；**发送或接收语音时改为显示 NRL 服务器 host —— 发送为红色、接收为青色，显示配置的 host 字符串而非解析后的 IP**。

LVGL（9.3.0）以**本地组件**形式内置在 `components/lvgl`，不经 ESP-IDF 组件管理器联网下载，因此本机和 CI 离线即可编译。LVGL 与 esp_lcd 的对接（缓冲区、刷新、tick）直接写在 `display.cpp` 里，未使用 `esp_lvgl_port`。LVGL 字体、颜色深度等通过 Kconfig 配置在 `sdkconfig.defaults`。

## 启动流程

固件入口在 `src/app/main.cpp`：

1. 初始化串口日志。
2. 初始化外部电台配置和频道选择 IO。
3. 应用已保存的音频配置。
4. 初始化 PTT、SQL 和状态灯 IO。
5. 初始化屏幕（仅格子派：ST7789 LCD + LVGL 界面）。
6. 启动 WiFi 配置门户。
7. 初始化 ES8311 音频编解码器并进入接收模式。
8. 启动 NRL 音频桥接任务。

收到网络下行语音时，固件会拉高 PTT 并开始向电台输出音频；语音包超时后释放 PTT。

## 构建和烧录

工程使用原生 ESP-IDF（≥6.1，含 ESP32-S31 支持），不再使用 PlatformIO。共有三块板：
`gezipai`（格子派，ESP32-S3）、`bh4tdv`（BH4TDV 3188，ESP32-S3）、`s31_korvo`
（ESP32-S31-Korvo-1，ESP32-S31）。

首次需安装 ESP-IDF 工具链（一次性）：

```powershell
C:\esp\esp-idf\install.ps1 esp32s3,esp32s31
```

之后每开一个终端，先激活 ESP-IDF 环境：

```powershell
C:\esp\esp-idf\export.ps1        # Linux/macOS 用: . export.sh
```

用构建助手按板名编译/烧录/监视（第一个参数是板名，其余原样透传给 idf.py）：

```powershell
python scripts/build.py gezipai build                     # 编译格子派
python scripts/build.py bh4tdv build                      # 编译 BH4TDV
python scripts/build.py s31_korvo flash monitor -p COM5   # S31: 编译+烧录+监视
python scripts/build.py gezipai menuconfig                # 修改配置
```

每块板有独立的 `build/<board>/` 目录与 `sdkconfig`，可随意切换互不干扰。板级配置：
`NRL_BOARD` 由脚本通过 `-DNRL_BOARD_ID` 传入；分区表与其它 Kconfig 来自
`sdkconfig.defaults`（S31 追加 `sdkconfig.defaults.esp32s31`，bh4tdv 用
`sdkconfig.bh4tdv.defaults` 覆盖分区表）。

GitHub Actions 会在每次 push、pull request 或手动触发时，用官方 ESP-IDF 镜像原生构建
三块板，并上传各板的 `firmware` / `partition-table` / `bootloader` 作为构建产物，
打 tag 时发布到 Release。

## 固件刷机

### USB 网页刷机

工程提供 `web-flasher/` 页面，适合首次烧录或恢复设备。它会写入 bootloader、分区表、OTA data、应用固件和 esp-sr 模型。

> 仅支持两块 ESP32-S3 板（`gezipai` / `bh4tdv`）。ESP32-S31 因 esptool-js 不支持，
> 只能串口烧录（`python scripts/build.py s31_korvo flash`）。

先编译这两块板，再打包页面（`stage_web_flasher.py` 从 `build/<board>/flasher_args.json`
读取各镜像的偏移并生成 esp-web-tools manifest）：

```powershell
python scripts/build.py gezipai build
python scripts/build.py bh4tdv build
python scripts/stage_web_flasher.py
python -m http.server 8000 -d web-flasher
```

然后用 Chrome 或 Edge 打开 `http://localhost:8000`，通过 USB 串口安装固件。
（CI 也会在 `web-flasher` 任务里自动打包，打 tag 时把 `web-flasher-<版本>.zip` 发布到 Release。）

### WiFi 网页刷机

设备已经运行双 OTA 分区表后，可通过配置门户刷机：

1. 连接设备配置 AP，或访问设备在局域网中的 IP。
2. 打开 `http://192.168.4.1/update`，或从配置首页点击 `Firmware update`。
3. 上传对应板子的应用固件 `build/<板名>/nrl-esp32.bin`（例如格子派 `build/gezipai/nrl-esp32.bin`、BH4TDV `build/bh4tdv/nrl-esp32.bin`、S31 `build/s31_korvo/nrl-esp32.bin`）。
4. 上传完成后设备会自动重启到新固件。

注意：WiFi OTA 需要 `part.csv` 中的 `app0/app1` 双 OTA 分区布局。旧分区布局设备应先用 USB 网页刷机或串口刷机更新分区表。

## 目录结构

```text
src/app/main.cpp                  固件入口
src/app/driver/board_pins.h       板级管脚定义
src/app/driver/external_radio.*   电台配置、频道和持久化
src/app/driver/status_io.*        PTT、SQL、状态灯
src/app/driver/es8311.*           ES8311/I2S 音频驱动
src/app/driver/display.*          ST7789 LCD + LVGL 界面（仅格子派）
src/app/driver/sci_serial.*       SCI 串口
src/lib/nrl_audio_bridge.*        NRL UDP 音频桥接
src/lib/nrl_at_commands.*         远程 AT 命令
src/lib/ble_config.*              BLE 蓝牙配置
src/lib/wifi_config_portal.*      Web 配置门户
src/lib/nrl_audio_config.h        默认网络和音频参数
web-flasher/                      USB 网页刷机页面
scripts/                          构建辅助脚本
```

## 许可证

本项目使用 MIT License，详见 `LICENSE`。
