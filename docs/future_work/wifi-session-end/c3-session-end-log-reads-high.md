# The C3s' session-end log line reads high

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, small.** On the C3s the network trim gives the secondary framebuffer (about 48 KB) back to the heap before
the session. So the `Session end [...]: internal free=...` line at the session end can read about 28 KB above the
boot baseline, on the very boards whose loss the line is meant to show. The verdict is not affected, because the C3s
restart regardless.

**Next step:** add `display.hasSecondaryBuffer()` to the line in `wifi_session::end()`
(`src/network/WifiSessionEnd.cpp`), or note the buffer next to the figures wherever C3 results are recorded.
