from pathlib import Path
import shutil
import sys


PROJECT_DIR = Path(__file__).resolve().parents[1]
BUILD_DIR = PROJECT_DIR / ".pio" / "build" / "app0_main"
OUT_DIR = PROJECT_DIR / "web-flasher" / "firmware"
BOOT_APP0_CANDIDATES = [
    PROJECT_DIR / ".platformio" / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin",
    Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin",
]


def find_boot_app0():
    for path in BOOT_APP0_CANDIDATES:
        if path.exists():
            return path
    return None


def copy_file(src, dst_name):
    if not src.exists():
        print(f"[web-flasher] missing: {src}")
        return False
    dst = OUT_DIR / dst_name
    shutil.copy2(src, dst)
    print(f"[web-flasher] staged {dst_name} ({dst.stat().st_size} bytes)")
    return True


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    boot_app0 = find_boot_app0()
    if boot_app0 is None:
        print("[web-flasher] missing boot_app0.bin from Arduino ESP32 package")
        return 1

    ok = True
    ok = copy_file(BUILD_DIR / "bootloader.bin", "bootloader.bin") and ok
    ok = copy_file(BUILD_DIR / "partitions.bin", "partitions.bin") and ok
    ok = copy_file(boot_app0, "boot_app0.bin") and ok
    ok = copy_file(BUILD_DIR / "firmware.bin", "firmware.bin") and ok

    if not ok:
        print("[web-flasher] run: platformio run -e app0_main")
        return 1

    print("[web-flasher] ready: python -m http.server 8000 -d web-flasher")
    return 0


if __name__ == "__main__":
    sys.exit(main())
