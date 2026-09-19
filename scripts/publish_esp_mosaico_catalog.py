#!/usr/bin/env python3
"""Create and publish the ESP-Mosaico entry in the NRL OTA catalog."""

from __future__ import annotations

import argparse
import base64
import os
from pathlib import Path

from publish_ota_mcp import MCPClient, MCPError


BOARD_ID = "esp_mosaico"

FEATURES = [
    {
        "key": "imu_motion",
        "label_zh": "六轴 IMU 与自动旋转",
        "label_en": "6-axis IMU with auto-rotate",
        "description_zh": "BMI270 加速度计/陀螺仪，屏幕方向随设备自动旋转（可开关）。",
        "description_en": "BMI270 accelerometer/gyroscope; the UI auto-rotates with the device (toggleable).",
        "group": "hardware",
        "display_order": 46,
        "active": True,
    },
    {
        "key": "dual_magnetometer",
        "label_zh": "双磁力计电子罗盘",
        "label_en": "Dual-magnetometer compass",
        "description_zh": "双 BMM150 磁场传感器提供航向角，并可通过差值检测局部磁干扰。",
        "description_en": "Dual BMM150 magnetometers provide heading and local magnetic-interference detection.",
        "group": "hardware",
        "display_order": 47,
        "active": True,
    },
    {
        "key": "fuel_gauge",
        "label_zh": "BQ27220 电量计",
        "label_en": "BQ27220 fuel gauge",
        "description_zh": "电池电压、电流、SOC 百分比与充电状态实时显示。",
        "description_en": "Real-time battery voltage, current, SOC percentage, and charge status.",
        "group": "hardware",
        "display_order": 48,
        "active": True,
    },
    {
        "key": "haptic_motor",
        "label_zh": "振动马达触觉反馈",
        "label_en": "Haptic vibration motor",
        "description_zh": "PTT/操作触觉反馈与 FMO 来电振动提醒。",
        "description_en": "PTT/UI haptic feedback and FMO incoming-call vibration alerts.",
        "group": "hardware",
        "display_order": 49,
        "active": True,
    },
]

BOARD = {
    "id": BOARD_ID,
    "name_zh": "ESP-Mosaico 智能交互终端",
    "name_en": "ESP-Mosaico Smart Terminal",
    "tagline_zh": "480×480 AMOLED 方屏、传感器丰富的 ESP32-S31 触控终端",
    "tagline_en": "ESP32-S31 touch terminal with a 480x480 square AMOLED and rich sensors",
    "description_zh": (
        "乐鑫 ESP-Mosaico 开发套件（ESP32-S31），配备 2.16 英寸 480×480 QSPI AMOLED "
        "触摸屏、ES8311 音频与 NS4150B 功放、BMI270 六轴 IMU、双 BMM150 磁力计、"
        "BQ27220 电量计和振动马达；全新 AMOLED Dark 主题方屏 UI，支持屏幕自动旋转。"
    ),
    "description_en": (
        "Espressif ESP-Mosaico kit (ESP32-S31) with a 2.16-inch 480x480 QSPI AMOLED "
        "touch screen, ES8311 audio with NS4150B amp, BMI270 6-axis IMU, dual BMM150 "
        "magnetometers, BQ27220 fuel gauge, and a vibration motor; new AMOLED Dark "
        "square-screen UI with auto-rotation."
    ),
    "chip_label": "ESP32-S31",
    "web_flash_chip_family": "ESP32-S31",
    "image_url": "",
    "display_order": 30,
    "status": "draft",
    "highlights_zh": [
        "480×480 QSPI AMOLED 方屏，AMOLED Dark 主题五页触控 UI",
        "BMI270 IMU 屏幕自动旋转（可开关）",
        "双 BMM150 磁力计航向与磁干扰检测",
        "BQ27220 电量计、振动马达触觉与来电振动",
    ],
    "highlights_en": [
        "480x480 QSPI AMOLED square screen, AMOLED Dark five-page touch UI",
        "BMI270 IMU auto screen rotation (toggleable)",
        "Dual BMM150 compass heading and magnetic-interference detection",
        "BQ27220 fuel gauge, haptic motor and incoming-call vibration",
    ],
    "features": {},
    "feature_notes": {},
    "created_at": 0,
    "updated_at": 0,
}

ASSIGNMENTS = {
    "nrl_voice": "yes",
    "wifi_portal": "yes",
    "remote_at_ota": "yes",
    "aprs": "yes",
    # Map works online; tile cache needs the SPI NAND (not enabled yet).
    "aprs_map": "partial",
    "sstv": "yes",
    "mdc1200": "yes",
    "dtmf": "yes",
    "ctcss": "yes",
    "cwdecode": "yes",
    "screen_signaling": "yes",
    # S31 boards flash over serial/USB first; web flash stays off.
    "web_flash": "no",
    "ble": "no",
    "es8311": "yes",
    "audio_processing": "yes",
    "radio_ptt_sql": "no",
    "sci": "no",
    "status_indicator": "yes",
    "color_display": "yes",
    "touch": "yes",
    "buttons": "yes",
    "battery": "yes",
    "tf_media": "no",
    "usb_host": "no",
    "smb_media": "yes",
    "espnow": "yes",
    "music_radio": "yes",
    "bluetooth_audio": "yes",
    "bluetooth_call": "yes",
    "ai_voice": "partial",
    "video_call": "no",
    "imu_motion": "yes",
    "dual_magnetometer": "yes",
    "fuel_gauge": "yes",
    "haptic_motor": "yes",
    # BMM150 heading works; installation orientation and calibration remain.
    "electronic_compass": "partial",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--image",
        type=Path,
        default=Path(__file__).resolve().parent.parent
        / "docs"
        / "ESP-Mosaico"
        / "esp-mosaico-front.png",
    )
    args = parser.parse_args()
    server = os.environ.get("OTA_SERVER_URL", "").strip()
    token = os.environ.get("OTA_ADMIN_TOKEN", "").strip()
    if not server or not token:
        raise SystemExit("OTA_SERVER_URL and OTA_ADMIN_TOKEN must be set")
    if not args.image.is_file() or args.image.stat().st_size > 5 * 1024 * 1024:
        raise SystemExit("board image must exist and be no larger than 5 MB")

    client = MCPClient(server, token)
    catalog = client.call_tool("catalog.list", {"include_drafts": True})
    boards = {item["id"]: item for item in catalog.get("boards", [])}
    existing_features = {item["key"] for item in catalog.get("features", [])}
    existing_board = boards.get(BOARD_ID)
    if existing_board and existing_board.get("status") == "published":
        print(f"{BOARD_ID}: already published; catalog mutation skipped")
        return 0

    for feature in FEATURES:
        result = client.call_tool(
            "feature.save",
            {"feature": feature, "confirm_update": feature["key"] in existing_features},
        )
        print(f"{feature['key']}: {result.get('status')}")

    result = client.call_tool("board.save_draft", {"board": BOARD})
    print(f"{BOARD_ID}: {result.get('status')}")
    result = client.call_tool(
        "board.set_features",
        {"board_id": BOARD_ID, "assignments": ASSIGNMENTS},
    )
    print(f"{BOARD_ID} features: {result.get('status')}")
    image = base64.b64encode(args.image.read_bytes()).decode("ascii")
    result = client.call_tool(
        "board.upload_image", {"board_id": BOARD_ID, "image_base64": image}
    )
    print(f"{BOARD_ID} image: {result.get('message')}")
    result = client.call_tool(
        "board.publish", {"board_id": BOARD_ID, "confirm": True}
    )
    print(f"{BOARD_ID}: {result.get('status')}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except MCPError as exc:
        raise SystemExit(str(exc)) from exc
