#include "SliderSettingPicker.h"

#include <HalFrontlight.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "SettingsList.h"

namespace SliderSetting {
namespace {

// Was the light already lit when the current preview opened?
//
// Previewing a brightness or a warmth needs a lit panel -- there is nothing to look at
// otherwise -- so the preview lights it. That is a change to the light's ON state, which the
// slider does not own: the switch does. Both endings put it back, so opening the brightness
// slider on a dark panel never leaves the light on behind the reader's back.
//
// One slot rather than a member: the activity stack is modal, so exactly one picker exists at
// a time, and configFor() is called once per opening.
bool previewLitBefore = false;

// Light the panel if needed so there is something to judge, then drive the level.
void previewFrontlight(const bool warmth, const int value) {
  if (!Frontlight.isOn()) Frontlight.setOn(true);
  if (warmth) {
    Frontlight.setWarmth(static_cast<uint8_t>(value));
  } else {
    Frontlight.setBrightness(static_cast<uint8_t>(value));
  }
}

// Undo whatever the preview lit, once the value itself has been settled either way.
void endFrontlightPreview() {
  if (!previewLitBefore) Frontlight.setOn(false);
}

constexpr int KO_CLOSE_PAGES_MAX = 60;
constexpr int KO_CLOSE_NEVER = KO_CLOSE_PAGES_MAX + 1;
constexpr int KO_INTERVAL_MAX = KOReaderCredentialStore::PUSH_INTERVAL_MAX_PAGES;
constexpr int KO_INTERVAL_NEVER = KO_INTERVAL_MAX + 1;

}  // namespace

bool configFor(const SettingAction action, SliderPickerActivity::Config& cfg) {
  switch (action) {
    case SettingAction::SleepTimeoutPicker:
      cfg = {.titleId = StrId::STR_TIME_TO_SLEEP,
             .hintId = StrId::STR_SLIDER_STEP_HINT,
             .minValue = 0,
             .maxValue = 60,
             .initialValue = SETTINGS.sleepTimeoutMinutes,
             .suffix = tr(STR_MIN_SUFFIX),
             .zeroLabel = tr(STR_NEVER)};
      return true;
    case SettingAction::RefreshFrequencyPicker:
      cfg = {.titleId = StrId::STR_REFRESH_FREQ,
             .hintId = StrId::STR_SLIDER_STEP_HINT,
             .minValue = 0,
             .maxValue = 60,
             .initialValue = SETTINGS.refreshFrequencyPages,
             .suffix = tr(STR_PAGES_SUFFIX),
             .zeroLabel = tr(STR_NEVER)};
      return true;
    case SettingAction::KOSyncOnClosePicker:
      // same as above - zero is "always"
      cfg = {.titleId = StrId::STR_KO_AUTO_ON_CLOSE,
             .hintId = StrId::STR_SLIDER_STEP_HINT,
             .minValue = 0,
             .maxValue = KO_CLOSE_NEVER,
             .initialValue = SETTINGS.koSyncOnBookClose
                                 ? std::min<int>(SETTINGS.koSyncMinSessionPages, KO_CLOSE_PAGES_MAX)
                                 : KO_CLOSE_NEVER,
             .suffix = tr(STR_PAGES_SUFFIX),
             .zeroLabel = tr(STR_ALWAYS),
             .maxLabel = tr(STR_NEVER)};
      return true;
    case SettingAction::KOSyncIntervalPicker: {
      const uint16_t interval = KOREADER_STORE.getPushIntervalPages();
      cfg = {.titleId = StrId::STR_KO_AUTO_WHILE_READING,
             .hintId = StrId::STR_SLIDER_STEP_HINT,
             .minValue = 1,
             .maxValue = KO_INTERVAL_NEVER,
             .initialValue = interval == 0 ? KO_INTERVAL_NEVER : std::min<int>(interval, KO_INTERVAL_MAX),
             .suffix = tr(STR_PAGES_SUFFIX),
             .maxLabel = tr(STR_NEVER)};
      return true;
    }
    case SettingAction::FrontlightBrightnessPicker:
      // The floor is MIN_BRIGHTNESS, not 0: turning the light off is the
      // separate on/off switch, so a 0% "on" level would only be a second,
      // worse way to reach the same place. No zeroLabel for the same reason.
      cfg = {.titleId = StrId::STR_LIGHT_BRIGHTNESS,
             .hintId = StrId::STR_SLIDER_STEP_HINT,
             .minValue = HalFrontlight::MIN_BRIGHTNESS,
             .maxValue = 100,
             .initialValue = SETTINGS.frontlightBrightness,
             .suffix = "%",
             .zeroLabel = "",
             .onPreview = [](const int v) { previewFrontlight(/*warmth=*/false, v); }};
      previewLitBefore = Frontlight.isOn();
      return true;
    case SettingAction::FrontlightWarmthPicker:
      // 0 = fully cool, 100 = fully warm. Total brightness is held constant
      // across the mix by FrontlightManager, so this is a pure colour control.
      cfg = {.titleId = StrId::STR_LIGHT_WARMTH,
             .hintId = StrId::STR_SLIDER_STEP_HINT,
             .minValue = 0,
             .maxValue = 100,
             .initialValue = SETTINGS.frontlightWarmth,
             .suffix = "%",
             .zeroLabel = "",
             .onPreview = [](const int v) { previewFrontlight(/*warmth=*/true, v); }};
      previewLitBefore = Frontlight.isOn();
      return true;
    default:
      return false;
  }
}

void apply(const SettingAction action, const uint8_t value) {
  // The field a slider edits is declared once, on the row itself (SettingInfo::persisting), and
  // read from there by BOTH this and JsonSettingsIO. It used to be written out twice — a switch
  // here and a hand-written line in the serialiser — and the two could disagree silently: the
  // frontlight sliders had the switch and no serialiser line, so the level applied, drove the
  // panel, and was gone at the next boot.
  //
  // Now a row that forgets to declare its field does not save AND does not apply, which is a
  // report on the first use rather than one after a power cycle.
  // Bound to a named local, NOT iterated straight out of the call: getSettingsList() returns the
  // vector BY VALUE, so a range-for over the call alone destroys it at the end of the loop and
  // anything still pointing into it dangles. Same form JsonSettingsIO uses.
  switch (action) {
    case SettingAction::KOSyncOnClosePicker:
      SETTINGS.koSyncOnBookClose = value >= KO_CLOSE_NEVER ? 0 : 1;
      if (value < KO_CLOSE_NEVER) SETTINGS.koSyncMinSessionPages = value;
      return;
    case SettingAction::KOSyncIntervalPicker:
      KOREADER_STORE.setPushIntervalPages(value >= KO_INTERVAL_NEVER ? 0 : value);
      KOREADER_STORE.saveToFile();
      return;
    default:
      break;
  }

  const auto settings = getSettingsList();
  const auto row = std::find_if(settings.begin(), settings.end(), [action](const SettingInfo& info) {
    return info.type == SettingType::ACTION && info.action == action && info.persistPtr;
  });
  if (row == settings.end()) {
    LOG_ERR("SET", "Slider action %d edits no declared field; see SettingInfo::persisting", static_cast<int>(action));
    return;
  }
  SETTINGS.*(row->persistPtr) = value;

  // Side effects only. Both light sliders drive the hardware as well as the setting: the picker
  // is the only place the level is chosen, so waiting for a reboot to see it would make the
  // control unusable.
  switch (action) {
    case SettingAction::FrontlightBrightnessPicker:
      Frontlight.setBrightness(value);
      endFrontlightPreview();
      break;
    case SettingAction::FrontlightWarmthPicker:
      Frontlight.setWarmth(value);
      endFrontlightPreview();
      break;
    default:
      break;
  }
}

void cancel(const SettingAction action) {
  switch (action) {
    case SettingAction::FrontlightBrightnessPicker:
    case SettingAction::FrontlightWarmthPicker:
      // SETTINGS still holds the old levels: apply() is the only writer, and the preview
      // deliberately drove the hardware without touching them. So re-driving from SETTINGS IS
      // the restore -- there is no separate "value before" to remember.
      Frontlight.setBrightness(SETTINGS.frontlightBrightness);
      Frontlight.setWarmth(SETTINGS.frontlightWarmth);
      endFrontlightPreview();
      break;
    default:
      break;
  }
}

}  // namespace SliderSetting
