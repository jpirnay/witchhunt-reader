# SPIKE ONLY (spike/ble-s3-probe): give the BLE sources alone (NimBLE-Arduino, the SDK's BleKeyboardHost and
# src/BleSpike.cpp) the host capability, NimBLE-Arduino's role switches and, on boards with PSRAM, NimBLE's host in
# PSRAM (scripts/s3_nimble_psram.h, CrossPoint #3785). Every other object keeps its cached build.
import os

Import("env")  # noqa: F821

_flags = ["-DFREEINK_CAP_BLE_HID_HOST=1", "-DFREEINK_BLE_HID_SCAN_DEBUG=1", "-DCONFIG_BT_NIMBLE_ROLE_PERIPHERAL_DISABLED=1",
          "-DCONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED=1",
          "-include", os.path.join(env.subst("$PROJECT_DIR"), "scripts", "s3_nimble_psram.h")]  # noqa: F821
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
