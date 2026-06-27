#!/usr/bin/env python3
"""One-shot setup for building the s31_korvo (ESP32-S31) environment.

PlatformIO espressif32 7.0.1 predates the ESP32-S31 / ESP-IDF 6.2, so a handful
of machine-local fixes are required that do NOT live in the git repo. This script
reapplies them so a fresh clone can build/flash s31_korvo. It is idempotent --
run it as many times as you like.

What it does:
  1. Writes a framework-espidf package.json into the local ESP-IDF checkout (the
     symlink:// target in platformio.ini) so PlatformIO accepts it as a package.
  2. Patches the installed PlatformIO espressif32 builder (main.py / espidf.py)
     to treat esp32s31 as RISC-V, generate the IDF-6.2 split bootloader linker
     scripts, feed the bootloader its own sdkconfig defaults, and run esptool via
     "-m esptool" (the bundled 4.x esptool doesn't know esp32s31).
  3. Installs esptool 5.3.0 into the PlatformIO penv (knows esp32s31).
  4. Installs the full ESP-IDF core Python requirements + idf-component-manager
     3.0.3 + esptool 5.3.0 into the IDF venv (PlatformIO's auto-populate misses
     rich_click / esp_pylib and ships too old a component manager).

Prerequisites:
  * ESP-IDF 6.2 checked out at the symlink path in platformio.ini (default
     C:/esp/esp-idf). Override with --idf-path or the IDF_PATH env var.
  * Run `pio run -e s31_korvo` once first if the IDF venv does not exist yet;
     this script will tell you if step 4 is skipped for that reason.

Usage:  python scripts/setup_s31_env.py [--idf-path PATH] [--pio-core PATH]
"""

import argparse
import glob
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# --- framework-espidf manifest written into the local IDF checkout -----------
PACKAGE_JSON = """\
{
  "name": "framework-espidf",
  "version": "3.60200",
  "description": "Espressif IoT Development Framework (local checkout, IDF 6.2.0)",
  "keywords": ["framework", "esp32", "esp32s3", "esp32s31", "espressif"],
  "homepage": "https://docs.espressif.com/projects/esp-idf/en/latest/esp32/",
  "license": "Apache-2.0",
  "repository": {"type": "git", "url": "https://github.com/espressif/esp-idf"}
}
"""

# --- PlatformIO espressif32 7.0.1 builder patches ----------------------------
# Each entry: (original snippet, patched snippet). Applied once, idempotently.
MAIN_PY_PATCHES = [
    # esp32s31 uses the riscv32-esp toolchain like c3/c6
    (
        'if mcu in ("esp32c3", "esp32c6"):\n    toolchain_arch = "riscv32-esp"',
        'if mcu in ("esp32c3", "esp32c6", "esp32s31"):\n    toolchain_arch = "riscv32-esp"',
    ),
    # ...and the riscv gdb package
    (
        '            "tool-riscv32-esp-elf-gdb"\n'
        '            if mcu in ("esp32c3", "esp32c6")\n'
        '            else "tool-xtensa-esp-elf-gdb"',
        '            "tool-riscv32-esp-elf-gdb"\n'
        '            if mcu in ("esp32c3", "esp32c6", "esp32s31")\n'
        '            else "tool-xtensa-esp-elf-gdb"',
    ),
    # elf2image via the penv esptool 5.x (bundled esptool 4.x lacks esp32s31)
    (
        "                        '\"$PYTHONEXE\" \"$OBJCOPY\"',\n"
        '                "--chip", mcu, "elf2image",',
        "                        '\"$PYTHONEXE\" -m esptool',\n"
        '                "--chip", mcu, "elf2image",',
    ),
    (
        "    ERASECMD='\"$PYTHONEXE\" \"$OBJCOPY\" $ERASEFLAGS erase_flash',",
        "    ERASECMD='\"$PYTHONEXE\" -m esptool $ERASEFLAGS erase_flash',",
    ),
    (
        "        UPLOADCMD='\"$PYTHONEXE\" \"$UPLOADER\" $UPLOADERFLAGS $ESP32_APP_OFFSET $SOURCE'",
        "        UPLOADCMD='\"$PYTHONEXE\" -m esptool $UPLOADERFLAGS $ESP32_APP_OFFSET $SOURCE'",
    ),
    (
        "            UPLOADCMD='\"$PYTHONEXE\" \"$UPLOADER\" $UPLOADERFLAGS $SOURCE',",
        "            UPLOADCMD='\"$PYTHONEXE\" -m esptool $UPLOADERFLAGS $SOURCE',",
    ),
]

ESPIDF_PY_PATCHES = [
    # esp32s31 -> toolchain-riscv32-esp
    (
        'TOOLCHAIN_DIR = platform.get_package_dir(\n'
        '    "toolchain-riscv32-esp"\n'
        '    if mcu in ("esp32c3", "esp32c6")',
        'TOOLCHAIN_DIR = platform.get_package_dir(\n'
        '    "toolchain-riscv32-esp"\n'
        '    if mcu in ("esp32c3", "esp32c6", "esp32s31")',
    ),
    # esp32s31 needs the RISC-V linker.lf fragment
    (
        '    if mcu in ("esp32c3", "esp32c6"):\n'
        '        result.append(\n'
        '            os.path.join(framework_components_dir, "riscv", "linker.lf")\n'
        '        )',
        '    if mcu in ("esp32c3", "esp32c6", "esp32s31"):\n'
        '        result.append(\n'
        '            os.path.join(framework_components_dir, "riscv", "linker.lf")\n'
        '        )',
    ),
    # give the bootloader sub-project its own defaults file when present
    (
        '        "-DSDKCONFIG=" + SDKCONFIG_PATH,\n'
        '        "-DPROJECT_SOURCE_DIR=" + PROJECT_DIR,',
        '        "-DSDKCONFIG=" + SDKCONFIG_PATH,\n'
        '        "-DSDKCONFIG_DEFAULTS=" + os.path.join(\n'
        '            PROJECT_DIR,\n'
        '            "sdkconfig.bootloader.defaults"\n'
        '            if os.path.isfile(\n'
        '                os.path.join(PROJECT_DIR, "sdkconfig.bootloader.defaults")\n'
        '            )\n'
        '            else "sdkconfig.defaults",\n'
        '        ),\n'
        '        "-DPROJECT_SOURCE_DIR=" + PROJECT_DIR,',
    ),
    # IDF 6.2 split the bootloader linker script into memory + sections .ld.in
    (
        '    framework_version = [int(v) for v in get_framework_version().split(".")]\n'
        '    if framework_version[:2] >= [6, 0]:\n'
        '        bootloader_linker_script = preprocess_linker_script(\n'
        '            os.path.join(\n'
        '                bootloader_src_dir,\n'
        '                "main",\n'
        '                "ld",\n'
        '                idf_variant,\n'
        '                "bootloader.ld.in",\n'
        '            ),\n'
        '            os.path.join(BUILD_DIR, "bootloader", "ld", "bootloader.ld"),\n'
        '            [\n'
        '                os.path.join(BUILD_DIR, "bootloader", "config"),\n'
        '                os.path.join(FRAMEWORK_DIR, "components", "esp_system", "ld"),\n'
        '            ],\n'
        '        )\n'
        '\n'
        '        # The linker script is generated before the final bootloader ELF\n'
        '        env.Depends(\n'
        '            os.path.join("$BUILD_DIR", "bootloader.elf"),\n'
        '            bootloader_linker_script,\n'
        '        )',
        '    framework_version = [int(v) for v in get_framework_version().split(".")]\n'
        '    if framework_version[:2] >= [6, 0]:\n'
        '        bootloader_ld_dir = os.path.join(\n'
        '            bootloader_src_dir, "main", "ld", idf_variant\n'
        '        )\n'
        '        ld_include_dirs = [\n'
        '            os.path.join(BUILD_DIR, "bootloader", "config"),\n'
        '            os.path.join(bootloader_src_dir, "main", "ld"),\n'
        '            os.path.join(FRAMEWORK_DIR, "components", "esp_system", "ld"),\n'
        '        ]\n'
        '        # ESP-IDF 6.2 split the single bootloader.ld.in into separate\n'
        '        # bootloader.memory.ld.in + bootloader.sections.ld.in templates\n'
        '        # (BOOTLOADER_LINKER_SCRIPT in bootloader/subproject/CMakeLists_v2.txt).\n'
        '        # The linker resolves them by basename from -L<build>/bootloader/ld, so\n'
        '        # preprocess each into that directory. Fall back to the legacy\n'
        '        # single-script layout for earlier 6.x.\n'
        '        split_scripts = ["bootloader.memory.ld.in", "bootloader.sections.ld.in"]\n'
        '        if all(\n'
        '            os.path.isfile(os.path.join(bootloader_ld_dir, s))\n'
        '            for s in split_scripts\n'
        '        ):\n'
        '            ld_in_files = split_scripts\n'
        '        else:\n'
        '            ld_in_files = ["bootloader.ld.in"]\n'
        '\n'
        '        for ld_in in ld_in_files:\n'
        '            bootloader_linker_script = preprocess_linker_script(\n'
        '                os.path.join(bootloader_ld_dir, ld_in),\n'
        '                # strip the trailing ".in" for the generated output name\n'
        '                os.path.join(BUILD_DIR, "bootloader", "ld", ld_in[:-3]),\n'
        '                ld_include_dirs,\n'
        '            )\n'
        '\n'
        '            # The linker script is generated before the final bootloader ELF\n'
        '            env.Depends(\n'
        '                os.path.join("$BUILD_DIR", "bootloader.elf"),\n'
        '                bootloader_linker_script,\n'
        '            )',
    ),
]


def log(msg):
    print(msg, flush=True)


def patch_file(path: Path, patches) -> bool:
    if not path.is_file():
        log(f"  [!] not found: {path} (PlatformIO espressif32 not installed?)")
        return False
    text = path.read_text(encoding="utf-8")
    changed = False
    ok = True
    for i, (old, new) in enumerate(patches, 1):
        if new in text:
            log(f"  [=] patch {i}/{len(patches)} already applied")
            continue
        if old in text:
            text = text.replace(old, new, 1)
            changed = True
            log(f"  [+] patch {i}/{len(patches)} applied")
        else:
            ok = False
            log(f"  [!] patch {i}/{len(patches)} NOT found "
                f"(espressif32 version != 7.0.1?) -- review {path.name} manually")
    if changed:
        path.write_text(text, encoding="utf-8")
    return ok


def find_idf_path(arg_path):
    if arg_path:
        return Path(arg_path)
    env = os.environ.get("IDF_PATH")
    if env:
        return Path(env)
    # Parse the symlink:// target from platformio.ini
    ini = (REPO_ROOT / "platformio.ini").read_text(encoding="utf-8")
    m = re.search(r"symlink://([^\s]+)", ini)
    if m:
        return Path(m.group(1))
    return Path("C:/esp/esp-idf")


def pio_core_dir(arg_path):
    if arg_path:
        return Path(arg_path)
    env = os.environ.get("PLATFORMIO_CORE_DIR")
    if env:
        return Path(env)
    return Path.home() / ".platformio"


def venv_python(venv_dir: Path) -> Path:
    win = venv_dir / "Scripts" / "python.exe"
    return win if win.exists() else venv_dir / "bin" / "python"


def pip_install(python: Path, args, label):
    if not python.exists():
        log(f"  [!] python not found: {python} -- skipping {label}")
        return False
    cmd = [str(python), "-m", "pip", "install", *args]
    log(f"  [>] {label}: pip install {' '.join(args)}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        log(f"  [!] pip failed:\n{res.stdout[-800:]}\n{res.stderr[-800:]}")
        return False
    log(f"  [+] {label} done")
    return True


def main():
    ap = argparse.ArgumentParser(description="Set up the s31_korvo build environment.")
    ap.add_argument("--idf-path", help="Path to the local ESP-IDF 6.2 checkout")
    ap.add_argument("--pio-core", help="Path to the PlatformIO core dir (~/.platformio)")
    args = ap.parse_args()

    idf = find_idf_path(args.idf_path)
    core = pio_core_dir(args.pio_core)
    builder = core / "platforms" / "espressif32" / "builder"

    log("== ESP32-S31 environment setup ==")
    log(f"  IDF path        : {idf}")
    log(f"  PlatformIO core : {core}")

    # 1. framework-espidf manifest
    log("\n[1/4] framework-espidf package.json")
    if not idf.is_dir():
        log(f"  [!] IDF not found at {idf} -- clone ESP-IDF 6.2 there "
            f"(or pass --idf-path) and re-run.")
    else:
        pkg = idf / "package.json"
        if pkg.is_file():
            log(f"  [=] already present: {pkg}")
        else:
            pkg.write_text(PACKAGE_JSON, encoding="utf-8")
            log(f"  [+] wrote {pkg}")

    # 2. platform builder patches
    log("\n[2/4] PlatformIO espressif32 builder patches")
    log(" main.py:")
    patch_file(builder / "main.py", MAIN_PY_PATCHES)
    log(" frameworks/espidf.py:")
    patch_file(builder / "frameworks" / "espidf.py", ESPIDF_PY_PATCHES)

    # 3. penv esptool 5.3.0
    log("\n[3/4] PlatformIO penv: esptool 5.3.0")
    pip_install(venv_python(core / "penv"), ["esptool==5.3.0"], "penv esptool")

    # 4. IDF venv python deps
    log("\n[4/4] IDF venv Python dependencies")
    venvs = sorted(glob.glob(str(core / "penv" / ".espidf-6.*")))
    if not venvs:
        log("  [!] no .espidf-6.* venv yet. Run `pio run -e s31_korvo` once to let")
        log("      PlatformIO create it, then re-run this script.")
    else:
        py = venv_python(Path(venvs[-1]))
        log(f"  venv: {venvs[-1]}")
        req = idf / "tools" / "requirements" / "requirements.core.txt"
        if req.is_file():
            pip_install(py, ["-r", str(req)], "IDF core requirements")
        pip_install(py, ["idf-component-manager==3.0.3"], "idf-component-manager")
        pip_install(py, ["esptool==5.3.0"], "IDF venv esptool")

    log("\n== Done. Now build with: "
        "pio run -e s31_korvo  (then --target upload to flash) ==")


if __name__ == "__main__":
    main()
