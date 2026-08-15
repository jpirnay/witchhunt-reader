# Multi-board bring-up — X4 Pro and LilyGo T5S3

Date: 2026-08-14
Status: proposal, nothing implemented
Hardware: both boards physically in hand since ~2026-08-12
Related: [touch-input-migration-2026-08-14.md](touch-input-migration-2026-08-14.md) (workstream C)

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

## Workstream A — Build configuration

Mostly defects, all cheap, and they gate everything else.

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

**Fix:** move `FREEINK_DEVICE_*` and `FREEINK_X4_OVERCLOCK_SPI` out of `[base]`
into `env:default` / `gh_release` / `slim`, as upstream does — their `[base]`
carries neither. The S3 envs additionally need a `build_unflags` section, which
they currently lack entirely, so it must also re-list `${base.build_unflags}`.

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

**Fix:** `WOLFSSL_SP_SMALL` in `[base]`; `WOLFSSL_SP_RISCV32` only in the C3 envs.

A1 and A2 are the two known blockers for `env:lilygo_t5s3`. Neither is deep — but
until both are cleared, no S3 board compiles at all, which is why workstream A
comes first.

**Measured 2026-08-14** — `pio run -e lilygo_t5s3` fails in 117 s:

```
BoardConfig.h:88:2: error: #error "FreeInk: all selected devices must share one MCU family …"
*** [InputManager.cpp.o]   Error 1
*** [XteinkDetect.cpp.o]   Error 1
*** [BatteryMonitor.cpp.o] Error 1
lilygo_t5s3    FAILED    00:01:56
```

A1 is the *first* blocker; A2 sits behind it and only surfaces once A1 is fixed.
So the current honest status of S3 support is **"does not build"**, not
"builds but untested".

### A3. No `env:x4pro`

We have no X4 Pro environment at all. Upstream's is short and portable:

```ini
[env:x4pro]
extends = base
board = esp32-s3-devkitc1-n16r8
board_build.mcu = esp32s3
build_flags =
  ${base.build_flags}
  -DFREEINK_DEVICE_X4PRO=1
  -DBOARD_HAS_PSRAM
  -DUSE_BLOCK_DEVICE_INTERFACE=1   ; SD is native SDMMC (1-bit), not SPI
  ...
```

Note `USE_BLOCK_DEVICE_INTERFACE` — **X4 Pro's SD is SDMMC, not SPI.** That is a
storage-path difference, not a pin difference, and it interacts with `HalStorage`.

### A4. Missing SDK libraries

Upstream links these; we do not: `FrontlightManager`, `Rtc`, `Imu`
(plus `FreeInkUI` + `Icons`, which belong to the touch/UI track).

Both new boards have frontlights. X4 Pro has warm/cold dual-channel
(`FREEINK_CAP_WARMLIGHT`).

### A5. `env:lilygo_t5s3` omits `-DBOARD_HAS_PSRAM`

The board is `esp32-s3-devkitc1-n16r8` — 16 MB flash, **8 MB PSRAM** — declared
but never enabled. See workstream D for why this is a design question, not just a
flag.

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

   **This now carries a hard dependency that did not exist a month ago.** Light
   sleep stops the default LEDC PWM output, which visibly flashes an ESP-driven
   frontlight as the idle loop enters repeated sleep slices. Upstream guards it
   in both `HalPowerManager::lightSleep()` and `onEinkBusyWaitSlice()`:

   ```cpp
   if (WiFi.getMode() != WIFI_MODE_NULL || gpio.isUsbConnectedCached() ||
       (Frontlight.present() && Frontlight.isOn())) {
     return false;
   }
   ```

   Previous analysis dismissed this port on the grounds that we had no light
   sleep. **That is no longer true** — PR #146 (`feat/idle-light-sleep`) merged
   to master, and `HalPowerManager::lightSleep()` plus its stats now exist. So
   the moment a frontlight is wired on either board, this guard is required or
   the light will strobe during idle reading.
2. **RTC** — X4 Pro's BM8563 via the SDK `Rtc` lib; retire the DS3231 hardcoding.
3. **Capacitive Home key** — X4 Pro; upstream has `wasHomeKeyTapped()` /
   `wasHomeKeyLongPressed()` wired to home/reader-menu actions.
4. **SD over SDMMC** — X4 Pro; `USE_BLOCK_DEVICE_INTERFACE`, through `HalStorage`.
5. **USB-MSC** — optional, opt-in, and it changes USB mode for the whole build
   (`ARDUINO_USB_MODE=0`). Treat as a separate decision.
6. **PSRAM** — see below.

### PSRAM deserves an explicit decision

The T5S3 has 8 MB and X4 Pro's env declares `BOARD_HAS_PSRAM`. Our entire memory
strategy — the single 48 KB framebuffer, the borrow-vs-release discipline, the
arena work, `EINK_DISPLAY_SINGLE_BUFFER_MODE` — exists because the C3 has 380 KB
and no PSRAM.

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

A is done (below). B0 is small and unblocks everything without changing
behaviour. B is the real work and the real risk. C and D parallelize once B
lands. The UI/FUI question is deliberately last — it is about long-term
maintainability, not about making these boards work.

**First milestone: X4 Pro boots, mounts SD, renders a page.** Nothing else. That
exercises A, B0 and most of B, and will surface the board differences this
document is necessarily guessing at. T5S3 follows once the pattern is proven.

### Status

- **A — done and verified 2026-08-14.** `[base]` no longer carries a device set or
  an architecture-specific wolfSSL SP backend; new `[c3]` / `[s3]` sections own
  those, and `[env:x4pro]` exists. PSRAM (`-DBOARD_HAS_PSRAM`) deliberately **not**
  enabled — see workstream D.
  - `pio run -e default` → **SUCCESS** (650 s). C3 regression gate passed.
  - `pio run -e x4pro` → **FAILED**, but now 321 s in and past every configuration
    error. It fails on genuine architecture-specific code, which is workstream B.

### The two concrete B blockers (measured, X4 Pro)

Both are loud compile-time failures in the HAL — exactly where the plan predicted,
and both are the "MCU families differ" warning made real:

**1. Deep-sleep wakeup is a C3-only API.**
[HalPowerManager.cpp:318](../lib/hal/HalPowerManager.cpp#L318)

```
error: 'ESP_GPIO_WAKEUP_GPIO_LOW' was not declared … did you mean 'ESP_EXT1_WAKEUP_ANY_LOW'?
error: 'esp_deep_sleep_enable_gpio_wakeup' was not declared … did you mean 'esp_sleep_enable_gpio_wakeup'?
```

The C3 uses `esp_deep_sleep_enable_gpio_wakeup()`; the S3 wants the EXT1 path.
This needs a HAL-level wakeup abstraction, not a local `#ifdef` — deep sleep is
also where the GPIO13 guard and the power-latch work already live.

**2. The panic backtrace wrapper is RISC-V-specific.**
[HalSystem.cpp:46](../lib/hal/HalSystem.cpp#L46)

```
error: 'RvExcFrame' was not declared in this scope; did you mean 'XtExcFrame'?
```

`__wrap_panic_print_backtrace` walks a `RvExcFrame` (the comment says it was
copied from `esp_system/port/arch/riscv/panic_arch.c`). Xtensa's `XtExcFrame` has
a different layout, so this needs a per-architecture implementation — or to be
compiled out on S3 until someone ports it. Note `-Wl,--wrap=panic_print_backtrace`
is in `[base]`, so the wrapper is linked on every board.

Neither is deep. Both must be fixed before X4 Pro links.

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
- **The SDK is moving** (80 commits in six months on FreeInkUI alone). Board
  profiles may shift under us; pin the SDK revision during bring-up.

## Open questions

1. Which board leads — X4 Pro (upstream reference env exists) or T5S3 (our own
   port, no upstream counterpart)?
2. Is the X4 Pro's SDMMC path a `HalStorage` change or an SDK-level one?
3. PSRAM: enable-but-don't-spend, as argued above — agreed?
4. Is USB-MSC in scope for the first release of these boards?
5. ~~Single binary per board, or combined?~~ **Answered by A1**: X4 Pro and T5S3
   are both `FREEINK_MCU_S3`, so a combined S3 binary is possible exactly as
   X3/X4 share the C3 one. Still a *choice* (flash cost vs release simplicity),
   but not a constraint.
