"""Stage the USB web-flasher (esp-web-tools) from native ESP-IDF build output.

Only the ESP32-S3 boards (gezipai, bh4tdv) are web-flashable: esp-web-tools /
esptool-js has no support for the ESP32-S31, so s31_korvo is serial-only
(`python scripts/build.py s31_korvo flash`).

For each board it reads build/<board>/flasher_args.json -- the authoritative
offset->file map ESP-IDF emits -- copies every flash image into
web-flasher/firmware/<board>/ and writes web-flasher/manifest-<board>.json with
matching offsets. Build the boards first:

    python scripts/build.py gezipai build
    python scripts/build.py bh4tdv build
    python scripts/stage_web_flasher.py [--version X.Y.Z] [gezipai bh4tdv]
    python -m http.server 8000 -d web-flasher
"""
from pathlib import Path
import json
import re
import shutil
import sys


PROJECT_DIR = Path(__file__).resolve().parents[1]
WEB_DIR = PROJECT_DIR / "web-flasher"
VERSION_HEADER = PROJECT_DIR / "src" / "lib" / "nrl_version.h"

# board -> display name shown in the esp-web-tools manifest. All are ESP32-S3.
BOARDS = {
    "gezipai": "NRL ESP32 - Gezipai",
    "bh4tdv": "NRL ESP32 - BH4TDV 3188",
}
CHIP_FAMILY = "ESP32-S3"


def copy_file(src, dst):
    if not src.exists():
        print(f"[web-flasher] missing: {src}")
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    print(f"[web-flasher] staged {dst.relative_to(WEB_DIR)} ({dst.stat().st_size} bytes)")
    return True


def stage_board(board, display_name, version):
    build_dir = PROJECT_DIR / "build" / board
    flasher_args = build_dir / "flasher_args.json"
    if not flasher_args.exists():
        print(f"[web-flasher] {flasher_args} not found -- build it first: "
              f"python scripts/build.py {board} build")
        return False

    flash_files = json.loads(flasher_args.read_text(encoding="utf-8"))["flash_files"]
    out_dir = WEB_DIR / "firmware" / board

    parts = []
    ok = True
    # Sort by offset so the manifest is stable/readable.
    for offset_hex, rel in sorted(flash_files.items(), key=lambda kv: int(kv[0], 16)):
        name = Path(rel).name
        ok = copy_file(build_dir / rel, out_dir / name) and ok
        parts.append({"path": f"firmware/{board}/{name}", "offset": int(offset_hex, 16)})
    if not ok:
        return False

    manifest = {
        "name": display_name,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": CHIP_FAMILY, "parts": parts}],
    }
    manifest_path = WEB_DIR / f"manifest-{board}.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"[web-flasher] wrote {manifest_path.name} (version {version})")
    return True


def read_firmware_version():
    if not VERSION_HEADER.exists():
        return None
    text = VERSION_HEADER.read_text(encoding="utf-8", errors="ignore")
    match = re.search(r'^\s*#\s*define\s+NRL_FIRMWARE_VERSION\s+"([^"]+)"', text, re.MULTILINE)
    return match.group(1) if match else None


def parse_version(args):
    for arg in args:
        if arg.startswith("--version="):
            value = arg.split("=", 1)[1].strip()
            if value:
                return value
    if "--version" in args:
        i = args.index("--version")
        if i + 1 < len(args):
            return args[i + 1]
    return read_firmware_version() or "0.0.0"


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

    ok = True
    for board in boards:
        if board not in BOARDS:
            print(f"[web-flasher] unknown/unsupported board: {board} "
                  f"(web-flashable: {', '.join(BOARDS)})")
            ok = False
            continue
        ok = stage_board(board, BOARDS[board], version) and ok

    if not ok:
        return 1
    print("[web-flasher] ready: python -m http.server 8000 -d web-flasher")
    return 0


if __name__ == "__main__":
    sys.exit(main())
