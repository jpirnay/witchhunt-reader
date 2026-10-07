# E-Ink Controllers

A contributor's overview of the panel controllers this firmware drives: which board carries which,
how the drivers are organised, and the hardware facts that shape the display code. It is not a driver
manual; the driver headers are. Where each controller keeps its previous frame, and what a grayscale
pass leaves behind on each, is in [secondary-buffer-management.md](../secondary-buffer-management.md).
The T5S3 panel has its own reference: [lilygo-t5s3-display-stack.md](../lilygo-t5s3-display-stack.md).

---

## Where the code lives

- `freeink-sdk/libs/display/FreeInkDisplay/`: the display facade `freeink::FreeInkDisplay`
  (`include/FreeInkDisplay.h`). It owns the framebuffers and delegates every panel operation to a
  `PanelDriver` (`src/driver/PanelDriver.h`), one driver per controller in `src/driver/`.
  `include/EInkDisplay.h` is only a compatibility alias (`using EInkDisplay = freeink::FreeInkDisplay;`)
  so that code written against the old name still builds.
- `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h`: the board profiles, and the mapping
  from each `-DFREEINK_DEVICE_*` build flag to the set of drivers that is linked (`FREEINK_DRIVER_*`).
- `lib/hal/HalDisplay.{h,cpp}`: the only firmware code that talks to the facade. Everything above it
  goes through `GfxRenderer`.

---

## Boards, controllers and drivers

| Board | Build env | Panel | Controller | Driver |
|---|---|---|---|---|
| X3 | `default` (ESP32-C3) | 792×528 | UC8253 | `Uc8253X3Driver` |
| X3, newer units | same | same | UC8279d | `Uc8279Driver` |
| X4 | `default` (ESP32-C3) | 800×480 (GDEQ0426T82) | SSD1677 | `Ssd1677Driver` |
| X4, newer units | same | same | UC8179 or UC8279 | `Uc8179Driver`, `Uc8279X4Driver` |
| X4 Pro | `x4pro` (ESP32-S3) | 800×480 | SSD1677, UC8179 or UC8279 | the same three |
| LilyGo T5S3 | `lilygo_t5s3` (ESP32-S3) | ED047TC2, 960×540, 16 grey levels | none: LovyanGFX's `Panel_EPD` drives the parallel panel through the S3 LCD peripheral | `LgfxEpdDriver` |

The `default` binary serves both the X3 and the X4. At boot `HalGPIO::begin()` tells the two apart,
then `freeink::applyXteinkDisplayController()` probes the display bus (a register readback the
SSD1677 does not answer) to find which controller this unit carries; on the X4 Pro
`HalDisplay::begin()` runs the probe. `FreeInkDisplay::selectDriver()` then picks the driver from
`BoardConfig::ACTIVE.displayController` and the panel selection (`setDisplayX3()`).

To find out which controller a unit has: Settings > System Information shows it, and the refresh
waits in the serial log name it: `X3_DRF…` (UC8253), `8279_…` (UC8279d), `refresh` (SSD1677),
`8179_…` (UC8179), `8279x4_…` (UC8279 X4).

The SDK has more drivers (`Ed2208M5Driver`, `M5OfficialDriver`, `It8951Driver`, `PaperMonoDriver`,
`Uc8253MurphyDriver`, `Uc8279cA4Driver`) for boards this firmware does not build for.

---

## The two controller families

### SSD1677 (Solomon Systech)

- Two RAM planes, BW (`0x24`) and RED (`0x26`), 48,000 bytes each.
- A differential (FAST) refresh compares BW, the new frame, against RED, the previous frame. **The
  host keeps RED current:** each FAST writes the new frame to BW and the facade's previous frame
  (`prev`, the secondary framebuffer) to RED. That is why this is the one controller where releasing
  the secondary buffer affects fast refresh.
- HALF and FULL set `CTRL1_BYPASS_RED` (`0x21` = `0x40`), so RED is ignored, and write both planes.
- Refreshes run the built-in OTP waveforms, chosen by the update-sequence byte (`0x22`); the X4
  profile uses `0xFC` for FAST, `0xD7` for HALF and `0xF7` for FULL. A custom 105-byte LUT (`0x32`)
  is loaded only for grayscale.
- BUSY is high while busy.

### UltraChip UC81xx (UC8253, UC8279d, UC8179, UC8279)

- Two RAM planes, DTM1 (`0x10`, "old") and DTM2 (`0x13`, "new"), one framebuffer each.
- **The controller keeps its own baseline:** after every refresh the driver writes the displayed
  frame back into DTM1 (in `displayFinish()`). The host's `prev` is ignored.
- CDI (`0x50`) chooses whether a bank selects per pixel on (old, new) or ignores the old plane.
- Waveforms: UC8253 loads five LUT registers per bank (VCOM, WW, BW, WB, BB at `0x20`-`0x24`).
  UC8179 runs the OTP waveforms but needs an explicit PLL, booster and VCOM bring-up, without which
  it develops no image. The UC8279 drivers load raw OEM banks for grayscale (XTF_AA and
  XTF_PRE_BW_MID, plus XTH4 on the UC8279d).
- Partial-window commands PTL (`0x90`), PTIN (`0x91`) and PTOUT (`0x92`) exist. The drivers use them
  for whole-panel partial refreshes and windowed grayscale preconditioning, not for
  `displayWindow()`. Of the drivers our builds link, only the SSD1677 driver implements a real
  window; the others inherit the base, which refreshes the whole panel with FAST.
- BUSY is low while busy.

The four UltraChip drivers are separate because their bring-up and their banks differ, and so does
the meaning of each refresh mode: HALF, for example, is a different waveform on every one of them.
The per-driver table is in [secondary-buffer-management.md](../secondary-buffer-management.md).

### Grayscale on both families

Both families produce four grey levels from the two planes; a grayscale waveform reads them together.
The firmware writes planes in two encodings (overlay masks for text anti-aliasing, absolute planes for
images), and each driver translates them into its controller's selector pattern. See
[grayscale-aa-rendering.md](../grayscale-aa-rendering.md).

---

## RAM retention and sleep

From the datasheets:

- **SSD1677:** deep sleep (`0x10` + `0x01`) does not keep RAM ("Cannot retain RAM data"); waking needs
  a hardware reset and the full init. The SW reset command (`0x12`) leaves RAM alone. After boot or
  wake the driver promotes the first paint to an absolute refresh, because RED no longer describes
  the glass.
- **UC81xx:** power-off (POF, `0x02`) keeps both planes and the registers until VDD goes away; deep
  sleep (DSLP, `0x07` + check code `0xA5`) does not, and needs a hardware reset to exit. The drivers
  use POF to power the panel down between refreshes when asked, so DTM1 stays valid as the baseline;
  device sleep sends DSLP.

---

## Adding or changing a driver

- Implement `PanelDriver` and report capabilities truthfully: `supportsAsyncDisplay()` must agree with
  what `displayStart()` returns, and `grayscaleCapabilities()` must list only what the driver does.
  Callers branch on these, so changing one changes behaviour
  (see [lilygo-t5s3-display-stack.md](../lilygo-t5s3-display-stack.md), section 7).
- `cleanupGrayscaleBuffers(bus, nullptr)` means "there is no baseline": drop the synced claim and take
  a clean sync on the next push.
- Track greys left on the glass by a gray waveform. Restoring controller RAM does not clear them;
  only a waveform that drives every pixel does (`_grayOnGlass` in `Uc8253X3Driver`).
- In `displayGrayscaleBase()`, treat a Half or Full fallback as a floor, not a hint (`Uc8179Driver`).
- Link the driver in `BoardConfig.h` (`FREEINK_DRIVER_*`), select it in
  `FreeInkDisplay::selectDriver()`, and add the controller to `displayControllerName()` in
  `src/SystemStatus.h`.
- Add host tests next to the existing ones in `libs/display/FreeInkDisplay/test/host/`
  (`run_pro.py`, `run_uc8253_power.py`, `run_uc8279.py`).

---

## Sources

- SSD1677 datasheet, Solomon Systech, Rev 1.0, Nov 2018: DC characteristics (deep sleep current,
  "Cannot retain RAM data"), SW Reset ("RAM are unaffected by this command"), initialisation flow.
- UC8179c datasheet v0.6, UltraChip: POF ("register data will be kept until VDD turned OFF or Deep
  Sleep Mode"), DSLP.
- UC8151d datasheet v0.6, UltraChip / Adafruit: SHD_N bit, Deep Sleep Mode, register retention.
- Good Display application note *Driving Epaper Display with Low Power Consumption*: standby vs deep
  sleep RAM retention ("IC data cannot be saved"); GDEQ0426T82 product page (confirms the SSD1677).
