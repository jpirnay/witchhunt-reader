#include "FootnoteSheet.h"

#include <Epub/FootnotePreviews.h>
#include <Epub/Page.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "ButtonEventManager.h"
#include "MappedInputManager.h"
#include "SerialTestApi.h"  // SERIAL_TEST_API
#include "components/TapZones.h"
#include "components/UITheme.h"
#include "components/themes/ButtonHintStrip.h"
#include "fontIds.h"
#include "util/SerialCommandGrammar.h"

namespace {

// The line along the sheet's page-side edge: the reader bars' bottom band draws the same
// (ReaderBarsLayout::Metrics::rule).
constexpr int kRule = 2;

}  // namespace

FootnoteSheet::FootnoteSheet(GfxRenderer& renderer, MappedInputManager& mappedInput, const int fontId,
                             const char* marker, const LanguageHyphenator* hyphenator, const Place place)
    : renderer(renderer), mappedInput(mappedInput), fontId(fontId), hyphenator(hyphenator), place(place) {
  if (marker) snprintf(this->marker, sizeof(this->marker), "%s", marker);
}

bool FootnoteSheet::layOut(std::string markup, const Page& page, const int pageY) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // In the book's orientation, as the reader holds it; the hint strip lies outside this area, on whichever edge the
  // keys are.
  area = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);
  const int inset = metrics.contentSidePadding;
  const int gap = metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(fontId);
  // The rule, a gap, the title line, half a gap, the note, a gap; the rule on the edge that faces the page.
  const int chrome = kRule + gap + renderer.getLineHeight(UI_10_FONT_ID) + gap / 2 + gap;
  // The half of this area the marker is not in, so the sentence that calls the note stays in view. Halved against the
  // screen, as it was, a marker just above the screen's middle got a bottom sheet over it wherever the hint strip
  // shortens the area. A marker the reader found no box for leaves the sheet at the bottom.
  sheetAtTop = place.markerY >= 0 && place.markerY >= area.y + area.height / 2;
  // At most half the area, so the page the note belongs to stays readable beside it; a longer note pages. Nor over the
  // marker's line: an edge that cut it would cover it whole (below).
  int room = area.height / 2;
  if (place.markerY >= 0 && !page.isVertical()) {
    const auto holdsMarker = [&](const auto& element) {
      if (!element || element->getTag() != TAG_PageLine) return false;
      const int top = pageY + element->yPos;
      return place.markerY >= top && place.markerY < top + lineHeight;
    };
    const auto line = std::find_if(page.elements.begin(), page.elements.end(), holdsMarker);
    if (line != page.elements.end()) {
      const int top = pageY + (*line)->yPos;
      room = std::min(room, sheetAtTop ? top - area.y : area.y + area.height - (top + lineHeight));
    }
  }
  const int bodyMax = std::max(lineHeight, room - chrome);
  const int bodyWidth = area.width - 2 * inset;
  if (!body.layOut(renderer, std::move(markup), /*html=*/true, FootnotePreviews::MAX_NOTE_MARKUP_BYTES, fontId,
                   bodyWidth, bodyMax, hyphenator)) {
    LOG_ERR("FNP", "Footnote sheet: no memory to lay the note out");
    return false;
  }
  const int bodyHeight = body.pageCount() > 1 ? bodyMax : std::max(lineHeight, body.usedHeight(renderer, 0));
  const int sheetHeight = chrome + bodyHeight;
  if (sheetAtTop) {
    sheet = Rect{area.x, area.y, area.width, sheetHeight};
    ruleY = sheet.y + sheet.height - kRule;
    headerY = sheet.y + gap;
  } else {
    sheet = Rect{area.x, area.y + area.height - sheetHeight, area.width, sheetHeight};
    ruleY = sheet.y;
    headerY = sheet.y + kRule + gap;
  }
  bodyBox = Rect{area.x + inset, headerY + renderer.getLineHeight(UI_10_FONT_ID) + gap / 2, bodyWidth, bodyHeight};
  currentPage = place.startAtLastPage ? body.pageCount() - 1 : 0;

  // The fill grows over a page line either edge would cut, so the line goes whole: half a line of text beside the rule
  // reads as a fault. A line's yPos is its top; a vertical page's lines run across the screen and stay.
  coverTop = sheet.y;
  coverBottom = sheet.y + sheet.height;
  if (!page.isVertical()) {
    for (const auto& element : page.elements) {
      if (!element || element->getTag() != TAG_PageLine) continue;
      const int top = pageY + element->yPos;
      if (top < sheet.y && top + lineHeight > sheet.y) coverTop = std::min(coverTop, top);
      if (top < sheet.y + sheet.height && top + lineHeight > sheet.y + sheet.height) {
        coverBottom = std::max(coverBottom, top + lineHeight);
      }
    }
  }
  return true;
}

FootnoteSheet::Event FootnoteSheet::nextPage() {
  if (currentPage + 1 < body.pageCount()) {
    currentPage++;
    return Event::Redraw;
  }
  return hasNextNote() ? Event::Next : Event::Close;
}

FootnoteSheet::Event FootnoteSheet::previousPage() {
  if (currentPage > 0) {
    currentPage--;
    return Event::Redraw;
  }
  return hasPreviousNote() ? Event::Previous : Event::Close;
}

#if CP_TOUCH_UI
FootnoteSheet::Event FootnoteSheet::handleTap() {
  if (!mappedInput.hasTouch()) return Event::None;
  int x = 0;
  int y = 0;
  // A tap on the hint strip is the strip's (ActivityManager::dispatchHintStripTap, after the reader's loop): Back and
  // Go to note are there.
  if (mappedInput.peekScreenTapIn(touchtransform::Portrait, x, y) && ButtonHintStrip::hitTest(x, y) >= 0) {
    return Event::None;
  }
  if (!mappedInput.wasScreenTapped(x, y)) return Event::None;
  // On the page beside the sheet: closed, as a tap beside a drawer closes it.
  const bool inArea = x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height;
  if (inArea && (sheetAtTop ? y >= sheet.y + sheet.height : y < sheet.y)) return Event::Close;
  // The note's halves page it, as a book page's do, and go on to the next or previous note; at the page's first or last
  // note a tap there does nothing (the page beside the sheet closes it).
  switch (TapZones::halfOfBand(x, y, bodyBox.x, bodyBox.y, bodyBox.width, bodyBox.height)) {
    case TapZones::Half::Previous:
      if (currentPage > 0 || hasPreviousNote()) return previousPage();
      break;
    case TapZones::Half::Next:
      if (currentPage + 1 < body.pageCount() || hasNextNote()) return nextPage();
      break;
    case TapZones::Half::None:
      break;
  }
  return Event::None;
}
#endif  // CP_TOUCH_UI

FootnoteSheet::Event FootnoteSheet::loop(ButtonEventManager& events) {
#if CP_TOUCH_UI
  const Event tapped = handleTap();
  if (tapped != Event::None) return tapped;
#endif
  ButtonEventManager::ButtonEvent ev;
  while (events.consumeEvent(ev)) {
    if (ev.type != ButtonEventManager::PressType::Short) continue;
    using B = MappedInputManager::Button;
    using D = MappedInputManager::Direction;
    if (ev.button == B::Back) return Event::Close;
    if (ev.button == B::Confirm) return Event::Follow;
    // Either axis pages, as in the footnote list: the logical directions, so a side key is one step and not two
    // (buttonFor() never answers PageBack/PageForward).
    if (MappedInputManager::isDirection(ev.button, D::Up) || MappedInputManager::isDirection(ev.button, D::Left)) {
      return previousPage();
    }
    if (MappedInputManager::isDirection(ev.button, D::Down) || MappedInputManager::isDirection(ev.button, D::Right)) {
      return nextPage();
    }
  }
  return Event::None;
}

void FootnoteSheet::draw() const {
  // The sheet over its cover rows, then the hint strip's band: the strip paints only its boxes, and the page would show
  // between them. The band is whatever of the screen lies outside the content area, on one edge; fillRect() draws
  // nothing for an empty rectangle, as three of these four are.
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  renderer.fillRect(sheet.x, coverTop, sheet.width, coverBottom - coverTop, false);
  renderer.fillRect(0, 0, screenWidth, area.y, false);
  renderer.fillRect(0, area.y + area.height, screenWidth, screenHeight - area.y - area.height, false);
  renderer.fillRect(0, 0, area.x, screenHeight, false);
  renderer.fillRect(area.x + area.width, 0, screenWidth - area.x - area.width, screenHeight, false);
  renderer.fillRect(sheet.x, ruleY, sheet.width, kRule, true);

  // The title: which note this is, and where it stands among the page's notes; on a long note, right, which page of it.
  // One format, so each language puts the marker where it goes ("Note 12", "12. lábjegyzet").
  char title[96];
  const int used = snprintf(title, sizeof(title), tr(STR_NOTE_TITLE), marker);
  if (place.count > 1 && used > 0 && static_cast<size_t>(used) < sizeof(title)) {
    char position[32];
    snprintf(position, sizeof(position), tr(STR_NOTE_POSITION), place.position + 1, static_cast<int>(place.count));
    snprintf(title + used, sizeof(title) - used, "  \xC2\xB7  %s", position);
  }
  int counterWidth = 0;
  if (body.pageCount() > 1) {
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, body.pageCount());
    counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, bodyBox.x + bodyBox.width - counterWidth, headerY, counter);
  }
  const int titleWidth = std::max(0, bodyBox.width - (counterWidth > 0 ? counterWidth + 16 : 0));
  const std::string shownTitle = renderer.truncatedText(UI_10_FONT_ID, title, titleWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, bodyBox.x, headerY, shownTitle.c_str(), true, EpdFontFamily::BOLD);

  // The note in a prewarm scope of its own, as a page is drawn: its glyphs load from the card in one batch. The scan
  // records codepoints only; the draw after it is the real one.
  auto scope = renderer.getFontCacheManager()->createPrewarmScope();
  body.draw(renderer, currentPage, bodyBox.x, bodyBox.y);
  scope.endScanAndPrewarm();
  body.draw(renderer, currentPage, bodyBox.x, bodyBox.y);

  // "<" and ">" while the keys have somewhere to go: another page of this note, or another note on the page.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_GO_TO_NOTE), currentPage > 0 || hasPreviousNote() ? "<" : "",
                            currentPage + 1 < body.pageCount() || hasNextNote() ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

#if SERIAL_TEST_API
void FootnoteSheet::appendDebugState(std::string& out) const {
  char value[24];
  snprintf(value, sizeof(value), "%d/%d", place.position + 1, static_cast<int>(place.count));
  SerialCommandGrammar::appendField(out, "note", value);
  snprintf(value, sizeof(value), "%d/%d", currentPage + 1, body.pageCount());
  SerialCommandGrammar::appendField(out, "note_page", value);
  SerialCommandGrammar::appendField(out, "note_at", sheetAtTop ? "top" : "bottom");
}
#endif  // SERIAL_TEST_API
