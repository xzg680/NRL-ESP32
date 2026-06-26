"""Restore src/idf_component.yml before every build.

PlatformIO may regenerate component-manager metadata during configuration.
We declare real IDF Component Manager dependencies there (esp-sr,
esp-nimble-cpp), so it must exist when CMake configures. This script writes
the canonical content before each build.
"""
import os
from pathlib import Path

Import("env")  # noqa: F821 - SCons-provided

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
TARGET = PROJECT_DIR / "src" / "idf_component.yml"
CONTENT = """dependencies:
  espressif/esp-sr:
    version: ">=2.0.0"
  # esp-nimble-cpp: pure-IDF C++ wrapper around the NimBLE host stack used by
  # src/lib/ble_config.cpp. Requires CONFIG_BT_NIMBLE_ENABLED=y (set in
  # sdkconfig.defaults); Bluedroid must be off (mutually exclusive stacks).
  h2zero/esp-nimble-cpp:
    version: ">=2.2.0"
  # LVGL (gezipai ST7789 LCD UI) is vendored at components/lvgl/ by
  # scripts/fetch_lvgl.py at build time. It is deliberately NOT listed here
  # because the IDF component manager would (a) overwrite it from the
  # registry and (b) prune the directory on reconfigure if its lock file
  # doesn't reference it.
  idf:
    version: ">=6.0.1"
"""

TARGET.parent.mkdir(parents=True, exist_ok=True)
if not TARGET.exists() or TARGET.read_text(encoding="utf-8") != CONTENT:
    TARGET.write_text(CONTENT, encoding="utf-8")
    print(f"[idf_component] (re)wrote {TARGET.relative_to(PROJECT_DIR)}")
