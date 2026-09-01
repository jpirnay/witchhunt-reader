# Issue #155 — "Stuck at Sleep Screen": handover

Status as of 2026-09-01. **The root cause is not identified.** This document separates what
is established from what is not, because several confident explanations in this issue's
history turned out to be wrong and were corrected only by field data.

Reporters: `hamidmala17`, `jesusmabas`. Symptom as reported: after the device has been on
the sleep screen, a normal Power press does not start it; recovery requires a brief Reset
press followed by holding Power.

---

## 1. Established facts

Everything in this section is read directly from code, from IDF sources, or from reporter
screenshots. Nothing here is inference.

### 1.1 Hardware

Both reporters are on the same variant, read off their Boot Diagnostics screens:

| | |
|---|---|
| Board profile | `xteink_x4` |
| Panel controller | `SSD1677` |
| Panel probe | no response (all-`FF`) on both |

The panel probe returning all-`FF` is normal for X4 — the controller does not answer that
probe there, and `BoardConfig::ACTIVE.displayController` keeps the profile default.

### 1.2 The device boots during the failure

Every failure capture shows the same three records around the fault, read oldest-first:

```
sleep  … panel-asleep  <trigger>       ← the sleep
boot   power-on  long-hold  sd@…       ← a boot that reached the SD mount
boot   power-on  long-hold  sd@…       ← the reporter's Reset + Power
```

A boot record is written by `BootDiag::persistBoot()`, called immediately after
`Storage.begin()` succeeds. Its existence therefore proves that boot powered up, ran the
wake gate, and mounted the card. The gate value on those records is `long-hold`, i.e.
**accepted**.

Seen in: jesusmabas ×3 occurrences, hamidmala17 ×1.

### 1.3 The failing sleep was from an open book

jesusmabas' capture of 2026-09-01 17:06, history entry 27:

```
27 sleep off panel-asleep timeout bo…
```

The trailing token is `book` — `BootDiagRing.h`'s `kFlagFromReader`. This is the first
direct evidence of that flag's value in a failure; before this build it was recorded but
never displayed.

### 1.4 The ESP32-C3 cannot distinguish a reset press from a power-on

`components/esp_system/port/soc/esp32c3/reset_reason.c` has **no `ESP_RST_EXT` case at
all**. `RESET_REASON_CHIP_POWER_ON` covers both a real power-on and a CHIP_PU (EN pin)
reset, and both surface as `ESP_RST_POWERON`. Verified in the IDF 5.5.2 source used by this
build.

### 1.5 The deep-sleep GPIO wake arming is correct

`gpio_deep_sleep_wakeup_prepare()` enables the pull-up on GPIO3 and calls `gpio_hold_en()`
on it *before* `esp_sleep_isolate_digital_gpio()` runs, so the isolate sweep skips it
(`gpio_hal_is_digital_io_hold`). `esp_deep_sleep_wakeup_io_reset()` releases the hold in
`cpu_start.c` on a `RESET_REASON_CORE_DEEP_SLEEP`. Verified in IDF source.

### 1.6 The reader-resume path paints nothing

`src/main.cpp`: `showBootScreen = false` when `lastSleepFromReader && !openEpubPath.empty()`.
The `BootResume::ReaderResume` branch states it outright: *"Deliberately paints nothing: the
panel keeps physically showing the sleep screen until the reader's own first render lands."*
No splash, no timeout, and (before this branch) no progress on that path.

### 1.7 The EPUB load runs inside `setup()` on a reader resume

`ActivityManager::replaceActivity()` calls `onEnter()` **inline** when `currentActivity` is
null. The `BootResume::ReaderResume` branch creates no activity, so `currentActivity` is
null when `activityManager.goToReader(path)` runs in `setup()`. `ReaderActivity::onEnter()`
therefore runs synchronously and calls `Epub::load()` inside `setup()`. It then queues
`EpubReaderActivity`; that activity's `onEnter()`, section construction, first render, and
panel refresh run later through `ActivityManager::loop()` and the render task.

The field data neither proves nor disproves that the load was reached: every captured boot
trace shows `route+0`, but the trace stops at activity routing and carries no post-route
milestone.

### 1.8 `readerActivityLoadCount` already routes a failed resume to Home

The resume branch clears `APP_STATE.openEpubPath` and increments
`readerActivityLoadCount` **before** opening the book; only
`EpubReaderActivity::onExit()` clears it. A reader that never exits therefore leaves it
set, and `setup()`'s routing sends the following boot to Home. This is why the reporters'
recovery boot works.

### 1.9 Measurements from healthy boots (jesusmabas, build `2.26-dev+0209e40`)

```
entry+105 nvs+1 gate+0   hw+70 sd+19  cfg+95 disp+136 paint+1721 store+21 route+0 rel+209
entry+28  nvs+1 gate+279 hw+70 recov+62 sd+161 cfg+97 disp+139 paint+1722 store+21 route+0 rel+209
```

To first paint: 2147 ms and 2559 ms. Aborted boots lifetime counter: 2.

### 1.10 Local X4 control captures with the NVS marker build

Two healthy captures on 2026-09-01 produced no `STALLED` or `BOOT-STALL` record.
The reader-resume capture showed:

```
boot  deep-sleep  long-hold
route+138  setup=2297
```

The synchronous reader route, including `Epub::load()`, returned in 138 ms and setup
completed. The history also contained one `ABORTED ... no-second-press` immediately before
the accepted boot: the first wake gesture was rejected and went back to sleep, then the
next long hold resumed the reader normally. This is the healthy control shape, not issue
#155.

That sequence exposed two diagnostics artifacts:

- `previous=unknown` stopped at the intervening abort summary instead of scanning through
  it to the sleep boundary. The classifier now uses the already-loaded record list, skips
  abort summaries, and is host-tested for this exact ordering.
- Older builds rendered IDF 5.5 reset reasons 11-15 as `other`. The screen now names USB,
  JTAG, eFuse, power-glitch, and CPU-lockup resets and includes the raw value in serial.

The older `sleep open panel-asleep power-hold book` record in that capture is a third,
still-unresolved presentation artifact: an early rejected-wake sleep breadcrumb is appended
on the next accepted boot without finalising the preceding on-card sleep record. It does not
mean the reader remained stuck; subsequent wake code and the successful reader route prove
that session ended. Do not infer its final sleep outcome from the later wake's reset reason,
because that can misclassify latch-kept configurations.

---

## 2. Ruled out

Each of these was checked and found not to be the cause.

| Hypothesis | How it was eliminated |
|---|---|
| Battery latch (GPIO13) asserted too late in boot | Field captures show the device boots and reaches the SD mount. Stated publicly on the issue and since retracted. |
| `createSectionFile`'s `while (true)` loop spinning | The `RetryNoCss` retry sets `embeddedStyle = false` in the build state; the guard is `else if (st.params.embeddedStyle)` (`Section.cpp:1289`), so the second pass cannot re-enter. Bounded to two iterations. |
| A loop in the reader activity | `EpubReaderActivity` contains exactly one `while` — the button-event drain. |
| A reboot loop | A heap-recovery restart produces `sw-restart`. Both boots in every failure window are `power-on`. |
| Aborted-boot loop (wake, refuse, sleep, repeat) | `Aborted boots` reads `none` in every failure capture, with a non-zero lifetime counter proving the counter works. |
| X3/X4 being two different bugs | Both reporters are `xteink_x4` / `SSD1677`. |

---

## 3. Not known

- **Where the boot stops.** The boot record is written at the SD mount and was never
  updated afterwards, so a boot that hung later is indistinguishable from one that
  completed. This is the decisive missing datum.
- **Whether the device is slow or wedged.** No capture yet carries the liveness signal.
- **Whether the reader open is even reached.** Section 1.7 establishes it *would* run inside
  `setup()`; nothing establishes that it started in a failing boot.
- **Why hamidmala17 correlates the fault with the Embedded CSS setting.** He has reported it
  with the setting both off (2026-08-28) and on (2026-08-31). No mechanism is established.
- **Whether any change on the open branch affects the fault.** Nothing on it has run on a
  device exhibiting the failure.
- **What could wedge an SD transfer.** Arduino-ESP32's C3 SPI HAL has unbounded waits on the
  peripheral's `cmd.update` and `cmd.usr` bits below SdFat's protocol deadlines. That gives a
  concrete place where an SD call can remain blocked forever while holding `storageMutex`,
  but no panel/card interleaving or peripheral-state transition that causes either bit to
  stick has been demonstrated.

---

## 4. What was changed

### 4.1 Merged — PR #213 (merge commit `a7d1e3589`, 2026-08-31)

Power-path fixes, none of which is known to address the reported fault:

- `holdPowerRails()` called as the first statement of `setup()`. It was never called
  directly; GPIO13 was first driven HIGH inside `gpio.begin()`. BoardConfig documents an X4
  revision that does not self-latch.
- `waitForStablePowerRelease()` bounded at 5 s. It was `while (true)` with no exit, run
  with the input sampler stopped and the panel already asleep; the loop task is not
  subscribed to the task WDT (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0` unset).
- The AfterUSBPower and wake-gate-rejected paths now sleep under the same
  `keepClockAliveForSleep()` policy as `enterDeepSleep()` and call
  `HalClock::saveBeforeSleep()`. They previously took `startDeepSleep()`'s
  `keepClockAlive=false` default.
- `gpio_hold_dis()` before driving GPIO13 in `lightSleep()`, and on both branches of
  `startDeepSleep()` (X3's SD rail could not be driven LOW).

Plus the Boot Diagnostics feature: `BootDiagnostics` (RTC breadcrumb + 16-entry SD ring at
`/.crosspoint/bootdiag.bin`), `BootDiagRing.h` (pure record/ring logic, host-tested),
`BootDiagnosticsActivity` (Settings → System → Boot Diagnostics), and hardware-variant
reporting.

### 4.2 Open — PR #216, branch `fix/155-resume-liveness` (7 commits, 25 files, +603/−25)

- Five diagnostics corrections found by reading reporter photos: stale `Wake cause` on
  non-deep-sleep boots; `kFlagFromReader` never displayed; `Previous session` asserting a
  value with fewer than two records; raw `FF FF FF FF FF` panel probe; truncated history
  lines. Also fixed an aborted-boot history line that was never applied in #213 and rendered
  abort records through the boot branch.
- Progress feedback: first hint at 2 s, then a liveness update every 15 s naming the stage
  and progress. Covers `Epub::load()` (via new `LongTaskProgress`, a sibling to
  `CooperativeAbort` with the same injection direction) and the section build (the blocking
  build passed `nullptr` as its progress callback; the parser also *zeroed* progress ticks on
  low heap, which is now thinned to 50% rather than silenced).
- Liveness recording at every `CooperativeAbort::shouldAbortLongTask()` call site, giving a
  `still ticking` / `NO-TICKS` distinction. Those sites cannot paint: they are mid-write into
  the framebuffer and `drawPopup()` resyncs the write buffer from the displayed frame.
- Stall reporter task. **It initially could not fire for the likeliest case** — it was
  created at the end of `setup()`, after the routing, and by §1.7 the book open runs inside
  `setup()`. Now started immediately after `persistBoot()`, plus a second watch for `setup()`
  itself not completing, recorded with the furthest `BootPhase`.
- An earlier commit on this branch restarted to Home after 20 s; a later commit removed that
  deliberately. Nothing acts on a stall now — it is recorded only.
- The stall marker no longer writes directly to the SD ring. Every lock-taking `HalStorage`
  and `HalFile` call publishes a fixed-size snapshot (`open`, `read`, `write`, `seek`, etc.;
  waiting for shared SPI, waiting for the storage mutex, or active). At 20 s the reporter
  writes the phase, liveness, storage state, and operation age to one NVS slot without taking
  either storage lock. The next boot that mounts the card drains it into the existing ring.
- Shared SPI is now fail-closed. `HalSpiBus::Lock` previously timed out after 5 s and let the
  caller continue without owning the bus, which could turn legitimate contention into
  concurrent panel/card traffic. It now waits on the recursive mutex and publishes a
  lock-free snapshot naming the active or waiting operation (`storage`, `display-refresh`,
  `display-sleep`, etc.). The NVS stall marker also stores its age and the raw EPD BUSY level
  in the existing 16-byte record; no ring migration or extra marker allocation is required.

### 4.3 Duplicate PR

**#217 is a duplicate of #216.** Identical content — the tree diffs against master have the
same `git patch-id` (`aeec6ae1436177d7c2ba5a1b77ec40627f40105c`). It exists because commits
kept being pushed to `fix/155-sleep-wake-hang` after #213 merged it. #217 should be closed
and that branch deleted.

`origin/master` has since advanced (#219, #220). #216 still reports `mergeable=true`.

---

## 5. Next field check

The one thing that would move this forward is a field capture from a build containing the
NVS stall marker, after reproducing the fault, pressing Reset, and opening Boot Diagnostics.
The history line now ends with one of:

| Marker | Interpretation |
|---|---|
| `spi storage/active <n>s` with `sd read/active <n>s` | Strongly supports a call wedged below `HalStorage`, including the low-level SPI theory. |
| `spi display-refresh/active <n>s busy=1` | Panel controller is still signalling BUSY while the refresh owns the bus. Investigate the X4 BUSY/power path. |
| `spi display-refresh/active <n>s busy=0` | Display call retained the bus after BUSY deasserted; investigate post-waveform SPI work or bookkeeping. |
| `spi <op>/waiting <n>s` | The named operation is waiting behind the active owner. The snapshot prefers the active owner when one exists, so this should mainly appear during an acquisition handoff. |
| `sd <op>/wait-spi <n>s` | Storage is blocked behind the SPI owner named earlier on the same line. |
| `sd <op>/wait-mutex <n>s` | Storage serialization is blocked, though the active-operation snapshot should normally identify its owner instead. |
| `sd idle` with `NO-TICKS` | Falsifies this storage-operation theory for that occurrence; follow the recorded wake phase into parser/render code. |
| `sd idle` with `working` | Slow work rather than an uninterruptible SD wedge; optimize the recorded phase. |
| `spi untracked` or `sd untracked` | Record came from an older build and carries no evidence for that subsystem. |

No marker after a known 20-second failure is also evidence: on the single-core C3 it means
the reporter task did not get scheduled, NVS failed, or execution stopped before either the
book-open or generic setup watchdog could classify it.

### 5.1 What a fix would look like

- **Confirmed active SD/SPI wedge:** first ship bounded recovery: persist the NVS marker,
  restart, and let `readerActivityLoadCount` route to Home. The root repair is a bounded
  low-level SPI transfer with card/bus reinitialisation after timeout; a storage-mutex timeout
  alone cannot release a task already spinning inside the peripheral transfer.
- **Confirmed shared-SPI wait:** use the recorded owner to fix the specific display path
  holding the bus. The unsafe
  timeout-to-unlocked behavior has already been removed; waiting now preserves serialization
  while the independent reporter records the owner.
- **Idle or still progressing:** do not apply SPI recovery. Fix or optimize the phase named by
  the marker instead.

The restart is a user-recovery fix, not proof of root cause. It should follow marker capture,
not replace it, or the firmware will hide the distinction this instrumentation was added to
measure.

### 5.2 Reporter test protocol

Ask an affected X4 owner to use this order. The first capture must happen before changing the
card or clearing caches, otherwise the most useful comparison is lost.

1. Back up the original card, including hidden files and the `/.crosspoint` directory. Leave
  the card itself unchanged.
2. Flash the diagnostic firmware and retry the exact reported flow on the original card:
  open the implicated book, sleep from the reader, then wake back into it. Keep Embedded CSS
  at the value used when the fault was seen. Try ten sleep/wake cycles if it does not fail
  immediately.
3. If the sleep screen remains for 20 seconds, wait at least 10 more seconds so the NVS marker
  is committed, then press Reset once. Do not power-cycle, remove the card, clear cache, or
  make repeated reset attempts before collecting the evidence.
4. Open Settings → System → Boot Diagnostics. Send a photo of the whole screen and copy the
  complete diagnostic block from serial. Also report whether the book eventually opened,
  card brand/model/capacity, Embedded CSS setting, and which sleep gesture was used. A
  no-failure result after ten cycles is useful and should still be reported.
5. Power off and insert a different known-good card, freshly formatted as FAT32. Make a
  file-level copy of the original card, including hidden files and `/.crosspoint`, but omit
  `/.crosspoint/bootdiag.bin` on the destination so histories cannot be confused. Do not use
  a sector-by-sector clone. Repeat the same ten-cycle test without changing settings.
6. If the two cards behave differently, preserve both cards in that state. Only after both
  diagnostic captures exist, use the firmware's Clear Cache action on the failing card and
  repeat. Failure following the physical card after a cache clear supports card, filesystem,
  or power integrity; failure disappearing after the clear supports generated cache state.

The replacement-card test is not proof by itself: a file copy changes FAT allocation and
physical sectors as well as the card. It is a discriminator. Reproducing on both cards points
back toward content, cache, display, or firmware timing; reproducing only on the original
pushes card/media/power behavior higher on the list.

---

## 6. Verification

```
pio run -e default -e gh_release -e slim     # x4pro FAILS — pre-existing on master, unrelated
cd test/build_test && cmake --build . -j4 && ctest -j4      # 635 tests
pio check -e default                          # grep the output for defect lines; the
                                              # summary says PASSED regardless
```

The `x4pro` env does not build on master either (`RvExcFrame`, and the C3-only
`esp_deep_sleep_enable_gpio_wakeup`, both used unguarded on the S3).
