#include "CrossPointSettings.h"

#include <BoardConfig.h>
#include <Epub/LineSpacing.h>
#include <Epub/ParagraphIndent.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "DarkMode.h"
#include "DarkModeNow.h"
#include "SdCardFontGlobals.h"
#include "fontIds.h"

// Font ID 0 is reserved as the SD card font "not found" sentinel
// (SdCardFontManager::computeFontId() never returns 0). Guard against any
// hash accidentally producing 0 — would cause silent fallback to built-in.
static_assert(SERIF_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SERIF_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SERIF_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SERIF_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SERIF_20_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SERIF_22_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SERIF_24_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SERIF_26_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SERIF_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_20_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_22_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_24_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_26_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(UI_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(UI_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SMALL_FONT_ID != 0, "Font ID collision with sentinel");

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

void CrossPointSettings::applyDeviceButtonDefaults() {
  btnDoubleConfirm = BTN_DEFAULT;
  btnLongConfirm = BTN_DEFAULT;
  btnDoublePower = BTN_LIGHT_TOGGLE;

  const auto& input = BoardConfig::ACTIVE.input;
  // No Back key and no Home key to carry Back (BOOX Picco: Power, Up, Down and touch): a double press of Power is
  // Back, so Back is a key away and not only a hint to tap. The light that double press toggles elsewhere is the slide
  // switch's on that board. Murphy is left out by the dedicated Power key: its Power is its Confirm.
  const bool hasDedicatedPowerKey = input.power >= 0 && input.power != input.confirm;
  if (input.back < 0 && !BoardConfig::hasHomeKey() && hasDedicatedPowerKey) {
    btnDoublePower = BTN_BACK;
  }

  if (!BoardConfig::isMurphyM3() || input.confirm < 0) return;

  if (input.back < 0) btnDoubleConfirm = BTN_BACK;
  const bool hasDedicatedPower = input.power >= 0 && input.power != input.confirm;
  if (!hasDedicatedPower) btnLongConfirm = BTN_SLEEP;
}

namespace {
constexpr char SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";

void enforceFixedShortActions(CrossPointSettings& settings) {
  settings.btnShortBack = static_cast<uint8_t>(CrossPointSettings::BUTTON_ACTION::BTN_DEFAULT);
  settings.btnShortConfirm = static_cast<uint8_t>(CrossPointSettings::BUTTON_ACTION::BTN_DEFAULT);
}

}  // namespace

void CrossPointSettings::normalizeDependentSettings(CrossPointSettings& settings) {
  if (settings.sleepScreen == SLEEP_SCREEN_MODE::QUICK_RESUME) {
    settings.quickResumeSleepScreen = QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  }
}

bool CrossPointSettings::isReaderScopedAction(const uint8_t action) {
  switch (static_cast<BUTTON_ACTION>(action)) {
    case BTN_PAGE_FORWARD:
    case BTN_PAGE_BACK:
    case BTN_PAGE_FORWARD_10:
    case BTN_PAGE_BACK_10:
    case BTN_OPEN_TOC:
    case BTN_STAR_PAGE:
    case BTN_FOOTNOTES:
    case BTN_NEXT_SECTION:
    case BTN_PREV_SECTION:
    case BTN_EXIT_READER:
    case BTN_READER_MENU:
    case BTN_READER_BARS:
    case BTN_TOGGLE_BIONIC_READING:
    case BTN_KOREADER_SYNC:
    case BTN_CYCLE_FONT_SIZE:
    case BTN_FONT_SIZE_SMALLER:
    case BTN_FONT_SIZE_LARGER:
    case BTN_CYCLE_ORIENTATION:
    case BTN_CYCLE_ORIENTATION_BACK:
    case BTN_QUICK_OVERRIDES:
    case BTN_DICTIONARY:
      return true;
    default:
      // BTN_GO_HOME / BTN_SLEEP / BTN_FORCE_*_REFRESH / BTN_OPEN_BOOKMARKS /
      // BTN_LIGHT_* / BTN_TOGGLE_TOUCH_UI / BTN_IGNORE are global.
      return false;
  }
}

void CrossPointSettings::dropUnsupportedActions(CrossPointSettings& settings, const bool hasFrontlight,
                                                const bool hasWarmLight) {
  if (hasFrontlight && hasWarmLight) return;

  // Every field that stores a BUTTON_ACTION. Listed once, here, so a sweep over
  // them cannot quietly miss one the way scattered per-field checks would.
  static constexpr uint8_t CrossPointSettings::* ACTION_FIELDS[] = {
      &CrossPointSettings::btnShortBack,
      &CrossPointSettings::btnShortConfirm,
      &CrossPointSettings::btnShortLeft,
      &CrossPointSettings::btnShortRight,
      &CrossPointSettings::btnShortPageBack,
      &CrossPointSettings::btnShortPageForward,
      &CrossPointSettings::btnShortPower,
      &CrossPointSettings::btnDoubleBack,
      &CrossPointSettings::btnDoubleConfirm,
      &CrossPointSettings::btnDoubleLeft,
      &CrossPointSettings::btnDoubleRight,
      &CrossPointSettings::btnDoublePageBack,
      &CrossPointSettings::btnDoublePageForward,
      &CrossPointSettings::btnDoublePower,
      &CrossPointSettings::btnLongBack,
      &CrossPointSettings::btnLongConfirm,
      &CrossPointSettings::btnLongLeft,
      &CrossPointSettings::btnLongRight,
      &CrossPointSettings::btnLongPageBack,
      &CrossPointSettings::btnLongPageForward,
      &CrossPointSettings::btnLongPower,
      &CrossPointSettings::gestSwipeLeft,
      &CrossPointSettings::gestSwipeRight,
      &CrossPointSettings::gestSwipeUpLeft,
      &CrossPointSettings::gestSwipeUpRight,
      &CrossPointSettings::gestSwipeDownLeft,
      &CrossPointSettings::gestSwipeDownRight,
      &CrossPointSettings::gestSwipeInLeftZone,
      &CrossPointSettings::gestSwipeInRightZone,
      &CrossPointSettings::gestTapLeft,
      &CrossPointSettings::gestTapRight,
      &CrossPointSettings::gestTapCentre,
      &CrossPointSettings::gestTapTop,
      &CrossPointSettings::gestTapBottom,
      &CrossPointSettings::gestLongTapLeft,
      &CrossPointSettings::gestLongTapRight,
      &CrossPointSettings::gestLongTapCentre,
      &CrossPointSettings::gestLongTapTop,
      &CrossPointSettings::gestLongTapBottom,
      &CrossPointSettings::gestLongTapTopLeft,
      &CrossPointSettings::gestLongTapTopRight,
      &CrossPointSettings::gestLongTapBottomLeft,
      &CrossPointSettings::gestLongTapBottomRight,
      &CrossPointSettings::gestPinchIn,
      &CrossPointSettings::gestPinchOut,
      &CrossPointSettings::gestRotateCw,
      &CrossPointSettings::gestRotateCcw,
  };

  for (const auto field : ACTION_FIELDS) {
    const uint8_t action = settings.*field;
    const bool needsLight = action == BTN_LIGHT_TOGGLE || action == BTN_LIGHT_BRIGHTER || action == BTN_LIGHT_DIMMER;
    const bool needsWarm = action == BTN_LIGHT_WARMER || action == BTN_LIGHT_COOLER;
    if ((needsLight && !hasFrontlight) || (needsWarm && !hasWarmLight)) {
      settings.*field = BTN_DEFAULT;
    }
  }
}

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

#include <Preferences.h>

void CrossPointSettings::loadStartupFromNvs() {
  Preferences nvs;
  nvs.begin("Crosspoint", true);  // read-only
  btnShortPower = nvs.getUChar("bSPwr", BTN_DEFAULT);
  btnDoublePower = nvs.getUChar("bDPwr", BTN_DEFAULT);
  useClock = nvs.getUChar("useClk", 0);
  measuredFastRefreshMs = nvs.getUShort("fastMs", 0);
  nvs.end();
}

void CrossPointSettings::saveStartupToNvs() const {
  Preferences nvs;
  nvs.begin("Crosspoint", false);  // read-write
  nvs.putUChar("bSPwr", btnShortPower);
  nvs.putUChar("bDPwr", btnDoublePower);
  nvs.putUChar("useClk", useClock);
  // Only when it has actually changed: this rides on the settings save, and SPIFFS/NVS sectors
  // have a finite erase budget. The measurement is stable per panel, so after the first boot on a
  // given device this writes nothing.
  if (nvs.getUShort("fastMs", 0) != measuredFastRefreshMs) nvs.putUShort("fastMs", measuredFastRefreshMs);
  nvs.end();
}

bool CrossPointSettings::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  saveStartupToNvs();
  return JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON);
}

bool CrossPointSettings::loadFromFile() {
  // Board-specific keymap defaults must be applied after hardware profile
  // selection and before JSON is read, so missing/legacy values inherit them.
  applyDeviceButtonDefaults();

  // Try JSON first
  if (Storage.exists(SETTINGS_FILE_JSON)) {
    String json = Storage.readFile(SETTINGS_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = JsonSettingsIO::loadSettings(*this, json.c_str(), &resave);
      if (result) {
        enforceFixedShortActions(*this);
        saveStartupToNvs();  // Ensure NVS is in sync on boot
        if (resave) {
          if (saveToFile()) {
            LOG_DBG("CPS", "Resaved settings to update format");
          } else {
            LOG_ERR("CPS", "Failed to resave settings after format update");
          }
        }
      }
      return result;
    }
  }

  return false;
}

static_assert(CrossPointSettings::LINE_SPACING_90 == static_cast<uint8_t>(LineSpacingStep::Pct90) &&
                  CrossPointSettings::LINE_SPACING_95 == static_cast<uint8_t>(LineSpacingStep::Pct95) &&
                  CrossPointSettings::LINE_SPACING_100 == static_cast<uint8_t>(LineSpacingStep::Pct100) &&
                  CrossPointSettings::LINE_SPACING_110 == static_cast<uint8_t>(LineSpacingStep::Pct110) &&
                  CrossPointSettings::LINE_SPACING_120 == static_cast<uint8_t>(LineSpacingStep::Pct120) &&
                  CrossPointSettings::LINE_SPACING_130 == static_cast<uint8_t>(LineSpacingStep::Pct130) &&
                  CrossPointSettings::LINE_SPACING_145 == static_cast<uint8_t>(LineSpacingStep::Pct145) &&
                  CrossPointSettings::LINE_SPACING_160 == static_cast<uint8_t>(LineSpacingStep::Pct160) &&
                  CrossPointSettings::LINE_SPACING_175 == static_cast<uint8_t>(LineSpacingStep::Pct175) &&
                  CrossPointSettings::FROM_BOOK == static_cast<uint8_t>(LineSpacingStep::FromBook),
              "the stored line-spacing steps are LineSpacingStep's");
static_assert(CrossPointSettings::PARAGRAPH_INDENT_NONE == static_cast<uint8_t>(ParagraphIndent::None) &&
                  CrossPointSettings::PARAGRAPH_INDENT_HALF_EM == static_cast<uint8_t>(ParagraphIndent::HalfEm) &&
                  CrossPointSettings::PARAGRAPH_INDENT_ONE_EM == static_cast<uint8_t>(ParagraphIndent::OneEm) &&
                  CrossPointSettings::PARAGRAPH_INDENT_ONE_AND_HALF_EM ==
                      static_cast<uint8_t>(ParagraphIndent::OneAndHalfEm) &&
                  CrossPointSettings::PARAGRAPH_INDENT_TWO_EM == static_cast<uint8_t>(ParagraphIndent::TwoEm) &&
                  CrossPointSettings::PARAGRAPH_INDENT_FROM_BOOK == static_cast<uint8_t>(ParagraphIndent::FromBook),
              "the stored paragraph indents are ParagraphIndent's");

float CrossPointSettings::getReaderLineCompression() const {
  // Without a book (the dictionary's pages): no CJK pitch, and Book's style reads as Normal.
  const LineSpacingScale scale = getReaderFontId() == getBuiltinReaderFontIdForPoints(NOTOSANS, fontPointSize)
                                     ? LineSpacingScale::BuiltinNotoSans
                                     : LineSpacingScale::Font;
  return lineSpacingFor(lineSpacing, scale, fontPointSize, 0).compression;
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes == 0) return ULONG_MAX;
  return static_cast<unsigned long>(sleepTimeoutMinutes) * 60UL * 1000UL;
}

// Halved while the screens are dark, which Scheduled leaves to the clock: DarkModeNow, not darkMode alone.
int CrossPointSettings::getRefreshFrequency() const {
  return DarkMode::refreshFrequency(static_cast<int>(refreshFrequencyPages), DarkModeNow::isDark());
}

int CrossPointSettings::getBuiltinReaderFontId(uint8_t family, FONT_SIZE size) {
  switch (family) {
    case SERIF:
    default:
      switch (size) {
        case PT_10:
          return SERIF_10_FONT_ID;
        case PT_12:
          return SERIF_12_FONT_ID;
        case PT_14:
        default:
          return SERIF_14_FONT_ID;
        case PT_16:
          return SERIF_16_FONT_ID;
        case PT_18:
          return SERIF_18_FONT_ID;
        // Synthesised from the 18 pt master; the scale lives on the ID (see main.cpp).
        case PT_20:
          return SERIF_20_FONT_ID;
        case PT_22:
          return SERIF_22_FONT_ID;
        case PT_24:
          return SERIF_24_FONT_ID;
        case PT_26:
          return SERIF_26_FONT_ID;
      }
    case NOTOSANS:
      switch (size) {
        case PT_10:
          return NOTOSANS_10_FONT_ID;
        case PT_12:
          return NOTOSANS_12_FONT_ID;
        case PT_14:
        default:
          return NOTOSANS_14_FONT_ID;
        case PT_16:
          return NOTOSANS_16_FONT_ID;
        case PT_18:
          return NOTOSANS_18_FONT_ID;
        case PT_20:
          return NOTOSANS_20_FONT_ID;
        case PT_22:
          return NOTOSANS_22_FONT_ID;
        case PT_24:
          return NOTOSANS_24_FONT_ID;
        case PT_26:
          return NOTOSANS_26_FONT_ID;
      }
  }
}

constexpr CrossPointSettings::ReaderFontRung CrossPointSettings::FONT_SIZE_RUNGS[];

int CrossPointSettings::getTallerBuiltinReaderFontId(const uint8_t family, const uint8_t size, const uint8_t stepUp,
                                                     uint8_t* const actualStep) {
  // Ascending pixel ladder, from the one table that defines it. Enum order now matches, but read
  // the table anyway: that agreement is a property of today's numbering, not a rule, and the
  // table is the thing a future insertion updates.
  constexpr int kLadderLen = FONT_SIZE_RUNG_COUNT;

  int idx = -1;
  for (int i = 0; i < kLadderLen; ++i) {
    if (FONT_SIZE_RUNGS[i].size == size) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    if (actualStep) *actualStep = 0;
    return 0;  // unknown size
  }
  const int target = std::min(idx + static_cast<int>(stepUp), kLadderLen - 1);
  if (actualStep) *actualStep = static_cast<uint8_t>(target - idx);
  return getBuiltinReaderFontId(family, FONT_SIZE_RUNGS[target].size);
}

int CrossPointSettings::getReaderFontId() const {
  // SD card font takes priority when one is selected globally.
  // resolveSdCardFontId() returns 0 if the named family isn't loaded
  // (e.g. SD card removed since selection) — fall through to built-in.
  if (sdFontFamilyName[0] != '\0') {
    int id = resolveSdCardFontId(sdFontFamilyName, fontPointSize);
    if (id != 0) return id;
  }
  return getBuiltinReaderFontIdForPoints(fontFamily, fontPointSize);
}

int CrossPointSettings::getTxtReaderFontId() const {
  if (txtSdFontFamilyName[0] != '\0') {
    int id = resolveSdCardFontId(txtSdFontFamilyName, txtFontPointSize);
    if (id != 0) return id;
  }
  return getBuiltinReaderFontIdForPoints(txtFontFamily, txtFontPointSize);
}
