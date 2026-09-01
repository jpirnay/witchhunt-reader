#include "CrossPointSettings.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "SdCardFontGlobals.h"
#include "fontIds.h"

// Font ID 0 is reserved as the SD card font "not found" sentinel
// (SdCardFontManager::computeFontId() never returns 0). Guard against any
// hash accidentally producing 0 — would cause silent fallback to built-in.
static_assert(BOOKERLY_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BOOKERLY_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BOOKERLY_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BOOKERLY_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(BOOKERLY_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(UI_10_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(UI_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(SMALL_FONT_ID != 0, "Font ID collision with sentinel");

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

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

void CrossPointSettings::dropUnsupportedActions(CrossPointSettings& settings, const bool hasFrontlight) {
  if (hasFrontlight) return;

  // Every field that stores a BUTTON_ACTION. Listed once, here, so a sweep over
  // them cannot quietly miss one the way scattered per-field checks would.
  static constexpr uint8_t CrossPointSettings::* ACTION_FIELDS[] = {
      &CrossPointSettings::btnShortBack,         &CrossPointSettings::btnShortConfirm,
      &CrossPointSettings::btnShortLeft,         &CrossPointSettings::btnShortRight,
      &CrossPointSettings::btnShortPageBack,     &CrossPointSettings::btnShortPageForward,
      &CrossPointSettings::btnShortPower,        &CrossPointSettings::btnDoubleBack,
      &CrossPointSettings::btnDoubleConfirm,     &CrossPointSettings::btnDoubleLeft,
      &CrossPointSettings::btnDoubleRight,       &CrossPointSettings::btnDoublePageBack,
      &CrossPointSettings::btnDoublePageForward, &CrossPointSettings::btnDoublePower,
      &CrossPointSettings::btnLongBack,          &CrossPointSettings::btnLongConfirm,
      &CrossPointSettings::btnLongLeft,          &CrossPointSettings::btnLongRight,
      &CrossPointSettings::btnLongPageBack,      &CrossPointSettings::btnLongPageForward,
      &CrossPointSettings::btnLongPower,         &CrossPointSettings::gestSwipeLeft,
      &CrossPointSettings::gestSwipeRight,       &CrossPointSettings::gestSwipeUpLeft,
      &CrossPointSettings::gestSwipeUpRight,     &CrossPointSettings::gestSwipeDownLeft,
      &CrossPointSettings::gestSwipeDownRight,   &CrossPointSettings::gestTapLeft,
      &CrossPointSettings::gestTapRight,         &CrossPointSettings::gestTapCentre,
      &CrossPointSettings::gestTapTop,           &CrossPointSettings::gestTapBottom,
      &CrossPointSettings::gestLongTapLeft,      &CrossPointSettings::gestLongTapRight,
      &CrossPointSettings::gestLongTapCentre,    &CrossPointSettings::gestLongTapTop,
      &CrossPointSettings::gestLongTapBottom,    &CrossPointSettings::gestPinchIn,
      &CrossPointSettings::gestPinchOut,         &CrossPointSettings::gestRotateCw,
      &CrossPointSettings::gestRotateCcw,
  };

  for (const auto field : ACTION_FIELDS) {
    const uint8_t action = settings.*field;
    if (action == BTN_LIGHT_TOGGLE || action == BTN_LIGHT_BRIGHTER || action == BTN_LIGHT_DIMMER) {
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
  nvs.end();
}

void CrossPointSettings::saveStartupToNvs() const {
  Preferences nvs;
  nvs.begin("Crosspoint", false);  // read-write
  nvs.putUChar("bSPwr", btnShortPower);
  nvs.putUChar("bDPwr", btnDoublePower);
  nvs.putUChar("useClk", useClock);
  nvs.end();
}

bool CrossPointSettings::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  saveStartupToNvs();
  return JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON);
}

bool CrossPointSettings::loadFromFile() {
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

float CrossPointSettings::getReaderLineCompression() const {
  const int effectiveFontId = getReaderFontId();
  const int notosansId = getBuiltinReaderFontId(NOTOSANS, fontSize);

  if (effectiveFontId == notosansId) {
    switch (lineSpacing) {
      case TIGHT:
        return 0.90f;
      case NORMAL:
      default:
        return 0.95f;
      case WIDE:
        return 1.0f;
    }
  }

  // Bookerly or any SD card font: use the Bookerly-style neutral values.
  switch (lineSpacing) {
    case TIGHT:
      return 0.95f;
    case NORMAL:
    default:
      return 1.0f;
    case WIDE:
      return 1.1f;
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes == 0) return ULONG_MAX;
  return static_cast<unsigned long>(sleepTimeoutMinutes) * 60UL * 1000UL;
}

int CrossPointSettings::getRefreshFrequency() const { return static_cast<int>(refreshFrequencyPages); }

int CrossPointSettings::getBuiltinReaderFontId(uint8_t family, uint8_t size) {
  switch (family) {
    case BOOKERLY:
    default:
      switch (size) {
        case TINY:
          return BOOKERLY_10_FONT_ID;
        case SMALL:
          return BOOKERLY_12_FONT_ID;
        case MEDIUM:
        default:
          return BOOKERLY_14_FONT_ID;
        case LARGE:
          return BOOKERLY_16_FONT_ID;
        case EXTRA_LARGE:
          return BOOKERLY_18_FONT_ID;
      }
    case NOTOSANS:
      switch (size) {
        case TINY:
          return NOTOSANS_10_FONT_ID;
        case SMALL:
          return NOTOSANS_12_FONT_ID;
        case MEDIUM:
        default:
          return NOTOSANS_14_FONT_ID;
        case LARGE:
          return NOTOSANS_16_FONT_ID;
        case EXTRA_LARGE:
          return NOTOSANS_18_FONT_ID;
      }
  }
}

constexpr uint8_t CrossPointSettings::FONT_SIZE_LADDER[];

int CrossPointSettings::getTallerBuiltinReaderFontId(const uint8_t family, const uint8_t size, const uint8_t stepUp,
                                                     uint8_t* const actualStep) {
  // Ascending pixel ladder (smallest -> largest). FONT_SIZE enum order is NOT pixel order
  // (TINY=4), so step through this explicit table instead of enum arithmetic.
  static constexpr uint8_t kLadder[] = {TINY, SMALL, MEDIUM, LARGE, EXTRA_LARGE};
  constexpr int kLadderLen = static_cast<int>(sizeof(kLadder) / sizeof(kLadder[0]));

  int idx = -1;
  for (int i = 0; i < kLadderLen; ++i) {
    if (kLadder[i] == size) {
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
  return getBuiltinReaderFontId(family, kLadder[target]);
}

int CrossPointSettings::getReaderFontId() const {
  // SD card font takes priority when one is selected globally.
  // resolveSdCardFontId() returns 0 if the named family isn't loaded
  // (e.g. SD card removed since selection) — fall through to built-in.
  if (sdFontFamilyName[0] != '\0') {
    int id = resolveSdCardFontId(sdFontFamilyName, fontSize);
    if (id != 0) return id;
  }
  return getBuiltinReaderFontId(fontFamily, fontSize);
}

int CrossPointSettings::getTxtReaderFontId() const {
  if (txtSdFontFamilyName[0] != '\0') {
    int id = resolveSdCardFontId(txtSdFontFamilyName, txtFontSize);
    if (id != 0) return id;
  }
  return getBuiltinReaderFontId(txtFontFamily, txtFontSize);
}
