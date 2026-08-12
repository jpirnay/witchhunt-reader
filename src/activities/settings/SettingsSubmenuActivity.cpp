#include "SettingsSubmenuActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SettingActionDispatch.h"
#include "SliderSettingPicker.h"
#include "activities/SliderPickerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SettingsSubmenuActivity::onEnter() {
  Activity::onEnter();
  needsHalfRefresh = true;
  initMenuList();
  requestUpdate();
}

void SettingsSubmenuActivity::onActionSelected(int index) {
  const auto& setting = menuItems[index];
  if (setting.isSeparator) return;

  if (setting.type == SettingType::ACTION) {
    SliderPickerActivity::Config sliderCfg;
    if (SliderSetting::configFor(setting.action, sliderCfg)) {
      const SettingAction sliderAction = setting.action;
      startActivityForResult(std::make_unique<SliderPickerActivity>(renderer, mappedInput, std::move(sliderCfg)),
                             [this, sliderAction](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 if (const auto* pr = std::get_if<PercentResult>(&result.data)) {
                                   SliderSetting::apply(sliderAction, static_cast<uint8_t>(pr->percent));
                                   SETTINGS.saveToFile();
                                 }
                               }
                               needsHalfRefresh = true;
                               requestUpdate();
                             });
      return;
    }

    auto activity = createActivityForAction(setting.action, renderer, mappedInput);
    if (activity) {
      startActivityForResult(std::move(activity), [this](const ActivityResult&) {
        CrossPointSettings::normalizeDependentSettings(SETTINGS);
        SETTINGS.saveToFile();
        needsHalfRefresh = true;
        requestUpdate();
      });
      return;
    }

    // Submenu items with no associated SettingAction still need to be routed through
    // the parent activity by nameId.
    setResult(MenuResult{-1, static_cast<int>(setting.nameId)});
    finish();
    return;
  }

  onSettingToggled(index);
}

std::string SettingsSubmenuActivity::getItemValueString(int index) const {
  const auto& item = menuItems[index];
  if (item.type == SettingType::ACTION && item.action != SettingAction::Submenu) {
    return {};
  }
  if (itemValueStringOverride) {
    return itemValueStringOverride(item);
  }
  return MenuListActivity::getItemValueString(index);
}

void SettingsSubmenuActivity::toggleCurrentItem() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(menuItems.size())) return;
  const auto& setting = menuItems[selectedIndex];
  if (setting.isSeparator) return;

  if (setting.usesSelectorActivity) {
    auto selector = createSelectorActivity(setting, renderer, mappedInput);
    if (selector) {
      startActivityForResult(std::move(selector), [this](const ActivityResult&) {
        if (persistSettingsOnChange) SETTINGS.saveToFile();
        needsHalfRefresh = true;
        requestUpdate();
      });
    }
    return;
  }

  MenuListActivity::toggleCurrentItem();
}

void SettingsSubmenuActivity::onSettingToggled(int /*index*/) {
  if (persistSettingsOnChange) SETTINGS.saveToFile();
}

void SettingsSubmenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 I18N.get(titleId));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;
  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool halfRefresh = gpio.deviceIsX3() && needsHalfRefresh;
  needsHalfRefresh = false;
  renderer.displayBuffer(halfRefresh ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
