#include "SliderPickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/SliderGeometry.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;

// The bar, in one place. render() paints it and handleSliderTouch() hit-tests it, and the two
// must agree to the pixel or the knob lands somewhere other than the fingertip. Derived rather
// than recorded: it depends only on the content rect and two constants, so both tasks can
// compute it and there is no snapshot to go stale.
constexpr int kBarWidth = 360;
constexpr int kBarHeight = 16;
constexpr int kBarY = 140;

// Finger-sized hit band around a 16 px bar. The SDK's own components use 44 px as the minimum
// touch target (CoverGridProps::minTouchSize) and a slider is the control where a near-miss is
// most annoying, so the band is centred on the bar and padded to at least that.
constexpr int kTouchBandHeight = 56;

int barLeft(const GfxRenderer& renderer) {
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  return contentRect.x + (contentRect.width - kBarWidth) / 2;
}
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

void SliderPickerActivity::setValue(const int newValue) {
  const int clamped = newValue < cfg.minValue ? cfg.minValue : (newValue > cfg.maxValue ? cfg.maxValue : newValue);
  // Only repaint on a real change. Load-bearing for the drag below: isScreenTouchHeld() reports
  // on EVERY loop pass while the finger is down, and a repaint per pass would queue e-paper
  // refreshes far faster than the panel can retire them.
  if (clamped == value) return;
  value = clamped;
  requestUpdate();
}

// Tap or drag the bar to name a value outright, instead of stepping to it.
//
// Drag is handled, and on e-paper that needs saying: each repaint is a panel refresh of several
// hundred ms, so a slider cannot track a finger the way an LCD one does. What makes it usable
// is that the VALUE follows the finger every pass while the repaint only happens when the value
// actually changes, and the render task supersedes a pending update when a newer one arrives.
// The number therefore lands where the finger stopped, even though the screen shows only some
// of the intermediate positions. That is the right trade here: the alternative, refusing drag
// and accepting taps only, makes fine adjustment on a 100-step range unpleasant.
//
// A tap does NOT confirm. It moves the value and leaves committing to Confirm, exactly as the
// buttons do — and Confirm is reachable by touch through the hint strip. Jumping straight to a
// tapped percentage would make a mis-tap a navigation the reader has to undo.
bool SliderPickerActivity::handleSliderTouch() {
  if (!mappedInput.hasTouch()) return false;

  const int barX = barLeft(renderer);
  // Deliberately more generous than the drawn bar in both axes: 16 px is far below a finger
  // target, and a contact just past either end reads as "go to that end" rather than as a miss
  // (SliderGeometry::valueForX clamps). Horizontal slack matches the vertical padding so the
  // band is a comfortable rectangle rather than a hairline.
  const int bandPad = (kTouchBandHeight - kBarHeight) / 2;
  const int bandTop = kBarY - bandPad;
  const int bandLeft = barX - bandPad;
  const int bandRight = barX + kBarWidth + bandPad;
  const auto inBand = [&](const int x, const int y) {
    return y >= bandTop && y < bandTop + kTouchBandHeight && x >= bandLeft && x < bandRight;
  };

  int x = 0;
  int y = 0;
  // Held first: a drag is a live contact, and isScreenTouchHeld() has no tap-slop gate, so it
  // reports the finger's position from the moment it lands rather than only on release.
  if (mappedInput.isScreenTouchHeld(x, y) && inBand(x, y)) {
    setValue(SliderGeometry::valueForX(x, barX, kBarWidth, cfg.minValue, cfg.maxValue));
    return true;
  }
  // Then the release edge, which catches a tap too brief to have been seen as held.
  if (mappedInput.wasScreenTapped(x, y) && inBand(x, y)) {
    setValue(SliderGeometry::valueForX(x, barX, kBarWidth, cfg.minValue, cfg.maxValue));
    return true;
  }
  return false;
}

void SliderPickerActivity::loop() {
  // Ahead of the button queue: while a finger is dragging the bar, a queued step press would
  // fight it for the same value.
  if (handleSliderTouch()) return;

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

  const int barX = barLeft(renderer);

  renderer.drawRect(barX, kBarY, kBarWidth, kBarHeight);

  // Same mapping the hit test inverts — see SliderGeometry.
  const int fillWidth = SliderGeometry::fillWidthFor(value, kBarWidth, cfg.minValue, cfg.maxValue);
  if (fillWidth > 0) {
    renderer.fillRect(barX + SliderGeometry::kTrackInset, kBarY + 2, fillWidth, kBarHeight - 4);
  }

  const int knobX = SliderGeometry::trackX(barX) + fillWidth - 2;
  renderer.fillRect(knobX, kBarY - 4, 4, kBarHeight + 8, true);

  renderer.drawCenteredText(SMALL_FONT_ID, kBarY + 30, I18N.get(cfg.hintId), true);

  // The slider runs across the screen, so - / + ride logical Left/Right and move to whichever
  // button pair lies on that axis; the coarse step rides logical Up/Down and is unlabelled.
  const auto hints = mappedInput.mapHints(tr(STR_BACK), tr(STR_SELECT), "-", "+", "", "");
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);

  renderer.displayBuffer();
}
