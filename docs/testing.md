# S31 集中实机测试清单

> 对应代码: Phase 0-4 全部功能 (提交 eb45f10 及之前)。
> 烧录: `python scripts/build.py s31_korvo flash monitor -p COM口`
> 注意: 分区表已从 part-gezipai 换到 part-s31 (7MB app 槽),
> 旧布局设备首次需整片重刷 (不能只 OTA)。

## 0. 回归基线 (最优先 -- Phase 0 音频路由重构后未实测)

- [ ] NRL 对讲: 收发正常, 无杂音/断续 (对照重构前印象)
- [ ] BT 耳机: 配对/自动重连/通话 (耳机 mic 上行 + 下行入耳机)
- [ ] AEC / AI 降噪 / 哑音过滤开关生效
- [ ] Web 门户 / 屏幕 / AT 三路配置正常

## 1. 音乐播放器

- [ ] TF 卡 /sdcard/music 扫描, 触摸列表点播, 自动连播
- [ ] MP3 / FLAC / WAV / M4A 各放一首; 44.1k 与 48k 各试
- [ ] 中文标签 + 封面显示; `AT+FONT=FT` / `BMP` 切换对比
      (FT 需 TF 卡 fonts/cjk.ttf)
- [ ] U 盘插入自动并入曲库, 播放中拔出 → 停止不死机
- [ ] `AT+SMB=NAS地址/共享,用户,密码` → 网络曲库播放, 断网重试
- [ ] 网络电台: Radio 页 / `AT+PLAY=http://...`; m3u8 电台
- [ ] 播放中来 NRL 语音 → 音乐让路, 语音完整

## 2. 保姆功能

- [ ] `AT+TARGET=NET`: 音乐推上 NRL 服务器 (对端听), 本地喇叭静,
      本板 mic 不混入; 期间本地对讲下行仍可听
- [ ] `AT+TARGET=BOTH`: 本地 + 网络同时, 两边同步
- [ ] `AT+BEACON=/sdcard/beacon/id.wav,2` (2 分钟快测): 定时插播,
      QSO 进行中顺延, 播完自动回到歌单; 重启后仍生效

## 3. 宽带语音 (Opus / NRL 类型 8)

- [ ] `AT+CODEC=OPUS` 两台互通: 音质明显优于 G711; 与 G711 台互通
      (RX 双类型常开)
- [ ] AFE 开/关下都能发 (开 AFE 时带宽受限是已知项)
- [ ] `AT+CODEC=G711` 回退, 老网络兼容

## 4. BT A2DP 音乐

- [ ] `AT+OUTPUT=BT` + 播歌 → 耳机出立体声音乐
- [ ] 播放中来 NRL 语音 → 音乐停/A2DP 挂起 → HFP 语音入耳机 →
      通话完可手动续播
- [ ] 耳机不可达时自动回退喇叭
- [ ] 观察: 音乐是否卡顿 (SBC 配速/coex 参数可调)

## 5. ESP-NOW 对讲

- [ ] 两台 `AT+ESPNOW=ON` (同 WiFi 信道): 屏幕 ESP-NOW 页 PTT 对讲
- [ ] `AT+ESPNOW=?` 显示对端呼号; 重启后状态恢复

## 6. 小智 AI

- [ ] `AT+AI=wss://服务器/xiaozhi/v1/,token` → `AT+AI=?` 显示 connected
- [ ] `AT+AITALK=START` 说话 `=STOP` → 喇叭播回答;
      日志有 STT/回答文本
- [ ] 断网自动重连

## 7. 视频通话

- [ ] 两台: A `AT+VIDEO=ON`, B 打开 Video 页 → 看到 A 画面 (~5fps)
- [ ] 双向同开; 同时语音通话, 观察语音是否受视频流量影响
- [ ] 屏幕 Cam ON/OFF 按钮; 摄像头 bring-up 失败看日志 (VIDEO tag)

## 8. 俄罗斯方块

- [ ] Apps → Tetris: 操作/消行/加速/GAME OVER/重开/Back
- [ ] 游戏中来 NRL 语音: 声音正常, 游戏不受影响

## 9. 高解析 (数据收集, 供后续开发)

- [ ] 播 96k / 24bit FLAC: 预期报 "unsupported PCM ... 16-bit only",
      把日志中解码器给出的采样率/位深发回
- [ ] `AT+PLAY` 一首 44.1k 后紧接 48k: 换采样率是否有爆音

## 已知限制 (设计内)

- AFE 开启时 Opus 上行有效带宽仍是 8k 上采样 (aec_processor 末级
  降采样待改造)
- 通话结束音乐不自动续播 (可加 voice-end 通知实现)
- 双源同时进喇叭 (如 ESP-NOW 与 NRL 同时来语音) 是交错不是混音
- U 盘与将来 UVC 摄像头共用 USB-OTG 口
- ID3v1 老文件 GBK 标签显示乱码 (ID3v2 中文正常)
