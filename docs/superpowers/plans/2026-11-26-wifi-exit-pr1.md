# Wi-Fi exits without a reboot, PR 1: the session-end step and the radio-only screens

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On the PSRAM boards, the eight radio-only Wi-Fi screens return without a reboot when the internal heap
passes fixed minimums; every board logs the figures it decided on.

**Architecture:** A pure verdict header (host-tested) and a device module `wifi_session` (radio teardown,
per-session TLS memory cleared, internal heap measured and logged, `CarryOn` or `Reboot`). Each site keeps its
existing reboot condition and calls the step inside it; `CarryOn` falls through to the site's ordinary exit.

**Tech Stack:** C++20 (no exceptions, no RTTI), Arduino-ESP32 3.3.11 / ESP-IDF heap caps, GoogleTest host suite,
PlatformIO, serial test API (`tools/devctl.py`).

**Spec:** `docs/superpowers/specs/2026-11-26-wifi-exit-without-reboot-design.md`

## Global Constraints

- Branch `perf/wifi-exit-in-place` in `C:/tmp/wt-wifi-exit`; the throwaway `spike/wifi-heap-map` never merges.
- Board differences only through `HalCapabilities`; no board-name `#ifdef` in feature code.
- Minimums: free internal >= 67,584 B (10,240 worker stack + 56 KB association headroom), largest internal block
  >= 24,576 B; internal means `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. The device run may raise them, never lower.
- The C3 boards still reboot at every site that rebooted before.
- Logging through `LOG_INF`/`LOG_ERR("WIFI", ...)` from `Logging.h`; no `Serial`.
- clang-format 21 (`bin/clang-format-fix -g`; new files `clang-format-21 -style=file -i`).
- Commit subjects `type(scope): summary`, lowercase, imperative; no AI co-author trailers. The KOReader sync and
  account-check changes name "PR #855 (trungtypo)" in the commit body.
- Builds: `default` and `gh_release` locally, `picco` locally for the device run; `x4pro`, `murphy`,
  `lilygo_t5s3` to CI.
- Device: the Picco on COM9 only (the T5S3 is reserved). Onyx stock table: flash `boot_app0.bin` at `0x15000` and
  `firmware.bin` at `0xE0000`, never `pio run -e picco -t upload`; copy `web-<hash>.bin` to the card's top folder
  after every flash and never delete `/web-*.bin`.

## Review Focus

1. A screen exits while the background sync worker holds the radio (S3): reboot as today, no crash (Task 6, step 5).
2. Deep sleep entered from a Wi-Fi screen: the device sleeps, no restart back up, no second teardown (Task 6, step 6).
3. A second Wi-Fi session right after a carry-on (KOReader sync, then Weather): joins and finishes (Task 6, step 4).
4. The radio already off when the step runs (the clock screens turn it off first): no extra 200 ms, `CarryOn`
   (Task 2's teardown rule; Task 6, step 3).
5. The minimums failing on an S3: `LOG_ERR`, then the site's old reboot target (Task 1's tests; Task 6, step 7).

---

### Task 1: The verdict and the shared minimums

**Files:**
- Create: `src/network/WifiSessionVerdict.h`
- Create: `test/wifi_session_verdict/CMakeLists.txt`, `test/wifi_session_verdict/WifiSessionVerdictTest.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(wifi_session_verdict)` beside `home_clock_cadence`)
- Modify: `src/activities/reader/KOReaderAutoSync.cpp:25-32`

**Interfaces:**
- Produces, in namespace `wifi_session`:
  - `enum class Exit : uint8_t { CarryOn, Reboot };`
  - `constexpr uint32_t kWorkerStackBytes = 10240;`, `kAssociationHeadroomBytes = 56 * 1024;`,
    `kMinFreeInternalBytes = kWorkerStackBytes + kAssociationHeadroomBytes;`, `kMinLargestInternalBytes = 24 * 1024;`
  - `constexpr Exit verdict(bool restartsAfterSession, uint32_t freeInternal, uint32_t largestInternal);`

- [ ] **Step 1: Write the failing tests** (model: `test/home_clock_cadence/CMakeLists.txt`, include dir `${REPO_ROOT}/src`)

```cpp
#include "network/WifiSessionVerdict.h"
using wifi_session::Exit;
using wifi_session::verdict;

TEST(WifiSessionVerdict, MinimumsAreBackgroundSyncs) {
  EXPECT_EQ(wifi_session::kMinFreeInternalBytes, 67584u);
  EXPECT_EQ(wifi_session::kMinLargestInternalBytes, 24576u);
}
TEST(WifiSessionVerdict, TheC3sAlwaysReboot) { EXPECT_EQ(verdict(true, 300000, 200000), Exit::Reboot); }
TEST(WifiSessionVerdict, ExactlyAtTheMinimumsCarriesOn) { EXPECT_EQ(verdict(false, 67584, 24576), Exit::CarryOn); }
TEST(WifiSessionVerdict, OneByteShortOfFreeReboots) { EXPECT_EQ(verdict(false, 67583, 200000), Exit::Reboot); }
TEST(WifiSessionVerdict, OneByteShortOfTheBlockReboots) { EXPECT_EQ(verdict(false, 300000, 24575), Exit::Reboot); }
TEST(WifiSessionVerdict, TheT5S3AfterItsFirstSessionCarriesOn) {
  EXPECT_EQ(verdict(false, 120020, 49140), Exit::CarryOn);
}
```

- [ ] **Step 2: Run them to see them fail**

Run (Bash): `cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/test --target WifiSessionVerdictTest`
Expected: compile error, `network/WifiSessionVerdict.h` not found.

- [ ] **Step 3: Write `WifiSessionVerdict.h`.** Header-only, no Arduino includes (`<cstdint>` only). The header
  comment says where the figures come from (`KOReaderAutoSync.cpp`, the worker's logs on an X4 Pro) and why the
  worker stack is in the free figure (a background sync may be the next session).
- [ ] **Step 4: Point `KOReaderAutoSync.cpp` at it.** Replace `ASSOCIATION_HEADROOM_BYTES`,
  `BG_SESSION_MIN_FREE_INTERNAL_BYTES` and `BG_SESSION_MIN_CONTIG_INTERNAL_BYTES` with the `wifi_session`
  constants, and add `static_assert(KOReaderSyncWorker::WORKER_STACK_BYTES == wifi_session::kWorkerStackBytes, ...)`.
- [ ] **Step 5: Run the suite target.** Expected: `WifiSessionVerdictTest` 6/6 pass.
- [ ] **Step 6: Commit** `refactor(network): one definition of what a wi-fi session needs, and the verdict on it`

### Task 2: The session-end step, the capability and the plumbing

**Files:**
- Create: `src/network/WifiSessionEnd.h`, `src/network/WifiSessionEnd.cpp`
- Modify: `lib/hal/HalCapabilities.h` (after `sendsSleepEventsFromCleanBoot()`, ~line 401)
- Modify: `src/network/HttpDownloader.h`, `src/network/HttpDownloader.cpp:87`
- Modify: `src/SilentRestart.h`, `src/main.cpp` (beside `deepSleepInProgress`, line 292; end of `setup()`, line 1966)
- Modify: `src/SerialTestApi.cpp:679-681`, `docs/serial-test-api.md:266`
- Modify: `src/activities/NetworkMemoryTrim.h:30-33` (comment)

**Interfaces:**
- Consumes: Task 1's `wifi_session::verdict`, `Exit`, the minimums.
- Produces:
  - `HalCapabilities::restartsAfterWifiSession()` = `!keepsSecondaryBuffer()`, with the `CPPCHECK_ANALYSIS`
    declaration-only form `sendsSleepEventsFromCleanBoot()` uses.
  - `static void HttpDownloader::forgetUnverifiableHosts();` (calls `g_unverifiableHosts.clear()`).
  - `bool deepSleepSupersedesRestart();` in `SilentRestart.h`, defined in `main.cpp` as `return deepSleepInProgress;`.
  - `void wifi_session::recordBootBaseline();` and `wifi_session::Exit wifi_session::end(const char* tag);`

- [ ] **Step 1: Add the capability.** Its comment says: the C3s gave up the secondary framebuffer (nothing
  reallocates it) and a full stop and deinit still left ~20.5 KB below the pre-session heap; the PSRAM boards gave
  up none and keep going when the minimums pass (`src/network/WifiSessionEnd.h`).
- [ ] **Step 2: Add `forgetUnverifiableHosts()` and `deepSleepSupersedesRestart()`.**
- [ ] **Step 3: Write `WifiSessionEnd.{h,cpp}`.** `end()` does, in order:
  1. If `deepSleepSupersedesRestart()`: return `Reboot`, nothing torn down.
  2. `#if CROSSPOINT_KOREADER_AUTOSYNC`: if `KOReaderSyncWorker::isBusy()`, `LOG_INF` it and return `Reboot`,
     nothing torn down (deinit under a job's socket crashes, `main.cpp:813`).
  3. If `WiFi.getMode() != WIFI_MODE_NULL`: for AP or APSTA `WiFi.softAPdisconnect(true)` first, then
     `HalClock::wifiOff(/*skipNtpSync=*/true)`. `getMode()` reads NULL only after `WiFi.mode(WIFI_OFF)` or when
     Wi-Fi never started (Arduino `WiFiGeneric.cpp:364-377,715-725`); a bare `esp_wifi_stop()` still reads STA, so
     this covers it and skips the 200 ms when the radio is already off.
  4. `HttpDownloader::forgetUnverifiableHosts()`.
  5. Read free and largest internal; `verdict(HalCapabilities::restartsAfterWifiSession(), free, largest)`.
  6. One log line: `Session end [<tag>]: internal free=%u largest=%u (boot %u/%u, min %u/%u) -> carry on|reboot`.
     `LOG_ERR` when the capability allowed carrying on and the minimums refused it, else `LOG_INF`.
  `recordBootBaseline()` stores the two internal figures in file-scope statics (no allocation).
- [ ] **Step 4: Call `wifi_session::recordBootBaseline()`** in `setup()` right after `logStartupMemory("setup_complete")`.
- [ ] **Step 5: `STATE`**: after `maxblock`, when `HalCapabilities::keepsSecondaryBuffer()`, append `imaxblock` =
  largest internal block. Add the row to `docs/serial-test-api.md` (`heap` is already internal there:
  `ESP.getFreeHeap()` is `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`).
- [ ] **Step 6: Comment**: `NetworkMemoryTrim.h:30-33` says every caller ends through `wifi_session::end()`, which
  reboots on the boards where this released the buffer.
- [ ] **Step 7: Build.** Run (PowerShell): `$env:PLATFORMIO_CORE_DIR='C:\pio'; $env:PYTHONUTF8='1'; & 'C:\pio\penv\Scripts\pio.exe' run -e default`
  Expected: `SUCCESS`; note the `RAM:` and `Flash:` lines.
- [ ] **Step 8: Commit** `feat(network): end a wi-fi session in one step that decides whether to reboot`

### Task 3: KOReader sync and the account check

**Files:**
- Modify: `src/activities/reader/KOReaderSyncActivity.cpp` (`logSyncMemSnapshot` ~line 47, `onExit` ~746-801,
  comments at ~328, ~624, ~644, ~1114-1117), `src/activities/reader/KOReaderSyncActivity.h:159-166`
- Modify: `src/activities/settings/KOReaderAuthActivity.cpp:146-160`
- Modify: `src/activities/settings/KOReaderSettingsActivity.{h,cpp}` (~line 193-211)

**Interfaces:**
- Consumes: `wifi_session::end`, `HalCapabilities::restartsAfterWifiSession()`.
- Reference: `git show perf/kosync-no-reboot-psram` (bbb6d5bdc) has these edits written against
  `restartsAfterSyncSession()`; take its `openAccountCheck()` and `logSyncMemSnapshot` internal figures, not its
  `onExit` early return.

- [ ] **Step 1: `KOReaderSyncActivity::onExit()`**: inside `if (wifiActivated)`, replace `WiFi.disconnect(false); delay(30);`
  with `if (wifi_session::end("KOSync") == wifi_session::Exit::CarryOn) return;` before the `switch`. The
  measured-decision comment stays and is scoped to the C3s. On carry-on the navigation `resumeReader()` queued
  (`KOReaderSyncActivity.cpp:862-875`) runs.
- [ ] **Step 2: Comments**: "full teardown happens at silent reboot" (three places) and the `dropRadioForRemoteMapping`
  one say the session end does it; `KOReaderSyncActivity.h:159-162` no longer claims `esp_wifi_stop()` makes
  `getMode()` NULL.
- [ ] **Step 3: `KOReaderSettingsActivity::openAccountCheck(Mode)`** as in bbb6d5bdc, gated on
  `!HalCapabilities::restartsAfterWifiSession()` (push with a result handler that calls `requestUpdate()`), else the
  existing `replaceActivity()` with its comment.
- [ ] **Step 4: `KOReaderAuthActivity::onExit()`**: `if (wifi_session::end("KOAuth") == wifi_session::Exit::CarryOn) return;`
  then `silentRestartToKOReaderSettings();`. The comment says the C3s reboot on every exit because `onEnter()`
  gave up the secondary buffer.
- [ ] **Step 5: Build `default`.** Expected `SUCCESS`.
- [ ] **Step 6: Commit** `perf(kosync): no restart after a sync or account check where the heap allows` (body credits PR #855, trungtypo).

### Task 4: The clock screens, Weather and the Settings actions

**Files:**
- Modify: `src/activities/settings/DetectTimezoneActivity.cpp:151-157`, `src/activities/settings/SyncTimeActivity.cpp:72-80`
- Modify: `src/activities/weather/WeatherActivity.cpp:121-135` and the comment at ~215
- Modify: `src/activities/settings/SettingActionDispatch.{h,cpp}:128-134`, `src/activities/settings/SettingsActivity.cpp:312,322`,
  `src/activities/settings/SettingsSubmenuActivity.cpp:36`

**Interfaces:**
- Consumes: `wifi_session::end`.
- Produces: `void endRadioLeftOnBySettingAction();` (renamed from `restartToSettingsIfRadioLeftOn()`, which no
  longer always restarts).

- [ ] **Step 1: The clock screens**: keep their `HalClock::wifiOff()` calls; inside `if (wifiWasUsed_)` call
  `silentRestartToClockSettings()` only when `wifi_session::end("TZ")` / `end("NTP")` returns `Reboot`. The
  SyncTime comment ("Turning the radio off does not give the heap back") is scoped to the C3s.
- [ ] **Step 2: Weather**: inside `if (WiFi.getMode() != WIFI_MODE_NULL)`, `silentRestart()` only on `Reboot` from
  `end("WEATHER")`; drop the `disconnect`/`delay`. The "silent reboot handles teardown" comment says the session end does.
- [ ] **Step 3: Settings actions**: the renamed function returns early when the mode is NULL, then reboots to
  Settings only on `Reboot` from `end("SET")`; update its three callers and the header comment.
- [ ] **Step 4: Build `default`.** Expected `SUCCESS`.
- [ ] **Step 5: Commit** `perf(settings): no restart after the clock, weather and network actions where the heap allows`

### Task 5: The OPDS browser and the File Transfer mode picker

**Files:**
- Modify: `src/activities/browser/OpdsBookBrowserActivity.cpp:306-327` and the comment at ~1206
- Modify: `src/activities/network/NetworkModeSelectionActivity.cpp:55-96`

**Interfaces:**
- Consumes: `wifi_session::end`.

- [ ] **Step 1: OPDS**: inside `if (WiFi.getMode() != WIFI_MODE_NULL)`, `return` on `CarryOn` from `end("OPDS")`
  (the queued `leave()` navigation runs), else the two restarts as today. The "restart below, which does not
  return" comment says it may return now.
- [ ] **Step 2: The mode picker's `handOff()`**: with the radio up, call `end("NETMODE")` once; on `CarryOn` take
  the radio-down branch (`goToBrowser()` / `goToSerialTransfer()` / `goToUsbDrive()`), on `Reboot` the
  `silentRestartTo*()` as today. The parent's `ServerSession::end()` then finds the mode NULL and returns
  (`ServerSession.cpp:106-107`). Rewrite the comment above `handOff()` accordingly.
- [ ] **Step 3: Build `default` and `gh_release`.** Expected `SUCCESS` for both; record `RAM:`/`Flash:` against a
  master build in the same worktree.
- [ ] **Step 4: Commit** `perf(network): no restart after the opds browser or a backed-out join where the heap allows`

### Task 6: Verification on the Picco, docs, PR

**Files:**
- Modify: `docs/network-stack-findings.md` (a "Wi-Fi session end" entry with the figures)
- Modify: the spec's evidence table if the figures differ from the T5S3's

- [ ] **Step 1: Host suite.** Run (Bash) the full suite: `cmake --build build/test -- -k 0 && ctest --test-dir build/test --output-on-failure -j4`.
  Expected: the Windows baseline (25 known EPUB-pipeline failures, ledger `fail-item1.txt`) plus
  `WifiSessionVerdictTest` passing; re-run any other failure with `-j1`.
- [ ] **Step 2: Flash the Picco.** `ListAgents` and `CMD:STATE` first. Build `picco`, flash per Global Constraints,
  copy the bundle with devctl `put:`, check `STATE` shows Home after boot.
- [ ] **Step 3: Every site once**, with `--log`: KOReader sync from a book (back in the book, same boot: uptime keeps
  rising), the account check (back on KOReader settings with Settings below), Detect Timezone and Sync Time (back
  on Clock settings), Weather (Home), Wi-Fi Networks (Settings, same tab), OPDS (File Transfer, OPDS row), a
  backed-out join then OPDS from the mode picker. Each logs `Session end [...] -> carry on`.
- [ ] **Step 4: Five sessions in a row** (KOReader sync, which opens TLS), then Weather: record `heap`/`imaxblock`
  before the radio, the log line after each; the Weather join after them must finish.
- [ ] **Step 5: A background sync in flight**: start a reader page-turn push (interval push), exit a Wi-Fi
  screen while the worker is busy: the log shows the busy line and the device restarts to the site's target, no
  panic in `BOOTLOG`.
- [ ] **Step 6: Sleep from a Wi-Fi screen** (power button on Weather): it sleeps; on wake, no restart loop.
- [ ] **Step 7: The minimums refusing**: a local, uncommitted `picco` build with `kMinFreeInternalBytes` set to
  500000, one Weather exit: `LOG_ERR` line, restart to Home. Rebuild and re-flash the real figures afterwards.
- [ ] **Step 8: Docs** with the measured figures; raise the minimums in Task 1's header (and its test) if any
  step needed more. Commit `docs(network): measured wi-fi session ends on the picco`.
- [ ] **Step 9: PR.** Format check (`bin/clang-format-fix -g`), push, open the PR (what, why, how verified, cost,
  open points; under 4 KB), `gh pr checks`. Leave the Picco on Home and say which build is on it.
