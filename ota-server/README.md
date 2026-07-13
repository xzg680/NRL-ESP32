# NRL OTA Server (Go + SQLite + embedded Vue)

The executable embeds `frontend/dist`, so deployment is one Go binary plus its
data directory. The API is shared by all four board identifiers:
`gezipai`, `bh4tdv`, `s31_korvo`, and `s31_function_coreboard`.

Build the embedded Vue admin site once, then build Go:

Vite+ (`vp`) runs under whatever `node` is on `PATH`; it needs **Node 24+**
(see `frontend/.node-version`). An older Node fails with
`ERR_UNKNOWN_FILE_EXTENSION` on the `vp` launcher. Select Node 24 first (e.g.
`nvm use 24`).

```powershell
cd ota-server/frontend
vp install --frozen-lockfile
vp check
vp build
cd ..
go build -o nrl-ota.exe .
$env:OTA_ADMIN_TOKEN = 'long-random-admin-secret'      # machine token (publish pipeline)
$env:OTA_ADMIN_USER = 'admin'                          # web login username (default: admin)
$env:OTA_ADMIN_PASSWORD = 'long-random-admin-password' # web login password (required for UI login)
$env:OTA_DEVICE_TOKEN = 'optional-device-access-token'
.\nrl-ota.exe -listen 127.0.0.1:8080 -data-dir D:\ota-data
```

After changing the Vue frontend, always rerun `vp build` and `go build` before
restarting the server: the Go executable embeds `frontend/dist` at build time.

Devices accept both `http://` and `https://` OTA URLs. Plain HTTP is intended
only for testing or a trusted private LAN because firmware and device reports
are not encrypted or server-authenticated. Put Caddy/nginx in front of the
process and use HTTPS for any other deployment. `OTA_DEVICE_TOKEN` is optional
for a private LAN but strongly recommended for an Internet-facing instance.

The site is a menu-based SPA: **Home** (board introductions), **Firmware**
(per-board version history and changelogs), **USB Flash**, and — after admin
login — a **Devices** management dashboard showing each device's online status,
board, firmware version (with an update-available badge), NRL callsign, SSID,
IP, and last-seen time. Admins sign in from the top-right with
`OTA_ADMIN_USER` / `OTA_ADMIN_PASSWORD`; login returns a signed session token
(12 h) that authorizes the admin API. The long-lived `OTA_ADMIN_TOKEN` is still
accepted directly and is what the machine publish pipeline uses. Password login
is disabled (HTTP 503) until `OTA_ADMIN_PASSWORD` is set. Session tokens are
signed with a per-process secret, so restarting the server logs admins out.

To publish automatically after a successful native firmware build, set these in
the build environment (the upload is deliberately disabled unless both values
exist):

```powershell
$env:OTA_SERVER_URL = 'https://ota.example.com'
$env:OTA_UPLOAD_TOKEN = $env:OTA_ADMIN_TOKEN
$env:OTA_VERSION = '0.5.2'       # optional; defaults to nrl_version.h
$env:OTA_RELEASE_NOTES = 'Fix …'
python scripts/build.py s31_korvo build
```

To publish the complete flash packages and OTA releases for all four boards in
one command, run `python scripts/publish_ota.py`. Pass one or more board
identifiers to publish only those boards. The script reads every image and its
flash offset from each build's `flasher_args.json`; a separate app-only upload
is not needed.

For a container deployment, build from `ota-server/`, mount `/data` persistently,
and set the two token environment variables. The included
`Caddyfile.example` terminates TLS and proxies to the local container port.

## USB web flasher

The site includes an in-browser USB flasher (esp-web-tools / Web Serial) for a
first-time full-flash install. It only works in Chrome/Edge over HTTPS (or
localhost), and **only for the two ESP32-S3 boards** (`gezipai`, `bh4tdv`) —
esptool-js has no ESP32-S31 support, so `s31_korvo` and `s31_function_coreboard`
are serial-only and the page says so.

The esp-web-tools assets ship embedded in the binary. Complete flash packages
are stored versioned under `<data-dir>/packages/<board>/<version>/`, and the
server generates each `manifest-<board>.json` dynamically. Publishing new
flashable firmware therefore does not require rebuilding the Go binary.

Build all boards, then publish every package and its OTA app image in one run:

```powershell
python scripts/build.py gezipai build
python scripts/build.py bh4tdv build
python scripts/build.py s31_korvo build
python scripts/build.py s31_function_coreboard build
python scripts/publish_ota.py
```

When `OTA_SERVER_URL` and `OTA_UPLOAD_TOKEN` are already set during a native
build, `scripts/build.py` publishes that board's complete package automatically.

## Device-side update controls

Every board exposes its own configuration server's `/update` page, including
boards without a display. After configuring its HTTP or HTTPS OTA server, the
serial AT console supports:

```text
AT+OTAURL=https://ota.example.com,device-token  # configure server/token
AT+OTAURL=?                                     # show URL and latest version
AT+OTACHECK=1                                   # fetch current release list
AT+OTALIST=?                                    # list compatible versions
AT+OTA=LATEST                                   # check then install newest
AT+OTA=0.5.1                                    # install a listed historical version
AT+OTA=?                                        # show OTA state
```

OTA configuration and installation commands are accepted only over the local
serial AT console. On the non-touch Gezipai, a new firmware notice is shown on
the LCD; holding `VOL+` and `VOL-` together checks and installs the latest
compatible release.
