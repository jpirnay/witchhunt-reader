# Multi-board bring-up — handover, 2026-08-15

*State: branch `fix/s3-build-config`, 5 commits ahead of `master`, **not pushed**.*

Targets: **Xteink X4 Pro** (lead) and **LilyGo T5S3**, both ESP32-S3 (Xtensa).
Shipped product is X3/X4 (ESP32-C3, RISC-V) and must not regress.

Plan: [multi-board-bringup-2026-08-14.md](multi-board-bringup-2026-08-14.md).
Touch (workstream C): [touch-input-migration-2026-08-14.md](touch-input-migration-2026-08-14.md).

Everything below is build-measured unless it says otherwise. **Nothing has been
run on hardware.** Both boards are on the desk; neither has been flashed.

---

## Landed

```
59189129  docs: bring the bring-up plan in line with what shipped
8803b7a0  chore: bump freeink-sdk 56efd2e -> 76e61c4
02c324cc  fix: split arch-specific HAL code so the S3 boards link
b4b94068  fix: split build flags per MCU family so S3 envs build
f666334b  docs: plan multi-board bring-up for X4 Pro and LilyGo T5S3
```

**b4b94068 — workstream A.** `[base]` carried the C3 device set
(`FREEINK_DEVICE_X4/X3`), the X4 SPI overclock, and `WOLFSSL_SP_RISCV32`; every
env inherits `${base.build_flags}`, so no S3 env could build. Moved into new
`[c3]` / `[s3]` interpolation sections; each env now opts into exactly one MCU
family. Added `[env:x4pro]`.

**02c324cc — the two B link blockers.** Both C3/RISC-V assumptions in `lib/hal`:

- `HalPowerManager.cpp` — deep-sleep wakeup. Now branches on **SoC capability**
  (`SOC_PM_SUPPORT_EXT1_WAKEUP` vs `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP`), not chip
  name, with an `#error` default.
- `HalSystem.cpp` — `__wrap_panic_print_backtrace` walked a RISC-V `RvExcFrame`.
  Only the SP extraction is arch-specific; Xtensa keeps it in `XtExcFrame::a1`.
  Gated on `__riscv` / `__XTENSA__`.

**8803b7a0 — SDK bump** `56efd2e` → `76e61c4` (submodule pointer), for SDK PR #39
so `FREEINK_FRONTLIGHT_LS` is available later.

---

## Verified state

| Env | Result | RAM | Flash |
|---|---|---|---|
| `default` (C3) | **SUCCESS** 786 s | 56,476 | 6,376,923 — **97.3 %** |
| `x4pro` | **SUCCESS** 766 s | 66,408 | 6,198,074 — **94.6 %** |
| `lilygo_t5s3` | **NOT BUILT SINCE THE FIX** | — | — |

X4 Pro links — the first S3 build ever to do so in this repo. It would **not
boot**: none of the pin/peripheral de-hardcoding is done (see next section).

**C3 delta: +94 bytes flash, RAM unchanged.** Attributed to the SDK bump, not the
HAL fix — SDK #39 adds 14 lines to the shared
`libs/display/FreeInkDisplay/src/bus/EpdBus.cpp`, which the C3 links, while both
HAL changes compile the identical C3 arm. **This is inference from the diff, not
an A/B measurement.** If it ever matters, build `02c324cc` and `8803b7a0`
separately.

**Flash is the binding constraint, not RAM.** ~177 KB free on C3, ~355 KB on
x4pro (the S3 carries one board profile; the C3 links both SSD1677 and UC8253).
Any *shared* addition is charged against the C3's 177 KB.

---

## Pick up here — B0, the board concept

The single most important idea in this work, and the reason not to write X4 Pro
code first.

`HalGPIO::DeviceType { X4, X3 }` + `deviceIsX3()` has **49 call sites**, and
`deviceIsX3()` is a stand-in for six unrelated questions: hardware-RTC presence,
RTC-survives-deep-sleep, "has a DS3231 at 0x68", screen-tall-enough-for-a-hint,
physical-front-button-positions, and panel-needs-half-refresh-settle.

**On X4 Pro `deviceIsX3()` returns `false`, so every one of those silently takes
the X4 branch.** The board will not fail loudly; it will be subtly wrong in six
places that each look like an unrelated bug. Widening the enum to
`{X3, X4, X4Pro, LilyGo}` multiplies the conflation by four.

**Fix: capability predicates over `BoardConfig::ACTIVE`** — ask what the board can
do, not what it is:

| Predicate | Source in `BoardProfile` |
|---|---|
| `hasHardwareRtc()` | `sensors.rtcType != RtcType::None` |
| `rtcType()` | `sensors.rtcType` (`Ds3231` / `Pcf8563` / `Rx8130`) |
| `hasTouch()` | `touch.controller != TouchController::None` |
| `hasFrontlight()` / `hasColorTemperature()` | `frontlight` / `frontlight.gpioWarm` |
| `hasPhysicalButtons()` / topology | `inputStyle` |
| `uiScale()` | `uiScale` |
| battery source | `batteryGauge.gaugeAddr != 0` vs `batteryAdc` |

Legitimate exception: genuine silicon quirks (the X3 half-refresh settle passes)
key on `ACTIVE.displayController` **with a comment saying why**.

**Order:** (1) add the accessors — pure additions, no call-site churn, C3 should
come out byte-identical, which makes a clean gate; (2) convert only the ~15 sites
where X4 Pro's answer differs from X4's; (3) rename the residue to say "panel".

Fold in the deliberate follow-up from `02c324cc`: the deep-sleep **wake level is
hardcoded LOW** and should read `BoardConfig::ACTIVE.input.powerActiveHigh`. Left
out on purpose — every board we build is active-LOW, so it was behaviour risk to
the shipped C3 for no present gain.

## Then B — de-hardcode the HAL (the real work)

Each must read `BoardConfig::ACTIVE` instead of the C3 macros in `HalGPIO.h:9-44`:

| Site | Current | Needs |
|---|---|---|
| `HalDisplay.cpp:17` | `EInkDisplay(EPD_SCLK, EPD_MOSI, …)` | board display pins |
| `HalGPIO.cpp:150` | `SPI.begin(EPD_SCLK, SPI_MISO, …)` | board SPI pins |
| `HalGPIO.cpp:153-154` | `pinMode(BAT_GPIO0)`, `pinMode(UART0_RXD)` | board battery / USB detect |
| `HalGPIO.cpp:531` | `digitalRead(UART0_RXD)` | per-board USB detection |
| `HalPowerManager.cpp:24` | `Wire.begin(X3_I2C_SDA, …)` | board I2C bus(es) |
| `HalPowerManager.cpp:344` | `BatteryMonitor(BAT_GPIO0)` | X4 Pro has a CW2017 gauge |
| `HalClock.cpp:19` | `DS3231_ADDRESS = 0x68` | X4 Pro is BM8563 @ 0x51 |

X4 Pro also mounts SD over **SDMMC**, not SPI (`USE_BLOCK_DEVICE_INTERFACE=1` is
already in its env) — that reaches into `HalStorage`.

**Milestone to aim at: X4 Pro boots, mounts SD, renders a page.** Nothing else.

---

## Open decisions

1. **PSRAM.** Both S3 boards have 8 MB; neither env enables it (deliberate).
   Recommendation in the plan: enable but *don't spend* — divergent allocation
   policy per board would make every heap bug board-specific and strand the
   accumulated C3 heap knowledge.
2. **Touch P1 — who services the I2C?** Our `HalGPIO` runs a background
   `btnsample` task (prio 2, 2 KB stack, 10 ms) that calls `inputMgr.update()`;
   on a touch board that silently puts GT911 I2C on that task, while `HalClock`
   drives the RTC over `Wire` from the loop task. **Upstream has no sampler at
   all**, so there is nothing to copy. Preferred: keep touch off the sampler and
   rely on the SDK's `popTouchTap`/`popSwipe` queues; fallback is a HAL I2C mutex.
3. **FUI adoption** (touch doc §3). Upstream replaced `MenuListActivity` with
   `UiListActivity` on FreeInkUI and their touch stack sits on top of it. It is a
   per-screen opt-in mixin, so incremental — but 34 of our 76 activities have no
   upstream counterpart. Explicitly *not* blocking board support.
4. **`FREEINK_FRONTLIGHT_LS`** — port with the frontlight work, or earlier?
5. `papermono` / `sticky` in scope at all?

---

## Gotchas that cost time

- **`pio run ... | tail` reports `tail`'s exit code.** A failed build shows
  "exited with code 0". Always grep the output for `SUCCESS` / `FAILED`, or don't
  pipe.
- **`BoardConfig.h:88` hard-`#error`s on mixed MCU families.** This is a feature —
  an S3 binary cannot silently carry C3 profiles — but it means a misconfigured
  env fails at the *first* SDK translation unit with a confusing message.
- **X4 Pro + T5S3 *can* share a binary** (both `FREEINK_MCU_S3`); C3 and S3 never
  can. Whether a combined S3 binary is affordable is a flash question, still open.
- **Don't reason about flash from "text size".** The 2.22 MB figure in older notes
  is text only; the linked image is 6.38 MB of a 6.55 MB partition. Quote the
  `Flash:` line from a real build.
- **The SDK is a git submodule and moves fast.** Bump it in its own commit with
  its own C3 regression build; never fold it into board work.
- **A stale in-flight build is worthless after an SDK pull.** Kill and restart —
  it will otherwise mix old and new headers.

## Verification recipe

```bash
PIO="$HOME/.platformio/penv/Scripts/pio.exe"   # not on PATH
"$PIO" run -e default      # C3 regression gate — MUST stay green
"$PIO" run -e x4pro
"$PIO" run -e lilygo_t5s3  # never built since workstream A
```

Cold builds are ~13 min each; run them backgrounded. Gate for any B0/B change is
**zero flash/RAM delta on `env:default`** — B0 in particular should be
byte-identical, since it only adds unused accessors.

## Reference

- Upstream touch/board branch: `upstream/feat-x4-papermono-support`
  (crosspoint-reader). Their `HalGPIO` touch passthrough and
  `MappedInputManager(gpio, renderer)` are the port targets for workstream C.
- crosspoint PR **#3032** — frontlight survives light sleep; **replaces** the old
  mutual exclusion. Do not port the exclusion as the design.
- SDK PRs **#38** (`FREEINK_X4PRO_FAST_DU_SHORTCUT`, opt-in, left off — validate
  on hardware first) and **#39** (RC_FAST frontlight + EpdBus BUSY grace).
- Our fork vs `upstream/develop`: **2944 ahead / 1142 behind**. We are an
  independent product, not a tracking fork.
