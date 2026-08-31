#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <iosfwd>

class CrossPointSettings {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  // Static instance
  static CrossPointSettings instance;

 public:
  // Delete copy constructor and assignment
  CrossPointSettings(const CrossPointSettings&) = delete;
  CrossPointSettings& operator=(const CrossPointSettings&) = delete;

  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    BLANK = 4,
    COVER_CUSTOM = 5,
    OVERLAY = 6,
    QUICK_RESUME = 7,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_IMAGE_PICK_MODE { PICK_RANDOM = 0, PICK_SEQUENTIAL = 1, SLEEP_IMAGE_PICK_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    // Greyscale like NO_FILTER, but stretches the image's own tonal range first.
    ADAPTIVE_TONE = 3,
    // Greyscale too, but levels via the histogram's own CDF rather than its endpoints.
    // Reaches bimodal images that ADAPTIVE_TONE leaves flat, at the cost of more dither
    // texture in smooth gradients -- see AdaptiveTone.h.
    EQUALIZE_TONE = 4,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };
  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Status bar enum - legacy
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum STATUS_BAR_ITEMS_POSITION {
    STATUS_BAR_ITEMS_TOP = 0,
    STATUS_BAR_ITEMS_BOTTOM = 1,
    STATUS_BAR_ITEMS_POSITION_COUNT
  };

  // Which end of the status-item lane the clock sits on. There is deliberately no centre option:
  // the middle of the lane is the title's slot, so a centred clock would either displace the title
  // or collide with it.
  enum STATUS_BAR_CLOCK_POSITION {
    STATUS_BAR_CLOCK_LEFT = 0,
    STATUS_BAR_CLOCK_RIGHT = 1,
    STATUS_BAR_CLOCK_POSITION_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { BOOKERLY = 0, NOTOSANS = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Font size options
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3, TINY = 4, FONT_SIZE_COUNT };
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, LYRA_CAROUSEL = 3 };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  // File browser sort mode (per-session, not persisted)
  enum FILE_SORT_MODE { SORT_BY_NAME = 0, SORT_BY_DATE = 1, SORT_BY_SIZE = 2, SORT_BY_TYPE = 3, FILE_SORT_MODE_COUNT };

  // File browser sort direction (per-session, not persisted)
  enum FILE_SORT_DIRECTION { SORT_ASCENDING = 0, SORT_DESCENDING = 1, FILE_SORT_DIRECTION_COUNT };

  // Action mapped to each tilt gesture direction.
  enum TILT_GESTURE_ACTION {
    TILT_ACT_NONE = 0,
    TILT_ACT_NEXT_PAGE = 1,
    TILT_ACT_PREV_PAGE = 2,
    TILT_GESTURE_ACTION_COUNT
  };

  // Text darkness for AA glyph rendering (forwarded to GfxRenderer::setTextDarkness)
  enum TEXT_DARKNESS {
    DARKNESS_NORMAL = 0,      // true 4-level AA
    DARKNESS_DARK = 1,        // historical default — bolder dark-gray AA
    DARKNESS_EXTRA_DARK = 2,  // both AA shades pushed to darkest gray state
    DARKNESS_MAXIMUM = 3,     // skip grayscale passes — AA pixels stay solid black from BW pass
    TEXT_DARKNESS_COUNT
  };
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  enum IMAGE_DITHERING {
    IMAGE_DITHER_BAYER = 0,
    IMAGE_DITHER_ATKINSON = 1,
    IMAGE_DITHER_DIFFUSED_BAYER = 2,
    IMAGE_DITHERING_COUNT
  };
#else
  enum IMAGE_DITHERING { IMAGE_DITHER_BAYER = 0, IMAGE_DITHERING_COUNT };
#endif

  // Timezone options (POSIX TZ rules for DST support)
  enum TIMEZONE {
    TZ_UTC = 0,
    TZ_CET = 1,
    TZ_EET = 2,
    TZ_MSK = 3,
    TZ_UTC_PLUS4 = 4,
    TZ_IST = 5,
    TZ_UTC_PLUS7 = 6,
    TZ_UTC_PLUS8 = 7,
    TZ_UTC_PLUS9 = 8,
    TZ_AEST = 9,
    TZ_NZST = 10,
    TZ_UTC_MINUS3 = 11,
    TZ_EST = 12,
    TZ_CST = 13,
    TZ_MST = 14,
    TZ_PST = 15,
    TZ_AST_ADT = 16,
    TZ_ACST_ACDT = 17,
    TZ_AKST_AKDT = 18,
    TIMEZONE_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Apply information overlay with reading progress on sleep cover
  uint8_t sleepCoverOverlay = 0;
  // Sleep image pick mode (random vs sequential walk-through)
  uint8_t sleepImagePickMode = PICK_RANDOM;
  // Quick Resume on Timeout: keep current page on display with a moon icon when sleeping by timeout,
  // and on wake restore the page directly (skipping the boot screen).
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;
  // Status bar settings (statusBar, statusBarProgressBar, statusBarProgressBarThickness retained for migration only)
  uint8_t statusBar = FULL;
  uint8_t statusBarChapterPageCount = 1;
  // Printed ("physical") page number from the book's page-list. Drawn in parentheses to the left of
  // the device page counter when both share a location; otherwise on its own. Default on.
  uint8_t statusBarPrintedPage = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarUpperProgressBar = HIDE_PROGRESS;
  uint8_t statusBarUpperProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarLowerProgressBar = HIDE_PROGRESS;
  uint8_t statusBarLowerProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarItemsPosition = STATUS_BAR_ITEMS_BOTTOM;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  // X3-only: when on, the AA refresh uses the 7-frame community grayscale LUT
  // (~130 ms panel time) instead of the OEM 53-frame LUT (~2.4 s). Mid-tones
  // run slightly darker than X4. Matches what papyrix-reader has shipped since
  // 2025-11. Default on — the 2.2 s/page win dwarfs the subtle mid-tone shift.
  // No effect on X4.
  //
  // JSON key was bumped from "fastAntiAliasing" to "fastAntiAliasingV2" when
  // the default flipped to 1: existing settings files with the old key are
  // ignored, so every device picks up the new C++ default on next load.
  uint8_t fastAntiAliasing = 1;
  // Text darkness (0 = normal, 1 = dark, 2 = extra dark). Default 1 preserves
  // historical AA rendering (both grayscale shades drawn in the MSB pass).
  uint8_t textDarkness = DARKNESS_DARK;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings (EPUB)
  uint8_t fontFamily = BOOKERLY;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Folder name under /dictionaries (or /.dictionaries) of the StarDict
  // dictionary used for word lookup; empty means no dictionary is selected.
  // A folder NAME, not a path: DictionaryRegistry::resolveBasePath rejects
  // separators and dot prefixes so a hand-edited value cannot escape the roots.
  char dictionaryName[32] = "";
  uint8_t fontSize = MEDIUM;
  // Reader font settings (TXT / MD) — defaults to EPUB settings when not explicitly set
  uint8_t txtFontFamily = NOTOSANS;
  char txtSdFontFamilyName[32] = "";
  uint8_t txtFontSize = MEDIUM;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Legacy enum fields — kept for JSON migration only; not used at runtime.
  uint8_t sleepTimeout = SLEEP_10_MIN;
  uint8_t refreshFrequency = REFRESH_15;
  // Auto-sleep timeout in minutes (0 = never sleep, 1–60). Replaces sleepTimeout enum.
  uint8_t sleepTimeoutMinutes = 10;
  // Full-refresh frequency in pages (0 = never full-refresh, 1–60). Replaces refreshFrequency enum.
  uint8_t refreshFrequencyPages = 15;
  // Perform a half refresh on the page immediately following an EPUB page that displayed images.
  uint8_t halfRefreshAfterImagePage = 1;
  uint8_t hyphenationEnabled = 0;
  // Snap near-body font sizes (within ±10% of the body size) to plain body text, so publisher
  // sizing renders native — both inline wrappers like <span style="font-size:0.92em"> around whole
  // paragraphs and a size stated on the block itself (p.body { font-size: 1.1em }). Default on.
  uint8_t fontSizeNormalization = 1;

  // Reader screen margin settings
  uint8_t screenMargin = 5;
  // OPDS browser settings
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Show file extensions in the file browser (0 = hidden, 1 = show)
  uint8_t showFileExtensions = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Show a placeholder for large images (>800×600 source pixels) instead of decoding immediately.
  // The user can press OK on the placeholder page to decode the image on demand.
  uint8_t largeImagePlaceholder = 1;
  // Dithering mode for decoded images (EPUB/JPG/PNG)
  uint8_t imageDithering = IMAGE_DITHER_BAYER;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = 0;
  // Action when the computed tilt value crosses the positive threshold.
  uint8_t tiltPositiveAction = TILT_ACT_NEXT_PAGE;
  // Action when the computed tilt value crosses the negative threshold.
  uint8_t tiltNegativeAction = TILT_ACT_PREV_PAGE;
  // Enable synthetic TOC fallback for malformed/sparse TOC books (1 = enabled, 0 = disabled)
  uint8_t syntheticTocFallback = 1;
  // Default bionic reading in EPUB pages when no per-book override is set (1 = enabled, 0 = disabled)
  uint8_t bionicReading = 0;
  // Guide dots reading aid in EPUB pages: a small dot centered in each inter-word gap
  // (1 = enabled, 0 = disabled). Idea from CrossInk (https://github.com/uxjulia/CrossInk).
  uint8_t guideDots = 0;
  // Expand semantic EPUB footnote references with a short inline preview.
  uint8_t inlineFootnotePreviews = 0;
  // Automatically push reading progress to the KOReader sync server when leaving the reader
  // (1 = enabled, 0 = disabled). The push only fires when credentials are configured and the
  // reader session advanced at least koSyncMinSessionPages pages, and is skipped when remote
  // progress is already ahead.
  uint8_t koSyncOnBookClose = 0;
  // Pages that must be turned in a reader session before auto-push-on-close will fire. Opening a
  // book to check the cover or skim the TOC should not burn a network round-trip, but how much
  // reading counts as "a session" is a matter of taste — someone reading a page or two over
  // breakfast wants a lower bar than someone who only reads in long sittings.
  uint8_t koSyncMinSessionPages = 3;
  // Move finished book to /COMPLETED when the end-of-book screen action is selected.
  uint8_t moveFinishedBooksToCompleted = 0;
  // Remove finished book from Recent Books when the end-of-book screen action is selected.
  uint8_t removeFinishedBooksFromRecents = 0;
  // Push reading progress to the KOReader sync server when the end-of-book screen action is
  // selected (1 = enabled, 0 = disabled). Only offered when KOReader credentials are configured;
  // composes with whichever action (home / open next / OPDS search) was picked rather than
  // replacing it — see BookFinished::launchFinishedBookFlow.
  uint8_t syncFinishedBookToKOReader = 0;
  // Show clock in the reader status bar
  uint8_t statusBarClock = 0;
  // Which end of the status-item lane the clock is drawn at (see STATUS_BAR_CLOCK_POSITION).
  uint8_t statusBarClockPosition = STATUS_BAR_CLOCK_LEFT;
  // Clock format: 0 = 24h (14:00), 1 = 12h (2:00pm)
  uint8_t clockFormat12h = 0;
  // Timezone selection (applies POSIX TZ rules for DST)
  uint8_t timeZone = TZ_UTC;
  // Preferred NTP server (host or IP). Empty = use built-in servers only
  // (Cloudflare anycast IP + pool.ntp.org). When set, it is polled first, with
  // the built-ins kept as fallbacks. Passed into HalClock::syncNtp() by callers.
  char ntpServer[64] = "";
  // Use clock and keep the LP timer running during deep sleep (GPIO13 HIGH)
  // so time can be accurately restored on wake. Increases sleep current by ~3-4 mA.
  uint8_t useClock = 0;
  // Show the Weather home screen menu item (1 = enabled, 0 = hidden)
  uint8_t useWeather = 1;
  // Include release candidate builds when checking for OTA updates.
  uint8_t includeBetaUpdates = 0;
  // Accept any TLS certificate on https requests (1 = skip validation).
  //
  // For self-hosted servers with a private CA or a self-signed certificate —
  // an OPDS catalog or a local KOReader sync server — where the alternative is
  // that the device simply cannot reach them. It disables authentication of the
  // peer for every https call the firmware makes, so credentials sent to such a
  // server are only as safe as the network path. OTA is deliberately exempt:
  // firmware is executed, so it stays verified whatever this is set to.
  uint8_t skipHttpsValidation = 0;

  // Configurable actions for short / double / long press on each logical button.
  //
  // BTN_DEFAULT means "do the activity's natural behaviour for this button" — it
  // is context-sensitive on purpose (e.g. Confirm-short opens the reader menu but
  // selects an entry in a list), which is why it can't be replaced by baking a
  // fixed action into the defaults. BTN_IGNORE is the opposite: explicitly do
  // nothing and suppress the built-in behaviour for that press.
  //
  // New values MUST be appended before BUTTON_ACTION_COUNT — the settings option
  // list (SettingsList.h btnActionOptions) and persisted JSON map by numeric value.
  enum BUTTON_ACTION {
    BTN_DEFAULT = 0,
    BTN_PAGE_FORWARD,
    BTN_PAGE_BACK,
    BTN_PAGE_FORWARD_10,
    BTN_PAGE_BACK_10,
    BTN_GO_HOME,
    BTN_SLEEP,
    BTN_FORCE_REFRESH,
    BTN_FORCE_FAST_REFRESH,
    BTN_OPEN_TOC,
    BTN_OPEN_BOOKMARKS,
    BTN_STAR_PAGE,
    BTN_FOOTNOTES,
    BTN_NEXT_SECTION,
    BTN_PREV_SECTION,
    BTN_EXIT_READER,
    BTN_READER_MENU,
    BTN_TOGGLE_BIONIC_READING,
    BTN_KOREADER_SYNC,
    BTN_CYCLE_FONT_SIZE,
    BTN_CYCLE_ORIENTATION,
    BTN_QUICK_OVERRIDES,
    BTN_IGNORE,
    BTN_DICTIONARY,
    BUTTON_ACTION_COUNT
  };

  // Short-press actions (default: built-in)
  uint8_t btnShortBack = BTN_DEFAULT;
  uint8_t btnShortConfirm = BTN_DEFAULT;
  uint8_t btnShortLeft = BTN_DEFAULT;
  uint8_t btnShortRight = BTN_DEFAULT;
  uint8_t btnShortPageBack = BTN_DEFAULT;
  uint8_t btnShortPageForward = BTN_DEFAULT;
  uint8_t btnShortPower = BTN_DEFAULT;

  // Double-press actions (default: BTN_DEFAULT = disabled, no disambiguation wait)
  uint8_t btnDoubleBack = BTN_DEFAULT;
  uint8_t btnDoubleConfirm = BTN_DEFAULT;
  uint8_t btnDoubleLeft = BTN_PAGE_BACK_10;
  uint8_t btnDoubleRight = BTN_PAGE_FORWARD_10;
  uint8_t btnDoublePageBack = BTN_DEFAULT;
  uint8_t btnDoublePageForward = BTN_DEFAULT;
  uint8_t btnDoublePower = BTN_DEFAULT;

  // Long-press actions (default: built-in)
  uint8_t btnLongBack = BTN_DEFAULT;
  uint8_t btnLongConfirm = BTN_DEFAULT;
  uint8_t btnLongLeft = BTN_PREV_SECTION;
  uint8_t btnLongRight = BTN_NEXT_SECTION;
  uint8_t btnLongPageBack = BTN_DEFAULT;
  uint8_t btnLongPageForward = BTN_DEFAULT;
  uint8_t btnLongPower = BTN_DEFAULT;

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  // How long the power button must be held to put the device to sleep, measured from the
  // press edge in loop(). Device-measured at 401-410 ms of real hold on X3 and X4.
  static constexpr uint16_t getPowerButtonDuration() { return 400; }

  // The same gesture in the other direction, but the wake gate compares against millis()
  // — time since app start, not since the press (see HalGPIO::verifyPowerButtonWakeup).
  // The press that caused the wake began before the app did, so the ROM and second-stage
  // bootloader are already part of the user's hold and nothing charges for them: the real
  // hold is prelude + this value. Shortened from the sleep threshold to claw some of that
  // asymmetry back, and deliberately not shortened further — the gate's job is to reject a
  // stray pocket press, and the prelude is the only margin left doing that.
  static constexpr uint16_t getPowerWakeHoldDuration() { return 300; }
  int getReaderFontId() const;
  int getTxtReaderFontId() const;
  // Pure built-in lookup (size enum + family enum -> font ID). Independent of
  // SD-card font selection. Used by the per-book fontFamilyOverride path so
  // an override forces back to a known built-in even when an SD font is the
  // global default.
  static int getBuiltinReaderFontId(uint8_t family, uint8_t size);
  // Heading sizing: return the built-in fontId `stepUp` sizes taller than `size` for
  // `family`, clamped at the largest size. Steps walk the ascending-pixel ladder
  // (TINY<SMALL<MEDIUM<LARGE<EXTRA_LARGE), not the FONT_SIZE enum order. `actualStep`
  // (out) receives how many steps were actually taken before the cap (so the caller can
  // compute a residual multiplier when clamped). Returns 0 for unknown families.
  static int getTallerBuiltinReaderFontId(uint8_t family, uint8_t size, uint8_t stepUp, uint8_t* actualStep = nullptr);

  bool saveToFile() const;
  bool loadFromFile();
  void loadStartupFromNvs();
  void saveStartupToNvs() const;

  static void validateFrontButtonMapping(CrossPointSettings& settings);

  // Enforce settings whose values depend on others (e.g. sleepScreen=QUICK_RESUME implies
  // quickResumeSleepScreen=ON). Call after any setting mutation that could invalidate the pair.
  static void normalizeDependentSettings(CrossPointSettings& settings);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
