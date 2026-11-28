#pragma once

#include <BoardConfig.h>

// Ask what the board CAN DO, not what it IS.
//
// Why this file exists (see docs/contributing/board-support.md,
// "Ask what the board can do"):
//
// `HalGPIO::deviceIsX3()` has ~38 call sites and is a stand-in for at least six
// unrelated questions — is there a hardware RTC, is there an IMU, is the battery
// read over I2C or ADC, does the panel need a half-refresh settle, how is USB
// presence detected, and where are the physical buttons. That works only while
// exactly two boards exist and every question happens to correlate.
//
// It breaks silently on a third board. On X4 Pro `deviceIsX3()` returns false,
// so every one of those questions takes the **X4** branch by default. The board
// does not fail loudly; it is subtly wrong in six places that each look like an
// unrelated bug. Widening `DeviceType` to {X3, X4, X4Pro, LilyGo} multiplies the
// conflation by four rather than removing it.
//
// So each predicate below answers ONE question, from the active board profile.
// Adding a board then means filling in a profile, not editing call sites.
//
// The call sites have moved over: board questions to the predicates here, panel
// questions to the SDK's display (GfxRenderer::fastRefreshNeedsHostFrame(),
// asyncRefreshHoldsFrameBuffer(), ghostsUnderFastRefresh()), key placement to
// ButtonGeometry. Each answers as `deviceIsX3()` did on every shipped board.
//
// `deviceIsX3()` itself stays for the one question it IS: which C3 board this is.
// The dual X3+X4 binary's runtime detection in HalGPIO::begin() sets it, selects
// the SDK profile from it, and HalDisplay::begin() hands it to the panel driver.
// Nothing else should ask.
namespace HalCapabilities {

// --- Sensors -----------------------------------------------------------------

// A battery-backed real-time clock is present. X3 has a DS3231, X4 Pro a BM8563;
// X4 has none and falls back to the ESP32 RTC across deep sleep.
inline bool hasHardwareRtc() { return BoardConfig::ACTIVE.sensors.rtcType != BoardConfig::RtcType::None; }

// Which RTC, for the register/address differences. DS3231 is 0x68, the
// PCF8563-compatible BM8563 is 0x51 — same bus, different protocol, so a caller
// that only checks "is there an RTC" would talk nonsense to the wrong one.
inline BoardConfig::RtcType rtcType() { return BoardConfig::ACTIVE.sensors.rtcType; }

// An IMU is present, so tilt gestures are physically possible. X3 carries a
// QMI8658; X4 and X4 Pro carry nothing.
inline bool hasTiltSensor() { return BoardConfig::ACTIVE.sensors.imuType != BoardConfig::ImuType::None; }

// --- Battery -----------------------------------------------------------------

// Charge comes from an I2C fuel gauge rather than an ADC divider. X3 has a
// BQ27220, X4 Pro a CW2017; X4 reads an ADC pin. These are mutually exclusive
// and each needs a completely different read path.
inline bool hasI2cFuelGauge() { return BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0; }

// Charge comes from an ADC pin. Note this is not simply !hasI2cFuelGauge(): a
// profile may legitimately have neither, in which case there is no battery
// reading at all and the UI should not pretend otherwise.
inline bool hasAdcBattery() { return !hasI2cFuelGauge() && BoardConfig::ACTIVE.batteryAdc >= 0; }

// Any battery reading is available, by either route.
inline bool hasBatteryReading() { return hasI2cFuelGauge() || hasAdcBattery(); }

// --- Power and USB -----------------------------------------------------------

// The power button closes a battery MOSFET that a latch pin then holds (the X4's
// GPIO13; PowerConfig::latch0GatesBattery). So being powered at all means the
// button was pressed, and driving the latch LOW is the battery power-off. The
// X4 Pro also carries a latch0, but it keeps a peripheral rail up, so it is not
// this; the X3 has no latch at all, and its GPIO13 is the SD rail enable.
inline bool hasButtonGatedBatteryLatch() {
  return BoardConfig::ACTIVE.power.latch0GatesBattery &&
         BoardConfig::ACTIVE.power.latch0 != BoardConfig::PIN_UNASSIGNED;
}

// USB presence has to come from the fuel gauge's charge current: the board has a
// BQ27220 to read, no charger IC that could report the input rail, and no pin
// that reads the cable. That is the X3. Its GPIO20 is the gauge's I2C data line,
// which reads HIGH from the bus pull-ups whatever the cable does; the profile
// used to list it as usbDetect anyway and now leaves usbDetect unassigned
// (Free-Ink edcc629), so both forms count as "no pin".
//
// The charger test is what keeps the T5S3 out. It has a BQ27220 and no usbDetect
// either, but its BQ25896 answers for the rail, and a cable inferred from charge
// current would close the serial-log gate on a full battery (main.cpp).
//
// Reading the gauge is an I2C transaction, so it is polled on a throttle rather
// than every loop like a pin.
constexpr bool usbPresenceFromFuelGauge(const BoardConfig::BoardProfile& p) {
  const bool noDetectPin = p.usbDetect == BoardConfig::PIN_UNASSIGNED || p.usbDetect == p.batteryGauge.i2cSda;
  return p.batteryGauge.gaugeAddr != 0 && p.batteryGauge.gaugeType == BoardConfig::GaugeType::Bq27220 &&
         p.batteryGauge.chargerAddr == 0 && noDetectPin;
}
inline bool usbPresenceFromFuelGauge() { return usbPresenceFromFuelGauge(BoardConfig::ACTIVE); }

// A charger IC reports whether power comes in (the Picco's SGM41562, the T5S3's BQ25896). On these boards it is the
// only thing that sees a wall charger: no detect pin, and a charger enumerates no USB host link.
inline bool chargerReportsInputPower() { return BoardConfig::ACTIVE.batteryGauge.chargerAddr != 0; }

// Pinned against the shipped profiles, so an SDK resync that changes any of the
// fields above fails the build here instead of silently turning the X3's
// charging indication and USB wake handling off.
static_assert(usbPresenceFromFuelGauge(BoardConfig::XTEINK_X3) &&
                  usbPresenceFromFuelGauge(BoardConfig::XTEINK_X3_UC8279),
              "X3: USB presence must come from the BQ27220");
static_assert(!usbPresenceFromFuelGauge(BoardConfig::XTEINK_X4) &&
                  !usbPresenceFromFuelGauge(BoardConfig::XTEINK_X4_PRO) &&
                  !usbPresenceFromFuelGauge(BoardConfig::LILYGO_T5S3),
              "only the X3 infers USB from the fuel gauge");

// --- Display -----------------------------------------------------------------

// A genuine silicon quirk, and the one case B0 allows to key on the controller:
// the UC8253 needs extra settle passes after a half refresh or it ghosts. This
// is a property of the panel controller, not of "being an X3" — a future UC8253
// board would need it too, and an SSD1677 X3 variant would not.
inline bool panelNeedsHalfRefreshSettle() {
  return BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8253;
}

// The panel visibly fades in direct sunlight unless it is powered down between
// refreshes, so the "sunlight fading fix" is worth offering here.
//
// Recorded per board rather than derived: it is a property of the glass and the
// enclosure, not of the controller, and it is not something the firmware can
// probe. Mirrors SettingRequires::SunlightFadingPanel.
inline bool panelFadesInSunlight() { return BoardConfig::ACTIVE.panelFadesInSunlight; }

// --- Input / chrome ----------------------------------------------------------

// A touch panel is present. Thin alias over the SDK predicate so call sites read
// consistently with the rest of this header.
inline bool hasTouch() { return BoardConfig::hasTouch(); }

// The board's touch controller can report more than one contact, as its profile says: the
// controllers InputManager::supportsMultiTouch() can say yes for. HalGPIO::supportsMultiTouch()
// is the answer for this boot (the TMA525C's own GET_SYSINFO gives its finger limit), and this
// must stay a superset of it. boardKeeps() persists the two-finger gesture rows by this answer,
// so a controller missing here gets rows the settings screen offers and settings.json neither
// saves nor loads: the Picco's TMA525C, from the SDK's multi-touch support (Free-Ink 2264d13)
// until it was named here.
inline bool hasMultiTouch() {
  switch (BoardConfig::ACTIVE.touch.controller) {
    case BoardConfig::TouchController::Gt911:
    case BoardConfig::TouchController::Gslx680:
    case BoardConfig::TouchController::Tma525c:
      return true;
    case BoardConfig::TouchController::None:
    case BoardConfig::TouchController::Chsc6x:
    case BoardConfig::TouchController::Ft5x06:
    case BoardConfig::TouchController::Ft6336u:
    case BoardConfig::TouchController::Cst816s:
      return false;
  }
  return false;
}

// The touch controller reports a capacitive Home key (X4 Pro). On that board it
// is the only "back" the hardware has — there is no back button.
inline bool hasHomeKey() { return BoardConfig::hasHomeKey(); }

// Dedicated Back and Confirm buttons exist in hardware.
//
// This is the one that bites hardest on X4 Pro: its InputPins are
// {back=-1, confirm=-1, left=-1, right=-1, up=0, down=7, power=3}, i.e. two nav
// keys and power, with back and confirm coming from the GT911 (touch plus the
// capacitive Home key). Any UI that assumes a Back button — button hint strips,
// "press Back to cancel" prompts, the escape path out of a modal — has no
// hardware to bind to there. Note every supported board has SOME button
// topology, so InputStyle has no None value; presence is the wrong question and
// this is the right one.
inline bool hasBackAndConfirmButtons() {
  const BoardConfig::InputPins& in = BoardConfig::ACTIVE.input;
  return in.back != BoardConfig::PIN_UNASSIGNED && in.confirm != BoardConfig::PIN_UNASSIGNED;
}

// Left, Right, Up and Down are keys of their own (X3, X4): a screen laid out in rows and columns can
// move along a row with one pair and between rows with the other. A board with only Up and Down
// (X4 Pro, T5S3, Murphy, Picco) steps through such a screen as a list.
inline bool hasFourDirectionKeys() {
  const BoardConfig::InputPins& in = BoardConfig::ACTIVE.input;
  return in.left != BoardConfig::PIN_UNASSIGNED && in.right != BoardConfig::PIN_UNASSIGNED &&
         in.up != BoardConfig::PIN_UNASSIGNED && in.down != BoardConfig::PIN_UNASSIGNED;
}

// The physical button topology, for the places that genuinely need to know the
// arrangement rather than a yes/no capability.
inline BoardConfig::InputStyle inputStyle() { return BoardConfig::ACTIVE.inputStyle; }

// The front strip's Left and Right are touch targets on the panel rather than keys in the case, so
// they turn with the screen while the real keys stay Up and Down (Murphy).
inline bool frontStripIsVirtual() { return inputStyle() == BoardConfig::InputStyle::DigitalFiveKey; }

// True when the SD card is reached over SPI, false when the board drives the
// slot through the native SDMMC peripheral (X4 Pro, X4 Classic).
//
// Everything in the SdPins struct -- spiHz, cs, the shared-bus locking -- is
// dead configuration on an SDMMC board, which mounts through SdmmcBlockDevice
// and never touches those pins. Code that tunes or reports the SPI path must
// ask this first, or it ends up announcing an SPI clock on a board that has no
// SD SPI bus.
inline bool sdUsesSpi() { return BoardConfig::ACTIVE.sdmmc.busWidth == 0; }

// The MCU pin the ROM samples at reset to choose boot vs. download mode. Holding
// it LOW while the chip comes out of reset enters ROM download mode, so firmware
// never runs and no boot-time key combo on that pin can ever be observed.
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
inline constexpr int8_t BOOT_MODE_STRAP_PIN = 9;
#else  // ESP32 / ESP32-S2 / ESP32-S3
inline constexpr int8_t BOOT_MODE_STRAP_PIN = 0;
#endif

// True when this board wires its Up key to that strapping pin, so a boot-time
// combo must use a different key. The X4 Pro and X4 Classic both put Up on
// GPIO0; on those, "hold Up + power" drops the device into ROM download mode
// instead of into the firmware check the user was aiming for, which looks
// exactly like a dead device. Derived from the profile rather than a board list,
// so the next board that reuses the strap is covered without an edit.
//
// The InputStyle test is load-bearing, not defensive: on the X3/X4 ladder the
// InputPins fields hold LADDER INDICES, not GPIO numbers (X4's tuple is
// {0,1,2,3,4,5,...}), so comparing input.up against a pin number there is a
// category error that happens to be false today only because no index reaches 9.
inline bool upKeyIsBootStrap() {
  return BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder &&
         BoardConfig::ACTIVE.input.up == BOOT_MODE_STRAP_PIN;
}

// Chrome scale factor for finger-sized targets. 1.0 on button boards, 1.2 on the
// touch boards. Read by ThemeMetrics in touch phase 5.
inline float uiScale() { return BoardConfig::ACTIVE.uiScale; }

// FAST ghosts badly enough on these panels that the UI cannot rely on each screen's own cleaning:
// every activity change gets a HALF and every screen follows the refresh frequency setting, the
// way the reader's pages do (ActivityManager::armGhostClearingRefresh()). Murphy's UC8253, and the
// T5S3's ED047TC2, whose fast bank drives only the pixels whose target changed: photographed
// 2026-11-13, Settings opened over Home showed the cover through its rows, and Home the Settings rows.
// Not the same question as the SDK's ghostsUnderFastRefresh(), which is true on the X3 too and only
// asks for the occasional HALF on a menu.
inline bool clearsGhostsOnEveryScreen() {
  return BoardConfig::isMurphyM3() || BoardConfig::ACTIVE.board == BoardConfig::Board::LilyGoT5S3;
}

// A HALF refresh takes the whole page through black. The Picco's driver draws every refresh that is not FAST with
// Onyx's full waveform (Ssd2677Driver::display), so the extra refresh after a page with pictures blanks the page to
// black on every turn of an illustrated book; the X3's HALF bank, by contrast, only drives toward white. The Picco's
// FAST re-drives the pixels a gray pass left (Ssd2677Driver::displayStart, _grayPending), so a page after a picture
// is clean without it. Per board: the Picco's profile names its controller SSD1677, and the driver is chosen by board.
inline bool halfRefreshFlashesBlack() { return BoardConfig::ACTIVE.board == BoardConfig::Board::Picco; }

// The EEGO A4's UC8279C glass fuzzes and shows earlier screens while its controller stays powered: upstream
// CrossPoint's finding (idleDisplayWhileAwake() in its main.cpp), and the stock firmware powers it down 500 ms after
// each refresh. Not seen here: no A4 is on the desk. Its driver has no booster-only power-off, so
// HalDisplay::powerOffPanelBetweenFrames() parks it through controllerIdle(): power-off and controller deep sleep,
// and the next paint resets and re-initializes it (Uc8279cA4Driver). Keyed on the controller, which only this board
// carries.
inline bool panelIdlesThroughControllerSleep() {
  return BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279C;
}

// The panel's booster is powered down once the device is idle, and again after each frame drawn
// while it stays idle (main.cpp's idle loop, HalDisplay::powerOffPanelBetweenFrames). Both drivers
// keep the booster up after a refresh unless the Sunlight Fading Fix asks for POF. Murphy's next
// refresh resets and powers it up anyway; the X3's (UC8253, not the UC8279 run) powers up and keeps
// its FAST, because the driver tells this power-off from a wake (Uc8253X3Driver::_idlePoweredOff).
// The EEGO A4 needs it for the glass (panelIdlesThroughControllerSleep).
inline bool powersPanelOffWhenIdle() {
  return BoardConfig::isMurphyM3() || BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         panelIdlesThroughControllerSleep();
}

// How long after the last input the booster goes down (powersPanelOffWhenIdle). Murphy powers up
// for every refresh anyway, so it goes down as soon as the loop idles, and so does the EEGO A4, whose
// stock firmware waits 500 ms (the loop idles after IDLE_LIGHT_SLEEP_MS, 1 s). The X3's next refresh waits
// for the power-up, 128 ms measured 2026-11-20, which would land on nearly every page turn after a
// second of reading: it waits past a page's reading time, so only a pause pays it.
inline unsigned long panelIdlePowerOffAfterMs() {
  return (BoardConfig::isMurphyM3() || panelIdlesThroughControllerSleep()) ? 0 : 120000;
}

// The 4-level image dither's table for this board's panel (quantizeGray4() in BitmapHelpers.h, copied there by
// setupDisplayAndFonts()): the three thresholds, and for each level the value fed back to the diffuser, the sRGB gray
// whose L* matches where that level sits between the panel's black and white. Measured on the gray-lab stand with
// full-screen uniform grays, every shot mapped through the reference card in it. Boards not measured yet keep the
// thresholds once tuned for the X4 and the even feedback that every board had before. Sleep pictures and the BMP viewer
// use it; the 16-level panels draw sleep pictures on their own levels.
struct ImageGray4Levels {
  uint8_t thresholds[3];
  uint8_t feedback[4];
};
// Feedback from black (0) to white (255) and thresholds both strictly rising, so every level can be reached. A
// threshold may sit below the level under it: the old table's 50 promotes grays from 50 up to light gray on purpose.
constexpr bool wellFormed(const ImageGray4Levels& l) {
  if (l.feedback[0] != 0 || l.feedback[3] != 255 || l.thresholds[0] == 0) return false;
  for (int i = 0; i < 3; i++) {
    if (l.feedback[i] >= l.feedback[i + 1]) return false;
    if (i > 0 && l.thresholds[i - 1] >= l.thresholds[i]) return false;
  }
  return true;
}
inline constexpr ImageGray4Levels kUnmeasuredImageGray4 = {{30, 50, 140}, {0, 85, 170, 255}};
// Murphy's UC8253, 2026-11-22: alone the levels read L* 19.0 / 26.4 / 33.5 / 52.1, light gray 44 % of the way to white
// where even spacing puts it at 67 %. Fed back as measured (0/53/104/255) uniform grays came within 4.4 L* of an even
// lightness ramp; with light gray fed back darker, as 82, within 2.9, because a fine light-gray and white mix reads
// darker on this glass than its share. Both rise at every step; the even table missed by up to 8.6.
inline constexpr ImageGray4Levels kMurphyImageGray4 = {{27, 68, 169}, {0, 53, 82, 255}};
// The X3's UC8253 (the later UC8279 run is not measured), 2026-11-22: alone the levels read L* about -4 (below the
// card's darkest step) / 6.4 / 22.7 / 72.7, dark gray 13 % and light gray 35 % of the way to white. The even table
// missed an even lightness ramp by up to 24.3 L*, mid grays far too dark. Fed back as measured (0/34/81/255)
// within 10.6, with flat stretches; both grays fed back darker, as 28 and 64, within 5.5 and rising at every step,
// because sparse lighter dots read smaller than their share on this glass. Thresholds from a dither model fitted to
// four sweeps.
inline constexpr ImageGray4Levels kX3Uc8253ImageGray4 = {{4, 68, 152}, {0, 28, 64, 255}};
// The X4's SSD1677, 2026-11-22: alone the levels read L* 16.2 / 27.6 / 61.8 / 80.8, dark gray 18 % and light gray 71 %
// of the way to white. The even table missed an even lightness ramp by up to 10.4 L*, shadows too dark and mid grays
// too light, 144 and 160 the same. Fed back as measured (0/43/173/255) within 6.2, with flat stretches; with dark gray
// fed back darker and light gray lighter, as 36 and 192, within 3.1 and rising at every step. Both from a dither model
// fitted to two sweeps, which predicted the next two within 2 L*. Grays from 236 up come out white.
inline constexpr ImageGray4Levels kX4ImageGray4 = {{24, 68, 172}, {0, 36, 192, 255}};
static_assert(wellFormed(kUnmeasuredImageGray4) && wellFormed(kMurphyImageGray4) && wellFormed(kX3Uc8253ImageGray4) &&
              wellFormed(kX4ImageGray4));
// A table is its panel controller's, measured on one. An X4 stays Board::XteinkX4 whichever controller its batch has
// (HalGPIO::begin() resolves the SSD1677, UC8179 or UC8279 after choosing the board), so its table also asks for the
// SSD1677 it was measured on; an X3 with the UC8279 is a board of its own (Board::XteinkX3Uc8279).
constexpr const ImageGray4Levels& imageGray4LevelsFor(const BoardConfig::Board board,
                                                      const BoardConfig::DisplayController controller) {
  switch (board) {
    case BoardConfig::Board::MurphyM3:
      return kMurphyImageGray4;
    case BoardConfig::Board::XteinkX3:
      return kX3Uc8253ImageGray4;
    case BoardConfig::Board::XteinkX4:
      return controller == BoardConfig::DisplayController::SSD1677 ? kX4ImageGray4 : kUnmeasuredImageGray4;
    default:
      return kUnmeasuredImageGray4;
  }
}
static_assert(&imageGray4LevelsFor(BoardConfig::Board::XteinkX4, BoardConfig::DisplayController::SSD1677) ==
              &kX4ImageGray4);
// An X4 with another controller has not been measured.
static_assert(&imageGray4LevelsFor(BoardConfig::Board::XteinkX4, BoardConfig::DisplayController::UC8179) ==
              &kUnmeasuredImageGray4);
static_assert(&imageGray4LevelsFor(BoardConfig::Board::XteinkX4, BoardConfig::DisplayController::UC8279) ==
              &kUnmeasuredImageGray4);
static_assert(&imageGray4LevelsFor(BoardConfig::Board::XteinkX3, BoardConfig::DisplayController::UC8253) ==
              &kX3Uc8253ImageGray4);
static_assert(&imageGray4LevelsFor(BoardConfig::Board::XteinkX3Uc8279, BoardConfig::DisplayController::UC8279) ==
              &kUnmeasuredImageGray4);
inline ImageGray4Levels imageGray4Levels() {
  return imageGray4LevelsFor(BoardConfig::ACTIVE.board, BoardConfig::ACTIVE.displayController);
}

// The panel can show gray at all. Murphy's only can in a build with -DFREEINK_MURPHY_GRAY: without
// it the UC8253 driver has no gray waveforms and ignores a gray pass
// (Uc8253MurphyDriver::displayGray), so a setting for one means nothing there.
inline bool panelShowsGray() { return !BoardConfig::isMurphyM3() || FREEINK_MURPHY_GRAY; }

// Every S3 build: the secondary framebuffer is never given up, because PSRAM has the room. A release
// frees nothing the heap is short of and takes the previous frame away from FAST: on Murphy that
// left the old screen up with the new one faint over it. HalDisplay refuses the release, and
// borrowSecondaryBuffer() hands out a dedicated block of PSRAM instead, which also gives a borrower
// far more room: Murphy's 12,480 byte framebuffer cannot hold a ZIP entry's 32 KB inflate ring.
inline bool keepsSecondaryBuffer() {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  return true;
#else
  return false;
#endif
}

// The reader menu's Sync tab without a KOReader account: one row that opens the KOReader settings over the book
// (EpubReaderMenuActivity::rebuildSyncItems). The C3s, for now, keep the tab for an account that is set up, as before:
// the settings and their keyboard over the reader and its menu have not been measured on the X4's heap
// (docs/future_work/memory-and-parsing/c3-heap-for-kosync-setup-from-the-reader-menu.md). The PSRAM boards offer it.
// cppcheck, which reads one board's build, sees only the declaration and checks both sides.
#if defined(CPPCHECK_ANALYSIS)
bool offersSyncSetupInReader();
#else
inline bool offersSyncSetupInReader() { return keepsSecondaryBuffer(); }
#endif

// A chapter build lays out in PSRAM, in a region of its own (Section::setOwnedBuildRegionBytes) or in the arena lent in
// place of the secondary buffer, so it touches neither frame buffer nor the heap the reader draws from: the page the
// build shows can get its gray while the build goes on, from the secondary buffer (BuildPageGray.h, Pass::Restore). Not
// the C3s, whose build holds the secondary buffer (Pass::Redraw) or the X4's heap (the gray at the build's end).
// cppcheck, which reads one board's build, sees only the declaration and checks both sides (as defersLiveFontChange
// below).
#if defined(CPPCHECK_ANALYSIS)
bool buildsInPsram();
#else
inline bool buildsInPsram() { return keepsSecondaryBuffer(); }
#endif

// True when an image decode can run through a refresh: nothing it needs is on the bus the waveform holds. The card is
// not (native SDMMC), and neither is the arena it is lent (PSRAM). Seen on the X4 Pro, and on the Picco since its
// SE0400 driver defers a refresh: a chapter build laid out its pages 2 to 50 while the first page's single push ran
// (1.1 s, two waveforms and their power cycles; 2026-11-17). The same on the Murphy since its UC8253 driver defers:
// a build went from page 1 to page 50 of its chapter's 94 between the trigger of the first page's single push and its
// end (1.65 s: 0.35 s of reset, init and planes, the HALF waveform, the nudge; 2026-11-17).
inline bool decodesThroughRefresh() { return !sdUsesSpi() && keepsSecondaryBuffer(); }

// A new font on the reader bars' Layout tab waits for them to close; every other value there is laid out under them at
// once (EpubReaderActivity's live bars). The C3s: a new family is copied into the flash font area before the chapter
// is laid out again, a 3 MB erase and the file, 7 s on the X3 for each Font press while the reader goes round the
// fonts (2026-11-13). The live build itself fits their heap: its low point was 2.6 KB under that of a build after the
// bars close (29.6 against 32.2 KB free, X3). The PSRAM boards lay a new font out at once (4 to 15 s a step on the
// X4 Pro, by family).
// cppcheck, which reads one board's build, is left without the value, so it checks both sides instead of calling one
// dead (as PanelMetrics::COMPACT).
#if defined(CPPCHECK_ANALYSIS)
bool defersLiveFontChange();
#else
inline bool defersLiveFontChange() { return !keepsSecondaryBuffer(); }
#endif

// Plugin events that bring Wi-Fi up at sleep entry go from a clean boot: the sleep screen is painted, the chip
// restarts, that boot sends the queue before the display takes its framebuffers, and restarts into the sleep (main.cpp,
// SILENT_REBOOT_TARGET_SLEEP_SEND_EVENTS). The C3s: beside two framebuffers, joining Wi-Fi left the X3 less than 8 KB
// in one block, too little for TLS (67 KB free and a 57 KB block before the join, sleeping out of a book; before the
// display takes its buffers, its boot has 212 KB free; 2026-11-20). The PSRAM boards join where they are (Murphy: 175
// KB free before the join, a download and a PUT went through). cppcheck sees only the declaration, as above.
#if defined(CPPCHECK_ANALYSIS)
bool sendsSleepEventsFromCleanBoot();
#else
inline bool sendsSleepEventsFromCleanBoot() { return !keepsSecondaryBuffer(); }
#endif

// A Wi-Fi screen's exit restarts the device (wifi_session::end(), src/network/WifiSessionEnd.h). The C3s: the session
// gave up the secondary framebuffer, which nothing reallocates, and a full Wi-Fi stop and deinit still left about
// 20 KB below the heap before it (-20,560 / -20,588 / -20,644 B, three KOReader syncs on the X3). The PSRAM boards give
// up no framebuffer: they turn the radio off and go on in the same boot while the internal heap still covers the next
// session (the T5S3 kept 120,020 B free, a 49,140 B block, after its first session). cppcheck sees only the
// declaration, as above.
#if defined(CPPCHECK_ANALYSIS)
bool restartsAfterWifiSession();
#else
inline bool restartsAfterWifiSession() { return !keepsSecondaryBuffer(); }
#endif

// A new UI theme or UI font size waits for Settings to close (SettingsActivity::onExit), rather than laying the open
// screen out again at once (SettingsActivity::applyUiLookIfChanged). The C3s, for now: the live change replaces the
// theme object and binds the UI fonts again with the screens below Settings still resident, which nobody has measured
// on the X4's heap yet (docs/future_work/memory-and-parsing/c3-heap-for-the-live-ui-theme-and-font-size.md). The PSRAM
// boards change at once (T5S3: Settings at 109 KB internal free before and after a theme and a font size round trip,
// 2026-11-26). cppcheck sees only the declaration, as above.
#if defined(CPPCHECK_ANALYSIS)
bool defersUiLookChange();
#else
inline bool defersUiLookChange() { return !keepsSecondaryBuffer(); }
#endif

// --- Radio -------------------------------------------------------------------

// This build links a Bluetooth LE host for remotes (the SDK's BleKeyboardHost on NimBLE, src/BleRadio.h). The S3
// boards, whose PSRAM holds NimBLE's host ([ble_s3] in platformio.ini); the C3 boards link none: there the controller
// alone does not fit beside the reader (docs/future_work/ble-keyboards.md). A build capability, so it is constexpr.
constexpr bool hasBleRadio() { return FREEINK_CAP_BLE_HID_HOST != 0; }

// A frontlight is present: PWM (LEDC or PM1-driven), or an LED driver on the I2C bus (the BOOX Picco's).
inline bool hasFrontlight() { return BoardConfig::hasPwmFrontlight() || BoardConfig::hasI2cFrontlight(); }

// A frontlight on a board whose hardware RTC keeps the time of day through sleep: what Dark Mode's night hours need.
// Every frontlit board this firmware builds for has the clock too (X4 Pro BM8563, T5S3 PCF8563, Murphy M3 RX8010,
// Picco PCF8563-class).
inline bool hasFrontlightAndRtc() { return hasFrontlight() && hasHardwareRtc(); }

// The frontlight has a second (warm) channel, so colour temperature is
// adjustable rather than brightness-only.
inline bool hasColorTemperature() { return hasFrontlight() && BoardConfig::hasColorTemperatureFrontlight(); }

// Human-readable board name for the UI. The profile's own `name` is a build
// slug ("xteink_x4", "lilygo_t5s3") and would read badly on screen, so the
// display spelling lives here. Deliberately exhaustive with no default: a new
// board then fails to compile rather than silently reporting the wrong device,
// which is exactly the failure this replaces -- the old deviceIsX3() ternary
// called every S3 board an "X4".
inline const char* boardDisplayName(BoardConfig::Board board) {
  switch (board) {
    case BoardConfig::Board::XteinkX4:
      return "X4";
    // Same board and glass as the X3, differing only in the panel controller,
    // which is reported on its own row.
    case BoardConfig::Board::XteinkX3:
    case BoardConfig::Board::XteinkX3Uc8279:
      return "X3";
    case BoardConfig::Board::XteinkX4Pro:
      return "X4 Pro";
    case BoardConfig::Board::XteinkX4Classic:
      return "X4 Classic";
    case BoardConfig::Board::EegoA4:
      return "EEGO A4";
    case BoardConfig::Board::M5StackPaperColor:
      return "M5Paper Color";
    case BoardConfig::Board::MurphyM3:
      return "Murphy M3";
    case BoardConfig::Board::MurphyM4:
      return "Murphy M4";
    case BoardConfig::Board::Silkscreen:
      return "Silkscreen";
    case BoardConfig::Board::LilyGoT5S3:
      return "T5 S3 Pro";
    case BoardConfig::Board::M5PaperV11:
      return "M5Paper v1.1";
    case BoardConfig::Board::Sticky:
      return "Sticky";
    case BoardConfig::Board::PaperMono:
      return "Paper Mono";
    case BoardConfig::Board::M5PaperS3:
      return "M5Paper S3";
    case BoardConfig::Board::Picco:
      return "Picco";
  }
  return "Unknown";
}

}  // namespace HalCapabilities

// Boards whose support layer owns bus bring-up.
//
// The LilyGo T5S3 ships a board-support library (freeink-sdk BoardT5S3) whose
// begin() owns BOTH bus inits: beginI2C() starts Wire on the board's pins, and
// prepareSdBus() drives LORA_CS and SD_CS HIGH before SPI.begin(). That
// deselect matters — the board puts a LoRa radio on the SD's SPI bus, and an
// undriven chip select there corrupts SD transactions. It also configures the
// BOOT button and registers the PCA9535 expander's user button through
// InputManager::setButtonHook().
//
// Where such a layer exists, the HAL must NOT also start those buses: a second
// Wire.begin() on one port breaks the ESP-IDF i2c_master driver outright, and a
// second SPI.begin() would undo the CS deselects.
// This is a board NAME rather than a capability, deliberately and narrowly:
// board-support glue is per-board by definition, unlike the hardware questions
// HalCapabilities answers. Keeping it to this one macro is what stops it
// spreading into the conflation B0 removed.
#define BOARD_SUPPORT_OWNS_BUSES (FREEINK_DEVICE_LILYGO)
