# SNTP is stopped only when the radio was up

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, hardening.** `esp_sntp_stop()` runs inside `HalClock::wifiOff()`. `wifi_session::end()` skips `wifiOff()`
when `WiFi.getMode()` already reads off (`radioOff()` in `src/network/WifiSessionEnd.cpp`). Every PR 1 site that
turns the radio off itself also stops SNTP, so nothing is left running today. The spec's anti-goals rule out SNTP
left running across an exit, though, and a later site that turns the radio off some other way would break that.

**Next step:** in `end()`, stop SNTP whether or not the radio was up: `if (esp_sntp_enabled()) esp_sntp_stop();`.
