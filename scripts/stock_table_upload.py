"""
PlatformIO post-configuration hook for an env that installs onto a board's stock partition table
([env:picco_stock]): `pio run -t upload` writes the app at the table's ota_0 offset (the platform
derives ESP32_APP_OFFSET from the table) and otadata selecting it, and nothing else. The vendor
bootloader, partition table, NVS and recovery partition stay as they are.

The platform's default upload also writes the Arduino bootloader at 0x0, partitions.bin at 0x8000
and boot_app0.bin at a fixed 0xe000 (pioarduino-build.py, `upload.arduino.boot_app0`). On a stock
table 0xe000 is inside another partition (the Picco's NVS), so boot_app0.bin is moved to the
table's own otadata offset. boot_app0.bin is otadata with sequence 1 in its first sector, which
selects ota_0 on any table.

FLASH_EXTRA_IMAGES alone is not enough: the platform's builder/main.py appends each extra image to
UPLOADERFLAGS when it sets the esptool protocol up, before any post script runs. Replacing only
FLASH_EXTRA_IMAGES (which flash_layout.json reads) left the upload writing the bootloader, the
table and 0xe000 anyway: it did so on the first BOOX Picco install, 2026-11-13. So the upload
command's image list is rebuilt here too, and the configuration stops if it still names anything
but otadata and the app.

Based on crosspoint-reader PR #3947 (Justin Mitchell, scripts/app_only_upload.py).
"""

import os

Import("env")  # noqa: F821  (SCons)


def _otadata_offset(csv_path):
    with open(csv_path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = [t.strip() for t in line.split(",")]
            if len(fields) >= 4 and fields[1] == "data" and fields[2] == "ota" and fields[3]:
                return int(fields[3], 0)
    raise ValueError("%s has no otadata partition with an offset" % csv_path)


def _boot_app0(images):
    for _, path in images:
        if os.path.basename(env.subst(str(path))) == "boot_app0.bin":  # noqa: F821
            return path
    raise ValueError("the platform's upload images have no boot_app0.bin: %r" % (images,))


def _without_images(flags):
    # The esptool flags main.py builds end in "--flash-size detect"; every pair after that is an
    # extra image it appended (offset, path).
    flags = list(flags)
    for i in range(len(flags) - 1):
        if flags[i] == "--flash-size":
            return flags[: i + 2]
    raise ValueError("no --flash-size in the upload flags: %r" % (flags,))


_csv = env.subst("$PARTITIONS_TABLE_CSV")  # noqa: F821
_otadata = hex(_otadata_offset(_csv))
_image = _boot_app0(env.get("FLASH_EXTRA_IMAGES", []))  # noqa: F821
env.Replace(FLASH_EXTRA_IMAGES=[(_otadata, _image)])  # noqa: F821
if env.subst("$UPLOAD_PROTOCOL") == "esptool":  # noqa: F821
    _flags = _without_images(env["UPLOADERFLAGS"]) + [_otadata, env.subst(_image)]  # noqa: F821
    env.Replace(UPLOADERFLAGS=_flags)  # noqa: F821
    _offsets = [f for f in _flags[_flags.index("--flash-size") + 2 :] if str(f).startswith("0x")]
    if _offsets != [_otadata]:
        raise ValueError("stock_table_upload: the upload would still write %r" % (_offsets,))
print("stock_table_upload: upload writes otadata at %s and the app only (%s)" % (_otadata, os.path.basename(_csv)))
