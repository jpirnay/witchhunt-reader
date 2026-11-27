# SPIKE ONLY: apply the BLE spike's flags to the BLE sources alone (NimBLE-Arduino, the SDK's BleKeyboardHost and
# src/BleSpike.cpp), so every other object keeps its cached build. A global build_flags change recompiles all of them.
# Registered after git_branch.py's middleware: a node that one already turned into an object (a NodeList, for files
# naming the Git defines) passes through untouched, since no BLE source names them.
import os

Import("env")  # noqa: F821

_header = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "ble_spike_nimble_c3.h")  # noqa: F821
_flags = ["-include", _header, "-DFREEINK_CAP_BLE_HID_HOST=1"]
# `custom_ble_spike_controller = flash`: scripts/ble_spike_flash_ctrl.py links the flash-only controller, and
# src/BleSpike.cpp compiles what stands in for flash-only bt.c.
if env.GetProjectOption("custom_ble_spike_controller", "rom") == "flash":  # noqa: F821
    _flags.append("-DBLE_SPIKE_FLASH_CTRL=1")
_markers = ("nimble-arduino", "blekeyboardhost", "blespike.cpp")


def _ble_flags(build_env, node):
    if not hasattr(node, "srcnode"):
        return node
    path = node.srcnode().get_path().replace("\\", "/").lower()
    if not any(marker in path for marker in _markers):
        return node
    scoped_env = build_env.Clone()
    scoped_env.Append(CCFLAGS=_flags)
    return scoped_env.Object(node)


env.AddBuildMiddleware(_ble_flags)  # noqa: F821
