#include <Arduino.h>
#include <CooperativeAbort.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalSpiBus.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>

#include <cstring>
#include <vector>

#include "ButtonEventManager.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalBookmarkIndex.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ReadingSessionTracker.h"
#include "ReadingStats.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WeatherSettingsStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"
#include "util/WakeTrace.h"

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
#include <BootHeapProbe.h>
// Static-init heap probes bracketing this TU's globals (slots 4/5); see BootHeapProbe.h.
static BootHeapProbe s_probeMainFirst(4);
#endif
MappedInputManager mappedInputManager(gpio);
ButtonEventManager buttonEventManager(mappedInputManager);

// Lets lib-layer long tasks (image decoders) bail out mid-work so a queued button
// press is serviced on the next main-loop pass. Installed once in setup().
static bool hasPendingButtonInput() { return mappedInputManager.hasPendingInput(); }
ButtonEventManager& globalButtonEvents() { return buttonEventManager; }
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());

// Fonts
EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFont bookerly14BoldItalicFont(&bookerly_14_bolditalic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                   &bookerly14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont bookerly10RegularFont(&bookerly_10_regular);
EpdFont bookerly10BoldFont(&bookerly_10_bold);
EpdFont bookerly10ItalicFont(&bookerly_10_italic);
EpdFont bookerly10BoldItalicFont(&bookerly_10_bolditalic);
EpdFontFamily bookerly10FontFamily(&bookerly10RegularFont, &bookerly10BoldFont, &bookerly10ItalicFont,
                                   &bookerly10BoldItalicFont);
EpdFont bookerly12RegularFont(&bookerly_12_regular);
EpdFont bookerly12BoldFont(&bookerly_12_bold);
EpdFont bookerly12ItalicFont(&bookerly_12_italic);
EpdFont bookerly12BoldItalicFont(&bookerly_12_bolditalic);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont,
                                   &bookerly12BoldItalicFont);
EpdFont bookerly16RegularFont(&bookerly_16_regular);
EpdFont bookerly16BoldFont(&bookerly_16_bold);
EpdFont bookerly16ItalicFont(&bookerly_16_italic);
EpdFont bookerly16BoldItalicFont(&bookerly_16_bolditalic);
EpdFontFamily bookerly16FontFamily(&bookerly16RegularFont, &bookerly16BoldFont, &bookerly16ItalicFont,
                                   &bookerly16BoldItalicFont);
EpdFont bookerly18RegularFont(&bookerly_18_regular);
EpdFont bookerly18BoldFont(&bookerly_18_bold);
EpdFont bookerly18ItalicFont(&bookerly_18_italic);
EpdFont bookerly18BoldItalicFont(&bookerly_18_bolditalic);
EpdFontFamily bookerly18FontFamily(&bookerly18RegularFont, &bookerly18BoldFont, &bookerly18ItalicFont,
                                   &bookerly18BoldItalicFont);

EpdFont notosans10RegularFont(&notosans_10_regular);
EpdFont notosans10BoldFont(&notosans_10_bold);
EpdFont notosans10ItalicFont(&notosans_10_italic);
EpdFont notosans10BoldItalicFont(&notosans_10_bolditalic);
EpdFontFamily notosans10FontFamily(&notosans10RegularFont, &notosans10BoldFont, &notosans10ItalicFont,
                                   &notosans10BoldItalicFont);
EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&inter_ui_10_regular);
EpdFont ui10BoldFont(&inter_ui_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&inter_ui_12_regular);
EpdFont ui12BoldFont(&inter_ui_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// SilentRestart.h definitions. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t heapRecoveryRestartLatch;
// Single-shot latch for the boot-time heap integrity recovery restart.
// Prevents an infinite reset loop if the heap is still corrupt after one clean restart.
RTC_NOINIT_ATTR uint32_t heapCorruptionBootLatch;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
// Boot straight back into the USB serial file-transfer activity. Armed (without
// restarting) while that activity is open, so the unavoidable C3 USB-Serial/JTAG
// reset that fires when a host opens the port lands back in the activity instead
// of Home — making the transfer reset-tolerant. See armSerialTransferReboot().
constexpr uint32_t SILENT_REBOOT_TARGET_SERIAL_TRANSFER = 2;
// Boot into the clock settings screen after a timezone-detection WiFi session
// (WiFi teardown fragments the heap; need a clean reboot before re-entering the UI).
constexpr uint32_t SILENT_REBOOT_TARGET_CLOCK_SETTINGS = 3;
// Sleep was requested while the display framebuffers were released (web server
// session frees both for WiFi heap). SleepActivity cannot render without a
// framebuffer, so reboot to reestablish it and finish the sleep transition right
// after setup(). Two targets so fromTimeout survives the reboot (it gates
// "Quick Resume on Timeout").
constexpr uint32_t SILENT_REBOOT_TARGET_SLEEP = 4;
constexpr uint32_t SILENT_REBOOT_TARGET_SLEEP_TIMEOUT = 5;
// Boot into the KOReader settings screen after an auth/register WiFi session, so
// the user lands back where they started instead of on Home.
constexpr uint32_t SILENT_REBOOT_TARGET_KOREADER_SETTINGS = 6;
// Upper bound for the cold-boot sanity check on silentRebootTarget (RTC_NOINIT is
// uninitialized on power-up). Must equal the highest target above — keep it in
// sync when adding one, or the new target silently reads as HOME.
constexpr uint32_t SILENT_REBOOT_TARGET_MAX = SILENT_REBOOT_TARGET_KOREADER_SETTINGS;
constexpr uint32_t HEAP_RECOVERY_RESTART_LATCH_MAGIC = 0x48EA9C01;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
  // Wake from a plain deep sleep that is heading straight back into the reader. Distinct from
  // QuickResume because it paints NOTHING at boot: the X3's one mandatory post-begin() full
  // sync (~2.3 s) is reserved for the page itself rather than spent on an interstitial. There
  // is no saved frame on this path — enterDeepSleep() only persists one for Quick Resume.
  ReaderResume,
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  // ESP.restart() bypasses activity onExit(), so flush any in-flight reading
  // session manually — otherwise a heap-defrag reboot mid-read loses the session.
  globalReadingSessionTracker().end();
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  globalReadingSessionTracker().end();
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  delay(50);
  ESP.restart();
}

void silentRestartToClockSettings() {
  if (deepSleepInProgress) return;
  globalReadingSessionTracker().end();
  silentRebootTarget = SILENT_REBOOT_TARGET_CLOCK_SETTINGS;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=clock-settings)");
  delay(50);
  ESP.restart();
}

void silentRestartToKOReaderSettings() {
  if (deepSleepInProgress) return;
  globalReadingSessionTracker().end();
  silentRebootTarget = SILENT_REBOOT_TARGET_KOREADER_SETTINGS;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=koreader-settings)");
  delay(50);
  ESP.restart();
}

bool trySilentRestartToReaderForHeapRecovery() {
  if (deepSleepInProgress) return false;  // sleeping supersedes the heap-defrag reboot
  if (heapRecoveryRestartLatch == HEAP_RECOVERY_RESTART_LATCH_MAGIC) {
    LOG_ERR("MAIN", "Heap-recovery restart suppressed by safety latch");
    return false;
  }
  heapRecoveryRestartLatch = HEAP_RECOVERY_RESTART_LATCH_MAGIC;
  globalReadingSessionTracker().end();
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_ERR("MAIN", "Silent restart (target=reader, heap recovery)");
  delay(50);
  ESP.restart();
  return true;
}

// Restart as part of an in-progress sleep transition: setup() reestablishes the
// framebuffers, then routes straight back into enterDeepSleep(). No
// deepSleepInProgress guard — this restart IS the sleep path, not a competing
// heap-defrag reboot.
static void silentRestartToSleep(bool fromTimeout) {
  globalReadingSessionTracker().end();
  silentRebootTarget = fromTimeout ? SILENT_REBOOT_TARGET_SLEEP_TIMEOUT : SILENT_REBOOT_TARGET_SLEEP;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_INF("MAIN", "Silent restart (target=sleep, framebuffers released, fromTimeout=%d)", fromTimeout ? 1 : 0);
  delay(50);
  ESP.restart();
}

void armSerialTransferReboot() {
  // Does NOT restart — just arms the RTC target so that *if* the device resets
  // (the C3 hardware reset that fires when a host opens the USB serial port),
  // setup() routes straight back into the serial-transfer activity. Re-armed on
  // every entry into that activity (setup() read-and-clears the magic).
  silentRebootTarget = SILENT_REBOOT_TARGET_SERIAL_TRANSFER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
}

void disarmSerialTransferReboot() {
  // Clear the arm on a clean exit so the next plain reboot shows Home as usual.
  if (silentRebootMagic == SILENT_REBOOT_MAGIC && silentRebootTarget == SILENT_REBOOT_TARGET_SERIAL_TRANSFER) {
    silentRebootMagic = 0;
    silentRebootTarget = 0;
  }
}

// ---- Retained-frame persistence across deep sleep ----
//
// Deep sleep loses the framebuffer (and the SDK's previous-frame copy) with the power domain, so
// anything the wake path wants to composite onto must be persisted to SD first.
//
// Only Quick Resume saves one. It keeps the last reader page on screen during deep sleep (just
// overlaid with a moon icon) and restores that frame on wake, so reading resumes with no visible
// boot at all — the user is looking at their own page for the whole load.
//
// A plain reader resume deliberately saves NOTHING, because painting a restored frame would
// consume the X3's one mandatory post-wake full sync on an interstitial and push the page behind
// a second refresh (see enterDeepSleep). The presence of this file is therefore also what setup()
// uses to tell the two resume flows apart.
constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  FsFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) {
    return;
  }
  const size_t bufferSize = renderer.getBufferSize();
  const size_t written = file.write(renderer.getFrameBuffer(), bufferSize);
  file.close();
  if (written != bufferSize) {
    LOG_DBG("MAIN", "Quick Resume frame save short write: %u/%u", static_cast<unsigned>(written),
            static_cast<unsigned>(bufferSize));
    Storage.remove(SLEEP_FRAME_FILE);
  }
}

// Restores the previously saved framebuffer into the display buffer. Returns false if the file is
// missing or the size does not match — in that case the caller should fall back to a boot screen.
// The file is always removed (success or failure) so a future cold boot does not see a stale frame.
static bool loadSleepFrameBuffer() {
  FsFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) {
    return false;
  }
  const size_t bufferSize = display.getBufferSize();
  const size_t fileSize = file.size();
  const int bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  Storage.remove(SLEEP_FRAME_FILE);
  if (fileSize != bufferSize || bytesRead < 0 || static_cast<size_t>(bytesRead) != bufferSize) {
    LOG_DBG("MAIN", "Quick Resume frame size mismatch (file=%u, read=%d, expected=%u)", static_cast<unsigned>(fileSize),
            bytesRead, static_cast<unsigned>(bufferSize));
    return false;
  }
  return true;
}

// Earliest millis() value at which a held-power-button press is allowed to trigger sleep.
// Set near the end of setup() so a wake-press held a bit too long does not bounce straight
// back into deep sleep before the user sees the page.
static unsigned long allowSleepAt = 0;

// ---- Leftover wake-press guard ---------------------------------------------------------
// The wake press is usually STILL HELD when setup() finishes. Nothing on screen confirms
// that the gesture was accepted (a reader resume deliberately paints nothing until the page
// lands — see BootResume::ReaderResume), so the user keeps holding "to be sure".
//
// setup() used to answer that by blocking on gpio.waitForStablePowerRelease(). That charged
// the user's finger straight to the boot: measured on X4, the `rel` phase cost 1290 ms and
// 3050 ms on two long-hold wakes, against 200 ms — the stability tail alone — on a wake the
// user had already released. Same firmware, same book: 6.8 s to the page versus 3.9 s. The
// hold was not slow to *register*, it was slow because holding is what stalled the boot.
//
// So boot no longer waits. This latch swallows the leftover press from the main loop
// instead, which preserves the one thing the wait was for: the release bounce (10-50 ms on
// these switches, well past InputManager's 5 ms debounce) must not reach the press-type FSM
// as a second click, which on a device with double-click-to-sleep configured would put it
// straight back to sleep.
//
// Serviced by serviceBootPowerRelease(), defined below markBootPhase().
static bool bootPowerReleasePending = false;
static unsigned long bootPowerHighSince = 0;

static void logStartupMemory(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t minFree = esp_get_minimum_free_heap_size();
  const uint32_t free8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  const uint32_t largest8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  LOG_INF("MEM", "Startup[%s] free=%lu min=%lu 8bit=%lu largest8=%lu internal=%lu largestInternal=%lu", stage,
          static_cast<unsigned long>(freeHeap), static_cast<unsigned long>(minFree),
          static_cast<unsigned long>(free8bit), static_cast<unsigned long>(largest8bit),
          static_cast<unsigned long>(freeInternal), static_cast<unsigned long>(largestInternal));
}

// --- Boot phase trace ---------------------------------------------------------------
// millis() stamp per boot phase, summarized at the end of setup() and again whenever the
// serial link comes up. The wake gesture is only a ~300 ms gate (getPowerWakeHoldDuration),
// but the splash lands seconds later because the settle waits, the SD mount, the config
// loads, the panel bring-up and the first (non-differential) waveform all sit between the
// two. Without per-phase stamps that gap is invisible in the log — every phase before
// Serial.begin() has no output at all — and "the power button feels unresponsive" reports
// cannot be attributed to a phase.
//
// The stamps stay live in .bss for the whole session precisely so they can be re-emitted:
// a wake from deep sleep re-enumerates USB, so a host monitor reconnecting mid-boot misses
// the first lines, and the RTC ring buffer (16 entries) has usually rolled past them by
// the time anyone looks.
//
// Cost is 2 bytes per phase of .bss plus one log line; no heap, no allocation. Stamps are
// clamped to 16 bits (a boot that reaches 65 s is pathological and reads as 65535).
enum class BootPhase : uint8_t {
  SetupEntry,      // first statement of setup()
  NvsSettings,     // startup settings read from NVS (precedes the wake gate)
  WakeGate,        // power-button wake gesture decided
  HwInit,          // system / SPI bus / GPIO / power / tilt brought up
  SerialUp,        // USB-CDC opened (only when USB is connected; earlier phases log nothing)
  RecoverySettle,  // 500 ms UP+POWER recovery-combo sample window
  SdMount,         // Storage.begin()
  ConfigLoad,      // settings, app state, i18n, KOReader, OPDS, weather, theme
  DisplayFonts,    // panel init + framebuffer alloc + font registration + SD font scan
  FirstPaint,      // splash / quick-resume frame handed to the panel (logo now visible)
  StoreLoad,       // clock, recent books, bookmarks, reading stats
  ActivityRoute,   // target activity entered (home / reader / recovery)
  PowerRelease,    // wake press released (stable), input sampler about to start
  Count,
};

static uint16_t bootPhaseMs[static_cast<uint8_t>(BootPhase::Count)] = {};
// Which phases were reached — a stamp of 0 is a legal millis() value, so "reached" cannot
// be inferred from the value. 13 phases fit a uint16_t mask with room to spare.
static uint16_t bootPhaseReached = 0;
// What the wake gate decided this boot. Kept for the whole session so it can be re-emitted
// alongside the phase trace; 4 bytes.
static HalGPIO::WakeCheck bootWakeCheck;
static_assert(static_cast<uint8_t>(BootPhase::Count) <= 16, "bootPhaseReached mask is 16 bits wide");

static void markBootPhase(BootPhase phase) {
  const unsigned long ms = millis();
  bootPhaseMs[static_cast<uint8_t>(phase)] = static_cast<uint16_t>(ms > UINT16_MAX ? UINT16_MAX : ms);
  bootPhaseReached |= static_cast<uint16_t>(1u << static_cast<uint8_t>(phase));
}

// Short labels so the whole trace fits one 256-byte ring-buffer entry (Logging.cpp).
static constexpr const char* BOOT_PHASE_NAMES[] = {"entry", "nvs",  "gate",  "hw",    "ser",   "recov", "sd",
                                                   "cfg",   "disp", "paint", "store", "route", "rel"};
static_assert(sizeof(BOOT_PHASE_NAMES) / sizeof(BOOT_PHASE_NAMES[0]) == static_cast<size_t>(BootPhase::Count),
              "BOOT_PHASE_NAMES must stay in sync with BootPhase");

// One "name+costMs" token per reached phase, then the two absolute numbers worth
// quoting. Deltas rather than absolutes because the cost of a phase is the actionable
// figure and absolutes are just their running sum — and because the whole line has to
// fit one 256-byte ring-buffer entry including logPrintf's timestamp/level prefix
// (Logging.cpp, MAX_ENTRY_LEN). Unreached phases are skipped (e.g. `ser` on battery,
// `recov` on a non-button boot), so a short trace is itself a signal about which path
// the boot took.
static void logBootTrace() {
  char line[160];
  size_t used = 0;
  uint16_t previous = 0;
  for (uint8_t i = 0; i < static_cast<uint8_t>(BootPhase::Count); i++) {
    if ((bootPhaseReached & (1u << i)) == 0) continue;
    const int written = snprintf(line + used, sizeof(line) - used, "%s%s+%u", used ? " " : "", BOOT_PHASE_NAMES[i],
                                 static_cast<unsigned>(bootPhaseMs[i] - previous));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(line) - used) break;
    used += static_cast<size_t>(written);
    previous = bootPhaseMs[i];
  }
  // millis() excludes the ROM/2nd-stage bootloader (~200-300 ms), so time-to-logo as the
  // user experiences it is that much longer than `logo` reports.
  // Worst case (five-digit stamps throughout) still fits MAX_ENTRY_LEN: 29 prefix + 15
  // here + 160 line + 38 tail = 242 of 256. Keep that budget in mind when editing either.
  LOG_INF("BOOT", "phase cost ms: %s | logo=%u setup=%u (+bootloader)", line,
          bootPhaseMs[static_cast<uint8_t>(BootPhase::FirstPaint)], previous);
}

// The whole boot story in two lines: what the power button was judged to be, and where the
// time went. Re-emitted on every serial (re)connect — see the note above the phase table.
static void logBootSummary() {
  LOG_INF("BOOT", "Wake gate: %s (decided at %u ms, gate saw the press for %u ms, required %u ms)",
          HalGPIO::wakeVerdictName(bootWakeCheck.verdict), bootWakeCheck.decidedAtMs, bootWakeCheck.heldMs,
          CrossPointSettings::getPowerWakeHoldDuration());
  logBootTrace();
}

// Poll the raw power pin and keep the boot press away from the event system until it has
// been released for RELEASE_STABLE_MS. See the latch's declaration for why boot no longer
// blocks on this. Call once per loop tick, after ButtonEventManager has drained the sampler
// queue and before any power handling.
static void serviceBootPowerRelease() {
  if (!bootPowerReleasePending) return;
  // Same window waitForStablePowerRelease() used; the raw pin is read directly for the same
  // reason it did — the 5 ms debounce is too short for mechanical release bounce.
  constexpr unsigned long RELEASE_STABLE_MS = 200;
  const unsigned long now = millis();
  if (digitalRead(InputManager::POWER_BUTTON_PIN) == HIGH) {
    if (bootPowerHighSince == 0) bootPowerHighSince = now;
  } else {
    bootPowerHighSince = 0;
  }
  // Unconditional, including on the tick that clears the latch: drain() resets every FSM and
  // flushes the sampler's edge queue plus the pending press/release bits, so neither the boot
  // press, its release, nor the bounce that follows can be seen by loop() or by an activity.
  // It also keeps gpio.wasPressed(BTN_POWER) false, which is what stops the hold-to-sleep
  // timer in loop() from ever starting on the wake press.
  buttonEventManager.drain();
  if (bootPowerHighSince != 0 && now - bootPowerHighSince >= RELEASE_STABLE_MS) {
    bootPowerReleasePending = false;
    // Stamped here rather than in setup(), so `rel` in the boot trace still reports how long
    // the user kept holding past the point the device was ready — it just no longer costs
    // that time. Read it back with CMD:BOOTLOG, which now prints it after the fact.
    markBootPhase(BootPhase::PowerRelease);
    LOG_DBG("MAIN", "Wake press released at %lu ms (boot was not blocked on it)", now);
  }
}

// Enter deep sleep mode. fromTimeout=true marks an auto-sleep (gates "Quick Resume on Timeout").
void enterDeepSleep(bool fromTimeout = false) {
  LOG_DBG("MAIN", "enterDeepSleep called at millis=%lu, powerBtn isPressed=%d, rawPin=%d", millis(),
          gpio.isPressed(HalGPIO::BTN_POWER), digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);
  // The web server activity releases BOTH framebuffers for WiFi heap
  // (releaseFrameBuffers() nulls the renderer's pointer). SleepActivity would
  // render the sleep screen into nullptr and crash, so reboot instead: setup()
  // reallocates the buffers and routes straight back here to finish the sleep.
  if (renderer.getFrameBuffer() == nullptr) {
    silentRestartToSleep(fromTimeout);
    return;  // not reached — silentRestartToSleep() restarts the chip
  }
  // Stop the background sampler before tearing down the display/power rails so no
  // ADC read races with that teardown. From here sleep prep reads the power pin
  // directly (HalGPIO::startDeepSleep / waitForStablePowerRelease).
  gpio.stopInputSampler();
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  // On X3 the DS3231 keeps time independently, so there's no need to keep the MCU
  // powered during deep sleep for LP timer preservation.
  const bool keepLpAlive = SETTINGS.useClock && !gpio.deviceIsX3();
  HalClock::saveBeforeSleep(keepLpAlive);
  // If sleeping from a running reader the book loaded successfully, so the boot-loop
  // guard count is no longer needed. Reset it now because onExit() is never called
  // on the reader activity during a sleep transition (only queued as a pending action).
  if (APP_STATE.lastSleepFromReader) {
    APP_STATE.readerActivityLoadCount = 0;
  }

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Sleeping from a book with a book still open means the next boot's destination is already
  // known: setup() will route straight back into the reader. Paint NOTHING before that page.
  //
  // The reason is specific to the panel, not to taste. After begin() the X3 driver arms exactly
  // one mandatory full sync (Uc8253X3Driver: _initialFullSyncsRemaining = 1) — the controller's
  // DTM1 baseline was wiped to white by the power-down, so the first refresh must drive every
  // pixel whatever mode it asks for. Measured on X3: ~2.3 s. Whichever frame paints first
  // consumes it, and everything after it is a cheap differential.
  //
  // So an intermediate frame is not "a cheap extra refresh" — it spends the single expensive
  // sync on a throwaway image and forces the page to pay a second refresh on top. Both the boot
  // splash and a loading-icon-over-sleep-screen do exactly that (measured: wake 4.8 s, of which
  // 2.4 s was the intermediate paint). Skipping straight to the page lets the page itself
  // consume the mandatory sync, removing a whole refresh from the wake.
  //
  // The trade-off, deliberately taken: for the ~700 ms the book takes to open there is no
  // on-screen acknowledgement of the wake press. The panel is not blank — it still physically
  // holds the sleep screen — so this reads as "not woken yet" rather than as a fault.
  const bool resumeIntoReader = APP_STATE.lastSleepFromReader && !APP_STATE.openEpubPath.empty();
  APP_STATE.showBootScreen = !(isQuickResumeSleep || resumeIntoReader);

  APP_STATE.saveToFile();
  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  // Persist the moon-icon-overlaid framebuffer after goToSleep() has painted it. Quick Resume
  // only: it restores this frame and paints a loading icon over it, which is worth the mandatory
  // full sync because the restored frame IS the reader page — the user is looking at their book
  // for the whole load, not at an interstitial. A plain reader resume saves nothing and paints
  // nothing, so the page gets that sync instead (see showBootScreen above).
  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  halTiltSensor.deepSleep();

  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep (powerBtn isPressed=%d, rawPin=%d)", gpio.isPressed(HalGPIO::BTN_POWER),
          digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);

  powerManager.startDeepSleep(gpio, keepLpAlive);
}

void setupDisplayAndFonts(bool seamless = false, bool skipSdFontDiscovery = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(BOOKERLY_14_FONT_ID, bookerly14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(BOOKERLY_10_FONT_ID, bookerly10FontFamily);
  renderer.insertFont(BOOKERLY_12_FONT_ID, bookerly12FontFamily);
  renderer.insertFont(BOOKERLY_16_FONT_ID, bookerly16FontFamily);
  renderer.insertFont(BOOKERLY_18_FONT_ID, bookerly18FontFamily);

  renderer.insertFont(NOTOSANS_10_FONT_ID, notosans10FontFamily);
  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover SD card fonts (under /.crosspoint/fonts/) and load the family
  // currently selected in settings (if any). Safe to call without an SD card.
  // Skipped on the serial-transfer boot path: that activity only uses built-in
  // UI fonts, and it reboots on exit, so Home re-discovers fonts then. Saves an
  // SD directory scan on the reboot the host is waiting through.
  if (!skipSdFontDiscovery) {
    sdFontSystem.begin(renderer);
  }

  LOG_DBG("MAIN", "Fonts setup");
}

// Defined here to satisfy SdCardFontGlobals.h's extern declaration. Keeps
// activity-side callers out of SdCardFontSystem internals.
void ensureSdFontLoaded() { sdFontSystem.ensureLoaded(renderer); }

void unloadSdFontIfLoaded() { sdFontSystem.unload(renderer); }

void ensureSdFontLoadedForPath(const char* path) {
  if (!path) {
    ensureSdFontLoaded();
    return;
  }
  // Writing a not-yet-cached SD font into the flash partition stalls the open for a
  // moment; advertise it. Callers (goToReader/replaceWithReader) already hold a
  // RenderLock, so the lambda must not take one. Fires only on a genuine first load.
  const auto onColdFontLoad = [] { GUI.drawPopup(renderer, tr(STR_LOADING_FONT)); };

  const std::string_view filePath(path);
  const bool isTxtMd = static_cast<bool (*)(std::string_view)>(FsHelpers::hasTxtExtension)(filePath) ||
                       static_cast<bool (*)(std::string_view)>(FsHelpers::hasMarkdownExtension)(filePath);
  if (isTxtMd) {
    // TXT/MD has no per-book SD font override — use global settings directly.
    sdFontSystem.ensureLoaded(renderer, SETTINGS.txtSdFontFamilyName, SETTINGS.txtFontSize, onColdFontLoad);
    return;
  }

  // For EPUB: honour per-book SD font and/or size overrides. The book record
  // is available here — the reader activity hasn't started yet, but
  // RecentBooksStore already has the persisted overrides for this path.
  const RecentBook book = RECENT_BOOKS.getBookByPath(path);
  const uint8_t effectiveSize =
      (book.fontSizeOverride >= 0) ? static_cast<uint8_t>(book.fontSizeOverride) : SETTINGS.fontSize;

  if (!book.sdFontFamilyOverride.empty()) {
    // Per-book SD font override: load that family at the effective size.
    sdFontSystem.ensureLoaded(renderer, book.sdFontFamilyOverride.c_str(), effectiveSize, onColdFontLoad);
  } else if (book.fontFamilyOverride >= 0) {
    // Per-book built-in font override: no SD font needed; unload if one was active.
    sdFontSystem.ensureLoaded(renderer, "", effectiveSize, onColdFontLoad);
  } else {
    // No family override: use global SD font (if any) at the effective size.
    sdFontSystem.ensureLoaded(renderer, SETTINGS.sdFontFamilyName, effectiveSize, onColdFontLoad);
  }
}

// --- Temporary boot-phase heap-corruption bisect probes -------------------------------
// Field data shows the heap can already be corrupt at the existing setup() check even
// on the boot AFTER esp_restart()'s full DRAM re-init — i.e. the corruption is
// re-created during early init, not inherited from the crashed session. These probes
// record WHICH phase breaks it; results are logged once serial is up.
//   preCtors  — C constructor at priority 101, runs before all C++ global ctors
//               (default priority 65535). false here ⇒ IDF/Arduino SDK init.
//   setupEntry — first statement of setup(). false here (with preCtors true) ⇒ one of
//               OUR C++ global constructors.
//   (the existing post-OTA check then isolates the OTA/NVS block)
// Remove once the writer is found.
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
static bool s_heapOkPreCtors = true;
static bool s_heapOkSetupEntry = true;
extern "C" __attribute__((constructor(101))) void heapProbePreCppCtors() {
  s_heapOkPreCtors = heap_caps_check_integrity_all(/*print_errors=*/false);
}
#endif

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
// Last probe of this TU — constructed after every global declared above (slot 5).
static BootHeapProbe s_probeMainLast(5);
#endif

// Maps the user's configured power-button sleep gesture(s) to the matching wake
// gesture(s), so waking mirrors however the device was put to sleep: a long hold
// wakes from a long-hold sleep, a double click wakes from a double-click sleep, and
// so on. Several can be configured at once (e.g. short AND long both power off), in
// which case any one of them wakes it. Relies on SETTINGS already having been loaded
// via loadStartupFromNvs() — btnShortPower/btnDoublePower/btnLongPower are NVS-backed
// specifically so this decision doesn't need the full settings file loaded from SD.
static HalGPIO::WakeGestures wakeGestureFromSettings() {
  using BA = CrossPointSettings::BUTTON_ACTION;
  HalGPIO::WakeGestures gestures;
  gestures.shortAllowed = SETTINGS.btnShortPower == BA::BTN_SLEEP;
  gestures.doubleClick = SETTINGS.btnDoublePower == BA::BTN_SLEEP;
  // Long-press-to-sleep is not user-remappable (loop()'s power-hold timer always owns
  // it, independent of btnLongPower's value — see the "not user-remappable" comment at
  // its call site), so long-hold-to-wake is unconditionally enabled to match.
  gestures.longHold = true;
  return gestures;
}

void setup() {
  markBootPhase(BootPhase::SetupEntry);
  // Load just the settings we need before any other init, so the wake gesture mirrors
  // whichever press type(s) the user configured to put the device to sleep.
  SETTINGS.loadStartupFromNvs();
  markBootPhase(BootPhase::NvsSettings);
  bootWakeCheck =
      gpio.verifyPowerButtonWakeup(wakeGestureFromSettings(), CrossPointSettings::getPowerWakeHoldDuration());
  markBootPhase(BootPhase::WakeGate);

  // print_errors=true: the corrupt block's address and overwritten values go to the
  // boot console (USB-CDC on boot — the same channel the panic dumps reach), giving the
  // exact damaged block for THIS build so the written value can be symbolized.
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
  s_heapOkSetupEntry = heap_caps_check_integrity_all(/*print_errors=*/true);
#else
  heap_caps_check_integrity_all(/*print_errors=*/true);
#endif
  {
    esp_ota_img_states_t otaState;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &otaState) == ESP_OK && otaState == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
    }
  }

  // Heap integrity check — must run before any allocation.
  //
  // The ESP32-C3 does not zero DRAM on a soft/WDT reset, so the heap metadata
  // from the previous boot is still in place. If that boot ended with a watchdog
  // timeout while malloc/free was mid-flight, the TLSF free-block sentinel
  // (0xabba1234) will have been partially overwritten. Any subsequent heap walk
  // (malloc, free, getMaxAllocHeap, heap_caps_check_integrity) will then read
  // the corrupted sentinel and either crash or loop until the interrupt WDT fires,
  // producing another WDT reset — a self-perpetuating crash loop.
  //
  // Mitigation: detect corruption here and reset immediately via esp_restart().
  // esp_restart() triggers a SW_CPU_RESET which goes through the ROM bootloader,
  // re-initialises DRAM fully, and produces a clean heap on the following boot.
  // A RTC_NOINIT latch prevents infinite loops if the corruption re-appears.
  //
  // print_errors=true writes details to UART0 before Arduino Serial is open.
  {
    static constexpr uint32_t HEAP_CORRUPTION_BOOT_MAGIC = 0xBEEF4321;
    const bool alreadyAttempted = (heapCorruptionBootLatch == HEAP_CORRUPTION_BOOT_MAGIC);
    if (!alreadyAttempted && !heap_caps_check_integrity_all(/*print_errors=*/true)) {
      heapCorruptionBootLatch = HEAP_CORRUPTION_BOOT_MAGIC;
      esp_restart();  // clean DRAM reset — heap will be intact on next boot
    }
    heapCorruptionBootLatch = 0;  // clear latch on a clean-heap boot
  }
  const bool heapIntactAtBoot = heap_caps_check_integrity_all(/*print_errors=*/false);

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t silentRebootTargetSnapshot =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_MAX) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  if (!isSilentReboot) {
    heapRecoveryRestartLatch = 0;
  }

  // When rebooting back into the USB serial-transfer activity (after the host's
  // open-triggered reset), keep all boot LOG_* off the wire so the reconnecting
  // host sees a clean binary protocol stream, not interleaved boot logs. The
  // activity unmutes on exit; a plain boot is unaffected.
  if (silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SERIAL_TRANSFER) {
    setSerialWireMuted(true);
  }

  HalSystem::begin();
  // Create the shared-SPI-bus mutex before anything can touch the panel or the
  // SD card, so no first-use allocation happens inside a refresh or read path.
  HalSpiBus::begin();
  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  gpio_deep_sleep_hold_dis();  // Release deep sleep GPIO hold state from previous sleep cycle
  markBootPhase(BootPhase::HwInit);

  const auto wakeupReason = gpio.getWakeupReason();

  if (wakeupReason == HalGPIO::WakeupReason::AfterUSBPower) {
    // If USB power caused a cold boot, go back to sleep immediately without initializing subsystems
    LOG_DBG("MAIN", "Wakeup reason: After USB Power => Deep sleep");
    halTiltSensor.deepSleep();
    powerManager.startDeepSleep(gpio);
    return;
  }

#ifdef ENABLE_SERIAL_LOG
  if (gpio.isUsbConnected()) {
    // Enlarge the USB-CDC RX buffer from the 256-byte default before begin().
    // The serial file-transfer protocol receives 2048-byte chunks in bursts; a
    // 256-byte ring drops bytes whenever the byte-by-byte drain stalls briefly,
    // which surfaces as "Timeout waiting for ACK" / failed uploads. 4096 matches
    // MicroReader's usb_serial_jtag rx_buffer_size. setRxBufferSize() recreates
    // the queue, so it takes effect even though CDC_ON_BOOT already ran begin().
    logSerial.setRxBufferSize(4096);
    // Enlarge the TX buffer too (default 256) so file downloads stream out in
    // larger bursts without the device having to block mid-chunk on a full ring
    // (a full ring that doesn't drain within HWCDC's tx timeout flips the link
    // to "disconnected" and silently drops TX).
    logSerial.setTxBufferSize(8192);
    Serial.begin(115200);
    const unsigned long start = millis();
    while (!Serial && (millis() - start) < 500) {
      delay(10);
    }
    markBootPhase(BootPhase::SerialUp);
  }
#endif

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
  // The gate ran before Serial was open, so this is the first chance to report it. INF
  // level: a rejected wake leaves no other trace, and release builds must be able to
  // answer "why did nothing happen when I pressed power".
  LOG_INF("BOOT", "Wake gate: %s (decided at %u ms, gate saw the press for %u ms, required %u ms)",
          HalGPIO::wakeVerdictName(bootWakeCheck.verdict), bootWakeCheck.decidedAtMs, bootWakeCheck.heldMs,
          CrossPointSettings::getPowerWakeHoldDuration());
  LOG_DBG("MAIN", "Wakeup reason: %d, millis=%lu, rawPowerPin=%d", static_cast<int>(wakeupReason), millis(),
          digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
  // Re-log the boot-time heap integrity results now that serial is open. The three
  // probes bracket the boot phases: preCtors CORRUPT ⇒ IDF/Arduino SDK init;
  // setupEntry CORRUPT (preCtors OK) ⇒ one of our C++ global constructors;
  // postOta CORRUPT (setupEntry OK) ⇒ the OTA-rollback/NVS block at the top of setup().
  LOG_INF("MEM", "Heap integrity at boot: preCtors=%s setupEntry=%s postOta=%s", s_heapOkPreCtors ? "OK" : "CORRUPT",
          s_heapOkSetupEntry ? "OK" : "CORRUPT", heapIntactAtBoot ? "OK" : "CORRUPT");
  {
    // Static-init bisect pairs (see BootHeapProbe.h). A CORRUPT slot whose paired "pre"
    // slot is OK convicts the global bracketed by that pair; all-OK pairs with
    // setupEntry=CORRUPT mean the writer is a global in an unprobed translation unit.
    const bool* s = bootHeapProbeSlots();
    LOG_INF(
        "MEM",
        "Static-init probes: preDisplay=%s postDisplay=%s preTheme=%s postTheme=%s mainFirst=%s mainLast=%s "
        "preHyphenation=%s postHyphenation=%s preGPIO=%s postGPIO=%s prePower=%s postPower=%s preTilt=%s postTilt=%s",
        s[0] ? "OK" : "CORRUPT", s[1] ? "OK" : "CORRUPT", s[2] ? "OK" : "CORRUPT", s[3] ? "OK" : "CORRUPT",
        s[4] ? "OK" : "CORRUPT", s[5] ? "OK" : "CORRUPT", s[6] ? "OK" : "CORRUPT", s[7] ? "OK" : "CORRUPT",
        s[8] ? "OK" : "CORRUPT", s[9] ? "OK" : "CORRUPT", s[10] ? "OK" : "CORRUPT", s[11] ? "OK" : "CORRUPT",
        s[12] ? "OK" : "CORRUPT", s[13] ? "OK" : "CORRUPT");
  }
  if (!s_heapOkSetupEntry) {
    // Corruption geometry: the bad head sits at SOC_ROM_STACK_START - 0x2000 across
    // builds — exactly 8 KB below the heap's top boundary, which abuts the ROM/startup
    // stack that static init runs on. Dump the words around the canary: a downward
    // stack spill leaves 0x42xxxxxx return addresses and 0x3fcdxxxx frame pointers;
    // anything else points at a different writer.
    const uintptr_t suspect = SOC_ROM_STACK_START - 0x2000;
    const uint32_t* base = reinterpret_cast<const uint32_t*>(suspect - 0x40);
    for (int row = 0; row < 8; row++) {
      LOG_INF("MEM", "dump %08x: %08lx %08lx %08lx %08lx", static_cast<unsigned>(suspect - 0x40 + row * 16),
              static_cast<unsigned long>(base[row * 4]), static_cast<unsigned long>(base[row * 4 + 1]),
              static_cast<unsigned long>(base[row * 4 + 2]), static_cast<unsigned long>(base[row * 4 + 3]));
    }
  }
#endif
  logStartupMemory("after_hw_init");

  if (wakeupReason == HalGPIO::WakeupReason::PowerButton && !bootWakeCheck.accepted()) {
    LOG_INF("BOOT", "Wake gate rejected the press (%s), returning to deep sleep",
            HalGPIO::wakeVerdictName(bootWakeCheck.verdict));
    halTiltSensor.deepSleep();
    powerManager.startDeepSleep(gpio);
    return;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // This window sits between the wake gesture and the splash, so every millisecond here
    // is dead time on a dark screen — it measured 501-508 ms across X3 and X4, roughly
    // 15% of time-to-logo, spent to answer one yes/no question.
    //
    // The combo itself is read from fresh ADC samples (isHeldNow), which need no warm-up.
    // The loop remains only to prime the cache that isPressed() reads, because the "hold
    // Back at boot to skip resuming the reader" check further down consumes that cache and
    // nothing calls update() in between. The 5 ms debounce means a handful of passes is
    // enough; the 500 ms this replaces was not backed by any documented HalGPIO contract.
    const unsigned long primeStart = millis();
    while (millis() - primeStart < 60) {
      gpio.update();
      delay(10);
    }
    if (gpio.isHeldNow(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
    markBootPhase(BootPhase::RecoverySettle);
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    markBootPhase(BootPhase::DisplayFonts);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    markBootPhase(BootPhase::FirstPaint);
    logBootSummary();
    return;
  }
  markBootPhase(BootPhase::SdMount);
  logStartupMemory("after_storage_begin");

  SETTINGS.loadFromFile();
  // APP_STATE is needed before display init so Quick Resume can skip the on-wake resync
  // and so the seamless-wake path can paint the LoadingIcon over the restored framebuffer.
  APP_STATE.loadFromFile();

  HalSystem::checkPanic();
  HalSystem::clearPanic();  // TODO: move this to an activity when we have one to display the panic info
  HalClock::applyTimezone(SETTINGS.timeZone);
  I18N.loadSettings();
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  WEATHER_SETTINGS.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  // In LandscapeCounterClockwise the front-button strip runs bottom-to-top on screen, so the
  // logical Left/Right roles (and their hints) swap to keep "previous/Up" above "next/Down".
  // See MappedInputManager::setStripReversedPredicate and issue #87.
  MappedInputManager::setStripReversedPredicate(
      [] { return renderer.getOrientation() == GfxRenderer::Orientation::LandscapeCounterClockwise; });

  markBootPhase(BootPhase::ConfigLoad);
  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);
  logStartupMemory("before_display_fonts");

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  //
  // Both suppressed-splash flows set showBootScreen=false, so the saved frame is what tells
  // them apart: enterDeepSleep() persists one only for Quick Resume. Present means "restore it
  // and paint the loading icon"; absent means a plain reader resume, which paints nothing at
  // boot so the page gets the X3's one mandatory full sync (see BootResume::ReaderResume).
  const bool haveSleepFrame = !APP_STATE.showBootScreen && Storage.exists(SLEEP_FRAME_FILE);
  const BootResume resume = isSilentReboot             ? BootResume::Silent
                            : APP_STATE.showBootScreen ? BootResume::Splash
                            : haveSleepFrame           ? BootResume::QuickResume
                                                       : BootResume::ReaderResume;

  // Booting straight into the USB serial-transfer activity? Skip SD-font
  // discovery (only built-in UI fonts are used there) to shorten the reboot the
  // host is waiting through. Safe because that activity reboots on exit.
  const bool bootToSerialTransfer = (silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SERIAL_TRANSFER);
  setupDisplayAndFonts(resume != BootResume::Splash, bootToSerialTransfer);
  markBootPhase(BootPhase::DisplayFonts);
  logStartupMemory("after_display_fonts");

  switch (resume) {
    case BootResume::Silent:
      // After a silent reboot the panel still shows the previous session's pixels but
      // the SDK's RED-RAM diff buffer was cleared by begin(). A FAST refresh would only
      // flip pixels the SDK *thinks* changed, leaving the old screen visible. Force the
      // first paint to HALF_REFRESH so the panel cleanly repaints; subsequent paints
      // resume FAST as normal.
      renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon. Worth the mandatory full
        // sync here because the restored frame IS the reader page — the user reads their book
        // while it loads rather than watching an interstitial.
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::ReaderResume:
      // Deliberately paints nothing: the panel keeps physically showing the sleep screen until
      // the reader's own first render lands, so the X3's single mandatory full sync is spent on
      // the page instead of an interstitial. Still re-arms the splash for the next plain boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }
  // The paints above are synchronous (BootActivity::onEnter -> displayBuffer), so this
  // stamp is the moment the panel finished the waveform — i.e. when the user actually
  // sees the logo. On a power-button boot it is also the first visible feedback of any
  // kind, which is what makes the gap back to `gate` the number that matters.
  markBootPhase(BootPhase::FirstPaint);

  HalClock::restore();
  RECENT_BOOKS.loadFromFile();
  GLOBAL_BOOKMARKS.load();
  READING_STATS.loadFromFile();
  markBootPhase(BootPhase::StoreLoad);

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (resume == BootResume::Silent && (silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SLEEP ||
                                              silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SLEEP_TIMEOUT)) {
    // Sleep was requested while the framebuffers were released (web server
    // session). They exist again now — finish the interrupted sleep transition.
    // The forced HALF_REFRESH from the Silent branch above cleanly repaints the
    // panel (still showing the old session's pixels) with the sleep screen.
    LOG_INF("MAIN", "Resuming sleep transition after framebuffer-recovery reboot");
    enterDeepSleep(/*fromTimeout=*/silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SLEEP_TIMEOUT);
    return;  // not reached — enterDeepSleep() never returns
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_SERIAL_TRANSFER) {
    // Reset fired while the USB transfer screen was open (host opened the port):
    // land straight back in it so the host's retried command is served.
    activityManager.goToSerialTransfer();
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_CLOCK_SETTINGS) {
    activityManager.goToClockSettings();
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_KOREADER_SETTINGS) {
    activityManager.goToKOReaderSettings();
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    // This is the wake-straight-back-into-the-book branch: tell the wake trace that the open
    // it is about to see is a resume, so its summary line separates resume cost from the cost
    // of an ordinary library open.
    WakeTrace::armResume();
    activityManager.goToReader(path);
  }

  markBootPhase(BootPhase::ActivityRoute);
  logStartupMemory("after_activity_route");

  // The wake press may still be held. Arm the latch that swallows it (and its release
  // bounce) from the main loop rather than blocking here — see bootPowerReleasePending.
  // Skip on silent reboot: the firmware triggered the restart, so the button isn't held.
  if (!isSilentReboot) {
    bootPowerReleasePending = true;
    bootPowerHighSince = 0;
  }
  // All boot-time power-button handling (which drives inputMgr.update() directly)
  // is done — hand button sampling to the background sampler so presses are caught
  // on a steady ~10ms cadence even while the loop task is busy building sections.
  gpio.startInputSampler();
  // Now that the background sampler is live, let long lib-layer tasks (cover image
  // decoders) poll for queued presses and yield so button input keeps priority.
  CooperativeAbort::setLongTaskAbortPredicate(&hasPendingButtonInput);
  // Flush any pin state transitions that occurred during boot before entering the main loop
  mappedInputManager.update();
  buttonEventManager.drain();

  // Block held-power sleep for the first 2 seconds. On Quick Resume the user often releases
  // the wake-press a fraction late; without this guard the loop() power-hold check would
  // immediately fire enterDeepSleep().
  allowSleepAt = millis() + 2000;
  logBootSummary();
  logStartupMemory("setup_complete");
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  buttonEventManager.update();
  // Must follow the drain above and precede every power consumer below.
  serviceBootPowerRelease();
  HalClock::updatePeriodic();
  halTiltSensor.update(static_cast<CrossPointTiltPageTurn::Value>(SETTINGS.tiltPageTurn),
                       static_cast<CrossPointOrientation::Value>(SETTINGS.orientation),
                       activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);
  renderer.setTextDarkness(SETTINGS.textDarkness);

  // Re-emit the boot summary when the serial link comes up. A power-up from deep sleep
  // re-enumerates USB, so a monitor attached across the wake misses everything setup()
  // printed — including the trace itself, which is the one line worth having.
  //
  // Strictly bounded, because `Serial` is not a reliable connect signal: HWCDC flips
  // itself to "disconnected" whenever its TX ring doesn't drain within the tx timeout
  // (see the buffer-sizing comment in setup()), which on a busy boot flaps several times
  // a second and once produced eight copies of this summary. The cap keeps a flapping
  // link from spamming the log while still catching a monitor attached late; anything
  // later than that is what `CMD:BOOTLOG` is for.
  {
    // Seeded true because setup() just emitted the summary: if the link was already up
    // then, this must not fire again on the first tick. A battery boot clears it on that
    // same tick (Serial is false), so a cable plugged in later still produces the edge.
    static bool serialWasUp = true;
    static uint8_t bootSummaryRepeats = 0;
    constexpr uint8_t MAX_BOOT_SUMMARY_REPEATS = 2;
    const bool serialIsUp = static_cast<bool>(Serial);
    if (serialIsUp && !serialWasUp && bootSummaryRepeats < MAX_BOOT_SUMMARY_REPEATS) {
      bootSummaryRepeats++;
      logBootSummary();
    }
    serialWasUp = serialIsUp;
  }

  if (Serial && millis() - lastMemPrint >= 10000) {
    // Keep runtime log lightweight: ESP.getMaxAllocHeap() walks heap metadata
    // and has triggered interrupt WDTs under heavy allocation churn.
    // CPU MHz included so the power-saving state (10 = idle low-power /
    // waveform wait, 160 = normal) is visible in a steady-state log.
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, CPU: %lu MHz", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), static_cast<unsigned long>(getCpuFrequencyMhz()));
    // Right-sizing aid for the background button sampler task (2 KB allocated).
    // High-water is the min free stack ever seen; shrink the xTaskCreate size if
    // this stays comfortably high across a session.
    LOG_INF("MEM", "btnSampler stack high-water=%u bytes free (min ever)",
            static_cast<unsigned>(gpio.samplerStackHighWater()));
#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
    // loop() runs on the Arduino loopTask (8 KB stack), which also drives the reader's
    // sliced background section builds (serviceBackgroundWork → createSectionFile /
    // HTML parse / image dimension reads). That task is NOT covered by the render-task
    // stack instrumentation, yet is an equally plausible source of a stack-into-heap
    // spill (the original crash SP sat against SOC_ROM_STACK_START). High-water is the
    // minimum free stack ever seen by this task (bytes); a small/shrinking value here
    // would point the finger at the loop task rather than the render task.
    LOG_INF("MEM", "loopTask stack high-water=%u bytes free (min ever)",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings.
  // Skip while an activity owns the serial input (e.g. the USB serial
  // file-transfer activity): it drives a binary protocol on logSerial and this
  // line reader would otherwise steal bytes from its stream.
  if (logSerial.available() > 0 && !activityManager.currentOwnsSerialInput()) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        uint8_t* buf = display.getFrameBuffer();
        if (buf) {
          const uint16_t width = display.getDisplayWidth();
          const uint16_t height = display.getDisplayHeight();
          const uint32_t bufferSize = display.getBufferSize();
          logSerial.printf("SCREENSHOT_START:%d:%d:%d\n", width, height, bufferSize);
          logSerial.write(buf, bufferSize);
          logSerial.printf("SCREENSHOT_END\n");
        } else {
          // Framebuffers are released during the web server session — nothing to send.
          logSerial.printf("SCREENSHOT_ERROR:framebuffer released\n");
        }
      } else if (cmd == "BOOTLOG") {
        // On-demand replay of this session's boot summary, for when the monitor was
        // attached too late to catch it and the automatic repeats are used up.
        logBootSummary();
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Power-hold timer for sleep. Hoisted above the screenshot block so the
  // screenshot path can clear it and avoid a stale POWER press triggering sleep
  // after the screenshot completes.
  static unsigned long powerHoldStart = 0;

  static bool screenshotButtonsReleased = true;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        // The reader may have left a pre-rendered next page in the frame buffer; ask the
        // current activity to redraw the visible page first so the screenshot matches the screen.
        activityManager.prepareFramebufferForCapture();
        ScreenshotUtil::takeScreenshot(renderer);
      }
      // Discard the POWER+DOWN presses so they don't fire Short/Long events
      // (e.g. page turn, sleep) once the user releases the combo.
      buttonEventManager.drain();
      powerHoldStart = 0;
    }
    return;
  } else {
    screenshotButtonsReleased = true;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(/*fromTimeout=*/true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Track power button hold for sleep.  We require a fresh press edge (wasPressed)
  // before starting to measure hold time, so that a hold carried over from boot
  // (wake-up press) is never misinterpreted as a "go to sleep" press.
  // The power button long-press is not user-remappable, so this path always owns it.
  // Sleep mapped to other buttons is handled by the dispatcher's BTN_SLEEP case below.
  if (gpio.wasPressed(HalGPIO::BTN_POWER)) {
    powerHoldStart = millis();
    LOG_DBG("MAIN", "loop: power button press detected (fresh edge)");
  }
  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) && powerHoldStart > 0) {
    const unsigned long heldTime = millis() - powerHoldStart;
    if (heldTime > SETTINGS.getPowerButtonDuration()) {
      // If the screenshot combination is potentially being pressed, don't sleep
      if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
        return;
      }
      LOG_DBG("MAIN", "loop: power button held for %lu ms (> %u ms), entering deep sleep", heldTime,
              SETTINGS.getPowerButtonDuration());
      enterDeepSleep();
      // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
      return;
    }
  }

  if (!gpio.isPressed(HalGPIO::BTN_POWER)) {
    powerHoldStart = 0;
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  // Dispatch globally-configured button actions before handing control to the activity.
  // Only non-Default actions are intercepted here; Default falls through to the activity.
  {
    using BA = CrossPointSettings::BUTTON_ACTION;
    using B = MappedInputManager::Button;
    auto actionFor = [&](const ButtonEventManager::ButtonEvent& ev) -> uint8_t {
      switch (ev.button) {
        case B::Back:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortBack;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleBack;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongBack;
          }
          break;
        case B::Confirm:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortConfirm;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleConfirm;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongConfirm;
          }
          break;
        case B::Left:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortLeft;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleLeft;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongLeft;
          }
          break;
        case B::Right:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortRight;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoubleRight;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongRight;
          }
          break;
        case B::PageBack:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortPageBack;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoublePageBack;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongPageBack;
          }
          break;
        case B::PageForward:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortPageForward;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoublePageForward;
            case ButtonEventManager::PressType::Long:
              return SETTINGS.btnLongPageForward;
          }
          break;
        case B::Power:
          switch (ev.type) {
            case ButtonEventManager::PressType::Short:
              return SETTINGS.btnShortPower;
            case ButtonEventManager::PressType::Double:
              return SETTINGS.btnDoublePower;
            case ButtonEventManager::PressType::Long:
              // Unreachable, and left that way on purpose. The power-hold timer above
              // sleeps the device at getPowerButtonDuration() (400 ms), so the press is
              // long gone before ButtonEventManager's LONG_PRESS_MS (1000 ms) could emit a
              // Long for Power. btnLongPower survives only as the settings screen's way of
              // stating "Long press: Sleep" (SettingsList.h gives it that single option and
              // no selector); dispatching it here would imply it were configurable.
              break;
          }
          break;
        default:
          break;  // Up/Down are the PageBack/PageForward buttons under their list-navigation
                  // names. The FSM emits both names for one press, and the PageBack/PageForward
                  // event above already carries the configured action — so these fall through
                  // to the activity, which is what list-style activities match on.
      }
      return BA::BTN_DEFAULT;
    };
    // Reader-scoped actions are only meaningful inside the reader (they funnel through
    // dispatchButtonAction(), a no-op elsewhere). When such an action is configured on a
    // button but we are NOT in the reader, intercepting it would silently SWALLOW the
    // event — shadowing the current activity's own built-in handling for that button
    // (e.g. RecentBooks long-Left=remove / long-Right=info, which defaulted to
    // BTN_PREV_SECTION / BTN_NEXT_SECTION). Outside the reader we must let the original
    // (button, pressType) event fall through to the activity instead.
    auto isReaderScopedAction = [](uint8_t a) {
      switch (static_cast<BA>(a)) {
        case BA::BTN_PAGE_FORWARD:
        case BA::BTN_PAGE_BACK:
        case BA::BTN_PAGE_FORWARD_10:
        case BA::BTN_PAGE_BACK_10:
        case BA::BTN_OPEN_TOC:
        case BA::BTN_STAR_PAGE:
        case BA::BTN_FOOTNOTES:
        case BA::BTN_NEXT_SECTION:
        case BA::BTN_PREV_SECTION:
        case BA::BTN_EXIT_READER:
        case BA::BTN_READER_MENU:
        case BA::BTN_TOGGLE_BIONIC_READING:
        case BA::BTN_KOREADER_SYNC:
        case BA::BTN_CYCLE_FONT_SIZE:
        case BA::BTN_CYCLE_ORIENTATION:
        case BA::BTN_QUICK_OVERRIDES:
          return true;
        default:  // BTN_GO_HOME / BTN_SLEEP / BTN_FORCE_*_REFRESH / BTN_OPEN_BOOKMARKS / BTN_IGNORE are global
          return false;
      }
    };
    ButtonEventManager::ButtonEvent ev;
    std::vector<ButtonEventManager::ButtonEvent> defaultEvents;
    defaultEvents.reserve(8);
    while (buttonEventManager.consumeEvent(ev)) {
      const uint8_t action = actionFor(ev);
      // Fall through to the activity when the event has no global effect here: either an
      // explicit Default mapping, or a reader-scoped action while not in the reader.
      if (action == BA::BTN_DEFAULT || (isReaderScopedAction(action) && !activityManager.isReaderActivity())) {
        defaultEvents.push_back(ev);
        continue;
      }

      switch (static_cast<BA>(action)) {
        case BA::BTN_PAGE_FORWARD:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_FORWARD);
          break;
        case BA::BTN_PAGE_BACK:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_BACK);
          break;
        case BA::BTN_PAGE_FORWARD_10:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_FORWARD_10);
          break;
        case BA::BTN_PAGE_BACK_10:
          activityManager.dispatchButtonAction(BA::BTN_PAGE_BACK_10);
          break;
        case BA::BTN_GO_HOME:
          activityManager.goHome();
          break;
        case BA::BTN_SLEEP:
          enterDeepSleep();
          return;  // enterDeepSleep() never returns, but return here to stop processing
        case BA::BTN_FORCE_REFRESH: {
          // In the reader, route through the activity so it re-displays the CURRENT page in
          // the requested mode (a raw displayBuffer() here can flush a Background-A pre-render
          // of the next page, which looks like a page turn). Elsewhere, raw-flush is correct.
          if (activityManager.isReaderActivity()) {
            activityManager.dispatchButtonAction(BA::BTN_FORCE_REFRESH);
          } else {
            RenderLock lock;
            renderer.displayBuffer(HalDisplay::HALF_REFRESH);
          }
          break;
        }
        case BA::BTN_FORCE_FAST_REFRESH: {
          if (activityManager.isReaderActivity()) {
            activityManager.dispatchButtonAction(BA::BTN_FORCE_FAST_REFRESH);
          } else {
            RenderLock lock;
            renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          }
          break;
        }
        case BA::BTN_OPEN_TOC:
          activityManager.dispatchButtonAction(BA::BTN_OPEN_TOC);
          break;
        case BA::BTN_OPEN_BOOKMARKS:
          activityManager.goToGlobalBookmarks();
          break;
        case BA::BTN_STAR_PAGE:
          activityManager.dispatchButtonAction(BA::BTN_STAR_PAGE);
          break;
        case BA::BTN_FOOTNOTES:
          activityManager.dispatchButtonAction(BA::BTN_FOOTNOTES);
          break;
        case BA::BTN_NEXT_SECTION:
          activityManager.dispatchButtonAction(BA::BTN_NEXT_SECTION);
          break;
        case BA::BTN_PREV_SECTION:
          activityManager.dispatchButtonAction(BA::BTN_PREV_SECTION);
          break;
        case BA::BTN_EXIT_READER:
          activityManager.dispatchButtonAction(BA::BTN_EXIT_READER);
          break;
        case BA::BTN_READER_MENU:
          activityManager.dispatchButtonAction(BA::BTN_READER_MENU);
          break;
        case BA::BTN_KOREADER_SYNC:
          activityManager.dispatchButtonAction(BA::BTN_KOREADER_SYNC);
          break;
        case BA::BTN_TOGGLE_BIONIC_READING:
          activityManager.dispatchButtonAction(BA::BTN_TOGGLE_BIONIC_READING);
          break;
        case BA::BTN_CYCLE_FONT_SIZE:
          activityManager.dispatchButtonAction(BA::BTN_CYCLE_FONT_SIZE);
          break;
        case BA::BTN_CYCLE_ORIENTATION:
          activityManager.dispatchButtonAction(BA::BTN_CYCLE_ORIENTATION);
          break;
        case BA::BTN_QUICK_OVERRIDES:
          activityManager.dispatchButtonAction(BA::BTN_QUICK_OVERRIDES);
          break;
        case BA::BTN_IGNORE:
          // Explicit "do nothing": swallow the event so neither a global action nor the
          // activity's built-in behaviour fires. A press that produced a Long emits no
          // Short, so a long-press mapped to Ignore is fully inert — the release cannot
          // leak through as a page turn.
          break;
        default:
          break;
      }
    }

    for (auto it = defaultEvents.rbegin(); it != defaultEvents.rend(); ++it) {
      buttonEventManager.pushEventFront(it->button, it->type);
    }
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    // Two-stage idle backoff, ported from crosspoint-reader PR #2525 (Brian Pugh /
    // @BrianPugh), which measured 9.68 mA -> 2.78 mA idle on an X3 with a PPK2.
    const unsigned long idleMs = millis() - lastActivityTime;
    // A still-held wake press is flushed before it reaches lastActivityTime, so the loop
    // reads as idle while the finger is down. Light sleep arms the power pin as a LEVEL
    // wake, which an already-LOW pin satisfies immediately: the chip would spin through
    // arm/sleep/reject/disarm every tick for as long as the user holds. Poll instead.
    if (idleMs >= HalPowerManager::IDLE_LIGHT_SLEEP_MS && !bootPowerReleasePending) {
      // Idle: light-sleep between polls instead of busy-delaying. Race-to-sleep —
      // run the brief wake windows at the normal clock, not LOW_POWER_FREQ. The
      // board's sleep-floor current is paid per millisecond regardless of CPU
      // speed, so finishing the per-wake work ~16x faster and returning to sleep
      // costs less charge than stretching the window at 10 MHz. The downclock
      // below only serves the pre-sleep 100 Hz polling phase. When lightSleep()
      // declines, the fallback delay() runs at the normal clock too — but that
      // only happens when USB (externally powered), WiFi, or a render Lock (full
      // speed wanted anyway) is active.
      powerManager.setPowerSaving(false);
      if (!powerManager.lightSleep(gpio)) {
        delay(10);  // declined (render Lock, WiFi, USB, mid-debounce): poll at 100 Hz
      }
    } else {
      // Response window after recent input: keep 100 Hz polling for snappy
      // interaction, but downclock once rapid-input bursts have settled — renders
      // re-raise the clock via HalPowerManager::Lock, so full speed only serves
      // loop bookkeeping here.
      if (idleMs >= HalPowerManager::IDLE_DOWNCLOCK_MS) {
        powerManager.setPowerSaving(true);
      }
      delay(10);
    }
  }
}
