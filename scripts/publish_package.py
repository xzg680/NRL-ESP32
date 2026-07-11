#!/usr/bin/env python3
"""Publish one build's complete flash package to an NRL OTA server.

Uploads the full flash image set (bootloader, partition table, ota_data, app,
srmodels, …) as read from build/<board>/flasher_args.json to the server's
/api/v1/admin/flash-package endpoint. The server stores it versioned, serves the
USB web-flasher manifest from it, and registers the app slice (the image at the
app offset) as the board's OTA release -- so the OTA online-update firmware is
literally a file inside the USB flashing package. One upload = OTA + USB, same
source.

Required environment: OTA_SERVER_URL, OTA_UPLOAD_TOKEN.
Optional environment: OTA_VERSION, OTA_CHANNEL (stable/beta), OTA_RELEASE_NOTES.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
import uuid
from pathlib import Path

# Only the ESP32-S3 boards are USB web-flashable (esptool-js has no ESP32-S31
# support). A non-empty chip family tells the server to expose a web-flash
# manifest; the others still upload their package so OTA works, just without the
# in-browser flasher.
CHIP_FAMILY = {
    "gezipai": "ESP32-S3",
    "bh4tdv": "ESP32-S3",
    "s31_korvo": "",
    "s31_function_coreboard": "",
}


def firmware_version(repo: Path) -> str:
    value = os.environ.get("OTA_VERSION", "")
    if value:
        return value.removeprefix("v")
    text = (repo / "src" / "lib" / "nrl_version.h").read_text(encoding="utf-8")
    import re

    match = re.search(r'NRL_FIRMWARE_VERSION\s+"([^"]+)"', text)
    if not match:
        raise SystemExit("could not determine firmware version; set OTA_VERSION")
    return match.group(1)


def build_multipart(meta: dict, files: list[tuple[str, Path]]) -> tuple[bytes, str]:
    boundary = uuid.uuid4().hex
    sep = b"--" + boundary.encode()
    body = bytearray()
    body += sep + b"\r\n"
    body += b'Content-Disposition: form-data; name="meta"\r\n\r\n'
    body += json.dumps(meta).encode("utf-8") + b"\r\n"
    for name, path in files:
        body += sep + b"\r\n"
        body += (
            f'Content-Disposition: form-data; name="{name}"; filename="{name}"\r\n'
        ).encode("utf-8")
        body += b"Content-Type: application/octet-stream\r\n\r\n"
        body += path.read_bytes() + b"\r\n"
    body += sep + b"--\r\n"
    return bytes(body), boundary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("board")
    parser.add_argument("--version")
    parser.add_argument("--channel", default=os.environ.get("OTA_CHANNEL", "stable"))
    parser.add_argument("--notes", default=os.environ.get("OTA_RELEASE_NOTES", ""))
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    base = os.environ.get("OTA_SERVER_URL", "").rstrip("/")
    token = os.environ.get("OTA_UPLOAD_TOKEN", "")
    if not base or not token:
        raise SystemExit("OTA_SERVER_URL and OTA_UPLOAD_TOKEN must be set")

    build_dir = repo / "build" / args.board
    flasher_args_path = build_dir / "flasher_args.json"
    if not flasher_args_path.is_file():
        raise SystemExit(f"{flasher_args_path} not found -- build it first: "
                         f"python scripts/build.py {args.board} build")
    flasher_args = json.loads(flasher_args_path.read_text(encoding="utf-8"))

    app_offset = int(flasher_args["app"]["offset"], 16)
    parts, files = [], []
    seen = set()
    for offset_hex, rel in sorted(flasher_args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        name = Path(rel).name
        if name in seen:
            raise SystemExit(f"duplicate part filename in flasher_args: {name}")
        seen.add(name)
        src = build_dir / rel
        if not src.is_file():
            raise SystemExit(f"missing flash image: {src}")
        parts.append({"offset": int(offset_hex, 16), "name": name})
        files.append((name, src))

    meta = {
        "board": args.board,
        "version": args.version or firmware_version(repo),
        "channel": args.channel,
        "notes": args.notes,
        "chip_family": CHIP_FAMILY.get(args.board, ""),
        "app_offset": app_offset,
        "parts": parts,
    }
    body, boundary = build_multipart(meta, files)
    request = urllib.request.Request(
        base + "/api/v1/admin/flash-package",
        data=body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
            "X-OTA-Token": token,
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            print(response.read().decode("utf-8"))
            return 0
    except urllib.error.HTTPError as exc:
        print(exc.read().decode("utf-8", errors="replace"), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
