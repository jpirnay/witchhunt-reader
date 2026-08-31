#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/themes/ListTouchBand.h"
#include "fontIds.h"

int XtcReaderChapterSelectionActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  constexpr int startY = 60;
  const int availableHeight = contentRect.height - startY - lineHeight;
  // Clamp to at least one item to prevent empty page math.
  return std::max(1, availableHeight / lineHeight);
}

int XtcReaderChapterSelectionActivity::findChapterIndexForPage(uint32_t page) const {
  if (!xtc) {
    return 0;
  }

  const auto& chapters = xtc->getChapters();
  for (size_t i = 0; i < chapters.size(); i++) {
    if (page >= chapters[i].startPage && page <= chapters[i].endPage) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void XtcReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  selectorIndex = findChapterIndexForPage(currentPage);

  requestUpdate();
}

void XtcReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

void XtcReaderChapterSelectionActivity::loop() {
  if (!xtc) {
    return;
  }

  const int pageItems = getPageItems();
  const int totalItems = static_cast<int>(xtc->getChapters().size());

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      const auto& chapters = xtc->getChapters();
      if (!chapters.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(chapters.size())) {
        setResult(PageResult{chapters[selectorIndex].startPage});
        finish();
      }
      return;
    }
    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return;
    }
  }

  // Up/Down step one chapter, Left/Right jump a screenful — a book with hundreds of chapters is
  // otherwise only crossable by holding a button down.
  buttonNavigator.onNextList(selectorIndex, totalItems, [this] { requestUpdate(); }, pageItems);
  buttonNavigator.onPreviousList(selectorIndex, totalItems, [this] { requestUpdate(); }, pageItems);
}

void XtcReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  const int pageItems = getPageItems();

  const int titleX =
      contentRect.x +
      (contentRect.width - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_SELECT_CHAPTER), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, contentRect.y + 15, tr(STR_SELECT_CHAPTER), true, EpdFontFamily::BOLD);

  const auto& chapters = xtc->getChapters();
  if (chapters.empty()) {
    const int emptyX =
        contentRect.x + (contentRect.width - renderer.getTextWidth(UI_10_FONT_ID, tr(STR_NO_CHAPTERS))) / 2;
    renderer.drawText(UI_10_FONT_ID, emptyX, contentRect.y + 120, tr(STR_NO_CHAPTERS));
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  // Rows published for touch — same numbers as the fill below, band top at the fill's y.
  ListTouchBand::recordUniformRows(contentRect.x, contentRect.width - 1, contentRect.y + 60 - 2, 30, pageStartIndex,
                                   std::min(pageItems, static_cast<int>(chapters.size()) - pageStartIndex));
  renderer.fillRect(contentRect.x, contentRect.y + 60 + (selectorIndex % pageItems) * 30 - 2, contentRect.width - 1,
                    30);
  for (int i = pageStartIndex; i < static_cast<int>(chapters.size()) && i < pageStartIndex + pageItems; i++) {
    const auto& chapter = chapters[i];
    const char* title = chapter.name.empty() ? tr(STR_UNNAMED) : chapter.name.c_str();
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, contentRect.y + 60 + (i % pageItems) * 30, title,
                      i != selectorIndex);
  }

  // Left/Right page when there is more than one page to cross, and fall back to stepping (which is
  // what ButtonNavigator::nextPageIndex does on a short list) when there is not.
  const bool pages = static_cast<int>(chapters.size()) > pageItems;
  // Paging rides logical Left/Right and stepping logical Up/Down, so which pair sits on the front
  // strip and which on the side buttons is the orientation's business — mapHints routes both sets
  // of labels to whichever buttons are doing the job.
  const auto hints = mappedInput.mapHints(tr(STR_BACK), tr(STR_SELECT), pages ? tr(STR_LIST_PAGE_PREV) : "",
                                          pages ? tr(STR_LIST_PAGE_NEXT) : "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);

  renderer.displayBuffer();
}

bool XtcReaderChapterSelectionActivity::selectListRow(const int index) {
  if (xtc == nullptr) return false;
  if (index < 0 || index >= static_cast<int>(xtc->getChapters().size())) return false;
  selectorIndex = index;
  return true;
}
