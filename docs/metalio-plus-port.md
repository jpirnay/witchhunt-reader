# Metalio E-Ink4 Plus port

The `metalio_eink4_plus` target combines WitchHunt 2.54 with the Metalio S31
board and EPDiy driver from FreeInk SDK. The hardware reference is CrossPoint
`feat-metalio-eink4plus` at `ea534874bd60074058b8862c0f57bf59def1f0ee`.
The SDK integration combines `ef99c6c8bd48fb357bf034b303e307acd1a0fe25`
and `99901125a1eee287e120b88d9edc7810fca8b904`.

Build with pioarduino Core 6.2.0. The environment selects Arduino-ESP32 4.0 RC1
and ESP-IDF 6.1. On Windows, set `PYTHONUTF8=1` before running the build.

```text
npm ci
pio run -e metalio_eink4_plus
```

The app artifact is `.pio/build/metalio_eink4_plus/firmware.bin`. Metalio
uses the 14 pt and 18 pt bitmap masters for built-in reader fonts and scales
the other sizes to fit the 5 MiB slot. SD font support is available.

The separate `metalio-plus-build` directory holds the delivered images.
The `DUAL-FULL.bin` image uses the official CloudZao 2.0.81 bootloader and
assets, with CloudZao in `ota_0` as the initial firmware and WitchHunt in
`ota_1`. It adds private WitchHunt NVS in the unused flash tail. This is a
whole-flash image and installing it replaces the internal data on the device.
The two `-app.bin` files identify the exact slot images for the switch tool.

The WitchHunt updater is disabled because the other slot can hold CloudZao.
The CloudZao updater can replace the inactive slot and therefore may replace
WitchHunt. The combined image has been validated offline. The app-only
build reached Home on a physical ESP32-S31 unit in an OTA slot layout.
Serial diagnostics confirmed SDMMC mount, successful EPD refresh calls,
and a nonblank framebuffer. Hardware checks on one unit covered sleep
and short-press wake, touch and bezel keys, and Wi-Fi discovery. The
combined CloudZao image has not been flashed.

The Metalio build uses the same WitchHunt touch UI and default Lyra theme as
the Picco build. Fresh Metalio settings open Home entries and list rows with
one tap. A saved touch activation preference takes precedence.
At boot the app watches the Metalio power key through the input sampler so
the touch event queue resumes once the power key is released.

The bezel Menu key sends Back on tap and defaults to Go Home on hold. The bezel
page keys send Left and Right, independently of the two side volume keys (Up and
Down). The side BOOT/AI key is WitchHunt's Confirm key. Power uses the board's
expander and defaults to Go Home on short press and Sleep on hold. CloudZao
voice and media functions belong to the CloudZao slot.

The 4.7-inch portrait UI (684 × 1216 px) uses 28 px side padding, 78 px minimum
list rows, 84 px tabs, and enlarged touch targets. Lists use the full content
width, and the three mirrored bezel-key hints are hidden across the UI.

The Windows bench script uses esptool 5.6.0. With only `--port` it reports
the current slot and app descriptions. A switch requires the destination slot
and the exact app image installed there. It verifies a byte-for-byte match
before updating one OTA metadata sector and rebooting.

```text
python scripts/metalio_boot_slot.py --port COM3
```

Once the dual image is installed and verified on the hardware, use
`--slot 0 --image <CloudZao app.bin>` to select CloudZao or
`--slot 1 --image <WitchHunt app.bin>` to select WitchHunt from the PC.

WitchHunt uses the optional `witch_nvs` partition for startup preferences
and leaves the other firmware's NVS alone (automatically erasing and
re-initializing `witch_nvs` only if it becomes full or incompatible). When
that partition is absent, startup preferences in NVS are unavailable. Normal
reader state stays on microSD. Sleep / Screen Off parks the panel, card and
light, then enters deep sleep with the expander's wake interrupt armed. On
deep-sleep wake, firmware probes the expander's Power key early (before
running the 300 ms touch reset) so spurious wakes return to sleep immediately
without swallowing short Power presses. If wake cannot be armed, firmware waits
for Power and restarts. Light sleep is disabled on the S31 RC toolchain
because the FreeInk bring-up reported PSRAM corruption after wake. Shared I2C
access between `HalI2cBus` and the SDK's board/PMIC/haptic helpers is
serialized through one adopted recursive mutex.

## Implementation Notes

- **Horizontal swipe page turns**: Swiping left/right turns one page along the
  book's reading direction on Metalio. Edge taps still turn pages in tap mode,
  and disabling touch page controls disables swipes as well.
- **Home & touch-first navigation**: Home applies the selected theme immediately
  with direct touch targets across Classic, Lyra, Lyra Tiles, and Lyra Carousel.
  Lyra Carousel uses a responsive multi-row menu grid with labels below the
  icons. Settings provides a header Back button, live UI font reload, and
  left-edge inward swipe to return from menus and sub-screens.
- **Power key & sleep/wake**: Short-pressing Power defaults to Go Home (restored
  from saved settings on boot) while holding Power enters Sleep. Deep sleep
  keeps the wake interrupt circuit powered so a short press of Power wakes the
  device back to Home.
- **Frontlight drawer**: Driven at 2 kHz / 14-bit PWM with constant-luminance
  CCT mixing and a 10% duty cap. Opens at the bottom of the screen via a
  top-edge swipe or the sun icon on the EPUB reader bar. Dragging brightness or
  warmth updates the light live and saves on finger release, with a Back button
  to dismiss.
- **Haptic feedback**: The Metalio vibration motor pulses on screen touches and
  capacitive bezel-key actions.
- **TJpgDec ROM symbol collision fix**: Boot loops when decoding JPEG covers
  were caused by ESP32-S31 ROM symbols colliding with TJpgDec functions that
  have a different ABI. Renaming the conflicting TJpgDec symbols resolved cover
  decoding on hardware.
- **UI font sizing**: Normal UI font size on Metalio uses Inter 12 pt for items
  and values and 14 pt for titles.
- **EPUB KOReader Sync**: Selecting Sync in the EPUB reader menu opens KOReader
  settings when no account is configured, and shows pull, push, and compare
  actions once credentials are saved.
