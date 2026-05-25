Import("env")

import csv
import atexit
import os
import re
from pathlib import Path

env.BoardConfig().update("upload.arduino.flash_extra_images", [])


def relax_arduino_esp_sr_constraint():
    framework_dir = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32") or "")
    manifest = framework_dir / "idf_component.yml"
    if not manifest.exists():
        return

    text = manifest.read_text(encoding="utf-8")
    pattern = re.compile(
        r"(?ms)^  espressif/esp-sr:\n"
        r"(?:(?!^  [A-Za-z0-9_/-]+:).)*?"
        r"^    version:\s*['\"]?\^1\.4\.2['\"]?\s*$"
    )
    match = pattern.search(text)
    if match is None:
        return
    old = match.group(0)
    new = re.sub(r"(?m)^    version:.*$", '    version: ">=2.0.0"', old)

    manifest.write_text(text[:match.start()] + new + text[match.end():], encoding="utf-8")
    print("[esp-sr] Relaxed Arduino framework esp-sr dependency to >=2.0.0 for this build")

    def restore():
        try:
            current = manifest.read_text(encoding="utf-8")
            if new in current:
                manifest.write_text(current.replace(new, old), encoding="utf-8")
                print('*** Original Arduino "idf_component.yml" restored ***')
        except OSError:
            pass

    atexit.register(restore)


def strip_tinyuf2_extra_image(source=None, target=None, env=env):
    images = []
    for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
        if os.path.basename(str(image)).lower() == "tinyuf2.bin":
            print(f"[upload] Skipping TinyUF2 image at {offset}: {image}")
            continue
        images.append((offset, image))
    env.Replace(FLASH_EXTRA_IMAGES=images)


def get_partition_size(partition_name, path):
    if not os.path.exists(path):
        print(f"[size] Partition file not found: {path}")
        return None

    with open(path, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#"):
                continue
            name, typ, subtype, offset, size = row[:5]
            if name.strip() == partition_name:
                return int(size, 0)
    return None


def fix_size(source, target, env):
    partfile = env.GetProjectOption("board_build.partitions")
    partname = env.GetProjectOption("board_build.app_partition")
    size = get_partition_size(partname, partfile)
    if size:
        print(f"[size] Using partition '{partname}' size {size} bytes")
        env["UPLOAD_MAXIMUM_SIZE"] = size
        env.BoardConfig().update("upload.maximum_size", str(size))
    else:
        print(f"[size] Could not find partition {partname} in {partfile}")


env.AddPreAction("checkprogsize", fix_size)
env.AddPreAction("upload", strip_tinyuf2_extra_image)
relax_arduino_esp_sr_constraint()
