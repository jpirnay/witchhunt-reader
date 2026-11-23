#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "activities/ActivityResult.h"
#include "components/TapZones.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Body text left/right inset, matching the reader's default feel.
constexpr int SIDE_PADDING = 20;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas are roughly the text plus ~7 bytes per word, plus per-line
// objects), so they roughly double the string's footprint while this activity is
// stacked over the reader and the word-select overlay. Bigger definitions take
// the span-based plain-text path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  // Normalize StarDict multi-type separators so the wrap loop and the C-string
  // font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  {
    // Both layouts measure in bodyFontId. For a CJK lookup that is the reader's font, and a
    // TrueType or CFF face may be measured only under the render lock, which onEnter() runs without.
    RenderLock lock(*this);
    const BodyArea area = bodyArea();
    // Say so (layoutFailed) rather than show part of the definition as if it were all of it.
    layoutFailed = !body.layOut(renderer, std::move(definition), htmlDefinition, MAX_STYLED_HTML_BYTES, bodyFontId,
                                area.width, area.height, hyphenator);
    currentPage = 0;
  }
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  // A definition can pull in a whole SD font's glyphs for a script the book never uses, and
  // those caches would otherwise stay resident behind the reader for the rest of the session.
  // They rebuild on demand, so handing the heap back on the way out costs a reload at worst
  // and buys back tens of KB the reader wants for its next page build.
  // Ported from crosspoint-reader PR #3317 (Uri Tauber <uritaube@gmail.com>). Theirs calls
  // FontCacheManager::releaseSdFontCaches(); ours is named clearCache() and does the same two
  // things -- drop the decompressor cache, then clear every SdCardFont.
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->clearCache();
  }
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Opened over the reader, so in the book's orientation: measured, like the header in render(),
  // from the content area's top, which the hint strip holds in PortraitInverted.
  const Rect rect = UITheme::getBodyRect(renderer, true, false);
  return {rect.x + SIDE_PADDING, rect.y, rect.width - 2 * SIDE_PADDING, rect.height - metrics.verticalSpacing};
}

void DictionaryDefinitionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Confirm is the only button page navigation does not already own (next and
  // previous each take two), so it cycles forward through the installed
  // dictionaries. Forward only: with two installed that is both directions
  // anyway, and with more it still reaches every one.
  //
  // The switch itself belongs to the overlay above, which owns the open
  // Dictionary and knows the word -- this just asks, and closes.
  if (canSwitchDictionary() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(DictionarySwitchResult{1});
    finish();
    return;
  }

  // A definition long enough to page turns by tapping the two halves of its text, the way a book
  // page does. Bounded to the body rectangle rather than the reader's full-height zones: this
  // screen draws a hint strip, and full-height zones would claim the taps meant for it.
  //
  // Only while there is somewhere to go, so a one-page entry leaves every tap to the strip.
  int tapX = 0;
  int tapY = 0;
  if (body.pageCount() > 1 && mappedInput.hasTouch() && mappedInput.wasScreenTapped(tapX, tapY)) {
    const BodyArea area = bodyArea();
    switch (TapZones::halfOfBand(tapX, tapY, area.x, area.y, area.width, area.height)) {
      case TapZones::Half::Previous:
        previousPage();
        return;
      case TapZones::Half::Next:
        nextPage();
        return;
      case TapZones::Half::None:
        break;
    }
  }

  buttonNavigator.onNext([this] { nextPage(); });
  buttonNavigator.onPrevious([this] { previousPage(); });
}

void DictionaryDefinitionActivity::nextPage() {
  if (currentPage + 1 < body.pageCount()) {
    currentPage++;
    requestUpdate();
  }
}

void DictionaryDefinitionActivity::previousPage() {
  if (currentPage > 0) {
    currentPage--;
    requestUpdate();
  }
}

// Power set to turn pages turned them in the book but did nothing here: a reader-scoped action
// reaches no screen but the reader, and the raw Power press that falls through instead is one this
// screen has no use for. Taken only from Power -- the front and side buttons already page here, and
// taking their configured page action as well would turn twice per press.
//
// Based on crosspoint-reader PR #3361 (Amin Alaee, @aminalaee), reworked to take the configured
// action through the shared dispatch instead of reading the Power setting here.
bool DictionaryDefinitionActivity::takesButtonAction(const CrossPointSettings::BUTTON_ACTION action,
                                                     const MappedInputManager::Button button) const {
  using BA = CrossPointSettings::BUTTON_ACTION;
  return button == MappedInputManager::Button::Power && (action == BA::BTN_PAGE_FORWARD || action == BA::BTN_PAGE_BACK);
}

void DictionaryDefinitionActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
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

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  // Header: matched headword left, page counter right. A CJK lookup has no headword here: its
  // body opens on the headword lines.
  const Rect header = UITheme::getHeaderRect(renderer);
  GUI.drawHeader(renderer, header, headword.empty() ? nullptr : headword.c_str());

  // Right of the header: which dictionary this came from, and which page of it.
  // The name only appears when there is more than one installed -- otherwise it
  // is noise, since it can neither change nor be chosen between.
  const int totalPages = body.pageCount();
  char status[64];
  if (canSwitchDictionary() && totalPages > 1) {
    snprintf(status, sizeof(status), "%s  %d/%d", dictionaryName.c_str(), currentPage + 1, totalPages);
  } else if (canSwitchDictionary()) {
    snprintf(status, sizeof(status), "%s", dictionaryName.c_str());
  } else if (totalPages > 1) {
    snprintf(status, sizeof(status), "%d/%d", currentPage + 1, totalPages);
  } else {
    status[0] = '\0';
  }
  if (status[0] != '\0') {
    // A long dictionary name would run under the headword; drop leading
    // characters until it fits rather than overprinting.
    const int available = contentRect.width / 2;
    const char* shown = status;
    while (*shown != '\0' && renderer.getTextWidth(UI_10_FONT_ID, shown) > available) shown++;
    const int statusWidth = renderer.getTextWidth(UI_10_FONT_ID, shown);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + contentRect.width - SIDE_PADDING - statusWidth,
                      header.y + header.height / 2, shown);
  }

  if (layoutFailed) {
    // No body to page through: the header stays above the message, and Back
    // (or Confirm, to try another dictionary) still answers below it.
    GUI.drawPopup(renderer, tr(STR_DICT_LOW_MEMORY), /*overlayDisplayedFrame=*/false, PopupShip::Caller);
  } else {
    // Body: two-pass draw inside a prewarm scope (the same pattern the reader's
    // page render uses) so SD-card font glyphs load from the card in one batch
    // instead of one on-demand read per character on every page turn.
    // The same rectangle the tap-to-page split is resolved against, so what is drawn and what
    // answers a finger cannot drift apart.
    const BodyArea area = bodyArea();
    auto* fcm = renderer.getFontCacheManager();
    auto scope = fcm->createPrewarmScope();
    body.draw(renderer, currentPage, area.x, area.y);  // scan pass: records codepoints only
    scope.endScanAndPrewarm();
    body.draw(renderer, currentPage, area.x, area.y);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canSwitchDictionary() ? tr(STR_DICTIONARY) : "",
                                            (currentPage > 0 ? "<" : ""), (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
