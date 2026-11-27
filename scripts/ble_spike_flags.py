# SPIKE ONLY (spike/ble-c3-rebuild-gate): give the BLE sources alone (NimBLE-Arduino, the SDK's BleKeyboardHost and
# src/BleSpike.cpp) the host capability and NimBLE-Arduino's role switches, so every other object keeps its cached
# build. NimBLE's sizes and the flash-only controller come from [firmware_tuned]'s custom_sdkconfig (the rebuilt core).
Import("env")  # noqa: F821

_flags = ["-DFREEINK_CAP_BLE_HID_HOST=1", "-DCONFIG_BT_NIMBLE_ROLE_PERIPHERAL_DISABLED=1",
          "-DCONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED=1"]
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
