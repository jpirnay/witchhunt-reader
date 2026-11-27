# The 28-byte block each Wi-Fi session keeps

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, low priority.** On the Picco every Wi-Fi session after the first keeps one 28-byte internal block (32 B with
the heap header). It does so for a join alone and for a join with an HTTPS request alike, and the first HTTPS
session also keeps 224 B once. The block lands at a different address each time. Its owner is not identified,
because heap tracing is compiled out of these builds. At 32 B per session, a PSRAM board needs thousands of sessions
in one boot to reach the minimums. The T5S3's earlier -288 B per session came from a different run (joins
cancelled through the UI) and is not explained either.

**Next step, if it ever matters:** a build with heap tracing (`CONFIG_HEAP_TRACING_STANDALONE`) and a stack depth
of a few frames, then one radio cycle between `heap_trace_start()` and `heap_trace_stop()`. Or narrow it down: Arduino's
`WiFi.mode(WIFI_STA)` / `WIFI_OFF` alone, then with `begin()`, using the heap-map method on the topic page.
