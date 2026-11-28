"""
PlatformIO post-configuration hook: link the ESP32-C3 envs with
ld/esp32c3-iram-budget/sections.ld instead of the Arduino core's own sections.ld. The C3 envs rebuild that core from
[firmware_tuned]'s custom_sdkconfig (platformio.ini), so "the core" here is the rebuilt one.

That script is generated, never edited: tools/iram-budget/gen_sections_ld.py runs ESP-IDF's ldgen at the commit the
core's libraries were built from, against those libraries, with the linker-only Kconfig options of
tools/iram-budget/options.txt flipped (FreeRTOS and ring buffer functions that need not run with the flash cache
disabled, and the unused RMT interrupt handlers, placed in flash instead of IRAM). On the C3 IRAM is carved from the
same SRAM as the heap, so every byte that leaves .iram0.text is a byte of heap at boot.

The generated script is only valid for the libraries it was generated against. Its header records the sha256 of the
platform's sections.ld it was derived from (its "Generated from:" line, a build path, left out); when the platform's
file differs (pioarduino updated its libraries, or custom_sdkconfig changed and the core was rebuilt), the build stops
here: regenerate (see gen_sections_ld.py) rather than link a stale script. It was generated for the DIO flash mode's
libspi_flash.a; any other flash mode stops the build too.

ESP32-S3 and ESP32 envs (X4 Pro, LilyGo T5S3, Murphy) are left alone.

A post: script, not a pre: one: the framework's builder sets LINKFLAGS (with `-T sections.ld`) after the pre: scripts
run. The swap replaces that flag with our script's absolute path, so it is part of the link command's signature:
a library path ahead of the platform's would find our file too, but SCons leaves library paths out of the signature,
and the build cache then hands back an ELF linked with the other script.
"""

import hashlib
import os
import re

Import("env")  # noqa: F821  (SCons)

board = env.BoardConfig()  # noqa: F821

# The run that rebuilds the core (custom_sdkconfig on a core not yet rebuilt from it, as on a cold CI cache) is
# pioarduino's hybrid compile: espidf.py sets ARDUINO_LIB_COMPILE_FLAG=Build, empties LINKFLAGS and compiles the IDF
# libraries; a post-action copies them into the framework and links the firmware in a child `pio run`, which runs this
# script again against the rebuilt core. Checking here would compare the prebuilt core and stop the rebuild.
rebuild_pass = env.subst("$ARDUINO_LIB_COMPILE_FLAG") == "Build"  # noqa: F821
if rebuild_pass and board.get("build.mcu", "") == "esp32c3":
    print("iram_budget: this run rebuilds the core; the Arduino build it starts next is checked")

if board.get("build.mcu", "") == "esp32c3" and not rebuild_pass:
    project = env.subst("$PROJECT_DIR")  # noqa: F821
    ours = os.path.join(project, "ld", "esp32c3-iram-budget", "sections.ld")
    libs = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")  # noqa: F821
    original = os.path.join(libs, "esp32c3", "ld", "sections.ld")

    with open(ours, encoding="utf-8") as f:
        head = f.read(4096)
    m = re.search(r"source-sha256: ([0-9a-f]{64})", head)
    if not m:
        print("iram_budget: %s has no source-sha256 header" % ours)
        env.Exit(1)  # noqa: F821
    with open(original, encoding="utf-8") as f:
        text = f.read()
    # A rebuilt core's sections.ld names the absolute .pio directory it was built in; as in
    # tools/iram-budget/gen_sections_ld.py (without_build_path), that line is left out of the hash.
    text = re.sub(r"^/\* Generated from: .* \*/$", "/* Generated from: esp_system/ld/sections.ld.in */", text, count=1,
                  flags=re.M)
    actual = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if actual != m.group(1):
        print("iram_budget: ERROR: the platform's %s (sha256 %s) is not the one %s was generated from (%s)."
              % (original, actual, ours, m.group(1)))
        print("iram_budget: the core's libraries changed (a pioarduino update, or a custom_sdkconfig change rebuilt them);"
              " regenerate with tools/iram-budget/gen_sections_ld.py.")
        env.Exit(1)  # noqa: F821

    flash_mode = env.GetProjectOption("board_build.flash_mode", board.get("build.flash_mode", "dio"))  # noqa: F821
    memory_type = board.get("build.arduino.memory_type", flash_mode + "_qspi")
    if memory_type != "dio_qspi":
        print("iram_budget: ERROR: %s was generated for dio_qspi's libspi_flash.a, this env links %s" % (ours, memory_type))
        env.Exit(1)  # noqa: F821

    flags = list(env["LINKFLAGS"])  # noqa: F821
    swapped = 0
    for i in range(len(flags) - 1):
        if flags[i] == "-T" and flags[i + 1] == "sections.ld":
            flags[i + 1] = ours
            swapped += 1
    if swapped != 1:
        print("iram_budget: ERROR: expected one `-T sections.ld` in LINKFLAGS, found %d" % swapped)
        env.Exit(1)  # noqa: F821
    env.Replace(LINKFLAGS=flags)  # noqa: F821
    env.Depends("$BUILD_DIR/${PROGNAME}.elf", ours)  # noqa: F821
    print("iram_budget: linking with %s" % os.path.relpath(ours, project))
