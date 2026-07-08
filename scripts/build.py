#!/usr/bin/env python3
"""Native ESP-IDF build helper for all NRL boards (replaces PlatformIO).

Run it from an ESP-IDF 6.2 environment (i.e. after sourcing export.ps1 /
export.sh so `idf.py` is on PATH):

    python scripts/build.py <board> [idf.py args...]

Examples:
    python scripts/build.py gezipai build
    python scripts/build.py s31_korvo flash monitor -p COM5
    python scripts/build.py bh4tdv fullclean
    python scripts/build.py gezipai menuconfig

Boards: gezipai, bh4tdv, s31_korvo. If you omit the idf.py args it defaults to
`build`.

Each board gets its own build directory (build/<board>) and sdkconfig
(build/<board>/sdkconfig), so you can switch boards without a reconfigure or
clobbering another board's config. This mirrors exactly what the GitHub CI does
(.github/workflows/firmware.yml) and what CMakeLists.txt's NRL_NATIVE_BUILD
block expects.
"""
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# board -> (IDF target, NRL_BOARD macro, SDKCONFIG_DEFAULTS list, needs LVGL,
#           extra -D args). Keep in sync with the CI matrix in
#           .github/workflows/firmware.yml.
BOARDS = {
    "gezipai": dict(
        target="esp32s3",
        macro="NRL_BOARD_GEZIPAI",
        sdkconfig=["sdkconfig.defaults"],
        lvgl=True,
        extra=[],
    ),
    "bh4tdv": dict(
        target="esp32s3",
        macro="NRL_BOARD_BH4TDV",
        sdkconfig=["sdkconfig.defaults", "sdkconfig.bh4tdv.defaults"],
        lvgl=False,
        extra=[],
    ),
    "s31_korvo": dict(
        target="esp32s31",
        macro="NRL_BOARD_S31_KORVO",
        sdkconfig=["sdkconfig.defaults", "sdkconfig.defaults.esp32s31"],
        lvgl=True,
        extra=["-DNRL_EXTRA_COMPONENT_DIRS=vendor/esp32_s31_korvo"],
    ),
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in BOARDS:
        print(__doc__)
        print(f"error: first arg must be one of: {', '.join(BOARDS)}")
        sys.exit(2)

    board = sys.argv[1]
    passthrough = sys.argv[2:] or ["build"]
    cfg = BOARDS[board]

    if cfg["lvgl"]:
        subprocess.run(
            [sys.executable, str(REPO / "scripts" / "fetch_lvgl.py")], check=True
        )

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        print("error: IDF_PATH is not set. Source the ESP-IDF 6.2 environment "
              "first (export.ps1 / export.sh), then re-run.")
        sys.exit(2)
    idf_py = Path(idf_path) / "tools" / "idf.py"

    build_dir = f"build/{board}"
    # Invoke idf.py through the current Python with shell=False: this resolves
    # idf.py cross-platform without relying on a PATH wrapper, and keeps the
    # semicolon-separated SDKCONFIG_DEFAULTS list intact (a Windows shell would
    # otherwise split it on ';').
    cmd = [
        sys.executable, str(idf_py),
        "-B", build_dir,
        "-DIDF_TARGET=" + cfg["target"],
        "-DNRL_NATIVE_BUILD=1",
        "-DNRL_BOARD_ID=" + cfg["macro"],
        "-DSDKCONFIG=" + build_dir + "/sdkconfig",
        "-DSDKCONFIG_DEFAULTS=" + ";".join(cfg["sdkconfig"]),
        *cfg["extra"],
        *passthrough,
    ]
    print("+ " + " ".join(cmd), flush=True)
    sys.exit(subprocess.run(cmd, cwd=REPO).returncode)


if __name__ == "__main__":
    main()
