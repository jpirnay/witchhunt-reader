# Wi-Fi exits without a reboot on the PSRAM boards: design

Date: 2026-11-26. Status: design approved in conversation (approach A, S3 boards first, fixed minimums);
awaiting review of this document.

## Why

Every screen that used Wi-Fi ends in a silent reboot (`src/SilentRestart.h`, `main.cpp:294-437`). The reboot
was introduced for the ESP32-C3, whose heap does not come back after a Wi-Fi session. On the user's devices it
costs time (X3: 2,023 ms to the first page through the reboot against 792 ms in place,
`KOReaderSyncActivity.cpp:762-778`) and loses state: the screens below the one that used Wi-Fi, the Settings tab
and row, the reader's return hint. The S3 boards (X4 Pro, T5S3, Murphy, Picco) have 8 MB of PSRAM and may not
need it. The goal is to measure what the heap got back at each Wi-Fi exit and to reboot only when that is not
enough, starting with the S3 boards.

## Goals, non-goals, anti-goals

Goals
- On the S3 boards, every Wi-Fi screen except those listed under "Keeps rebooting" returns without a reboot when
  the internal heap passes the minimums, and lands where its ordinary exit leads (its parent, with the stack
  below intact).
- One shared session-end step decides it, on every board, and logs the numbers it decided on.
- The C3 boards keep rebooting, but log the same numbers, which is the data for a later C3 decision.

Non-goals
- Changing what the C3 boards do (beyond a full radio teardown before their reboot, see "My calls").
- Finding where the one-time ~19-20 KB goes after the first Wi-Fi session.
- The reboots that are not about the heap: USB Drive's storage handoff (`restartToHomeAfterStorageHandoff`),
  a firmware install (OTA or SD), the sleep-events clean boot on the C3s, the reader's heap-recovery restart,
  Serial Transfer (USB only, no Wi-Fi; on the S3s reachable only through the serial test API).

Anti-goals (a failure even if it works)
- An S3 that later fails a Wi-Fi join, a TLS handshake or a chapter build because the reboot was skipped.
- A screen that comes back with stale state the reboot used to refresh: UI strings, theme, font size, font
  lists, plugin subscriptions.
- Anything left running or open across exits: the radio, SNTP, mDNS, the web server, the captive-portal DNS
  server, an upload file handle.
- A quiet change in TLS policy: the per-session memory of unverifiable servers outliving its session.

## Evidence so far

| Board | What ran | Internal heap (free / largest block) | Source |
|---|---|---|---|
| X3 (C3) | KOReader sync, full `esp_wifi_stop` + deinit | ~20.5 KB below the pre-Wi-Fi free heap (-20,560 / -20,588 / -20,644, three sessions); contig unchanged across a push (61,428) | `KOReaderSyncActivity.cpp:762-778` |
| T5S3 (S3) | Wi-Fi up and cancelled, no TLS, 3 cycles | 138,940 / 77,812 before; 120,020 / 49,140 after the first session; then about -288 B per session, largest block stays 49,140 | Metalio adoption ledger, item 8 (`perf/kosync-no-reboot-psram` @ bbb6d5bdc, local) |
| Picco (S3) | Heap map: a join, then three joins each with one HTTPS request (2026-11-27) | 182,452 / 139,252 before; 163,636 / 90,100 after the first session; then -224 B once and -32 B per session, largest block stays 90,100 | `docs/network-stack-findings.md`, "Wi-Fi session end on an ESP32-S3" |
| Picco (S3) | PR 1, every migrated screen, five KOReader syncs in a row | Lowest at a session end: 130,456 free, 53,236 largest; every screen carried on | same |

The in-place return itself worked on the T5S3 (the reader reopened in the same boot). No foreground S3 session
with TLS has been measured after teardown. The S3 background sync already runs TLS without a reboot, and its
`Internal heap[...]` lines (`KOReaderSyncWorker.cpp:38-43`, before the radio comes up, after association, after
the request) give figures after TLS, though not after the radio goes off.

**Where the one-time cost probably goes (read from the code, not measured).** The first Wi-Fi use creates
pieces that live until the next reboot, by design:
- Arduino's network event task (4 KB stack), with its queue, mutex and event group (Arduino
  `Network/src/NetworkEvents.cpp:50-95`). Only the global object's destructor frees them, and it never runs.
- ESP-IDF's default event loop task (`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE 2048`).
- lwIP's TCP/IP task (`CONFIG_LWIP_TCPIP_TASK_STACK_SIZE 4096`), started by `esp_netif_init()`
  (`NetworkManager.cpp:29`). `esp_netif_deinit()` returns `ESP_ERR_NOT_SUPPORTED` ("Deinitialization is not
  supported yet", `esp_netif.h:33-41`).

Together they are an estimated 11-13 KB, allocated after boot in the middle of the free heap, which would also
explain the drop in the largest block. They are paid once: a second session does not pay them again. The rest of
the ~19 KB and the 288 B per session are not explained.

**Measured on the Picco (2026-11-27).** The heap map confirmed the three tasks: their stacks and control blocks
are 15,184 B of the first session's 18.8 KB (the stack blocks are larger than the config values above: 4,864,
4,864 and 4,352 B). The rest is about 40 small blocks. The stacks sit at the head of the largest free region and
small long-lived blocks pin gaps further in, which is the drop in the largest block. Each later session keeps one
28-byte block, with or without TLS. Details: `docs/network-stack-findings.md`.

## What the reboot does today (audit, 2026-11-26)

Read from the code at master 387e7962a by two read-only audits; claims spot-checked.

Shared by every site:
- **The radio is left initialised.** Only `DetectTimezoneActivity` and `SyncTimeActivity` call
  `HalClock::wifiOff()` (`HalClock.cpp:1378-1389`: SNTP stop, disconnect, `WIFI_OFF`). The others call
  `WiFi.disconnect(false)` or a bare `esp_wifi_stop()` (`KOReaderSyncActivity.cpp:329,381,625,645`) and leave the
  deinit to the reboot. `syncNtp()`'s fallback SNTP client stays enabled after a success
  (`HalClock.cpp:914-952`) unless `wifiOff()` runs.
- **TLS memory of unverifiable servers** (`g_unverifiableHosts`, `HttpDownloader.cpp:87`) is RAM-only and
  documented as "never persisted, so a reboot verifies again" (`TlsPolicy.h:79-84`). Nothing clears it.
- `silentRestart*()` also ends the reading session, captures the light state and syncs the flash area
  (`main.cpp:302-320`). In place, the reader's own `onExit()` ends the session; the flash area syncs at the next
  sleep.

On the S3 boards the trims give up almost nothing: `HalDisplay::releaseSecondaryBuffer()` and
`HalDisplay::releaseBuffers()` refuse there (`HalDisplay.cpp:337,357`), so `trimMemoryForNetworkSession()` drops
only the glyph caches, which refill on demand.

| Group | Sites | What an in-place return needs on an S3 |
|---|---|---|
| Radio only | `KOReaderSyncActivity`, `KOReaderAuthActivity`, `DetectTimezoneActivity`, `SyncTimeActivity`, `WeatherActivity`, `SettingActionDispatch::restartToSettingsIfRadioLeftOn` (Wi-Fi Networks, weather city search), `OpdsBookBrowserActivity`, `NetworkModeSelectionActivity::handOff` | The radio off. Each site's ordinary navigation is already queued and the reboot preempts it (KOReader sync: `resumeReader()`, `KOReaderSyncActivity.cpp:862-875`; OPDS: `leave()`; the mode picker's radio-down branch). One exception: the account check replaces KOReader settings (`KOReaderSettingsActivity.cpp:196-208`), so on the S3s it is pushed over it instead (done in `perf/kosync-no-reboot-psram`). |
| Downloads | `FontDownloadActivity`, `OtaUpdateActivity` (check only) | Also: `releaseMemoryForDownload()` drops the buried Settings rows on every board (`NetworkMemoryTrim.cpp:38`, `ActivityManager::releaseBuriedActivityState`), and nothing rebuilds them; `ActivityManager.cpp:373-379` reboots if one is resumed. A UI language pack "is read at the next start" (`FontDownloadActivity.cpp:716-717`). `FontDownloadActivity::onExit()` reboots even with the radio down (`:66-78`). |
| Web server | File Transfer (`CrossPointWebServerActivity` + `ServerSession`, STA and hotspot), `CalibreConnectActivity` | Also: `releaseFrameBuffers()` nulls the renderer's pointer on every board (`GfxRenderer.h:1271-1275`) although the S3 keeps the buffers. State read only at boot that a session can change: plugin subscriptions (`PluginEvents.h:21-25`, built only at `main.cpp:1714`), the UI language pack (`I18N.loadSettings()`, `main.cpp:1703`), the UI theme and font size set through the web API or serial `SET` (`SettingsByKey.cpp` stores and saves only; the device applies them in `SettingsActivity::onExit()`, `SettingsActivity.cpp:254-261`), the SD-font family names after plain uploads into a font root (kept "until discover()", `SdCardFontRegistry.h:124-130`). File-scope upload state, including an `FsFile wsUploadFile` (`CrossPointWebServer.cpp:132-148`). Calibre's Back lands on File Transfer's Calibre row through the reboot target; a plain `finish()` opens the picker on its first row (`CrossPointWebServerActivity.cpp:100-112`). |

Keeps rebooting on every board: USB Drive, a firmware install, Serial Transfer, the heap-recovery restart.

## Approaches

**A. One shared session-end step; sites migrated in groups (chosen).** Each site calls one step that turns the
radio off, measures the internal heap, logs it and answers "carry on" or "reboot". Sites that carry on take
their ordinary exit, so the stack below survives. The reboot stays as the fallback wherever the check fails.
Three PRs, in the table's order.

**B. Change the `silentRestart*()` functions themselves (rejected).** On an S3 each would turn the radio off and
open its target directly. One file, but it lands where the reboot lands (Settings tab 0, the stack below gone),
so it saves the time and not the state, and it would navigate from inside `onExit()` while another navigation is
already queued.

**Pass mark: fixed minimums (chosen) against "back to where it started" (rejected).** With the T5S3's numbers the
first session after any boot loses ~19 KB, so a back-to-baseline rule never passes on an S3; that is why the
earlier attempt was dropped. Fixed minimums ask whether what is left still covers the next session.

## Design

### The session-end step

New module `src/network/WifiSessionEnd.{h,cpp}`, namespace `wifi_session`.

```cpp
namespace wifi_session {
enum class Exit : uint8_t { CarryOn, Reboot };
// Called once at the end of setup(): the internal heap the logs compare against.
void recordBootBaseline();
// Ends a Wi-Fi session: radio off, per-session TLS memory cleared, internal heap measured and logged.
Exit end(const char* tag);
}  // namespace wifi_session
```

`end()`, in order:
0. Returns `Reboot` at once, tearing nothing down, in two cases:
   - A deep sleep is in progress. The `silentRestart*()` that follows does nothing then, as it does today
     (`deepSleepInProgress`, `main.cpp:292,313`).
   - The background KOReader sync worker is busy (S3 only, `KOReaderSyncWorker::isBusy()`). Deinitialising
     Wi-Fi under a job's open socket crashes (`main.cpp:813`). Most of today's sites never reach that,
     because they reboot instead of deinitialising. This keeps today's reboot for that race and does not wait
     on the worker inside `onExit()`.
1. Tears the radio down unless `WiFi.getMode()` reads `WIFI_MODE_NULL`. That reading can be trusted for "off":
   `getMode()` returns NULL only once `WiFi.mode(WIFI_OFF)` has cleared `_esp_wifi_started`, or when Wi-Fi never
   started (Arduino `WiFiGeneric.cpp:364-377,715-725`). A bare `esp_wifi_stop()` still reads STA, so it is torn
   down too. That makes the comment at `KOReaderSyncActivity.h:159-162` wrong; fix it in PR 1. Skipping an
   already-off radio saves `wifiOff()`'s 200 ms of delays. For AP or APSTA, call `WiFi.softAPdisconnect(true)`
   first. Then call `HalClock::wifiOff(/*skipNtpSync=*/true)`; `WIFI_OFF` runs `esp_wifi_deinit` and destroys
   the netifs (`WiFiGeneric.cpp:319-377`).
2. Clears `g_unverifiableHosts` through a new `HttpDownloader` function, so the next session verifies again.
3. Reads `heap_caps_get_free_size()` and `heap_caps_get_largest_free_block()` for
   `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.
4. Decides: `Reboot` if `HalCapabilities::restartsAfterWifiSession()` or either figure is under its minimum,
   else `CarryOn`.
5. Logs one line with the tag, both figures, the boot baseline, the minimums and the verdict: `LOG_INF` for
   "carry on" and for the C3's reboot, `LOG_ERR` when an S3 reboots on the minimums, because that is the case
   worth seeing in a field log.

The decision itself is a pure function in a header (`WifiSessionVerdict.h` or similar) so a host GoogleTest
can cover it without Wi-Fi.

A site's exit becomes:

```cpp
if (/* the condition under which this site rebooted before */) {
  if (wifi_session::end(TAG) == wifi_session::Exit::Reboot) silentRestartToX();
  // else fall through: the site's ordinary exit (its queued navigation, or finish()).
}
```

The existing condition stays. On the C3s the reboot also restores the secondary framebuffer that the trim gave
up, so a C3 site keeps rebooting wherever it trimmed, Wi-Fi or not; the capability returns `Reboot` there
regardless.

### The capability

`HalCapabilities::restartsAfterWifiSession()`, `!keepsSecondaryBuffer()`, with the `CPPCHECK_ANALYSIS`
declaration-only form its neighbours use. It replaces `restartsAfterSyncSession()` from the earlier branch. Its
comment cites the evidence table above.

### The minimums

Start from background sync's, which were measured on an X4 Pro for exactly "can a Wi-Fi session start here"
(`KOReaderAutoSync.cpp:25-32`): free internal >= 67,584 B (`WORKER_STACK_BYTES` 10,240 + 56 KB association
headroom) and largest internal block >= 24,576 B. The constants move to the new module, and
`KOReaderAutoSync::heapAllowsBackgroundSession()` reads them from there, so both checks keep one definition.
The first device run (below) may raise them; it may not lower them without a measured reason.

With the T5S3's figures (120,020 / 49,140 after the first session, -288 B per session after that), an S3 carries
on for about 180 sessions before the free figure reaches the minimum and it reboots once.

### Site changes, by PR

**PR 1: the step and the radio-only group.**
- `WifiSessionEnd`, the capability, the shared minimums, the `HttpDownloader` clear function,
  `recordBootBaseline()` in `setup()`.
- Serial `STATE` (`SerialTestApi.cpp:679-681`): on PSRAM boards add `imaxblock`, the largest internal block.
  `heap` already counts internal RAM only (`ESP.getFreeHeap()` is `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`,
  Arduino `Esp.cpp:164-166`). `maxblock` includes the PSRAM.
- The eight radio-only sites. KOReader sync and the account check build on `perf/kosync-no-reboot-psram`
  (bbb6d5bdc), including `KOReaderSettingsActivity::openAccountCheck()`. That code came from PR #855
  (trungtypo); credit it as that branch does.
- Comment fixes where a comment says every caller reboots: `NetworkMemoryTrim.h:30-33`,
  `KOReaderSyncActivity.cpp:762-778` (the C3 rationale stays, scoped to the C3s), the "silent reboot handles
  teardown" lines in `WeatherActivity.cpp`, `OpdsBookBrowserActivity.cpp` and `KOReaderSyncActivity.cpp`.

**PR 2: downloads.**
- `releaseMemoryForDownload()`: on boards where `restartsAfterWifiSession()` is false, keep the buried screens'
  state and the loaded SD font. The download's lowest internal free figure is logged in the device run. If it is
  too tight, the fallback is to rebuild a released Settings on resume rather than to bring the reboot back.
- `FontDownloadActivity`: the step replaces the unconditional restart. After a carry-on, if a UI language pack
  was installed, re-apply the active language (`UiLanguage::apply(I18N.getLanguage())` or what it needs).
  Navigation follows each entry route: pushed over Settings, `finish()`; opened by replace (`PackOffer`, the OTA
  handoff), the same target as today's restart (the reader for a book, otherwise Settings or Home).
- `OtaUpdateActivity`, check only: the step; on carry-on, pop back to Settings with its tab and row. An install
  still reboots.

**PR 3: the web server.**
- `CrossPointWebServerActivity`: on boards where `restartsAfterWifiSession()` is false, do not call
  `releaseFrameBuffers()`; keep `buffersReleased` so that `render()` still paints nothing during the session.
- `ServerSession::end()`: the step in place of the bare restart. On carry-on, refresh what is read only at boot:
  `plugin_events::refreshSubscriptions()`; the active UI language; the UI font ladder and theme (share one
  function with `SettingsActivity::onExit()`); the SD-font family list (`discover()`). Unconditionally, unless
  one of them measures slow on the device.
- Builder task: list every `SettingsList.h` row writable through the web API or serial `SET` whose effect is
  applied only at boot or only by a device screen's hook, and apply each one after the session.
- `CrossPointWebServer`: reset the file-scope upload state when the server starts and stops, and close
  `wsUploadFile` if it is open.
- `CalibreConnectActivity`: on carry-on, return to File Transfer on its Calibre row.
- Check before starting: `feat/settings-live-ui-look` (Metalio adoption item 6, local, "the theme and the ui
  font size take effect at once on psram boards") may already provide the live re-apply; build on it if it has
  landed.

### Measurement and validation

- Host: the verdict function's tests (capability on and off, each minimum just above and below), and the full
  host suite.
- Builds: `default` and `gh_release` locally; `picco` locally only for the device run; `x4pro`, `murphy` and
  `lilygo_t5s3` left to CI.
- Device, the Picco (COM9; the T5S3 is reserved for another test), before each PR's S3 behaviour ships. The Picco
  runs Onyx's stock table: flash `boot_app0.bin` at `0x15000` and the app at `0xE0000` only, never
  `pio run -e picco -t upload`, and copy the build's `web-<hash>.bin` to the card's top folder. Every site in the
  PR at least once; one site five
  times in a row, at least one of them with a TLS connection (a KOReader sync or a Weather fetch); the internal
  figures before the radio comes up, at their lowest during the session and after the step, from the log and
  `STATE`. Then the first Wi-Fi session after a carry-on must still join and finish. The figures go into the PR
  body and into `docs/network-stack-findings.md`.
- A second S3 (the T5S3 once it is free, or the X4 Pro): an SPI card, a different panel.
- One case with a background sync job in flight when a Wi-Fi screen exits (S3): the step must reboot without a
  teardown, with no crash.
- Heap map, approved by the user 2026-11-26 and run on the Picco before PR 1: a temporary serial command lists every internal
  block (`heap_caps_walk()`, present in the shipped S3 libraries, `esp_heap_caps.h:509`; heap tracing is compiled
  out) together with the task list, before Wi-Fi, after session 1 and after session 2. Comparing them names what
  survives and where it sits. It runs on a throwaway branch (`spike/wifi-heap-map`). It can confirm the
  one-time reading above, find the 288 B per session, and show whether starting the network stack early at boot
  would keep the largest block whole.
- The C3 (X4): optional in PR 1, to confirm the C3 still reboots and to collect its log lines. X3 runs are opt-in.
- Shared devices: `ListAgents` and `CMD:STATE` before taking a port.

## Decided by the user

- Approach A, the S3 boards first.
- Fixed minimums for the pass mark, accepting about 19 KB less internal heap on an S3 after its first Wi-Fi use.

## My calls (correct any of these)

- Module name and place (`src/network/WifiSessionEnd`), the capability name, `iheap`/`imaxblock` in `STATE`.
- Starting minimums taken from background sync, shared through one definition.
- The step does a full radio teardown on every board, the C3s included. That costs the C3s about 200 ms of
  `wifiOff()` delays before their reboot, and it makes their logged figures meaningful for a later C3 decision.
- Clearing the unverifiable-server memory at each session end, which keeps its "per session" meaning.
- A busy background sync worker or a deep sleep in progress makes the step return `Reboot` without a teardown
  (step 0). The alternatives were rejected: draining the worker blocks `onExit()` under the render lock for up
  to 3-10 s, and leaving the radio to the worker breaks "nothing left running".
- On the S3s: keeping the buried Settings state and the SD font through a download, and the framebuffers through
  a web session, rather than releasing and rebuilding them.
- Refreshing the boot-only state after every web session, not only when it changed.
- Three PRs in the order above, each with its own device run.

## Left to the builder

- Whether the measured figures confirm that Arduino's `WIFI_OFF` releases what `esp_wifi_deinit` frees.
- The exact navigation call at each site, verified against how that site was opened (push or replace).
- `onExit()` runs under the exclusive `RenderLock` (`ActivityManager.cpp:419-430`); `wifiOff()` sleeps 200 ms
  there. Acceptable unless the device run shows a visible stall.
- Whether the flash area held in PSRAM (Picco stock table) needs a `FlashArea::sync()` on the carry-on path, or
  the next sleep is enough.

## Risks

- The one-time ~19 KB is 15.2 KB of network tasks plus about 3.5 KB of small blocks (measured, see "Where the
  one-time cost probably goes"). If a later S3 session needs more than the minimums assume, the first symptom is a
  failed join or handshake, not a crash: the device run included five sessions after a carry-on, and a sixth.
- Each session keeps one 28-byte block whose owner is not identified (the Picco, with and without TLS). The T5S3's
  -288 B per session came from a different run and is not explained. The minimums bound both.
- The local branches `perf/kosync-no-reboot-psram` and `feat/settings-live-ui-look` belong to the Metalio
  adoption workstream (ledger `C:/tmp/wt-metalio-adopt/LEDGER.md`). PR 1 supersedes item 8; its ledger row should
  say so.
