# Boards and touch

Status: open items as of 2026-10-07; the last three sections added 2026-11-15, from the upstream triage. Collected from multiboard-bringup-handover-2026-08-15.md, multi-board-bringup-2026-08-14.md, x4pro_support.md and touch-input-migration-2026-08-14.md.

Background for all of these is in [Board Support](../contributing/board-support.md) and
[Touch Architecture](../contributing/touch-architecture.md). Display items from the same sources
(the LGFX write-buffer copy before `displayGrayBuffer()`, the pre-render waiting for the deferred
AA pass, the PanelSel/baseline-model split) are in [Display and refresh](display-and-refresh.md).

## Light sleep with the frontlight

Open: the X4 Pro and the T5S3 never take idle light sleep. `HalPowerManager::lightSleep()` declines on
a board with a PWM frontlight while the light is lit, or while its touch cannot wake the chip (counted in
`rejFrontlight`). Their GT911 is polled and keeps only its latest frame, so a tap shorter than a 50 ms
slice would be lost asleep.

Why it matters: idle light sleep is the main idle-power saving. On Home with no sleep timeout and the
light off, a Murphy that could not light-sleep lost about 40 % of its charge in 10 h (user, 2026-11-16).

Done (2026-11-17, Murphy M3): with its light off the Murphy light-sleeps. Its CHSC6x data-ready line is
armed as a low-level wake (`InputManager::armTouchPulseWake()` in the SDK), with the pin's edge
interrupt switched off while the level type is set. Without that wake a test build missed about every
second tap. On battery, Home, light off: light sleep 26 min 11 s of 27 min 17 s (96 %); the battery drain
per hour was not measured.

The 2026-11-16 restarts were not the touch line. The idle loop runs on core 1, and IDF's dual-core
FreeRTOS winds pended ticks down only on core 0, so the ticks each slice handed to `xTaskCatchUpTicks()`
piled up until core 0 next resumed its scheduler. A flash operation does that
(`spi_flash_op_block_func()`), and then wound every tick since in one critical section: an interrupt
watchdog on CPU0 with CPU1 in `esp_partition_read()`, twice, each time on the first flash read after
light-sleeping (System Information, Boot Diagnostics). `catchUpTicksOnCore0()` now hands the ticks to
core 0 each slice. The Picco takes the same path (dual-core S3, no PWM light), so by the code it had
the same latent bug; no Picco restart was traced to it.

Next: (1) the X4 Pro and T5S3 need a touch wake first; the SDK's GT911 INT wake
(`FREEINK_GT911_INT_WAKE`, `InputManager::touchWakeIrqPin()`) is the candidate. (2) A lit light:
crosspoint-reader [PR #3032](https://github.com/crosspoint-reader/crosspoint-reader/pull/3032), with
[Free-Ink/freeink-sdk#39](https://github.com/Free-Ink/freeink-sdk/pull/39), clocks the frontlight LEDC
from RC_FAST with KEEP_ALIVE so the PWM survives light sleep (`FREEINK_FRONTLIGHT_LS`; the submitter
measured 92 % residency with the light on, X4 Pro). Our SDK carries the driver half. CrossPoint later
reverted its idle light sleep (#3077) and dropped the flag from the X4 Pro (#3289). RC_FAST (~17.5 MHz)
cannot drive the Murphy's 25 kHz at 10 bits, so the Murphy needs about 17 kHz or 9 bits first, or the
light stays dark. Setting the flag also needs `park()` before deep sleep and `releaseOnWake()` before
`Frontlight.begin()`.

Where: `HalPowerManager::lightSleep()`, `HalFrontlight`, the SDK's `InputManager` (touch wakes) and
`FrontlightManager`. The comment beside the `lightSleep()` fallback in `main.cpp`'s idle loop lists the
reasons it declines.

## SD clock on the T5S3

Open: `HalStorage::begin()` holds SD-over-SPI at 20 MHz on non-C3 boards whose profile leaves
`sd.spiHz` at 0, instead of the SDK's 40 MHz default. Today that is only the T5S3; the X4 Pro
mounts over SDMMC.

Why it matters: if the clamp is not needed it halves SD throughput on the T5S3.

Tried: nothing; the clamp was a precaution. The SD card may have worked at bring-up because
`BoardT5S3::begin()` deselects the LoRa radio on the same bus, not because of the lower clock.

Next: one T5S3 build at 40 MHz, then a book open, a section build and a USB-drive transfer. Keep
the clamp only if mounts or reads fail.

## BUTTON_TRACE scaffolding

Open: `-DBUTTON_TRACE=1` is commented out in `[lilygo_board]`, but `[env:murphy]` builds with it
(`platformio.ini:608`), so it is a live diagnostic there. Its code is in `HalGPIO::sampleOnce()` (raw
button, Home key and touch-release lines), `GestureEventManager`, `TouchGestures.h` and
`ActivityManager.cpp`, and its touch-edge state (`touchTraceWasHeld_`, `touchTraceDownNx_` and friends)
sits unguarded in `HalGPIO.h`. The `[lilygo_board]` comment still says to drop it once the board's input
is settled.

Why it matters: the Murphy build uses it as a diagnostic, while its one comment calls it a bring-up aid
to drop.

Next: decide. To keep it, reword the `platformio.ini` comment the way the `LGFX_EPD_PUSH_TRACE`
comment beside it is worded and check that a trace build compiles;
[Touch Architecture](../contributing/touch-architecture.md#debugging-touch) already describes the
output. To drop it, delete the trace blocks in those four files and the state in `HalGPIO.h` in
one commit.

## FREEINK_X4PRO_FAST_DU_SHORTCUT

Open: the SDK's opt-in fast-DU refresh for the X4 Pro (`Ssd1677Driver.cpp`) is not enabled in
`[x4pro_board]`.

Why it matters: it saves about 85 ms per refresh. It is off because it skips the per-refresh
temperature load and power sequencing, and the C3 X4's equivalent (`FREEINK_X4_FAST_DU_SHORTCUT`,
also off in `[c3]`) produced ghosting and blotching over long sessions on some panels upstream.

Constraint: it applies only to SSD1677-batch X4 Pros. UC8179 and UC8279 batches use other drivers
and never reach it, and the X4 Pro on the bench is a UC8179 unit.

Next: an SSD1677-batch X4 Pro and a long reading session across temperatures, before enabling it.

## Device validation

Open: parts of the touch layer and of the X4 Pro have no recorded device pass.

- The touch defaults in [Touch Gestures](../touch-gestures.md) have had one hardware session, on a
  T5S3 (2026-09-11). It found four faults, all fixed since. Whether the either-direction ten-page
  swipe is now reliable, and whether the gesture overview's labels fit legibly, were not checked
  again.
- The X4 Pro has run this firmware since the pre-flight fixes (PR #246, 2026-09-12), and display
  work has been tested on it since. Earlier plans said it was deliberately not flashed; that hold
  ended with PR #246. There is still no recorded pass for its gesture set (the warm/cool right edge
  column exists only there), two-finger gestures on its GT911, or the input sampler's stack
  high-water with its GT911 (the 4096-byte stack was measured only on the T5S3).

Why it matters: host tests cover geometry, not what a thumb can reach or what a screen shows. The
first T5S3 session found four faults that 880 host tests and two board builds had passed.

Next: one session per touch board against the tables in [Touch Gestures](../touch-gestures.md)
and the on-device Gesture overview, built with `BUTTON_TRACE`; on the X4 Pro also read the
`btnSampler stack high-water` line.

Device checks still owed for the 2026-10-21 PRs (all X3-only):

- T5S3: cache clear (#538, touch answers and label fit in long languages), auto-pull open (#539),
  reader menu and page saves (#540), heap-restart latch (#541), header layout (#543, Lyra and the
  gesture overview's subtitle), and the OPDS temp-cover cleanup (#534, step 7).
- X3: the Home open that builds a chapter on the book-open HALF (#535), the Library path and Settings
  Keep/Reset of the cache clear (#538), the held-key steps of the password editor (#537).
- X4: Base's portrait side-box gap and the X4 keyboard in landscape (#543).

Device checks still owed for the faster cold opens (#789, #792, 2.53; measured on the X3 and the X4 Pro only):

- X4: it builds resident, so it keeps the first open's Indexing popup for the chapter build (#792).
- T5S3, Murphy M3, BOOX Picco: the PSRAM build region (#789) and the book indexed while the popup paints (#792);
  on the Murphy and the Picco also the SDK's SDMMC read-ahead (freeink-sdk cf63426).

Test API gap that blocks some of these: no `CMD:HEAP:REFUSE` field that refuses the secondary-buffer
realloc (it would make the fragmented-heap restart reachable). Held keys are there since #741
(`CMD:BTN:<key>:<ms>`, [Serial Test API](../serial-test-api.md)).

## Localized tap flash

Open: screens on the recorder path get no tap acknowledgement except the selection repaint
(`ActivityManager::dispatchListTap()`, `Selected` branch). Those are the screens still on
`ListTouchBand`: the XTC chapter selector, the Markdown table of contents, footnotes,
the OPDS browser, Wi-Fi selection, global bookmarks, starred pages and KOReader sync
(`ButtonRemapActivity` is excluded on purpose). The EPUB chapter selector moved to `UiListActivity`
in #593 (2026-10-31, `eace39655`).

Already done elsewhere: screens on FreeInkUI (`UiListActivity`, `TabbedUiListActivity`,
`UiAppHost`) get FreeInkUI's tap flash, which paints the tapped element gray in the refresh that
shows the result (`FreeInkApp::route()`).

Ruled out: a flash with its own partial refresh bounded to the row. The SDK primitive it would have
used, `ListNav::rowRectFor()`, was removed upstream, and a windowed refresh on the SSD1677 and
UC8279 costs nearly as much as a full one (measurements in
[Display and refresh](display-and-refresh.md#windowed-refresh-capability-query-alignment-baseline)).

Next: move the remaining band screens onto `UiListActivity`, which brings the flash with it,
rather than building a second flash for the recorder path.

## Ten-page skip stops at a chapter boundary

Open: `EpubReaderActivity::jumpPages()` calls `stepPageStateLocked()` up to ten times. Crossing
into the next or previous spine item resets `section`, and the following step returns false
because there is no section, so "skip 10 pages" stops at the chapter boundary.

Why it matters: the outer-third swipe defaults (`gestSwipeInLeftZone`, `gestSwipeInRightZone`)
and the double-press button defaults (`btnDoubleLeft`, `btnDoubleRight`) all use these actions.

Ruled out: it is not why the ten-page swipe failed on the left side on the T5S3; that was
ergonomic, and the swipe now accepts either direction.

Next: carry the remaining count across the spine (for example, resolve a page offset once the new
section has loaded), or declare the stop intended and say so in
[Touch Gestures](../touch-gestures.md). Only the EPUB reader stops: the TXT, Markdown, XTC and line
readers' `BTN_PAGE_*_10` handlers move a whole-document page index by ten and clamp.

## No tappable scroll bar on lists

Open: no list has a tap target on its scroll indicator. Paging by tapping beside the thumb
(`ListScrollBar`, `ActivityManager::dispatchScrollBarTap()`) was removed when the lists moved to
FreeInkUI (cabc632e0, 2026-09-14). FreeInkUI's `list()` registers only rows as hit targets, and the
lists `BaseTheme` still draws show overflow arrows (`drawListOverflowArrows()`) with no target. Paging
by touch is a vertical swipe or a hint box; neither [Touch Gestures](../touch-gestures.md) nor the
user guide offers a scroll-bar tap.

Why it matters: the T5S3 has no Up key, and a scroll-bar tap was the most direct way back up a long
list.

Next: decide whether to add a scroll-track interaction to FreeInkUI's list (an SDK change that would
cover every FreeInkUI list).

## Long-press discoverability

Open: nothing on screen tells a touch user that a hint box can be held. A long tap on a hint box is
the only way to hold Back, Confirm, Left or Right on the touch boards
(`ActivityManager::dispatchHintStripTap()`), and some actions exist only as a hold, for example
long Back to go Home from a list screen (`UiListActivity`).

Partly done: `FileBrowserActivity` names most of its hold actions in the "short / long" form:
"» / Options" (`STR_LIST_PAGE_NEXT_OR_OPTIONS`) where Options is the hold of Right, "Right / Options"
in the cover grid, and in the Library "Up / Tab" / "Down / Tab" on the side hints
(`STR_DIR_UP_OR_TAB`, `STR_DIR_DOWN_OR_TAB`). The gesture overview marks taps and holds for the
reader zones. The browser still has two unnamed holds: long Back goes Home from a Books folder or an
opened author (the box says Back), and long Confirm on an EPUB opens it with a KOReader pull when
credentials are set (the box says Open).

Next: apply the browser's "short / long" form wherever a screen has a hold-only action.

## BOOX Picco: after the first build

Status: merged as #769 (2026-11-14), on the SDK pin of the #771 resync, which brought Free-Ink main's Picco display
fixes (grayscale LUTs, SSD2677 night mode, dark AA pixels on the SE0400NQW47, first-paint clearing, inverted gray
overlay). Notes from 2026-11-13: a retail unit's flash is dumped, and the `picco_stock` build runs on
it on Onyx's table: boot to Home in about 1 s of setup, SSD2677 with an SE0400NQW47 panel, TMA525C reporting two
contacts, charger configured, RTC and IMU found, the light present (`Frontlight up`), the flash area taken as the
PSRAM image beside the 512 KB `spiffs`, no System Update rows, the Slide Switch row and double Power = Back as
defaults (serial log, screenshots, `GET`). Tried by hand on the unit: it turns on, the screen is upright, taps work,
and the slide switch turns the light on and off. Board notes:
[Board Support](../contributing/board-support.md#onyx-boox-picco).

- **Validate on the device**: touch in the other three orientations (the SDK profile sets `swapXY` and `flipY`),
  Power / Up / Down and the double-Power Back, the light's warmth and that it is off in deep sleep, the battery
  reading, USB Drive, deep sleep and wake; then the full install on our table.
- **A boot-log error** on that unit, cause not looked at: `gpio_isr_handler_remove(): GPIO isr service is not
  installed`. (The other one, `i2c_master_transmit_receive failed: ESP_ERR_INVALID_STATE` right after `Charger
  configured`, was the tilt driver probing the X3's IMU address 0x6B first; the IDF reports that NACK as
  INVALID_STATE. Gone since tilt reads through the SDK's `Imu`, which asks the profile's 0x6A first.)
- **Updates on the stock table.** Onyx's recovery installs `/update/firmware.bin` from the card. An action that erases
  otadata and restarts would hand the stock-table Picco to it, the way the stock app's Power + Volume Up does.
- **USB presence from the charger.** The charging bolt and "Charging" now ask the charger, so a wall charger shows
  them (`HalPowerManager::isExternalPowerPresent()`). `HalGPIO::isUsbConnected()` still sees the host link only:
  upstream asks the charger there too, which would also change light sleep, the serial-log gate and the USB wake on
  the Picco and the T5S3. Decide with the sleep item below.
- **Sleep on USB power.** Upstream keeps the Picco in light sleep while USB powers it, with a charge strip on the sleep
  screen, because deep sleep cannot see the charger; and it powers on when a cable is plugged in, since that pulses
  the wake line. Here the wake gate rejects that wake and the device sleeps on. Decide after seeing it.
- **GPIO5.** The SDK carries it as Confirm; what drives it on the device is not known.
- **Slide switch and touch page turns.** Upstream's third binding. Here Touch Page Turn has four values, so "on" needs
  a remembered mode.

## Motion sensor: power and the SDK gating

Status: tilt page turns and Auto-Rotate read the IMU through the SDK's `freeink::Imu` since #794 (2.53), on the X3
and the BOOX Picco.

- **Auto-Rotate keeps the gyroscope awake.** While it is on, the reader keeps the accelerometer and the gyroscope
  running: the SDK `Imu` has no accelerometer-only mode, and the orientation needs only the accelerometer. The current
  this draws is not measured. Next: measure it on the X3 with Auto-Rotate on and off; if it matters, add an
  accelerometer-only mode to the SDK.
- **The driver gating upstream.** `FREEINK_IMU_<PART>` (SDK pin ef99c6c) compiles only the IMU drivers a build's
  boards use; without it the C3 image grew by 10.6 KB. Offered to Free-Ink as #217, open.

## USB attach during light sleep on the X4 (CrossPoint #3082)

Open, unproven here: upstream reports an X4 that freezes when USB is plugged in while it idles in light sleep, and
reverted its light sleep; ours still sleeps in slices. `HalPowerManager::lightSleep()` arms only the timer and the
power button (`lib/hal/HalPowerManager.cpp:277-283`, no `usbDetect` wake) and keeps the GPIO13 hold on while running,
on purpose (the comment at `:285-298`: a release between slices can drop the pad).

Next: a device test first: plug USB into an idle X4 at a reader page, ten times. If it ever freezes, take only the PR's
second commit (arm `BoardConfig::ACTIVE.usbDetect` as a level wake, disarm after); its first commit, dropping the hold
after wake, contradicts our documented reason. crosspoint-reader #3082, Brian Pugh (@BrianPugh); its author had no X4.

## Symbolized crash traces (CrossPoint #4477)

Open: `/crash_report.txt` (written at the boot after a captured panic, `HalSystem::checkPanic()`,
`lib/hal/HalSystem.cpp:185`) holds the stack as raw hex words, which need the build's ELF to read. Upstream writes
human-readable stack traces from a compressed symbol table in the image.

Constraint: the table's flash cost is unknown, and on the C3 each 64 KB of app growth costs the font cache one MMU
page. Next: measure the table for `default` before porting. crosspoint-reader #4477, David Boring (@david-boring).

## Button cursor on touch boards (matcha-reader #719): decide

Lists always draw the selected row (`UiListActivity`), also on the touch boards (X4 Pro, T5S3, Murphy, Picco). matcha
hides the list and tab cursor until a button is pressed, and again on the next touch (one static flag in
`MappedInputManager`; its #791 keeps the list from jumping without a cursor). UX taste, no bug. Decide whether to take
it. matcha-reader #719, Eszter Schuffert (eszter007).
