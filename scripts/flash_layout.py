"""
PlatformIO post-configuration hook: record how `pio run -t upload` lays this env out in flash, in
$BUILD_DIR/flash_layout.json, so scripts/full_install_image.py can merge the full-install image a
release publishes from exactly the same inputs.

The upload command (the platform's builder/main.py) writes
  esptool --chip <mcu> write-flash -z --flash-mode <mode> --flash-freq <freq> --flash-size detect
          <FLASH_EXTRA_IMAGES: bootloader, partitions.bin, boot_app0.bin> <ESP32_APP_OFFSET> firmware.bin
and every value below is taken from the same construction variables, substituted the same way:
the extra images and their offsets (the C3's custom bootloader, set by scripts/c3_bootloader.py,
or the S3 bootloader the build generates), the flash mode the platform derives from flash_mode and
memory_type, the image frequency, the app offset and the esptool it runs. The one value an image
file cannot copy is "detect": the upload reads the size from the chip, so the merge names the
board's upload.flash_size instead, as the platform's own firmware.factory.bin merge does.

Written after "checkprogsize", which every build and upload runs (AlwaysBuild): the platform sets
ESP32_APP_OFFSET in that target's pre-action, from the partition table, so it is not known while
the scripts load. Nothing in the build reads the file.
"""

import json
import os

Import("env")  # noqa: F821  (SCons)


def _write_layout(source, target, env):
    board = env.BoardConfig()

    def subst(value):
        return env.subst(str(value)).strip().strip('"')

    layout = {
        "env": subst("$PIOENV"),
        "chip": board.get("build.mcu", ""),
        "flash_mode": subst("${__get_board_flash_mode(__env__)}"),
        "flash_freq": subst("${__get_board_f_image(__env__)}"),
        "flash_size": board.get("upload.flash_size", ""),
        "partitions_csv": subst("$PARTITIONS_TABLE_CSV"),
        "esptool": subst("$UPLOADER"),
        "images": [{"offset": subst(offset), "path": subst(path)} for offset, path in env.get("FLASH_EXTRA_IMAGES", [])],
        "app": {"offset": subst("$ESP32_APP_OFFSET"), "path": subst("$BUILD_DIR/${PROGNAME}.bin")},
    }
    out = os.path.join(subst("$BUILD_DIR"), "flash_layout.json")
    text = json.dumps(layout, indent=2) + "\n"
    try:
        with open(out, encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)


action = env.Action(_write_layout)  # noqa: F821
action.strfunction = lambda target, source, env: ""
env.AddPostAction("checkprogsize", action)  # noqa: F821
