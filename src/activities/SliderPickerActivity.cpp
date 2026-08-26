#include "SliderPickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

void SliderPickerActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void SliderPickerActivity::onExit() { Activity::onExit(); }

void SliderPickerActivity::adjustValue(const int delta) {
  value += delta;
  if (value < cfg.minValue) value = cfg.minValue;
  if (value > cfg.maxValue) value = cfg.maxValue;
  requestUpdate();
}

void SliderPickerActivity::loop() {
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }

    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      setResult(PercentResult{value});
      finish();
      return;
    }

    // Fine step on logical Left/Right, coarse on logical Up/Down (below) — four directions, four
    // buttons, one meaning each. The PageBack/PageForward names that used to be matched here as
    // well are the SIDE buttons under a second name, i.e. the coarse pair: a single press of the
    // side back button ran both arms and moved the value by +10 then -1.
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left) &&
        ev.type == ButtonEventManager::PressType::Short) {
      adjustValue(-kSmallStep);
      return;
    }

    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right) &&
        ev.type == ButtonEventManager::PressType::Short) {
      adjustValue(kSmallStep);
      return;
    }
  }

  buttonNavigator.onPressAndContinuous(ButtonNavigator::getStepPreviousButtons(), [this] { adjustValue(kLargeStep); });
  buttonNavigator.onPressAndContinuous(ButtonNavigator::getStepNextButtons(), [this] { adjustValue(-kLargeStep); });
}

void SliderPickerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, I18N.get(cfg.titleId), true, EpdFontFamily::BOLD);

  std::string valueText;
  if (!cfg.zeroLabel.empty() && value == cfg.minValue) {
    valueText = cfg.zeroLabel;
  } else {
    valueText = std::to_string(value) + cfg.suffix;
  }
  renderer.drawCenteredText(UI_12_FONT_ID, 90, valueText.c_str(), true, EpdFontFamily::BOLD);

  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = contentRect.x + (contentRect.width - barWidth) / 2;
  constexpr int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int range = cfg.maxValue - cfg.minValue;
  const int fillWidth = range > 0 ? (barWidth - 4) * (value - cfg.minValue) / range : 0;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  renderer.drawCenteredText(SMALL_FONT_ID, barY + 30, I18N.get(cfg.hintId), true);

  // The slider runs across the screen, so - / + ride logical Left/Right and move to whichever
  // button pair lies on that axis; the coarse step rides logical Up/Down and is unlabelled.
  const auto hints = mappedInput.mapHints(tr(STR_BACK), tr(STR_SELECT), "-", "+", "", "");
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);

  renderer.displayBuffer();
}
