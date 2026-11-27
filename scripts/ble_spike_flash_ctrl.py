# SPIKE ONLY: link Espressif's flash-only BLE controller (libbtdm_app_flash.a) in place of the ROM-assisted one the
# prebuilt core links, without rebuilding the core. Enabled by `custom_ble_spike_controller = flash` in the env.
#
# What IDF v5.5.5 does differently for CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY on the C3, and what stands in for it here:
#   components/bt/controller/CMakeLists.txt:15-22   links libbtdm_app_flash.a  -> the blob under libbtdm_app.a's name,
#                                                    in a directory ahead of the framework's (so sections.ld's
#                                                    *libbtdm_app.a rules still place its .data/.bss)
#   components/esp_rom/CMakeLists.txt:122-125,289   drops rom.bt_funcs.ld and   -> the same two -T flags removed
#                                                    rom.eco3_bt_funcs.ld          (their plain assignments would bind
#                                                                                  the blob's own functions to ROM)
#   components/bt/controller/esp32c3/bt.c:874       interrupt without           -> -Wl,--wrap=esp_intr_alloc, the
#                                                    ESP_INTR_FLAG_IRAM            wrapper in src/BleSpike.cpp
#   components/bt/controller/esp32c3/bt.c:1355      no ROM table resets         -> nine empty stubs in src/BleSpike.cpp
#   components/bt/include/esp32c3/include/esp_bt.h  .run_in_flash = 1           -> set in src/BleSpike.cpp's config
# The define BLE_SPIKE_FLASH_CTRL that switches on the src/BleSpike.cpp side comes from scripts/ble_spike_flags.py.
#
# The blob is not in the repository: fetch it with tools/ble-spike/fetch_flash_controller.sh.
import hashlib
import os

Import("env")  # noqa: F821

# espressif/esp32c3-bt-lib at 0a08c4b32f3666003080b662a1a61794da24ff0f (the commit IDF b774170ff46 pins):
# esp32c3/libbtdm_app_flash.a, git blob id.
BLOB_GIT_SHA = "05cc82f93c20524bf239ac198d2e6ffff6cf1ce7"
DROPPED_ROM_SCRIPTS = ("esp32c3.rom.bt_funcs.ld", "esp32c3.rom.eco3_bt_funcs.ld")


def git_blob_sha(path):
    with open(path, "rb") as f:
        data = f.read()
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


if env.GetProjectOption("custom_ble_spike_controller", "rom") == "flash":  # noqa: F821
    project = env.subst("$PROJECT_DIR")  # noqa: F821
    blob_dir = os.path.join(project, ".pio", "ble-spike", "flash-controller")
    blob = os.path.join(blob_dir, "libbtdm_app.a")
    if not os.path.isfile(blob) or git_blob_sha(blob) != BLOB_GIT_SHA:
        print("ble_spike_flash_ctrl: ERROR: %s missing or not esp32c3-bt-lib's libbtdm_app_flash.a (git %s);"
              " run tools/ble-spike/fetch_flash_controller.sh" % (blob, BLOB_GIT_SHA))
        env.Exit(1)  # noqa: F821

    env.Prepend(LIBPATH=[blob_dir])  # noqa: F821

    flags = list(env["LINKFLAGS"])  # noqa: F821
    kept = []
    dropped = []
    i = 0
    while i < len(flags):
        if flags[i] == "-T" and i + 1 < len(flags) and os.path.basename(str(flags[i + 1])) in DROPPED_ROM_SCRIPTS:
            dropped.append(os.path.basename(str(flags[i + 1])))
            i += 2
            continue
        kept.append(flags[i])
        i += 1
    if sorted(dropped) != sorted(DROPPED_ROM_SCRIPTS):
        print("ble_spike_flash_ctrl: ERROR: expected to drop %s from LINKFLAGS, dropped %s"
              % (", ".join(DROPPED_ROM_SCRIPTS), ", ".join(dropped) or "nothing"))
        env.Exit(1)  # noqa: F821
    kept.append("-Wl,--wrap=esp_intr_alloc")
    env.Replace(LINKFLAGS=kept)  # noqa: F821
    print("ble_spike_flash_ctrl: flash-only controller from %s; dropped %s; esp_intr_alloc wrapped"
          % (blob_dir, ", ".join(dropped)))
