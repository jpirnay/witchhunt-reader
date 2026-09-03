# Multi-board bring-up — X4 Pro and LilyGo T5S3

Date: 2026-08-14 (last updated 2026-08-15)
Hardware: both boards physically in hand since ~2026-08-13
Lead board: **Xteink X4 Pro**
Related: [touch-input-migration-2026-08-14.md](touch-input-migration-2026-08-14.md) (workstream C)
**Resuming work?** Start from
[multiboard-bringup-handover-2026-08-15.md](multiboard-bringup-handover-2026-08-15.md)
— current state, what is verified, and where to pick up. This document is the
plan and rationale.

| Workstream | State |
|---|---|
| A — build configuration | **done**, committed on `fix/s3-build-config` |
| B0 — board concept / capability predicates | not started (next) |
| B — de-hardcode the HAL | two link blockers fixed, rest not started |
| C — touch | not started |
| D — board features | not started; SDK dependency cleared |

## Priority

Consumer devices are shipping now. This displaces the reader/Stage-1 rearchitecture,
which recent work has largely made unnecessary. Touch is **not** a standalone
feature here — it is one of four workstreams inside board support, and not the
one on the critical path.

## The core finding

**Our application layer has no concept of a board.**

```
$ grep -rn "FREEINK_DEVICE_LILYGO|FREEINK_DEVICE_X4PRO|FREEINK_CAP_FRONTLIGHT|
           FREEINK_CAP_USB_MSC|BOARD_HAS_PSRAM" src/ lib/
(no matches)
```

Not one branch on a board profile anywhere in `src/` or `lib/`. The SDK carries
full `BoardConfig` profiles for both target boards; we ignore them and hardcode
the C3 everywhere. Both new boards are also **ESP32-S3 (Xtensa)**, not C3
(RISC-V) — a different architecture, with PSRAM available.

This makes workstream B (below) load-bearing: until the HAL reads
`BoardConfig::ACTIVE`, nothing else can be validated on hardware.

---

## Workstream A — Build configuration ✅ DONE

Mostly defects, all cheap, and they gated everything else. Diagnosis kept below
for the record; **"Resolved"** notes say what actually shipped, which differs from
the original proposal in a couple of places.

### A1. `[base]` hardcodes the C3 device set

```ini
[base]
board = esp32-c3-devkitm-1
build_flags =
  -DFREEINK_DEVICE_X4=1
  -DFREEINK_DEVICE_X3=1
  -DFREEINK_X4_OVERCLOCK_SPI
```

`env:lilygo_t5s3` does `${base.build_flags}`, so it inherits both C3 device flags
on top of its own `FREEINK_DEVICE_LILYGO=1`. This does not produce a
mis-configured binary — it **fails to compile**, by design:

```c
/* BoardConfig.h:86 */
#if (FREEINK_MCU_C3 + FREEINK_MCU_S3 + FREEINK_MCU_ESP32) != 1
#error "FreeInk: all selected devices must share one MCU family …"
```

That guard is reassuring: an S3 binary *cannot* silently carry C3 profiles.
It also answers the combined-build question below — **X4 Pro and T5S3 are both
`FREEINK_MCU_S3` and so can share one binary**, exactly as X3/X4 share the C3 one.
C3 and S3 never can.

**Resolved.** `FREEINK_DEVICE_*` and `FREEINK_X4_OVERCLOCK_SPI` moved out of
`[base]` into a new **`[c3]`** interpolation section, with a matching **`[s3]`**.
Each env now references `${c3.build_flags}` or `${s3.build_flags}` and so opts
into exactly one MCU family.

This is a small departure from the original proposal, which was to unflag the C3
device set in the S3 envs. Moving the flags out of `[base]` instead means the S3
envs need no `build_unflags` at all — they keep inheriting `${base.build_unflags}`
through `extends = base`, which is both smaller and harder to get wrong.

### A2. `-DWOLFSSL_SP_RISCV32` applied to Xtensa boards

`[base]` sets `-DWOLFSSL_SP_RISCV32` (correct for the C3). wolfSSL gates it as:

```c
/* sp_int.c:4374 */
#if defined(WOLFSSL_SP_RISCV32) && SP_WORD_SIZE == 32
```

`SP_WORD_SIZE == 32` on the S3 too, so the RISC-V assembly path is selected on an
Xtensa target. This is measured, not theoretical: the Xtensa assembler rejects the
emitted asm with `unknown opcode 'sltu'/'mul'/'mulhu'`. Upstream uses
`-DWOLFSSL_SP_SMALL` in `[base]` instead.

**Resolved.** Neither flag lives in `[base]` now: `WOLFSSL_SP_RISCV32` sits in
`[c3]`, `WOLFSSL_SP_SMALL` in `[s3]`. (Upstream puts `SP_SMALL` in their `[base]`;
we keep the C3's RISC-V assembly path, so the SP backend has to be per-family
rather than a shared default.)

A1 and A2 were the two blockers for `env:lilygo_t5s3`. Neither was deep — but
until both were cleared, no S3 board compiled at all, which is why workstream A
came first.

**Measured 2026-08-14** — `pio run -e lilygo_t5s3` fails in 117 s:

```
BoardConfig.h:88:2: error: #error "FreeInk: all selected devices must share one MCU family …"
*** [InputManager.cpp.o]   Error 1
*** [XteinkDetect.cpp.o]   Error 1
*** [BatteryMonitor.cpp.o] Error 1
lilygo_t5s3    FAILED    00:01:56
```

A1 was the *first* blocker; A2 sat behind it and only surfaced once A1 was fixed.
At that point the honest status of S3 support was **"does not build"**, not
"builds but untested" — worth remembering when reading any earlier claim that the
T5S3 env merely needed testing.

### A3. No `env:x4pro`

We had no X4 Pro environment at all, despite it being the lead board.

**Resolved.** Added, modelled on upstream's but referencing `${s3.build_flags}`
and carrying `-DFREEINK_DEVICE_X4PRO=1`, `-DUSE_BLOCK_DEVICE_INTERFACE=1`, and the
usual version/log flags.

Two deliberate differences from upstream's env:

- **`-DBOARD_HAS_PSRAM` omitted.** Upstream sets it; we do not, pending the
  allocation-policy decision in workstream D. Enabling it is a one-line change
  once that is settled.
- **`-DFREEINK_X4PRO_FAST_DU_SHORTCUT` omitted** (it postdates upstream's env
  anyway) — see workstream D item 7.

Note `USE_BLOCK_DEVICE_INTERFACE` — **X4 Pro's SD is SDMMC, not SPI.** That is a
storage-path difference, not a pin difference, and it interacts with `HalStorage`.

### A4. Missing SDK libraries — still outstanding

Upstream links these; we do not: `FrontlightManager`, `Rtc`, `Imu`
(plus `FreeInkUI` + `Icons`, which belong to the touch/UI track).

Both new boards have frontlights. X4 Pro has warm/cold dual-channel
(`FREEINK_CAP_WARMLIGHT`).

**Deliberately deferred to workstream D**, not done with the rest of A: linking a
library only helps once there is HAL code calling it, and adding unused libraries
costs flash we do not have (see the flash section).

### A5. PSRAM not enabled on either S3 env — deliberate

Both S3 envs are `esp32-s3-devkitc1-n16r8` — 16 MB flash, **8 MB PSRAM** —
declared by the board but not enabled by a build flag. Left that way on purpose;
workstream D explains why this is a design question rather than a free win.

---

## Workstream B0 — The board concept (do this before any X4 Pro code)

Our board identity is a two-value enum:

```cpp
enum class DeviceType : uint8_t { X4, X3 };
bool deviceIsX3() const;  bool deviceIsX4() const;
```

49 call sites across `src/` and `lib/` branch on it.

### The problem is conflation, not cardinality

`deviceIsX3()` is a stand-in for at least six unrelated questions:

| Site | What it actually asks | X4 Pro's true answer |
|---|---|---|
| [ClockSettingsActivity.cpp:105](../src/activities/settings/ClockSettingsActivity.cpp#L105) | "no hardware RTC → warn about drift" | **has** a BM8563 → warning is wrong |
| [main.cpp:469](../src/main.cpp#L469) `keepLpAlive` | "RTC keeps time across deep sleep" | **has** RTC → we'd waste power |
| [HalClock.cpp:271](../lib/hal/HalClock.cpp#L271) | "has a DS3231 at 0x68" | PCF8563-family at 0x51 |
| [RecentBooksActivity.cpp:625](../src/activities/home/RecentBooksActivity.cpp#L625) | "screen tall enough for a gesture hint" | 800x480, but different chrome |
| [BaseTheme.cpp:171](../src/components/themes/BaseTheme.cpp#L171), [UITheme.cpp:155](../src/components/UITheme.cpp#L155) | "physical front-button positions / side hints" | **no front buttons at all** |
| [HalDisplay.cpp:87](../lib/hal/HalDisplay.cpp#L87), [SettingsActivity.cpp:390](../src/activities/settings/SettingsActivity.cpp#L390) | "panel needs half-refresh settle passes" | SSD1677 — different behaviour |

**The danger is the failure mode.** `deviceIsX3()` returns `false` on X4 Pro, so it
silently takes the **X4** branch at every one of those sites. X4 Pro will not fail
loudly — it will be subtly wrong in six places, each looking like an unrelated bug.

### Extending the enum is the wrong fix

`DeviceType { X3, X4, X4Pro, LilyGo }` multiplies the conflation by four: every
one of the 49 sites becomes an N-way switch, and every future board re-opens all
of them. It also keeps encoding *identity* where the code wants *capability*.

### The fix: ask what the board can do, not what it is

`BoardConfig::ACTIVE` already carries every fact these sites actually want:

| Capability predicate | Source in `BoardProfile` |
|---|---|
| `hasHardwareRtc()` | `sensors.rtcType != RtcType::None` |
| `rtcType()` | `sensors.rtcType` (`Ds3231` / `Pcf8563` / `Rx8130`) |
| `hasTouch()` | `touch.controller != TouchController::None` |
| `hasFrontlight()`, `hasColorTemperature()` | `frontlight`, `frontlight.gpioWarm` |
| `hasPhysicalButtons()`, button topology | `inputStyle` |
| `uiScale()` | `uiScale` |
| panel geometry / bezel | `displayWidth/Height`, `viewableInsets` |
| battery source | `batteryGauge.gaugeAddr != 0` vs `batteryAdc` |

**Where identity legitimately survives:** panel quirks really are per-silicon —
the X3 half-refresh settle passes are a UC8253/UC8279 property, not a capability.
Those should key on `ACTIVE.displayController`, with a comment saying why. That is
the only class of check allowed to name a specific part.

### Migration — incremental, not big-bang

This does **not** require touching all 49 sites before X4 Pro work starts:

1. **Add the capability accessors** to `HalGPIO` (thin readers over
   `BoardConfig::ACTIVE`). Zero call-site churn, no behaviour change.
2. **Convert only the sites where X4 Pro's answer differs from X4's** — the RTC
   trio, the button/hint group, and `uiScale`. Roughly 15 of the 49; the rest are
   X3-vs-X4 panel questions that are still correctly answered today.
3. **Rename the residue.** What remains of `deviceIsX3()` after step 2 is a panel
   question, so it should say so (`panelNeedsHalfRefreshSettle()` or keyed on
   `displayController`) rather than implying board identity.

Step 1 is a prerequisite for X4 Pro code — otherwise every new behaviour gets
written as `if (!deviceIsX3())` and the conflation gets baked in deeper.
Steps 2–3 can land per-capability as separate reviewable commits.

## Workstream B — De-hardcode the HAL (critical path)

Every one of these must read `BoardConfig::ACTIVE` instead of the C3 macros in
[`lib/hal/HalGPIO.h`](../lib/hal/HalGPIO.h#L9-L44):

| Site | Current | Needs |
|---|---|---|
| [HalDisplay.cpp:17](../lib/hal/HalDisplay.cpp#L17) | `EInkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)` | board display pins |
| [HalGPIO.cpp:150](../lib/hal/HalGPIO.cpp#L150) | `SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS)` | board SPI pins; X4 Pro SD is SDMMC |
| [HalGPIO.cpp:153-154](../lib/hal/HalGPIO.cpp#L153) | `pinMode(BAT_GPIO0)`, `pinMode(UART0_RXD)` | board battery/USB-detect config |
| [HalGPIO.cpp:531](../lib/hal/HalGPIO.cpp#L531) | `digitalRead(UART0_RXD)` for USB detect | per-board USB detection |
| [HalPowerManager.cpp:24](../lib/hal/HalPowerManager.cpp#L24) | `Wire.begin(X3_I2C_SDA, X3_I2C_SCL, …)` | board I2C bus(es) — X4 Pro shares one bus across GT911 + RTC + gauge |
| [HalPowerManager.cpp:344](../lib/hal/HalPowerManager.cpp#L344) | `BatteryMonitor(BAT_GPIO0)` | X4 Pro uses a CW2017 gauge, not an ADC divider |
| [HalClock.cpp:19](../lib/hal/HalClock.cpp#L19) | `DS3231_ADDRESS = 0x68`, skipped on non-X3 | X4 Pro has a **BM8563** at 0x51; SDK `Rtc` lib handles both |

`isXteinkDevice()` and the GPIO13 guard are already in place — that work was done.
The rest is not.

**This is the phase to do first and to do carefully.** It is also the one with no
upstream reference diff we can lean on, because upstream's HAL was written against
`BoardConfig` from the start rather than retrofitted.

---

## Workstream C — Touch

Covered in detail in
[touch-input-migration-2026-08-14.md](touch-input-migration-2026-08-14.md).
Both target boards are GT911; X4 Pro adds the capacitive Home key.

Its phases 0–3 (`HalGPIO` passthrough → `MappedInputManager` → reader gestures)
are the part that matters for bring-up. The FUI question raised there is a
**UI-layer** decision and should not be allowed to block board support — it can
start once these boards boot and render.

One bring-up-specific interaction: workstream B decides I2C bus ownership, and
touch is an I2C peripheral sharing that bus on both boards. Settle P1 (touch
servicing on our `btnsample` task vs the loop task) as part of B, not C.

---

## Workstream D — Board-specific features

Ordered by how visible their absence is:

1. **Frontlight** — both boards. Link `FrontlightManager`, add
   `lib/hal/HalFrontlight` + a panel/settings activity. X4 Pro additionally has
   warm/cold (`FREEINK_CAP_WARMLIGHT`), needing a two-axis control.

   **Frontlight vs light sleep — upstream has just changed the answer.**

   Light sleep stops the default LEDC PWM output, which visibly flashes an
   ESP-driven frontlight as the idle loop enters repeated sleep slices. Upstream
   handled that by making the two mutually exclusive — a lit frontlight blocks
   light sleep entirely, guarded in both `HalPowerManager::lightSleep()` and
   `onEinkBusyWaitSlice()`:

   ```cpp
   if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached() ||
       (Frontlight.present() && Frontlight.isOn())) {
     return false;
   }
   ```

   Earlier analysis dismissed porting this because we had no light sleep. That
   stopped being true when PR #146 (`feat/idle-light-sleep`) merged to master.

   **But the exclusion itself is no longer upstream's answer.** crosspoint PR
   [#3032](https://github.com/crosspoint-reader/crosspoint-reader/pull/3032)
   ("feat: light-sleep-surviving frontlight for the X4 Pro") **merged
   2026-08-14**, paired with SDK PR
   [Free-Ink/freeink-sdk#39](https://github.com/Free-Ink/freeink-sdk/pull/39):

   - **SDK side:** the frontlight LEDC is clocked from **RC_FAST with
     `KEEP_ALIVE`**, so its PWM survives light sleep blink-free — the flashing
     the guard existed to prevent no longer happens. The keep-on cost is
     refcounted against 0↔nonzero duty edges, so *dark* idle still sleeps at
     full depth.
   - **App side:** `[env:x4pro]` sets `FREEINK_FRONTLIGHT_LS`, under which the
     `Frontlight.present() && Frontlight.isOn()` clause **compiles out**. Boards
     without the define (Paper Mono, the C3s) keep the guard byte-identically.
   - **Measured by the submitter on X4 Pro hardware:** 92 % light-sleep residency
     over a ~31.5 min session (34,492 sleep entries, 0 rejects, ~50 ms average
     window) with the light on.

   **What this means for us.** Do not port the exclusion as the design — port the
   *pair*: the guard as the fallback for non-opt-in boards, and
   `FREEINK_FRONTLIGHT_LS` for X4 Pro. Since X4 Pro is our lead board and night
   reading with the light on is exactly when light sleep pays, taking only the
   guard would hand the X4 Pro a fully-awake chip for entire evening sessions.

   **SDK dependency — resolved 2026-08-15.** The `freeink-sdk` submodule was
   bumped `56efd2e` → **`76e61c4`**, which carries SDK PR #39, so
   `FREEINK_FRONTLIGHT_LS` is now available in `FrontlightManager`. Seven commits
   came in:

   | Commit | What |
   |---|---|
   | `f6f5940` / #37 | FreeInkUI list rows grow by measured wrapped lines — inert for us, we don't link FUI |
   | `877f951` / #38 | opt-in `FREEINK_X4PRO_FAST_DU_SHORTCUT` (see below) |
   | `cb7ec59`, `e458e6e` / #39 | RC_FAST frontlight LEDC + polled-wait BUSY grace |

   The app-side half of #3032 is still ours to port when the frontlight is wired.

   **Review risk flagged by the PR itself:** the SDK side uses
   `esp_private/esp_sleep_internal.h` (refcounted RC_FAST sleep sub-mode — no
   public API exists), plus a 20 ms BUSY-assert grace in `EpdBus`'s polled slice
   path to cover a BUSY edge taken while sleeping.
2. **RTC** — X4 Pro's BM8563 via the SDK `Rtc` lib; retire the DS3231 hardcoding.
3. **Capacitive Home key** — X4 Pro; upstream has `wasHomeKeyTapped()` /
   `wasHomeKeyLongPressed()` wired to home/reader-menu actions.
4. **SD over SDMMC** — X4 Pro; `USE_BLOCK_DEVICE_INTERFACE`, through `HalStorage`.
5. **USB-MSC** — optional, opt-in, and it changes USB mode for the whole build
   (`ARDUINO_USB_MODE=0`). Treat as a separate decision.
6. **PSRAM** — see below.
7. **Refresh tuning (opt-in, default off).** SDK PR #38 adds
   `-DFREEINK_X4PRO_FAST_DU_SHORTCUT`, the X4 Pro counterpart to the C3's
   `FREEINK_X4_FAST_DU_SHORTCUT` that `[c3]` already documents: ~85 ms/refresh on
   the same GDEQ0426T82 panel class, **SSD1677 batches only** (UC8179/UC8279
   batches select different drivers and are unaffected). Deliberately not enabled
   in `[env:x4pro]` — the C3 equivalent carries a "validate on hardware first"
   warning after upstream saw long-session ghosting, and the same caution applies
   until someone runs a long session on a real X4 Pro.

### PSRAM deserves an explicit decision

Both S3 boards ship 8 MB of PSRAM, and upstream's X4 Pro env enables it. **Ours
does not** — neither `env:x4pro` nor `env:lilygo_t5s3` sets `BOARD_HAS_PSRAM`,
pending this decision. Our entire memory strategy — the single 48 KB framebuffer,
the borrow-vs-release discipline, the arena work,
`EINK_DISPLAY_SINGLE_BUFFER_MODE` — exists because the C3 has 380 KB and no PSRAM.

The temptation is to "just use PSRAM" on S3 boards. Resist it as a default:
divergent memory behaviour between boards means every heap bug becomes
board-specific, and our accumulated heap knowledge stops transferring. The
conservative first step is to enable PSRAM, change **nothing** about allocation
policy, and treat the extra headroom as a safety margin rather than a budget to
spend. Revisit only with measurements.

---

## Sequencing

**Lead board: Xteink X4 Pro.** It has an upstream reference env and a complete
upstream feature branch to compare against; the T5S3 is our own port with no
upstream counterpart, so leading with it would mean debugging our port and our
board support at the same time.

```
A (build config)  →  B0 (capability predicates)  →  B (HAL de-hardcode)
                                                       ↓
                                              X4 Pro boots + renders
                                                       ↓
                                    C (touch 0-3)  ┐
                                    D (frontlight, ├→ X4 Pro usable  →  T5S3
                                       RTC, SDMMC) ┘
                                                            ↓
                                    UI/FUI decision (not blocking)
```

A is done and committed. B0 is small and unblocks everything without changing
behaviour — it is the next commit. B is the real work and the real risk; its two
*link* blockers are fixed, but none of the pin/peripheral de-hardcoding is. C and
D parallelize once B lands. The UI/FUI question is deliberately last — it is about
long-term maintainability, not about making these boards work.

**First milestone: X4 Pro boots, mounts SD, renders a page.** Nothing else. That
exercises A, B0 and most of B, and will surface the board differences this
document is necessarily guessing at. T5S3 follows once the pattern is proven.

### Build history

| Date | Build | Result |
|---|---|---|
| 08-14 | `lilygo_t5s3` (before A) | FAILED 117 s — MCU-family `#error` |
| 08-14 | `default` (after A) | **SUCCESS** 650 s — C3 regression gate passed |
| 08-14 | `x4pro` (after A) | FAILED 321 s — past all config errors, into the two B blockers below |
| 08-15 | `x4pro` (after B blockers + SDK bump) | **SUCCESS** 766 s — first S3 build ever to link |
| 08-15 | `default` (same) | **SUCCESS** 786 s — C3 gate held, +94 bytes flash (see below) |

### The two B link blockers — FIXED 2026-08-15

Both were loud compile-time failures in the HAL — exactly where the plan
predicted, and both the "MCU families differ" warning made real.

**1. Deep-sleep wakeup was a C3-only API.**
[HalPowerManager.cpp:318](../lib/hal/HalPowerManager.cpp#L318)

```
error: 'ESP_GPIO_WAKEUP_GPIO_LOW' was not declared … did you mean 'ESP_EXT1_WAKEUP_ANY_LOW'?
error: 'esp_deep_sleep_enable_gpio_wakeup' was not declared … did you mean 'esp_sleep_enable_gpio_wakeup'?
```

Fixed by branching on **SoC capability**, not chip name — verified from
`soc_caps.h` that C3 defines `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP` and no EXT1,
while S3 is the reverse, so each target compiles exactly one arm:

```cpp
#if SOC_PM_SUPPORT_EXT1_WAKEUP           // S3: RTC GPIOs, EXT1
  esp_sleep_enable_ext1_wakeup_io(powerPinMask, ESP_EXT1_WAKEUP_ANY_LOW);
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP  // C3: dedicated deep-sleep GPIO path
  esp_deep_sleep_enable_gpio_wakeup(powerPinMask, ESP_GPIO_WAKEUP_GPIO_LOW);
#else
#error "No deep-sleep wake source available for this target …"
#endif
```

`esp_sleep_enable_ext1_wakeup_io()` rather than `esp_sleep_enable_ext1_wakeup()`,
which IDF documents for deprecation in v6.0. X4 Pro's power key is GPIO3, which is
RTC-capable on the S3, so EXT1 is valid for it.

**2. The panic backtrace wrapper was RISC-V-specific.**
[HalSystem.cpp:46](../lib/hal/HalSystem.cpp#L46)

```
error: 'RvExcFrame' was not declared in this scope; did you mean 'XtExcFrame'?
```

Smaller than it looked: only the stack-pointer extraction is architecture-specific
— the raw stack walk below it dumps words upward and is arch-neutral. RISC-V keeps
SP in `RvExcFrame::sp`, Xtensa in `XtExcFrame::a1` (confirmed from
`xtensa_context.h:127`, *"stack pointer before interrupt"*). Gated on the
compiler's own `__riscv` / `__XTENSA__` so it does not depend on IDF config
macros, with an `#error` default.

**Deliberately not fixed while here:** the wake level is hardcoded LOW. It should
follow `BoardConfig::ACTIVE.input.powerActiveHigh`, but every board we currently
build is active-LOW, so making it runtime-derived would add behaviour risk to the
shipped C3 product for no present gain. Left as a latent issue for B0, where it
belongs with the other capability predicates.

## Flash is nearly full — this constrains everything below

Measured on the passing C3 build, 2026-08-14:

```
RAM:   [==        ]  17.2% (used   56,476 bytes from   327,680)
Flash: [==========]  97.3% (used 6,376,829 bytes from 6,553,600)
```

**~177 KB of flash left.** Earlier notes in this document and in
[touch-input-migration](touch-input-migration-2026-08-14.md) claimed we had ample
headroom, citing a ~2.22 MB figure — that number is *text only* and does not
include fonts and assets. The linked image is 6.38 MB of a 6.55 MB partition.

Consequences to carry into every decision here:

- **A combined S3 binary (X4 Pro + T5S3) is likely not affordable** without
  cutting something, even though the MCU family permits it. Plan for one binary
  per board and re-measure before assuming otherwise.
- **Every new board adds driver and asset flash**, and the S3 builds additionally
  pull in M5GFX/LovyanGFX for the parallel EPD path.
- **The FUI adoption argument loses one of its supports.** Capacity-templated
  types instantiate per-capacity, which is why upstream consolidated to a single
  `FreeInkApp<24,6>`. That consolidation is not optional for us.
- Font data is the dominant consumer and the obvious lever if room is needed
  (measured previously: Cyrillic alone is ~414 KB / 30 % of font data, and brotli
  `lgwin16` was worth ~95 KB).

This deserves its own measurement pass before workstream D starts adding board
features.

## Risks

- **B has no reference diff.** Upstream built against `BoardConfig` from the
  start; we are retrofitting. Expect this to be slower than it looks.
- **Regressing the C3.** X3/X4 are the shipped, working product. Every change in
  B must be a no-op there — a flash/RAM delta of zero on `env:default` is the gate.
- **Two boards at once.** They share an MCU family and a touch controller but
  differ in SD path, RTC, gauge, and frontlight channels. Bringing them up
  simultaneously risks conflating their failures; pick one to lead.
- **The SDK is moving fast** (80 commits in six months on FreeInkUI alone, and it
  moved twice during this planning). It is a git submodule pinned by pointer, so
  a bump is explicit and reviewable — but each one should be its own commit with
  its own C3 regression build, never folded into board work.

## Open questions

1. ~~Which board leads?~~ **Answered: X4 Pro** — it has an upstream reference env
   and a complete upstream feature branch to compare against.
2. Is the X4 Pro's SDMMC path a `HalStorage` change or an SDK-level one?
3. PSRAM: enable-but-don't-spend, as argued above — agreed?
4. Is USB-MSC in scope for the first release of these boards?
5. ~~Single binary per board, or combined?~~ **Answered by A1**: X4 Pro and T5S3
   are both `FREEINK_MCU_S3`, so a combined S3 binary is possible exactly as
   X3/X4 share the C3 one. Still a *choice*, and the flash section argues it is
   probably unaffordable — but it is not a constraint.
6. Should the app-side half of crosspoint #3032 (`FREEINK_FRONTLIGHT_LS`) land
   with the frontlight work in D, or earlier as a standalone port?
