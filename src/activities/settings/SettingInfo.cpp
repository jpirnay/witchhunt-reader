#include "SettingInfo.h"

#include <I18n.h>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

std::string SettingInfo::getTitle() const {
  if (isSeparator) {
    if (nameId == StrId::STR_NONE_OPT) return UITheme::makeSeparatorTitle(std::string{});
    return UITheme::makeSeparatorTitle(std::string{I18N.get(nameId)});
  }
  return std::string{I18N.get(nameId)};
}

bool SettingInfo::getToggleState() const {
  if (type != SettingType::TOGGLE) return false;
  if (valuePtr) return SETTINGS.*(valuePtr) != 0;
  if (valueGetter) return callValueGetter();
  return false;
}

std::string SettingInfo::getDisplayValue() const {
  if (isSeparator) return {};

  switch (type) {
    case SettingType::TOGGLE: {
      // A row with neither a field nor a getter has nothing to show -- distinct from one that
      // reads false, which getToggleState() cannot express on its own.
      if (!valuePtr && !valueGetter) return {};
      return std::string(getToggleState() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
    }
    case SettingType::ENUM: {
      uint8_t value;
      if (valuePtr)
        value = SETTINGS.*(valuePtr);
      else if (valueGetter)
        value = callValueGetter();
      else
        return {};
      // Through the shared accessor, so the row a picker shows and the value a row DISPLAYS
      // cannot disagree about what option `value` is. They used to be two copies of the same
      // two-way choice between enumValues and enumLabels, and a row carrying a third form
      // (enumLiteralLabelFn) rendered blank here while listing perfectly well in the picker.
      const int option = enumOptionOf(value);
      return option < 0 ? std::string{} : getEnumOptionLabel(static_cast<uint8_t>(option));
    }
    case SettingType::VALUE: {
      if (valuePtr) return std::to_string(SETTINGS.*(valuePtr));
      if (valueGetter) return std::to_string(callValueGetter());
      return {};
    }
    case SettingType::ACTION:
      if (stringGetter) return callStringGetter();
      return std::string(">>");
    case SettingType::STRING:
      return {};
  }
  return {};
}

void SettingInfo::toggleValue() const {
  if (isSeparator) return;

  switch (type) {
    case SettingType::TOGGLE:
      if (valuePtr) {
        SETTINGS.*(valuePtr) = !(SETTINGS.*(valuePtr));
      } else if (valueGetter && valueSetter) {
        callValueSetter(!callValueGetter());
      }
      break;

    case SettingType::ENUM: {
      // Same reason as getDisplayValue() above: one definition of how many options a row has.
      // A row whose options come from enumLiteralLabelFn counted as zero here, so cycling it was
      // a no-op -- invisible while the row opens a picker, wrong the moment one does not.
      const uint8_t count = getEnumOptionCount();
      if (count == 0) break;
      // By option, so a row whose options store other values (withOptionValues) steps to the next one it offers.
      const auto next = [this, count](const uint8_t value) {
        const int option = enumOptionOf(value);
        return enumValueOf(static_cast<uint8_t>(option < 0 ? 0 : (option + 1) % count));
      };
      if (valuePtr) {
        SETTINGS.*(valuePtr) = next(SETTINGS.*(valuePtr));
      } else if (valueGetter && valueSetter) {
        callValueSetter(next(callValueGetter()));
      }
      break;
    }

    case SettingType::VALUE:
      if (valuePtr) {
        const unsigned current = SETTINGS.*(valuePtr);
        SETTINGS.*(valuePtr) = static_cast<uint8_t>(
            (current + valueRange.step > valueRange.max) ? valueRange.min : current + valueRange.step);
      }
      break;

    default:
      break;
  }
}
