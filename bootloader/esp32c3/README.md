# ESP32-C3 bootloader (X3, X4)

`bootloader.bin` is the second-stage bootloader the ESP32-C3 builds ship instead of the Arduino
core's prebuilt `bootloader_dio_80m`. It is that bootloader, built from the same ESP-IDF and the
core's own configuration, with one option changed:

    CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON=y

`scripts/c3_bootloader.py` puts it into every ESP32-C3 build (`build.arduino.custom_bootloader`).
The ESP32-S3 boards keep the core's bootloader.

## Why

An X3 sleeps with its battery latch open, and so does an X4 with the clock off: the MCU is
unpowered, and the power button powers it up again. Every wake is a power-on reset, and on a
power-on the stock bootloader checks the whole app image (about 6 MB) before starting it. An X3
measured 1026 ms from power-on to the app, all of which the user has to hold the button through.
A wake from deep sleep already skips that check (`CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`).

## What is still checked

- An update is verified as it is written: OTA and SD-card updates go through `esp_ota_end()`,
  which checks the image before it can be selected.
- After any reset other than a power-on (the restart after an update included), the bootloader
  checks the app as before.
- Not checked: an app image damaged on the flash after it was written. A cold start then runs it
  instead of falling back to the other OTA slot.

## Who gets it

Only what writes address 0: the factory image (`firmware.factory.bin`, the web flasher) and
`pio run -t upload`. OTA and SD-card updates replace the app and never the bootloader, so a
device keeps the bootloader it was last flashed with over USB, and a device whose USB flashing
is locked keeps its own. Both kinds of bootloader run every app.

## Rebuilding

The bootloader and the app must come from the same ESP-IDF: they share the RTC memory the
bootloader reserves, and the image and partition formats. `scripts/c3_bootloader.py` stops the
firmware build when the Arduino core's ESP-IDF is not the one `sdkconfig.defaults` names. Then:

1. Regenerate the configuration from the new core:

       python scripts/make_bootloader_defaults.py \
           <core_dir>/packages/framework-arduinoespressif32-libs/esp32c3/sdkconfig \
           bootloader/esp32c3/sdkconfig.defaults

2. Set `platform` in `bootloader/esp32c3/platformio.ini` to the firmware's.
3. Build and copy:

       cd bootloader/esp32c3
       rm -f sdkconfig.bootloader
       pio run
       cp .pio/build/bootloader/bootloader.bin bootloader.bin

4. Check it before committing:
   - `sdkconfig.bootloader` differs from the core's `sdkconfig` only in
     `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON` (the core's file has names the plain ESP-IDF
     project does not, which do not matter).
   - It is a DIO bootloader: `riscv32-esp-elf-nm .pio/build/bootloader/bootloader.elf` lists no
     `bootloader_enable_qio_mode`. A QIO bootloader sets the flash's quad-enable bit at every boot.
     The core's `sdkconfig` selects QIO (the lib-builder cuts one bootloader per flash mode), which
     is why the generator overrides it and leaves out the deprecated `CONFIG_FLASHMODE_*` names
     that would select it again.
5. Flash a factory image to an X3 and an X4 and wake each from sleep on battery: `CMD:BOOTLOG`
   then reports the time before the app on its wake-gate line.
