"""Stage the USB web-flasher (esp-web-tools) from native ESP-IDF build output.

Only the released ESP32-S3 boards (gezipai, bh4tdv) are web-flashable:
esp-web-tools / esptool-js has no support for the ESP32-S31, so both S31
boards are serial-only (for example, `python scripts/build.py
s31_function_coreboard flash`).

For each board it reads build/<board>/flasher_args.json -- the authoritative
offset->file map ESP-IDF emits -- copies every flash image into
<out>/firmware/<board>/ and writes <out>/manifest-<board>.json with matching
offsets. The manifest part paths are relative, so the same layout is served both
by the standalone web-flasher/ page and by the OTA server under /flasher/.

    python scripts/build.py gezipai build
    python scripts/build.py bh4tdv build
    # standalone page (default output = web-flasher/):
    python scripts/stage_web_flasher.py
    python -m http.server 8000 -d web-flasher
    # OR stage straight into the OTA server data-dir it serves at /flasher/:
    python scripts/stage_web_flasher.py --out D:\ota-data\flasher
"""
from pathlib import Path
import argparse
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


def copy_file(src, dst, out_dir):
    if not src.exists():
        print(f"[web-flasher] missing: {src}")
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    print(f"[web-flasher] staged {dst.relative_to(out_dir)} ({dst.stat().st_size} bytes)")
    return True


def stage_board(board, display_name, version, out_dir):
    build_dir = PROJECT_DIR / "build" / board
    flasher_args = build_dir / "flasher_args.json"
    if not flasher_args.exists():
        print(f"[web-flasher] {flasher_args} not found -- build it first: "
              f"python scripts/build.py {board} build")
        return False

    flash_files = json.loads(flasher_args.read_text(encoding="utf-8"))["flash_files"]
    board_dir = out_dir / "firmware" / board

    parts = []
    ok = True
    # Sort by offset so the manifest is stable/readable.
    for offset_hex, rel in sorted(flash_files.items(), key=lambda kv: int(kv[0], 16)):
        name = Path(rel).name
        ok = copy_file(build_dir / rel, board_dir / name, out_dir) and ok
        parts.append({"path": f"firmware/{board}/{name}", "offset": int(offset_hex, 16)})
    if not ok:
        return False

    manifest = {
        "name": display_name,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": CHIP_FAMILY, "parts": parts}],
    }
    manifest_path = out_dir / f"manifest-{board}.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
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


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("boards", nargs="*", help="boards to stage (default: all web-flashable)")
    parser.add_argument("--version", default=None, help="firmware version (default: from nrl_version.h)")
    parser.add_argument(
        "--out", type=Path, default=WEB_DIR,
        help="output directory (default: web-flasher/). Point at the OTA server "
             "data-dir's flasher/ subdirectory to serve it from the OTA server.",
    )
    args = parser.parse_args()

    version = args.version or read_firmware_version() or "0.0.0"
    boards = args.boards or list(BOARDS.keys())
    out_dir = args.out.resolve()

    ok = True
    for board in boards:
        if board not in BOARDS:
            print(f"[web-flasher] unknown/unsupported board: {board} "
                  f"(web-flashable: {', '.join(BOARDS)})")
            ok = False
            continue
        ok = stage_board(board, BOARDS[board], version, out_dir) and ok

    if not ok:
        return 1
    print(f"[web-flasher] ready in {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
