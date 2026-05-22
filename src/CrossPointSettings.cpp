#include "CrossPointSettings.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

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
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
    default:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

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
