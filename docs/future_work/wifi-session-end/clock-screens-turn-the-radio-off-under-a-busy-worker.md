# Sync Time and Detect Timezone turn the radio off under a busy sync worker

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, pre-existing (found in PR 1's review, not caused by it).** Both clock screens call `HalClock::wifiOff()`
themselves: Sync Time after its sync and in `onExit()`, Detect Timezone in the same places
(`src/activities/settings/SyncTimeActivity.cpp`, `DetectTimezoneActivity.cpp`). `wifi_session::end()` refuses to
tear down while `KOReaderSyncWorker::isBusy()`, but these calls come before it or without it. On a PSRAM board,
a clock screen opened within the few seconds of a background push takes the radio down under the worker's open
socket. `main.cpp` (`enterDeepSleep`) records that a deinit under that socket crashes.

When it can happen: the reader's interval push or sleep push is in flight (about 4 s on the Picco: join, then the
TLS request), and the user reaches Clock settings and starts Sync Time or Detect Timezone in that time.

**Next step:** have both screens skip their own `wifiOff()` while the worker is busy and leave the teardown to
`wifi_session::end()`. Or make them wait for the worker on entry (`KOReaderSyncWorker::drain()`), as the KOReader
sync and account screens do. Check it on a PSRAM board with the worker-busy procedure on the topic page, using a
clock screen as the site.
