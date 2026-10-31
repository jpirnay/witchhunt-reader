"""
The ESP32-C3 builds (X3, X4) ship bootloader/esp32c3/bootloader.bin in place of the Arduino core's
prebuilt bootloader: the core's own, built from the same ESP-IDF and configuration, except that it
does not check the app image on a power-on (CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON). An X3, or
an X4 with its battery latch cut, wakes through a power-on, and that check of the whole ~6 MB image
took 1026 ms of the user's press on an X3. See bootloader/esp32c3/README.md.

The bootloader and the app have to come from the same ESP-IDF: they share the RTC memory it reserves
and the image and partition formats. So the build stops when the core's ESP-IDF is not the one
sdkconfig.defaults was taken from, rather than pairing an app with a bootloader of another version.

Only the factory image and USB uploads carry a bootloader: an OTA or SD-card update replaces the app
and leaves whatever bootloader the device already has.
"""
Import("env")  # noqa: F821  (SCons)

import os
import re
import sys

board = env.BoardConfig()  # noqa: F821

if board.get("build.mcu", "") == "esp32c3":
    project = env.subst("$PROJECT_DIR")  # noqa: F821
    folder = os.path.join(project, "bootloader", "esp32c3")
    image = os.path.join(folder, "bootloader.bin")

    def idf_version(path):
        try:
            with open(path, encoding="utf-8") as f:
                m = re.search(r"ESP-IDF\)? (\d+\.\d+\.\d+)", f.read(4096))
        except OSError:
            return None
        return m.group(1) if m else None

    libs = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")  # noqa: F821
    core = idf_version(os.path.join(libs, "esp32c3", "sdkconfig")) if libs else None
    built = idf_version(os.path.join(folder, "sdkconfig.defaults"))
    if not os.path.isfile(image) or core is None or core != built:
        sys.stderr.write(
            "c3_bootloader: bootloader/esp32c3 was built from ESP-IDF %s, the Arduino core is %s.\n"
            "Rebuild it as bootloader/esp32c3/README.md describes, or this build would pair the app\n"
            "with a bootloader of another ESP-IDF.\n" % (built, core)
        )
        env.Exit(1)  # noqa: F821

    # pioarduino-build.py joins this onto the variant folder; an absolute path replaces that.
    board.update("build.arduino.custom_bootloader", image)
    print("c3_bootloader: %s (ESP-IDF %s, no image check on power-on)" % (os.path.relpath(image, project), built))
