"""Fails when a planned restart in src/ can skip FlashArea::sync().

A board whose partition table has no usable `spiffs` partition keeps the flash area as an image in PSRAM, saved to
/.crosspoint/flash-area.img by FlashArea::sync() (docs/design/flash-area.md). A restart loses PSRAM, so every
ESP.restart() / esp_restart() in src/ must be the one in restartKeepingFlashArea() (main.cpp), which syncs first,
or carry a `flash-area:` comment within the three lines above it saying why it needs no sync (before the area is
chosen at boot; after the card was a USB host's).

The host test suite runs it (test/flash_area_select/CMakeLists.txt). By hand:
    python scripts/check_restart_flash_sync.py <repo root>
"""

import re
import sys
from pathlib import Path

RESTART = re.compile(r"\b(?:ESP\.restart|esp_restart)\s*\(\s*\)\s*;")
HELPER = "void restartKeepingFlashArea()"


def code_part(line: str) -> str:
    """The line without a trailing // comment (good enough for these call sites)."""
    return line.split("//", 1)[0]


def check(root: Path) -> list:
    problems = []
    for path in sorted((root / "src").rglob("*.cpp")):
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        helper_open = False
        for i, line in enumerate(lines):
            if line.startswith(HELPER):
                helper_open = True
            elif helper_open and line.startswith("}"):
                helper_open = False
            if not RESTART.search(code_part(line)):
                continue
            if helper_open:
                continue
            context = lines[max(0, i - 3) : i + 1]
            if any("flash-area:" in c for c in context):
                continue
            problems.append(f"{path.relative_to(root)}:{i + 1}: {line.strip()}")
    return problems


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    problems = check(root)
    for p in problems:
        print(f"restart without FlashArea::sync(): {p}")
    if problems:
        print("Use restartKeepingFlashArea() (SilentRestart.h), or say why no sync is needed in a `flash-area:` comment.")
        return 1
    print("every planned restart in src/ syncs the flash area")
    return 0


if __name__ == "__main__":
    sys.exit(main())
