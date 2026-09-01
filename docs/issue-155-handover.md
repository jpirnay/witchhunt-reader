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

### 1.7 The book open runs inside `setup()` on a reader resume

`ActivityManager::replaceActivity()` calls `onEnter()` **inline** when `currentActivity` is
null. The `BootResume::ReaderResume` branch creates no activity, so `currentActivity` is
null when `activityManager.goToReader(path)` runs in `setup()`. The whole book open —
`Epub::load()`, section build, first render — therefore executes inside `setup()`.

Corroborated by the field data: every captured boot trace shows `route+0`, and none of the
captures is a reader-resume boot (the healthy one is `sw-restart` → Home; the recovery boot
is routed to Home by `readerActivityLoadCount > 0`).

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

### 4.3 Duplicate PR

**#217 is a duplicate of #216.** Identical content — the tree diffs against master have the
same `git patch-id` (`aeec6ae1436177d7c2ba5a1b77ec40627f40105c`). It exists because commits
kept being pushed to `fix/155-sleep-wake-hang` after #213 merged it. #217 should be closed
and that branch deleted.

`origin/master` has since advanced (#219, #220). #216 still reports `mergeable=true`.

---

## 5. Next step

The one thing that would move this forward is a field capture from a build containing §4.2,
showing either:

- a `BOOT-STALL at <phase> after <n>s` history line — naming where in `setup()` it stopped; or
- a `STALLED in <phase> after <n>s working|NO-TICKS` line — naming where in the book open,
  and whether it was progressing.

Neither has been captured yet. Both were only made reachable by commit `8f1710896`.

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
