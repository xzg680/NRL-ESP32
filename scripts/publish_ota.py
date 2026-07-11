#!/usr/bin/env python3
"""Publish an ESP-IDF app binary to an NRL OTA server.

Required environment: OTA_SERVER_URL, OTA_UPLOAD_TOKEN.
Optional environment: OTA_VERSION, OTA_CHANNEL (stable/beta), OTA_RELEASE_NOTES.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path


def firmware_version(repo: Path) -> str:
    value = os.environ.get("OTA_VERSION", "")
    if value:
        return value.removeprefix("v")
    text = (repo / "src" / "lib" / "nrl_version.h").read_text(encoding="utf-8")
    match = re.search(r'NRL_FIRMWARE_VERSION\s+"([^"]+)"', text)
    if not match:
        raise ValueError("could not determine firmware version; set OTA_VERSION")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("board")
    parser.add_argument("--file", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--channel", default=os.environ.get("OTA_CHANNEL", "stable"))
    parser.add_argument("--notes", default=os.environ.get("OTA_RELEASE_NOTES", ""))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    image = args.file or repo / "build" / args.board / "nrl-esp32.bin"
    if not image.is_file():
        raise SystemExit(f"firmware image not found: {image}")
    base = os.environ.get("OTA_SERVER_URL", "").rstrip("/")
    token = os.environ.get("OTA_UPLOAD_TOKEN", "")
    if not base or not token:
        raise SystemExit("OTA_SERVER_URL and OTA_UPLOAD_TOKEN must be set")
    version = args.version or firmware_version(repo)
    headers = {
        "Content-Type": "application/octet-stream", "Content-Length": str(image.stat().st_size),
        "X-OTA-Token": token, "X-Firmware-Board": args.board,
        "X-Firmware-Version": version, "X-Firmware-Channel": args.channel,
        "X-Firmware-Filename": image.name, "X-Release-Notes": urllib.parse.quote(args.notes),
    }
    request = urllib.request.Request(base + "/api/v1/admin/releases", data=image.read_bytes(), headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            print(response.read().decode("utf-8"))
            return 0
    except urllib.error.HTTPError as exc:
        print(exc.read().decode("utf-8", errors="replace"), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
