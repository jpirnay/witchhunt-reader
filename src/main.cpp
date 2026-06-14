#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
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

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
#include <BootHeapProbe.h>
// Static-init heap probes bracketing this TU's globals (slots 4/5); see BootHeapProbe.h.
static BootHeapProbe s_probeMainFirst(4);
#endif
MappedInputManager mappedInputManager(gpio);
ButtonEventManager buttonEventManager(mappedInputManager);
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
constexpr uint32_t HEAP_RECOVERY_RESTART_LATCH_MAGIC = 0x48EA9C01;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
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

// ---- Quick Resume framebuffer persistence ----
//
// Quick Resume keeps the last reader page on screen during deep sleep (just overlaid with a moon
// icon) and skips the boot screen on wake by restoring the saved framebuffer + a loading icon.
// The buffer is persisted to SD only when the user actually enabled the feature.
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

// Enter deep sleep mode. fromTimeout=true marks an auto-sleep (gates "Quick Resume on Timeout").
void enterDeepSleep(bool fromTimeout = false) {
  LOG_DBG("MAIN", "enterDeepSleep called at millis=%lu, powerBtn isPressed=%d, rawPin=%d", millis(),
          gpio.isPressed(HalGPIO::BTN_POWER), digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);
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
  APP_STATE.showBootScreen = !isQuickResumeSleep;

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

  // Persist the moon-icon-overlaid framebuffer after goToSleep() has painted it.
  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  halTiltSensor.deepSleep();

  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep (powerBtn isPressed=%d, rawPin=%d)", gpio.isPressed(HalGPIO::BTN_POWER),
          digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);

  powerManager.startDeepSleep(gpio, keepLpAlive);
}

void setupDisplayAndFonts(bool seamless = false) {
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
  sdFontSystem.begin(renderer);

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

void setup() {
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
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  if (!isSilentReboot) {
    heapRecoveryRestartLatch = 0;
  }

  HalSystem::begin();
  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  gpio_deep_sleep_hold_dis();  // Release deep sleep GPIO hold state from previous sleep cycle

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
    Serial.begin(115200);
    const unsigned long start = millis();
    while (!Serial && (millis() - start) < 500) {
      delay(10);
    }
  }
#endif

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
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

  // Load just the settings we need *before* initializing the SD card to speed up and reduce power on unverified wakes
  SETTINGS.loadStartupFromNvs();

  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    LOG_DBG("MAIN", "Verifying power button press duration (required=%u ms)",
            CrossPointSettings::getPowerButtonDuration());

    // We only want to skip the hold verification (allowing a short press to wake) if the short
    // press or double press actually have an action assigned, or if the clock screensaver is active.
    // Otherwise, short presses from sleep should be ignored entirely and return to sleep.
    //
    // X3 always cuts all power during sleep (battery-latch MOSFET, keepClockAlive=false), so any
    // wakeup is a full cold boot. By the time verifyPowerButtonWakeup runs the button may already
    // be released — hardware design guarantees the press was intentional, so skip hold-verification.
    bool allowShortPress = gpio.deviceIsX3() || (SETTINGS.useClock != 0) ||
                           (SETTINGS.btnShortPower != CrossPointSettings::BTN_DEFAULT) ||
                           (SETTINGS.btnDoublePower != CrossPointSettings::BTN_DEFAULT);

    gpio.verifyPowerButtonWakeup(CrossPointSettings::getPowerButtonDuration(), allowShortPress);
    LOG_DBG("MAIN", "Power button verification passed, millis=%lu", millis());
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }
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

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);
  logStartupMemory("before_display_fonts");

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  setupDisplayAndFonts(resume != BootResume::Splash);
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
        // Frame restored: swap the sleep moon for the loading icon.
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  HalClock::restore();
  RECENT_BOOKS.loadFromFile();
  GLOBAL_BOOKMARKS.load();
  READING_STATS.loadFromFile();

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (resume == BootResume::Silent && silentRebootTargetSnapshot == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
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
    activityManager.goToReader(path);
  }

  logStartupMemory("after_activity_route");

  // Ensure we're not still holding the power button before leaving setup.
  // waitForStablePowerRelease protects against switch bounce that might register as a false double-press.
  // Skip on silent reboot: the firmware triggered the restart, so the button isn't held.
  if (!isSilentReboot) {
    gpio.waitForStablePowerRelease();
  }
  // Flush any pin state transitions that occurred during boot before entering the main loop
  mappedInputManager.update();
  buttonEventManager.drain();

  // Block held-power sleep for the first 2 seconds. On Quick Resume the user often releases
  // the wake-press a fraction late; without this guard the loop() power-hold check would
  // immediately fire enterDeepSleep().
  allowSleepAt = millis() + 2000;
  logStartupMemory("setup_complete");
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  buttonEventManager.update();
  HalClock::updatePeriodic();
  halTiltSensor.update(static_cast<CrossPointTiltPageTurn::Value>(SETTINGS.tiltPageTurn),
                       static_cast<CrossPointOrientation::Value>(SETTINGS.orientation),
                       activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);
  renderer.setTextDarkness(SETTINGS.textDarkness);

  if (Serial && millis() - lastMemPrint >= 10000) {
    // Keep runtime log lightweight: ESP.getMaxAllocHeap() walks heap metadata
    // and has triggered interrupt WDTs under heavy allocation churn.
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes", ESP.getFreeHeap(), ESP.getHeapSize(),
            ESP.getMinFreeHeap());
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
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint16_t width = display.getDisplayWidth();
        const uint16_t height = display.getDisplayHeight();
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d:%d:%d\n", width, height, bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
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
              return SETTINGS.btnLongPower;
          }
          break;
        default:
          break;  // Up/Down have no FSMs — ButtonEventManager never emits these
      }
      return BA::BTN_DEFAULT;
    };
    ButtonEventManager::ButtonEvent ev;
    std::vector<ButtonEventManager::ButtonEvent> defaultEvents;
    defaultEvents.reserve(8);
    while (buttonEventManager.consumeEvent(ev)) {
      const uint8_t action = actionFor(ev);
      if (action == BA::BTN_DEFAULT) {
        defaultEvents.push_back(ev);
        continue;
      }

      // When a non-default Long action fires for a page-turn button, mark it so that
      // detectPageTurn() suppresses the wasReleased-based page turn on button release.
      if (ev.type == ButtonEventManager::PressType::Long &&
          (ev.button == B::Left || ev.button == B::Right || ev.button == B::PageBack || ev.button == B::PageForward)) {
        buttonEventManager.markLongPressDispatched(ev.button);
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
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
