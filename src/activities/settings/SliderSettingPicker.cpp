#include "SliderSettingPicker.h"

#include <HalFrontlight.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "SettingsList.h"

namespace SliderSetting {

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
    case SettingAction::KOSyncMinPagesPicker:
      // Zero reads as "Always" rather than "Never" here: it means no minimum, so every book
      // close pushes. Whether anything is pushed at all is the separate Auto-Push toggle.
      // Useful in its own right — jumping via the TOC and closing changes the position without
      // turning a single page.
      cfg = {.titleId = StrId::STR_KO_MIN_SESSION_PAGES,
             .hintId = StrId::STR_SLIDER_STEP_HINT,
             .minValue = 0,
             .maxValue = 60,
             .initialValue = SETTINGS.koSyncMinSessionPages,
             .suffix = tr(STR_PAGES_SUFFIX),
             .zeroLabel = tr(STR_ALWAYS)};
      return true;
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
             .zeroLabel = ""};
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
             .zeroLabel = ""};
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
      break;
    case SettingAction::FrontlightWarmthPicker:
      Frontlight.setWarmth(value);
      break;
    default:
      break;
  }
}

}  // namespace SliderSetting
