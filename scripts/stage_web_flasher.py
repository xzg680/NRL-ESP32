from pathlib import Path
import json
import shutil
import sys


PROJECT_DIR = Path(__file__).resolve().parents[1]
WEB_DIR = PROJECT_DIR / "web-flasher"
BOOT_APP0_CANDIDATES = [
    PROJECT_DIR / ".platformio" / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin",
    Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin",
]

# PlatformIO env name -> display name shown in the web flasher manifest.
BOARDS = {
    "gezipai": "NRL ESP32 - 格子派",
    "bh4tdv": "NRL ESP32 - BH4TDV 3188",
}


def find_boot_app0():
    for path in BOOT_APP0_CANDIDATES:
        if path.exists():
            return path
    return None


def copy_file(src, dst):
    if not src.exists():
        print(f"[web-flasher] missing: {src}")
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    print(f"[web-flasher] staged {dst.relative_to(WEB_DIR)} ({dst.stat().st_size} bytes)")
    return True


def stage_board(board, display_name, version, boot_app0):
    build_dir = PROJECT_DIR / ".pio" / "build" / board
    out_dir = WEB_DIR / "firmware" / board

    ok = True
    ok = copy_file(build_dir / "bootloader.bin", out_dir / "bootloader.bin") and ok
    ok = copy_file(build_dir / "partitions.bin", out_dir / "partitions.bin") and ok
    ok = copy_file(boot_app0, out_dir / "boot_app0.bin") and ok
    ok = copy_file(build_dir / "firmware.bin", out_dir / "firmware.bin") and ok
    if not ok:
        print(f"[web-flasher] build it first: platformio run -e {board}")
        return False

    manifest = {
        "name": display_name,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [
                    {"path": f"firmware/{board}/bootloader.bin", "offset": 0},
                    {"path": f"firmware/{board}/partitions.bin", "offset": 32768},
                    {"path": f"firmware/{board}/boot_app0.bin", "offset": 57344},
                    {"path": f"firmware/{board}/firmware.bin", "offset": 65536},
                ],
            }
        ],
    }
    manifest_path = WEB_DIR / f"manifest-{board}.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"[web-flasher] wrote {manifest_path.name} (version {version})")
    return True


def parse_version(args):
    if "--version" in args:
        i = args.index("--version")
        if i + 1 < len(args):
            return args[i + 1]
    return "0.0.0"


def parse_boards(args):
    selected = []
    skip_next = False
    for arg in args:
        if skip_next:
            skip_next = False
            continue
        if arg == "--version":
            skip_next = True
            continue
        if arg.startswith("--"):
            continue
        selected.append(arg)
    return selected or list(BOARDS.keys())


def main():
    args = sys.argv[1:]
    version = parse_version(args)
    boards = parse_boards(args)

    boot_app0 = find_boot_app0()
    if boot_app0 is None:
        print("[web-flasher] missing boot_app0.bin from Arduino ESP32 package")
        return 1

    ok = True
    for board in boards:
        if board not in BOARDS:
            print(f"[web-flasher] unknown board: {board}")
            ok = False
            continue
        display_name = BOARDS[board]
        ok = stage_board(board, display_name, version, boot_app0) and ok

    if not ok:
        return 1
    print("[web-flasher] ready: python -m http.server 8000 -d web-flasher")
    return 0


if __name__ == "__main__":
    sys.exit(main())
