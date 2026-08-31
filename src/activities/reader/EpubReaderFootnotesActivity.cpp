#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderFootnotesActivity::onExit() { Activity::onExit(); }

void EpubReaderFootnotesActivity::loop() {
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }

    // Power short-press also selects the footnote, mirroring upstream's quick-access
    // gesture. This context-specific binding is active only while the footnote list is
    // open, alongside the navigation bindings below.
    if ((ev.button == MappedInputManager::Button::Confirm || ev.button == MappedInputManager::Button::Power) &&
        ev.type == ButtonEventManager::PressType::Short) {
      if (selectedIndex >= 0 && selectedIndex < static_cast<int>(footnotes.size())) {
        setResult(FootnoteResult{footnotes[selectedIndex].href});
        finish();
      }
      return;
    }

    // Either axis moves the selection: whichever pair runs up and down the screen and whichever
    // runs across it. Matching the four logical directions rather than the raw names is also what
    // keeps one press to one step — buttonFor() never answers PageBack/PageForward, which are the
    // side buttons under a second name and used to move the selection twice per press (see the
    // aliasing note in ButtonEventManager.h).
    if ((MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Up) ||
         MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left)) &&
        ev.type == ButtonEventManager::PressType::Short) {
      advanceSelection(-1);
      continue;
    }

    if ((MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Down) ||
         MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right)) &&
        ev.type == ButtonEventManager::PressType::Short) {
      advanceSelection(1);
      continue;
    }
  }
}

void EpubReaderFootnotesActivity::advanceSelection(int delta) {
  if (footnotes.empty()) {
    return;
  }
  const int n = static_cast<int>(footnotes.size());
  selectedIndex = ((selectedIndex + delta) % n + n) % n;
  requestUpdate();
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_FOOTNOTES), true, EpdFontFamily::BOLD);

  if (footnotes.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 90, tr(STR_NO_FOOTNOTES));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  constexpr int startY = 50;
  constexpr int lineHeight = 36;
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  constexpr int marginLeft = 20;

  const int visibleCount = std::max(1, (contentRect.height - startY) / lineHeight);
  if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
  if (selectedIndex >= scrollOffset + visibleCount) scrollOffset = selectedIndex - visibleCount + 1;

  for (int i = scrollOffset; i < static_cast<int>(footnotes.size()) && i < scrollOffset + visibleCount; i++) {
    const int y = contentRect.y + startY + (i - scrollOffset) * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(contentRect.x, y, contentRect.width, lineHeight, true);
    }

    // Show footnote marker, plus the note text when the book-level preview cache
    // resolved it (see FootnotePreviews) — truncated to the row.
    std::string label = footnotes[i].number;
    if (label.empty()) {
      label = tr(STR_LINK);
    }
    if (i < static_cast<int>(previews.size()) && !previews[i].empty()) {
      label += ": ";
      label += previews[i];
      label = renderer.truncatedText(UI_10_FONT_ID, label.c_str(), contentRect.width - 2 * marginLeft);
    }
    renderer.drawText(UI_10_FONT_ID, contentRect.x + marginLeft, y + 4, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
