#pragma once

#include <FreeInkApp.h>

#include "components/UiAppHost.h"

// One two-answer dialog, themed once, for every screen that asks a yes/no question.
//
// Four screens ask one: ConfirmationActivity (the standalone prompt), and an in-place WARNING /
// WAITING_CONFIRMATION state inside ClearCacheActivity, ScreenRepairActivity and
// OtaUpdateActivity. Those three cannot push the standalone prompt -- the question is one state of
// their own machine, and each wants its own accept label ("Clear", "Start", "Update") rather than
// a generic Confirm.
//
// So the composition lives here instead of four times over. What is shared is the part worth
// sharing: Screen::dialog() passes OptionDialogProps straight through WITHOUT substituting text
// styles, so every caller would otherwise have to remember that an unset TextStyle measures
// against font slot 0 and theme all five slots by hand.
namespace ConfirmDialog {

// optionDialog's three text slots, top to bottom. Any may be null.
//
// `title` is a small caption, `headline` a prominent bold line (a book title, "New update"),
// `message` a body line. The wrap caps are deliberate rather than unbounded: a book title or a
// cache warning is not guaranteed short, and optionDialogHeight() honours the same maxLines, so
// what the panel reserves is what gets drawn.
struct Spec {
  const char* title = nullptr;
  const char* headline = nullptr;
  const char* message = nullptr;
  const char* cancelLabel = nullptr;
  const char* acceptLabel = nullptr;
  freeink::ui::ActionId cancelAction = freeink::ui::NO_ACTION;
  freeink::ui::ActionId acceptAction = freeink::ui::NO_ACTION;
  // Default 1 keeps `title` a caption, which is what optionDialog assumes. A caller whose title is
  // really a sentence (the clear-cache warning) raises it.
  uint8_t titleMaxLines = 1;
  uint8_t headlineMaxLines = 3;
  uint8_t messageMaxLines = 4;
};

// Cancel left, accept right, fixed. The hint strip's order DOES follow the orientation (mapLabels
// swaps the pair when the front strip runs bottom-to-top), but these are a different control in a
// different frame: a reader aims at the button that says Clear, not at the one that happens to sit
// under a physical key. Keeping one order means the destructive answer is never where the safe one
// was a moment ago.
inline void draw(UiAppHost::UiScreen& screen, const Spec& spec) {
  namespace fui = freeink::ui;
  const auto& theme = screen.theme();

  fui::DialogOption options[2];
  options[0].label = spec.cancelLabel;
  options[0].action = spec.cancelAction;
  options[1].label = spec.acceptLabel;
  options[1].action = spec.acceptAction;

  fui::OptionDialogProps props;
  props.title = spec.title;
  props.headline = spec.headline;
  props.message = spec.message;
  props.options = options;
  props.optionCount = 2;

  props.titleText = theme.smallText;
  props.titleText.align = fui::TextAlign::Center;
  props.titleText.maxLines = spec.titleMaxLines;
  props.headlineText = theme.titleText;
  props.headlineText.bold = true;
  props.headlineText.align = fui::TextAlign::Center;
  props.headlineText.maxLines = spec.headlineMaxLines;
  props.messageText = theme.bodyText;
  props.messageText.align = fui::TextAlign::Center;
  props.messageText.maxLines = spec.messageMaxLines;
  props.buttonText = theme.bodyText;
  props.buttonText.bold = true;
  props.buttonText.align = fui::TextAlign::Center;
  props.buttonHeight = theme.minTouchSize;
  props.gap = theme.spaceMd;

  // The panel is sized to its text and is not clipped, so a long message (release notes, in
  // landscape at a large UI font) could push it off the screen. The message gives up lines first,
  // its last one ending in an ellipsis, until the panel fits. Width as Screen::dialog()'s default.
  const auto safe = screen.frame().safeRect();
  const auto width = static_cast<int16_t>(safe.width * 4 / 5);
  while (props.message && props.messageText.maxLines > 1 &&
         fui::optionDialogHeight(screen.target(), props, width) > safe.height) {
    --props.messageText.maxLines;
  }
  screen.dialog(props, width);
}

}  // namespace ConfirmDialog
