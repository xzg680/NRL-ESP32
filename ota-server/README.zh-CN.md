# NRL OTA 服务器（Go + SQLite + 内嵌 Vue）

可执行文件会内嵌 `frontend/dist`，因此部署时只需要一个 Go 二进制文件及其数据目录。以下四种板卡共用同一套 API：`gezipai`、`bh4tdv`、`s31_korvo` 和 `s31_function_coreboard`。

先构建一次内嵌的 Vue 管理站点，再构建 Go 服务：

Vite+（`vp`）使用 `PATH` 中的 `node` 运行，要求 **Node 24 或更高版本**（见 `frontend/.node-version`）。较低版本的 Node 运行 `vp` 启动器时会报 `ERR_UNKNOWN_FILE_EXTENSION`；请先切换至 Node 24，例如执行 `nvm use 24`。

```powershell
cd ota-server/frontend
vp install --frozen-lockfile
vp check
vp build
cd ..
go build -o nrl-ota.exe .
$env:OTA_ADMIN_TOKEN = 'long-random-admin-secret'      # 自动发布流水线使用的机器令牌
$env:OTA_ADMIN_USER = 'admin'                          # Web 登录用户名（默认：admin）
$env:OTA_ADMIN_PASSWORD = 'long-random-admin-password' # Web 登录密码（启用管理界面登录所必需）
$env:OTA_DEVICE_TOKEN = 'optional-device-access-token'
.\nrl-ota.exe -listen 127.0.0.1:8080 -data-dir D:\ota-data
```

修改 Vue 前端后，重启服务之前必须重新执行 `vp build` 和 `go build`：Go 可执行文件会在构建时将 `frontend/dist` 内嵌进去。

请使用 Caddy 或 nginx 为服务配置 HTTPS 反向代理。设备端有意只接受 `https://` 的 OTA 地址。内网私有部署时 `OTA_DEVICE_TOKEN` 可选；面向互联网部署时强烈建议设置。

网站是菜单式单页应用（SPA），提供以下页面：

- **首页**：板卡介绍。
- **固件**：按板卡展示版本历史和更新日志。
- **USB 烧录**：浏览器内 USB 全量烧录。
- **设备**：管理员登录后可见的设备管理面板，显示每台设备的在线状态、板卡型号、固件版本（含可升级标记）、NRL 呼号、SSID、IP 地址和最后在线时间。

管理员通过页面右上角使用 `OTA_ADMIN_USER` / `OTA_ADMIN_PASSWORD` 登录。登录成功后会返回一个有效期为 12 小时、用于授权管理 API 的签名会话令牌。长期有效的 `OTA_ADMIN_TOKEN` 仍可直接用于 API，自动发布脚本正是使用该令牌。未设置 `OTA_ADMIN_PASSWORD` 时，密码登录功能会被禁用并返回 HTTP 503。会话令牌使用每次进程启动时生成的密钥签名，因此重启服务器会使所有管理员退出登录。

要在原生固件构建成功后自动发布，请在构建环境中设置以下变量（只有同时设置前两个变量才会执行上传）：

```powershell
$env:OTA_SERVER_URL = 'https://ota.example.com'
$env:OTA_UPLOAD_TOKEN = $env:OTA_ADMIN_TOKEN
$env:OTA_VERSION = '0.5.2'       # 可选；默认从 nrl_version.h 读取
$env:OTA_RELEASE_NOTES = '修复说明'
python scripts/build.py s31_korvo build
```

要用一条命令发布四种板卡的完整刷机包及 OTA 版本，请运行 `python scripts/publish_ota.py`。如只需发布部分板卡，可在命令后指定一个或多个板卡标识。脚本会从各构建目录的 `flasher_args.json` 读取全部镜像及其烧录地址，不再需要单独上传应用固件。

容器化部署时，请在 `ota-server/` 目录中构建镜像，持久化挂载 `/data`，并设置两个令牌环境变量。项目提供的 `Caddyfile.example` 会终止 TLS 并将流量反向代理到容器本地端口。

## USB 网页烧录器

网站内置浏览器 USB 烧录器（esp-web-tools / Web Serial），可用于首次进行全量烧录。它只支持通过 HTTPS（或 localhost）访问的 Chrome/Edge，并且 **仅支持两种 ESP32-S3 板卡**：`gezipai` 与 `bh4tdv`。由于 esptool-js 不支持 ESP32-S31，`s31_korvo` 和 `s31_function_coreboard` 只能使用串口烧录，页面会给出相应提示。

esp-web-tools 资源会内嵌进二进制文件。完整刷机包按版本存储在 `<data-dir>/packages/<board>/<version>/`，服务器会动态生成各板卡的 `manifest-<board>.json`，因此发布新的可烧录固件时无需重新构建 Go 二进制文件。

先构建所有板卡，再用一条命令发布全部完整包及其中的 OTA 应用固件：

```powershell
python scripts/build.py gezipai build
python scripts/build.py bh4tdv build
python scripts/build.py s31_korvo build
python scripts/build.py s31_function_coreboard build
python scripts/publish_ota.py
```

如果原生构建时已经设置 `OTA_SERVER_URL` 和 `OTA_UPLOAD_TOKEN`，`scripts/build.py` 会在构建成功后自动发布该板卡的完整包。

## 设备端升级控制

所有板卡（包括没有屏幕的板卡）都在各自的配置服务器中提供 `/update` 页面。完成 HTTPS OTA 服务器配置后，串口 AT 控制台支持以下命令：

```text
AT+OTAURL=https://ota.example.com,device-token  # 配置服务器地址和令牌
AT+OTAURL=?                                     # 显示地址和最新版本
AT+OTACHECK=1                                   # 获取当前固件列表
AT+OTALIST=?                                    # 列出兼容版本
AT+OTA=LATEST                                   # 检查并安装最新版本
AT+OTA=0.5.1                                    # 安装列表中的历史版本
AT+OTA=?                                        # 显示 OTA 状态
```

OTA 配置与安装命令只接受本地串口 AT 控制台请求。无触摸屏的 Gezipai 会在 LCD 上提示有新固件；同时长按 `VOL+` 和 `VOL-` 会检查并安装最新的兼容版本。
