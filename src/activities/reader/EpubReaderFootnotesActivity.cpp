#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/themes/ListTouchBand.h"
#include "fontIds.h"

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  scrollOffset = 0;

  // Notes first, then navigation, each in page order. Built once here rather than at every
  // render: the list cannot change while it is open.
  order.clear();
  order.reserve(footnotes.size());
  for (size_t i = 0; i < footnotes.size(); ++i) {
    if (entryIsNote(i)) order.push_back(static_cast<uint16_t>(i));
  }
  firstLinkRow = -1;
  for (size_t i = 0; i < footnotes.size(); ++i) {
    if (!entryIsNote(i)) {
      if (firstLinkRow < 0) firstLinkRow = static_cast<int>(order.size());
      order.push_back(static_cast<uint16_t>(i));
    }
  }
  // A page of only links needs no divider — there is nothing above it to divide from.
  if (firstLinkRow == 0) firstLinkRow = -1;
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
      if (selectedIndex >= 0 && selectedIndex < static_cast<int>(order.size())) {
        setResult(FootnoteResult{footnotes[order[selectedIndex]].href});
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
  if (order.empty()) {
    return;
  }
  const int n = static_cast<int>(order.size());
  selectedIndex = ((selectedIndex + delta) % n + n) % n;
  requestUpdate();
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_FOOTNOTES), true, EpdFontFamily::BOLD);

  if (order.empty()) {
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

  // Rows published for touch. This list SCROLLS rather than pages, so the top row is
  // scrollOffset, and its fill sits exactly at the row y (no -2 nudge, unlike the chapter
  // selectors) -- so the band top is that same y.
  ListTouchBand::recordUniformRows(contentRect.x, contentRect.width, contentRect.y + startY, lineHeight, scrollOffset,
                                   std::min(visibleCount, static_cast<int>(order.size()) - scrollOffset));

  for (int i = scrollOffset; i < static_cast<int>(order.size()) && i < scrollOffset + visibleCount; i++) {
    const int y = contentRect.y + startY + (i - scrollOffset) * lineHeight;
    const bool isSelected = (i == selectedIndex);
    const size_t entry = order[i];

    if (isSelected) {
      renderer.fillRect(contentRect.x, y, contentRect.width, lineHeight, true);
    }

    // The line between the notes and the links. Drawn INSIDE the top of the first link row rather
    // than as a row of its own, so the rows stay uniform: nothing to skip when moving the
    // selection, and the touch band stays one contiguous run of activatable rows.
    if (i == firstLinkRow) {
      renderer.fillRect(contentRect.x + marginLeft, y, contentRect.width - 2 * marginLeft, 1, !isSelected);
    }

    // Footnote marker, plus the note text when the preview store resolved it — truncated to the
    // row. A navigation link has no note text by definition, so it is named for what it is; a
    // marker-less one was already shown that way.
    std::string label = footnotes[entry].number;
    const bool note = entryIsNote(entry);
    if (label.empty()) {
      label = tr(STR_LINK);
    } else if (!note) {
      label += "  ";
      label += tr(STR_LINK);
    }
    if (note && entry < previews.size() && !previews[entry].empty()) {
      label += ": ";
      label += previews[entry];
    }
    label = renderer.truncatedText(UI_10_FONT_ID, label.c_str(), contentRect.width - 2 * marginLeft);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + marginLeft, y + 4, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

ListRowTap::Result EpubReaderFootnotesActivity::selectListRow(const int index) {
  return ListRowTap::apply(index, static_cast<int>(order.size()), selectedIndex);
}
