# E-Ink Controller Reference: SSD1677 (X4) and UC8179 (X3)

Technical reference for the two e-paper display controllers used in the CrossPoint and WitchHunt readers. Covers architecture, RAM model, sleep/wake RAM retention, differential refresh, and features that are available in the hardware but not yet exploited by the firmware.

---

## Controllers at a Glance

| Property | X4 | X3 |
|---|---|---|
| IC | Solomon Systech **SSD1677** | UltraChip **UC8179** (UC81xx class) |
| Panel | GDEQ0426T82, 4.26", 800×480 | Xteink X3, 792×528 |
| RAM planes | BW RAM (`0x24`) + RED RAM (`0x26`) | DTM1 (`0x10`) + DTM2 (`0x13`) |
| Plane size | 48,000 bytes each | 52,272 bytes each |
| Differential role of plane 1 | "current frame" (BW) | "new frame" (DTM2) |
| Differential role of plane 0 | "previous frame" (RED) | "old frame" (DTM1) |
| BUSY polarity | HIGH = busy, LOW = done | LOW = busy, HIGH = done |
| Deep sleep command | `0x10` + data `0x01` | `0x07` + check code `0xA5` |
| RAM after deep sleep | **Lost** (datasheet: "cannot retain RAM data") | **Lost** (hardware reset required to wake) |
| RAM after power-off (no deep sleep) | Not applicable — no intermediate POF | **Retained** while VDD is present (UC8179 POF `0x02`) |
| SPI speed in firmware | 40 MHz | 16 MHz |

---

## RAM Architecture

Both controllers hold two independent 1-bit planes. Each pixel has one bit in each plane, giving a 2-bit value per pixel and four possible states. The same two planes serve double duty: for grayscale they encode intensity; for differential BW refresh they encode "current frame" and "previous frame."

### Grayscale encoding (X4 and X3)

| Plane 1 (RED/DTM2) | Plane 0 (BW/DTM1) | Gray level | Visual |
|--------------------|-------------------|------------|--------|
| 0 | 0 | 0 | Black |
| 0 | 1 | 1 | Dark gray |
| 1 | 0 | 2 | Light gray |
| 1 | 1 | 3 | White |

### Differential BW encoding (X4 fast refresh, X3 fast refresh)

| Plane 0 (BW/DTM2) | Plane 1 (RED/DTM1) | Meaning |
|--------------------|--------------------|---------|
| new frame pixel | same pixel one refresh ago | Controller drives only pixels that changed |

The LUT reads both planes simultaneously and executes the appropriate waveform per pixel state transition. Ghosting on fast BW page turns always traces back to plane 1 (RED/DTM1) not accurately reflecting the *previous* displayed frame.

---

## Sleep and RAM Retention

This is the most practically important difference between the two controllers.

### X4 (SSD1677): deep sleep wipes RAM

The SSD1677 datasheet DC Characteristics table states explicitly under the deep sleep current entry: **"Cannot retain RAM data."** The Good Display application note for SSD-series panels confirms: *"In deep sleep mode, the IC data cannot be saved, and at the second time of e-paper refresh, IC needs to be reset."*

What this means in practice:
- `deepSleep()` sends `0x10` + `0x01`. Both BW RAM and RED RAM are erased.
- Wake requires a hardware RST pulse.
- After wake, `begin()` must re-run the full init sequence and re-clear both planes via `AUTO_WRITE_BW_RAM` / `AUTO_WRITE_RED_RAM`.
- There is **no intermediate low-power mode on X4** that preserves RAM while saving power. The only choices are: analog on (display active), normal sleep (clocks gated, RAM accessible — no explicit firmware use), or deep sleep (RAM gone).

The SW Reset command (`0x12`) is different: it resets command/parameter registers but **does not affect RAM**. The firmware uses this only during init.

### X3 (UC8179): power-off preserves RAM; deep sleep does not

The UC8179 has a meaningful intermediate state:

- **POF — Power Off (`0x02`):** Turns off the charge pump, gate driver, source driver, and VCOM. The datasheet states: *"register data will be kept until VDD turned OFF or Deep Sleep Mode."* Both DTM1 and DTM2 SRAM contents survive as long as VDD remains. This is what the firmware uses.
- **DSLP — Deep Sleep (`0x07` + `0xA5`):** Exits via RST_N hardware reset. Equivalent to the SSD1677 deep sleep — registers and SRAM are not retained.

The firmware **only ever issues POF on X3**, never DSLP. This means DTM1 and DTM2 survive the display's "off" state between page turns and across the ESP32's active idle periods, as long as the device remains powered.

### Practical consequence for differential refresh

Because DTM1 on X3 survives POF, the controller always has the last-displayed BW frame available as the differential baseline when the next fast refresh is triggered. The firmware exploits this in `completeDisplay()`: after each fast X3 refresh, it writes the just-displayed frame back into DTM1 post-waveform — this keeps DTM1 in sync with what is visually on screen. On warm restart (e.g. after a UI state change with no power cycle), `skipInitialResync()` suppresses the two initial forced full-syncs because DTM1 is still valid.

On X4, there is no equivalent: RED RAM must be explicitly re-uploaded before every fast differential refresh. This is why `syncRedRamFromFrameBuffer()` exists only for X4 and `releaseSecondaryBuffer()` degrades X4 fast refresh to half-refresh (no previous-frame buffer available) while leaving X3 unaffected:

```cpp
// EInkDisplay.h — comment on releaseSecondaryBuffer()
// On X3 fast differential is unaffected (previous-frame lives in
// the controller's DTM1 RAM). Grayscale AA is unavailable until restored.
```

---

## Differential Refresh Mechanics

### X4

1. Write new frame → BW RAM (`0x24`).
2. Write previous frame → RED RAM (`0x26`).
3. Set `CTRL1_NORMAL` (`0x21` = `0x00`) to enable RED vs BW comparison.
4. Set update sequence `0x22` = `0x0C` (custom LUT path) or `0x1C` (built-in LUT).
5. Trigger `CMD_MASTER_ACTIVATION` (`0x20`). Wait BUSY LOW.

For HALF/FULL refresh, RED RAM is bypassed via `CTRL1_BYPASS_RED` (`0x40`), which tells the controller to treat RED RAM as all-zeros regardless of its content. This allows a full absolute refresh without writing RED RAM at all — useful when RED RAM state is uncertain.

### X3

1. Load LUT bank (five registers: VCOM/WW/BW/WB/BB via `0x20`–`0x24`).
2. Set CDI register (`0x50`) for differential (`0x29`) or absolute (`0xA9`) mode.
3. Write new frame → DTM2 (`0x13`).
4. (For full sync only) Fill DTM1 with white (`0xFF`) via `fillPlaneX3`.
5. Trigger `CMD_X3_DISPLAY_REFRESH` (`0x12`). Wait BUSY HIGH.
6. After waveform: write just-displayed frame → DTM1 (`0x10`) to keep baseline current.

The CDI register controls whether the LUT interprets pixel state as differential (comparing DTM2 against DTM1) or absolute. Loading the wrong CDI for a given LUT bank produces visible artifacts: the `lut_x3_*_half` bank must use CDI `0xA9` (absolute) because its WW/BW pair is byte-identical — the pixel state in DTM1 is irrelevant to the waveform choice, so treating it as absolute is correct.

---

## LUT Structure Comparison

| Property | X4 (SSD1677) | X3 (UC8179) |
|---|---|---|
| LUT command | `0x32` (single 105-byte upload) | Five registers: `0x20`–`0x24` (42 bytes each) |
| VS patterns | 50 bytes (5 groups × 10 bytes) | First byte of each 42-byte register is the VS pattern |
| Timing groups | 10 TP/RP groups × 5 bytes = 50 bytes | Bytes 1–11 encode TP/RP (6 phase slots × ~2 bytes) |
| Frame rate | 5 bytes | Embedded in TP encoding |
| Voltage control | Separate commands `0x03`, `0x04`, `0x2C` | Separate command `0x82` (VDCS) and power registers |
| OTP burn | `0x36` | Not used |
| Voltage state encoding | 2 bits per phase per state (4 states, 10 phases) | Per-register: one state per register, VS byte encodes phases |

The X3's five-register layout means each LUT register encodes the waveform for one pixel state transition (WW, WB, BW, BB, VCOM), while the X4 encodes all four states in one upload with interleaved VS patterns. This is why the X3 requires five separate `sendCommandDataX3` calls to load a LUT bank, while the X4 needs one `sendCommand(CMD_WRITE_LUT)` followed by 105 bytes.

---

## Features Available in Hardware, Not Yet Used

### 1. X4: `CTRL1_BYPASS_RED` for absolute refresh without RED RAM write

**Available:** `CMD_DISPLAY_UPDATE_CTRL1` (`0x21`) with data `0x40` bypasses RED RAM, treating all previous-frame pixels as zero (black). This forces a full absolute refresh — every pixel is driven to its target state without diffing.

**Current use:** Used for HALF_REFRESH and FULL_REFRESH. Not used for any targeted "clean slate" scenarios.

**Potential use:** After a warm restart when RED RAM is stale but there is no clean secondary buffer to re-upload from, a single `BYPASS_RED` full refresh would correctly initialize the display and establish a known RED RAM state — without needing to re-upload the previous frame. Currently `skipInitialResync()` avoids the issue by suppressing forced full syncs, but a single explicit `BYPASS_RED` pass would be more principled for the case where the previous-frame content is genuinely unknown.

### 2. X4: `AUTO_WRITE_BW_RAM` / `AUTO_WRITE_RED_RAM` for fast bulk fill

**Available:** Commands `0x46` and `0x47` fill an entire RAM plane with a single pattern byte (e.g. `0xF7` = white) via a hardware DMA-like auto-write. The controller handles the fill internally; no SPI bulk transfer of 48,000 bytes is needed.

**Current use:** Only during `initDisplayController()` for the initial RAM clear.

**Potential use:** Any time a full-plane white clear is needed (e.g. before a factory-mode grayscale refresh, or after a mode transition that leaves BW RAM in an unknown state), `0x46`/`0x47` would be faster and use zero SPI bandwidth compared to a manual `writeRamBuffer`.

### 3. X3: Partial window update (`PTL`/`PTIN`/`PTOUT`) — implemented, with a hardware caveat

**Available:** Commands `0x91` (PTIN), `0x90` (PTL, 9-byte window descriptor), `0x92` (PTOUT) allow the UC8179 to accept a DTM2 payload and constrain the refresh waveform to a rectangular sub-region of the panel, defined by a source-column range (`HRST`/`HRED`) and a gate/row range (`VRST`/`VRED`). The LUT waveforms and BUSY protocol are otherwise identical to a full refresh.

**Current use:** `EInkDisplay::displayWindow()` now implements this natively for X3 (previously it fell back to a full `displayBuffer()`), following the exact OEM conditioning-pass sequence: `PTIN → PTL → DTM2(full frame) → PTOUT → DRF` (no `DATA_STOP` between `DTM2` and `PTOUT` — the controller hangs if it's present there), using the `lut_x3_*_half` LUT bank (CDI `0xA9`) so the waveform selection is independent of DTM1's contents (a partial window cannot reliably carry a synced DTM1 baseline for its region).

**Hardware constraint discovered during implementation — narrow source-column ranges are unreliable:** On this panel, the UC8179 source driver cannot cleanly drive a PTL window whose source-column range (`HRST`/`HRED`) is narrower than the full panel width. Doing so produces an intermittent "speckle" artifact (a faint line every few pixels, roughly 1 clean refresh in 10 attempts) that is **identical regardless of which LUT bank is loaded** — fast/differential (`0x29`), normal (`0xA9`), and half/scrub (`0xA9`) all speckle equally, which rules out DTM1/waveform-selection as the cause. A window with the *full* source range but a *narrow gate/row range* (`VRST`/`VRED`), by contrast, came out perfectly clean on every attempt (confirmed over two independent test rounds, 9/9 clean). The OEM firmware's only documented partial-mode usage (the conditioning pass in `completeDisplay()`) always uses a full-panel window — consistent with this being a real silicon/panel limitation rather than a sequencing bug.

`displayWindow()` therefore **always widens the PTL source range to the full panel width** (`HRST=0`, `HRED=panelWidth-1`) and constrains only the gate/row range — the axis confirmed to work reliably. The caller's `(x, w)` are still used for framebuffer addressing/bounds; only the descriptor sent to the controller is widened (a `Serial` note is logged when this happens).

**Practical consequence — this limits which UI regions actually benefit:** Because only the gate/row range can be constrained, a windowed refresh only saves waveform time when the *physical* rectangle is full-width and row-constrained. `GfxRenderer` exposes a logical-portrait screen on this panel (rotated 90° from the physical 792×528 landscape panel), so:
- A **logical left/right-edge vertical strip spanning the full screen height** maps to a physical full-width/narrow-row band → genuinely faster partial refresh.
- A **logical top/bottom horizontal bar** (e.g. a status bar) maps to a physical narrow-source-column/full-row band → `displayWindow()` produces a *correct* result (no speckle, thanks to the auto-widen) but costs the same as a full-panel refresh, since the controller ends up scanning the full source range regardless.

This means the original motivating use case — a faster status-bar-only partial refresh in the default Portrait orientation — is **not achievable via native PTL windowing** on this controller/panel combination. `displayWindow()` remains correct and available for the left/right-edge-strip shape where it does help.

### 4. X3: LUT and CDI registers survive POF; only reload on mode change

**Available:** Since DTM1, DTM2, LUT registers, and CDI all survive POF, there is no requirement to reload the LUT bank before every refresh — only when the mode changes (e.g. switching from fast-diff to full, or from BW to grayscale).

**Current use:** The firmware reloads the LUT bank on every `triggerDisplay` call regardless of whether the mode has changed.

**Potential use:** A "last loaded LUT" cache variable could skip the five-register reload (5 × 42-byte = 210-byte SPI transfers) when the same LUT bank is being used for consecutive same-mode refreshes. At 16 MHz SPI this is a ~105 µs saving per refresh — minor, but the principle of avoiding redundant register writes improves clarity and reduces bus traffic.

### 5. X3: DTM1 content is preserved across POF — reuse without re-upload on resume

**Available:** After a fast refresh, DTM1 holds the just-displayed frame. If the device goes idle (ESP32 light sleep, no VDD cut), DTM1 remains valid on resume. The firmware currently re-uploads the frame to DTM1 after every fast refresh (`completeDisplay()` line 1342) to ensure correctness, which is the right default.

**Potential use:** If the firmware could guarantee that no other write to DTM1 occurred during the sleep (which it can, since DTM1 is only written by `EInkDisplay`), the post-refresh DTM1 re-upload could be skipped on the next refresh if no mode change or revert occurred. This is a marginal optimization but reflects the fact that the X3 controller is doing work the X4 controller cannot do: acting as a free frame store.

---

## Init Sequence Differences

| Step | X4 (SSD1677) | X3 (UC8179) |
|---|---|---|
| Reset | HW RST pulse (20ms / 2ms / 20ms) + 0ms settle | HW RST pulse + 50ms settle |
| Soft reset | `0x12`, wait BUSY | None |
| Panel setting | Driver output `0x01`, booster `0x0C` | PSR `0x00`, TRES `0x61`, GSST `0x65`, PFS `0x03`, PWR `0x01`, VDCS `0x82`, BTST `0x06`, PLL `0x30`, LVsel `0xE1` |
| RAM clear | `AUTO_WRITE_BW_RAM` `0x46`, `AUTO_WRITE_RED_RAM` `0x47` | Manual bulk write via `CMD_X3_DTM1` + `CMD_X3_DTM2` |
| Border | `CMD_BORDER_WAVEFORM` `0x3C` = `0x01` | Encoded in PSR |
| Temperature | `CMD_TEMP_SENSOR_CONTROL` `0x18` = `0x80` (internal) | Not set explicitly; UC8179 uses internal sensor by default |
| LUT | Built-in OTP LUT used by default; custom LUT loaded per refresh | Five LUT registers loaded explicitly before each refresh via `loadLutBankX3` |

The X3 init is notably more verbose: the UC8179 has no built-in default LUT and requires explicit voltage, PLL, resolution, and power sequencing. This is consistent with the UC81xx being a more "raw" controller that exposes more hardware registers directly, while the SSD1677 ships with sane defaults for most settings.

---

## Sources

- SSD1677 Datasheet, Solomon Systech, Rev 1.0 Nov 2018 — DC Characteristics table, deep sleep current entry ("Cannot retain RAM data"), SW Reset description ("RAM are unaffected by this command"), initialization flow
- UC8179c Datasheet v0.6, UltraChip — POF command description ("register data will be kept until VDD turned OFF or Deep Sleep Mode"), DSLP command
- UC8151d Datasheet v0.6, UltraChip / Adafruit — SHD_N bit description, Deep Sleep Mode section, register retention conditions
- Good Display application note: *Driving Epaper Display with Low Power Consumption* — standby vs deep sleep RAM retention table ("IC data cannot be saved"), GDEQ0426T82 product page (confirms SSD1677)
- EInkDisplay.cpp firmware source — comments at lines 836–856 (X3 RAM clear rationale), 113–114 (DTM1/DTM2 definitions), 1340–1344 (post-fast DTM1 resync), 1220–1231 (X4 RED RAM sync)
