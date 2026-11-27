# Experimental boards

Status: open items as of 2026-11-27, when the Xteink X4 Classic, the Seeed reTerminal Sticky and the EEGO Reader A4
were added as experimental boards. None has been run here. The gaps below come from comparing our reader code with
upstream CrossPoint's (`upstream/develop`, 2026-11-27) and from reading the SDK profiles; none was seen on a device.

Background: [Experimental boards](../contributing/board-support.md#experimental-boards).

## Graduating a board

A board stops being experimental after a device run: boot to Home, a book opens and turns pages, sleep and wake by
the Power key, the battery reading, Wi-Fi, an OTA or SD update, and touch in all four orientations where the board
has touch. Then its `[<board>_board]` block moves above the "EXPERIMENTAL BOARDS" line in `platformio.ini` without
`-DCROSSPOINT_EXPERIMENTAL_BOARD=1`, and its `experimental` marks go from `.github/workflows/release*.yml` and
`flasher/lib.js`. The README table and [Board Support](../contributing/board-support.md) move it to the supported
boards. Ask a user with the board for the run, or for a unit.

## EEGO A4: the rest of upstream's board code

The three fixes the board needs most are in (the active-high Power key, the panel powered down when idle, the Wi-Fi
TX cap). Upstream has about fifty more A4-specific lines in its reader code:

- **The SDK's A4 display work.** Upstream's SDK pin (`de19f154e`) is 51 commits ahead of ours (`758755bd7`), not
  counting merges; 18 of them change the A4's UC8279C driver: a three-level gray mode in one pass, idle in light sleep rather than deep sleep,
  the driver timing its own power-off 500 ms after a refresh, a refresh idle hook for text page turns, gray buffers
  from PSRAM. It comes with the next SDK resync ([For the next SDK resync](display-and-refresh.md#for-the-next-sdk-resync));
  after it, check whether `HalDisplay::powerOffPanelBetweenFrames()` should still call `controllerIdle()` itself.
- **Gray reader pages.** Upstream's `EpubReaderActivity` combines the base for image pages on the A4, draws clipping
  highlights and the status bar into the gray planes, and asks for a full refresh after a menu closes over the page;
  `ActivityManager` gives Home a clean first refresh after the reader. These depend on the driver above.
- **A foreground build keeps the panel powered.** The idle power-off skips a busy render lock. Upstream calls
  `controllerIdle()` between section build slices for that reason; check whether our first-open build holds the lock
  that long on the A4.
- **Power key timing.** Upstream wants a wake press held 400 ms past boot and a long press 400 ms longer on the A4
  (`CrossPointSettings::getPowerButtonDuration()`, its `main.cpp` wake gate), and defaults double Power to the
  frontlight.
- **Panel shape.** The profile carries `cornerRadius`, `bottomCornerRadius`, `headerOffsetY`, `statusBarOffsetY` and
  the tab-bar fields; we read only `viewableInsets`. Upstream also draws a slimmer reader progress bar and keeps the
  keyboard's and Home's tap targets off the rounded corners.
- **Factory UID** in About (`HalSystem`, upstream #4520), which EEGO's own service uses.

## X4 Classic

- **Button hints.** `ButtonGeometry::forActiveBoard()` has no layout for it, so the hints fall back to an even strip.
  Its keys are the X4 Pro's two side keys plus the X4's four bottom keys (the SDK profile); upstream draws side hints
  as on the X4 Pro (`hasEdgeSideButtons()`).
- **Overlay refreshes.** Upstream treats it as an X4-family panel in the reader (`xteinkClassPanel()`): chrome drawn
  over a fresh page gets the HALF ghost clean-up, and closing re-renders the page.

## Sticky

- **The flasher.** Its USB port is a WCH bridge (vendor ID 0x1A86), which the page's port filter hides. Listing the
  board needs that vendor ID for it, and a check that esptool-js's classic reset reaches the ROM loader through the
  bridge.
- **Serial test API.** It reads the native USB port; on the Sticky it would have to read `Serial0`
  (`BoardConfig::serialTransport()`).
- **The SD card on the panel's SPI bus.** The profile marks this as inferred, not exercised by Seeed's demo. Our
  `HalSpiBus` lock covers it as on the C3 boards; a first device run should open a book while pages turn.
