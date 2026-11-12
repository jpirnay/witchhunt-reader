#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIIcon.h>

#include <cstdint>
#include <cstdio>

#include "components/icons/moonIcons.h"
#include "components/icons/sunIcons.h"

// The light drawer's layout, kept out of FrontlightPanelActivity so that it needs FreeInkUI and nothing else of the
// firmware's: the activity draws it, and test/light_drawer_layout lays it out at each lit board's size.
//
// Two switches, the reading light (sun) and Dark Mode (moon), both square icon buttons in the slider kit's style and
// drawn filled while on, so the two read as one pair:
//  - with a warmth channel, each closes a slider row: the sun the brightness row, in the trailing slot fui::sliderRow
//    keeps for exactly this, and the moon the warmth row, whose slot was empty;
//  - without one, the brightness row gives its whole width to [-] [capsule] [+], and the two switches share a line
//    under it, half of it each.
namespace LightDrawerLayout {

namespace fui = freeink::ui;

constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_LIGHT = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_DISMISS = 6;
constexpr fui::ActionId ACTION_DARK_MODE = 7;

// What the drawer shows. The labels are the caller's, already translated.
struct Model {
  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  bool dark = false;
  bool hasWarmth = false;
  const char* brightnessLabel = nullptr;
  const char* warmthLabel = nullptr;
};

struct Metrics {
  // The theme header the activity draws over the top of the sheet once the pass is done (topPadding + headerHeight).
  int16_t headerBand = 0;
  // Between the sheet's sides and the controls.
  int16_t sideInset = 0;
  // Height of each [-] [capsule] [+] band, and the side of every square button in it.
  int16_t controlHeight = 0;
};

// `compact` is PanelMetrics::COMPACT, passed in rather than included so the host test can lay out every board.
//
// Elsewhere the kit's finger-sized band, minTouchSize + 12, and twice spaceLg a side. On the compact panel (Murphy:
// 240 px across at about 130 ppi, half the density of the X4 Pro and the T5S3) those made 56 px squares about 11 mm
// wide, and three of them with two 32 px margins left the brightness capsule no width at all. There the band is the
// touch floor itself, which is still about 8.5 mm, and the margin is one spaceLg.
inline Metrics metricsFor(const fui::ThemeTokens& theme, const int16_t headerBand, const bool compact) {
  Metrics metrics;
  metrics.headerBand = headerBand;
  metrics.sideInset = static_cast<int16_t>(compact ? theme.spaceLg : theme.spaceLg * 2);
  metrics.controlHeight = static_cast<int16_t>(compact ? theme.minTouchSize : theme.minTouchSize + 12);
  return metrics;
}

// One slider row as Screen::sliderRow() lays it out: the same substitutions it makes on unstyled props.
inline int16_t sliderRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& theme,
                               const int16_t controlHeight) {
  fui::SliderRowProps probe;
  probe.labelText = theme.smallText;
  probe.labelText.bold = true;
  probe.captionGap = theme.spaceMd;
  return fui::sliderRowHeight(target, probe, controlHeight);
}

// The height to hand Screen::sheet(): what build() takes, row by row, plus spaceLg of air and the grabber band.
inline int16_t sheetHeight(const fui::DrawTarget& target, const fui::ThemeTokens& theme, const Model& model,
                           const Metrics& metrics) {
  const int16_t row = sliderRowHeight(target, theme, metrics.controlHeight);
  int height = metrics.headerBand + theme.spaceLg;
  height += row + theme.spaceMd;
  // The warmth row, or the line of switches; Screen::sliderRow() and takeTop() below both leave spaceMd after it.
  height += (model.hasWarmth ? row : metrics.controlHeight) + theme.spaceMd;
  height += theme.spaceLg;
  // Screen::sheet() takes the grabber band out of the height it is given (sheetContentRect), so it is added back
  // here. Read from a default SheetProps rather than restated, so the two cannot drift if the SDK retunes the pill.
  const fui::SheetProps chrome;
  height += chrome.grabberMargin + chrome.grabberHeight + chrome.grabberInset;
  return static_cast<int16_t>(height);
}

inline fui::BitmapRef sunIcon(const bool on) { return fui::bitmapFromIcon(on ? icon_sun_filled_32 : icon_sun_32); }
inline fui::BitmapRef moonIcon(const bool on) { return fui::bitmapFromIcon(on ? icon_moon_filled_32 : icon_moon_32); }

template <typename ScreenT>
void build(ScreenT& screen, const Model& model, const Metrics& metrics, const int16_t height) {
  const fui::ThemeTokens& theme = screen.theme();

  fui::SheetProps chrome;
  chrome.anchor = fui::SheetEdge::Top;
  // The customary way to close a drawer. Registered by the component over the screen below the sheet, so nothing
  // here has to hit-test the page.
  chrome.dismissAction = ACTION_DISMISS;
  screen.sheet(chrome, height);

  screen.insetContent(fui::Insets{0, metrics.sideInset, 0, metrics.sideInset});
  screen.spacer(static_cast<int16_t>(metrics.headerBand + theme.spaceLg));

  // One buffer for both rows: sliderRow() draws at once, so the text is on the frame before the second snprintf.
  char value[16];

  fui::SliderRowProps light;
  light.label = model.brightnessLabel;
  snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(model.brightness));
  light.value = value;
  light.sliderValue = model.brightness;
  light.sliderAction = ACTION_BRIGHTNESS;
  light.decrement = ACTION_BRIGHTNESS_STEP;
  light.increment = ACTION_BRIGHTNESS_STEP;
  if (model.hasWarmth) {
    light.toggleAction = ACTION_LIGHT;
    light.toggleIcon = sunIcon(model.lightOn);
  }
  screen.sliderRow(light, metrics.controlHeight);

  if (model.hasWarmth) {
    fui::SliderRowProps warm;
    warm.label = model.warmthLabel;
    snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(model.warmth));
    warm.value = value;
    warm.sliderValue = model.warmth;
    warm.sliderAction = ACTION_WARMTH;
    warm.decrement = ACTION_WARMTH_STEP;
    warm.increment = ACTION_WARMTH_STEP;
    warm.toggleAction = ACTION_DARK_MODE;
    warm.toggleIcon = moonIcon(model.dark);
    screen.sliderRow(warm, metrics.controlHeight);
    return;
  }

  // The switches without a warmth row to close: as tall as the slider band, styled as its buttons are, and wide
  // enough to hit without looking.
  const fui::Rect line = screen.takeTop(metrics.controlHeight, theme.spaceMd);
  const int16_t half = static_cast<int16_t>((line.width - theme.spaceMd) / 2);
  fui::ButtonProps toggle;
  toggle.styles = fui::sliderRowStepStyles(fui::resolveRadius(theme.controlRadius, 18));
  toggle.inputMask = fui::InputTouch;
  toggle.action = ACTION_LIGHT;
  toggle.icon = sunIcon(model.lightOn);
  screen.button(toggle, fui::Rect{line.x, line.y, half, line.height});
  toggle.action = ACTION_DARK_MODE;
  toggle.icon = moonIcon(model.dark);
  screen.button(toggle, fui::Rect{static_cast<int16_t>(line.right() - half), line.y, half, line.height});
}

}  // namespace LightDrawerLayout
