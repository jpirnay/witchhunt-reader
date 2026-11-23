#include "FootnotePopupActivity.h"

#include <Epub/FootnotePreviews.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "TouchUi.h"
#include "activities/ActivityResult.h"
#include "components/TapZones.h"
#include "components/UITheme.h"
#include "components/themes/ButtonHintStrip.h"
#include "fontIds.h"

namespace {

// The line along the sheet's top edge: the reader bars' bottom band draws the same (ReaderBarsLayout::Metrics::rule).
constexpr int kRule = 2;

bool contains(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

}  // namespace

FootnotePopupActivity::FootnotePopupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::unique_ptr<Page> page, const int pageFontId, const int pageX,
                                             const int pageY, std::string marker, std::string href, std::string markup,
                                             const LanguageHyphenator* hyphenator)
    : Activity("FootnotePopup", renderer, mappedInput),
      page(std::move(page)),
      pageFontId(pageFontId),
      pageX(pageX),
      pageY(pageY),
      marker(std::move(marker)),
      href(std::move(href)),
      markup(std::move(markup)),
      hyphenator(hyphenator) {}

bool FootnotePopupActivity::layOut() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // In the book's orientation, as the reader holds it; the hint strip lies outside this area, on whichever edge the
  // keys are.
  area = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);
  const int inset = metrics.contentSidePadding;
  const int gap = metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(pageFontId);
  // The rule, a gap, the title line, half a gap, the note, a gap.
  const int chrome = kRule + gap + renderer.getLineHeight(UI_10_FONT_ID) + gap / 2 + gap;
  // At most half the screen, so the page the note belongs to stays readable above it; a longer note pages.
  const int bodyMax = std::max(lineHeight, area.height / 2 - chrome);
  const int bodyWidth = area.width - 2 * inset;
  if (!body.layOut(renderer, std::move(markup), /*html=*/true, FootnotePreviews::MAX_NOTE_MARKUP_BYTES, pageFontId,
                   bodyWidth, bodyMax, hyphenator)) {
    LOG_ERR("FNP", "Footnote popup: no memory to lay the note out");
    return false;
  }
  const int bodyHeight = body.pageCount() > 1 ? bodyMax : std::max(lineHeight, body.usedHeight(renderer, 0));
  const int sheetHeight = chrome + bodyHeight;
  sheet = Rect{area.x, area.y + area.height - sheetHeight, area.width, sheetHeight};
  headerY = sheet.y + kRule + gap;
  bodyBox = Rect{area.x + inset, headerY + renderer.getLineHeight(UI_10_FONT_ID) + gap / 2, bodyWidth, bodyHeight};
  currentPage = 0;
  return true;
}

void FootnotePopupActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void FootnotePopupActivity::close() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void FootnotePopupActivity::goToNote() {
  setResult(FootnoteResult{href, marker});
  finish();
}

bool FootnotePopupActivity::nextPage() {
  if (currentPage + 1 < body.pageCount()) {
    currentPage++;
    requestUpdate();
    return true;
  }
  close();
  return false;
}

bool FootnotePopupActivity::previousPage() {
  if (currentPage > 0) {
    currentPage--;
    requestUpdate();
    return true;
  }
  close();
  return false;
}

bool FootnotePopupActivity::handleTap() {
#if CP_TOUCH_UI
  if (!mappedInput.hasTouch()) return false;
  int x = 0;
  int y = 0;
  // A tap on the hint strip is the strip's (ActivityManager::dispatchHintStripTap, after this loop): Back and Go to
  // note are there.
  if (mappedInput.peekScreenTapIn(touchtransform::Portrait, x, y) && ButtonHintStrip::hitTest(x, y) >= 0) return false;
  if (!mappedInput.wasScreenTapped(x, y)) return false;
  // On the page above the sheet: closed, as a tap beside a drawer closes it.
  if (contains(area, x, y) && y < sheet.y) {
    close();
    return true;
  }
  // A long note pages by its halves, as a book page does.
  if (body.pageCount() > 1) {
    switch (TapZones::halfOfBand(x, y, bodyBox.x, bodyBox.y, bodyBox.width, bodyBox.height)) {
      case TapZones::Half::Previous:
        if (currentPage > 0) previousPage();
        return true;
      case TapZones::Half::Next:
        if (currentPage + 1 < body.pageCount()) nextPage();
        return true;
      case TapZones::Half::None:
        break;
    }
  }
#endif  // CP_TOUCH_UI
  return false;
}

void FootnotePopupActivity::loop() {
  if (handleTap()) return;
  // Classified events, not raw edges, as the footnote list reads them: the footnote button may be a long press, which
  // fires while the key is still down, and its release must not land here as a Confirm.
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.type != ButtonEventManager::PressType::Short) continue;
    using B = MappedInputManager::Button;
    using D = MappedInputManager::Direction;
    if (ev.button == B::Back) {
      close();
      return;
    }
    if (ev.button == B::Confirm) {
      goToNote();
      return;
    }
    // Either axis pages, as in the footnote list: the logical directions, so a side key is one step and not two
    // (buttonFor() never answers PageBack/PageForward).
    if (MappedInputManager::isDirection(ev.button, D::Up) || MappedInputManager::isDirection(ev.button, D::Left)) {
      if (!previousPage()) return;
    } else if (MappedInputManager::isDirection(ev.button, D::Down) ||
               MappedInputManager::isDirection(ev.button, D::Right)) {
      if (!nextPage()) return;
    }
  }
}

// Power set to turn pages turns the note's pages here, as it does the book's (DictionaryDefinitionActivity, the same
// rule): taken from Power only, the other keys already page.
bool FootnotePopupActivity::takesButtonAction(const CrossPointSettings::BUTTON_ACTION action,
                                              const MappedInputManager::Button button) const {
  using BA = CrossPointSettings::BUTTON_ACTION;
  return button == MappedInputManager::Button::Power && (action == BA::BTN_PAGE_FORWARD || action == BA::BTN_PAGE_BACK);
}

void FootnotePopupActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  using BA = CrossPointSettings::BUTTON_ACTION;
  switch (action) {
    case BA::BTN_PAGE_FORWARD:
      nextPage();
      break;
    case BA::BTN_PAGE_BACK:
      previousPage();
      break;
    default:
      break;
  }
}

void FootnotePopupActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // The page and the note in one prewarm scope: both are in the reader's font, so their glyphs load from the card in
  // one batch. The scan records codepoints only; the draws after it are the real ones.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  if (page) page->renderTextOnly(renderer, pageFontId, pageX, pageY);
  body.draw(renderer, currentPage, bodyBox.x, bodyBox.y);
  scope.endScanAndPrewarm();
  if (page) page->render(renderer, pageFontId, pageX, pageY);

  // The sheet, and the hint strip's band around it: the strip paints only its boxes, and the page would show between
  // them. The band is whatever of the screen lies outside the content area, on one edge.
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  renderer.fillRect(sheet.x, sheet.y, sheet.width, sheet.height, false);
  // A page line the sheet's edge would cut goes whole: half a line of text above the rule reads as a fault. A line's
  // yPos is its top; a vertical page's lines run across the screen and are left as they are.
  if (page && !page->isVertical()) {
    const int lineHeight = renderer.getLineHeight(pageFontId);
    int coverTop = sheet.y;
    for (const auto& element : page->elements) {
      if (!element || element->getTag() != TAG_PageLine) continue;
      const int top = pageY + element->yPos;
      if (top < sheet.y && top + lineHeight > sheet.y) coverTop = std::min(coverTop, top);
    }
    if (coverTop < sheet.y) renderer.fillRect(sheet.x, coverTop, sheet.width, sheet.y - coverTop, false);
  }
  if (area.y > 0) renderer.fillRect(0, 0, screenWidth, area.y, false);
  if (area.y + area.height < screenHeight) {
    renderer.fillRect(0, area.y + area.height, screenWidth, screenHeight - area.y - area.height, false);
  }
  if (area.x > 0) renderer.fillRect(0, 0, area.x, screenHeight, false);
  if (area.x + area.width < screenWidth) {
    renderer.fillRect(area.x + area.width, 0, screenWidth - area.x - area.width, screenHeight, false);
  }
  renderer.fillRect(sheet.x, sheet.y, sheet.width, kRule, true);

  // The title: which note this is, and on a long one which page of it.
  char title[64];
  if (marker.empty()) {
    snprintf(title, sizeof(title), "%s", tr(STR_NOTE));
  } else {
    snprintf(title, sizeof(title), "%s %s", tr(STR_NOTE), marker.c_str());
  }
  const int titleWidth = std::max(0, bodyBox.width / 2);
  const std::string shownTitle = renderer.truncatedText(UI_10_FONT_ID, title, titleWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, bodyBox.x, headerY, shownTitle.c_str(), true, EpdFontFamily::BOLD);
  if (body.pageCount() > 1) {
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, body.pageCount());
    renderer.drawText(UI_10_FONT_ID, bodyBox.x + bodyBox.width - renderer.getTextWidth(UI_10_FONT_ID, counter), headerY,
                      counter);
  }

  body.draw(renderer, currentPage, bodyBox.x, bodyBox.y);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GO_TO_NOTE), currentPage > 0 ? "<" : "",
                                            currentPage + 1 < body.pageCount() ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
