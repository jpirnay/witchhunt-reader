#include "DictionaryWordSelectActivity.h"

#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <climits>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
bool isSelectableToken(const char* text) {
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) return true;
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) break;  // truncated sequence: skipping would step past the NUL
      p += 2;                // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

// Fed to the watchdog while the index pass streams a multi-megabyte .idx.
void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  // No null check: a failed allocation just disables the differential fast path
  // (drawHighlightWithSnapshot skips the read), leaving the full repaint.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  requestUpdate();
}

// Collect the selectable words and their screen boxes. Called from render()
// after the font cache has been prewarmed for this page: the boxes come from
// TextBlock::wordBox, which measures the text, and measuring an SD-card font
// before the prewarm loads every glyph from the card one overflow slot at a time.
void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  words.reserve(128);
  rowCount = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    bool rowHasWords = false;
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!isSelectableToken(text)) continue;

      const TextBlock::WordBox geometry =
          block->wordBox(renderer, i, fontId, line->xPos + marginLeft, line->yPos + marginTop);
      if (geometry.width <= 0 || geometry.height <= 0) continue;

      WordBox box;
      box.x = geometry.x;
      box.y = geometry.y;
      box.width = geometry.width;
      box.height = geometry.height;
      box.row = rowCount;
      box.text = text;
      box.fontId = geometry.fontId;
      box.style = geometry.style;
      box.scale = geometry.scale;
      words.push_back(box);
      rowHasWords = true;
    }
    if (rowHasWords) rowCount++;
  }

  wordsExtracted = true;
  // Start on the middle row's word nearest mid-screen instead of top-left: any
  // word on the page is then at most half a page of moves away.
  if (!words.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words.
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[selected];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != selected) {
    selected = best;
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::performLookup() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    popup = Popup::Error;
    popupMsg = StrId::STR_DICT_NO_DICT_SET;
    popupTime = millis();
    requestUpdate();
    return;
  }

  popup = Popup::Busy;
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
    // needsIndex() opens and validates the .qidx sidecar, so ask it once per
    // open rather than once per word: the answer only changes when we build the
    // sidecar ourselves, which is handled below.
    dictNeedsIndex = dictOpenOk && dict.needsIndex();
  }
  popupMsg = dictNeedsIndex ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
  requestUpdateAndWait();  // paint the page + busy popup before blocking on SD

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    dictNeedsIndex = !ok;  // a successful build leaves the sidecar fresh; a failed one retries
  }

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  const bool found = ok && dict.lookup(words[selected].text, definition, headword, &result);

  if (found) {
    popup = Popup::None;
    const bool html = dict.definitionsAreHtml();
    // The definition viewer lays the text out and may take the styled path
    // through the chapter parser, which wants every contiguous byte it can get.
    // The session buffers are no use while it is on top, so hand them back.
    dict.releaseCaches();
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                          std::move(definition), html),
                           [this](const ActivityResult&) {
                             // The child overdrew the page; the snapshot no longer matches.
                             snapshotIdx = -1;
                             requestUpdate();
                           });
    return;
  }

  // Name the failure: a genuine miss is "Not found"; a word that WAS found but
  // couldn't be read is a real error -- and decompression, a low-memory
  // allocation and a generic read error are told apart.
  if (!ok) {
    popup = Popup::Error;
    // An index build allocates a scan buffer, so it fails the same way lookups
    // do on a fragmented heap -- name that rather than a generic error.
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        popupMsg = StrId::STR_DICT_ERROR;  // dict.open() failed, not the index
        break;
    }
  } else {
    switch (result) {
      case Dictionary::LookupResult::Decompress:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_DECOMPRESS_ERROR;
        break;
      case Dictionary::LookupResult::LowMemory:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::LookupResult::ReadError:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::LookupResult::NotFound:
      default:
        popup = Popup::NotFound;
        popupMsg = StrId::STR_DICT_NOT_FOUND;
        break;
    }
  }
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::loop() {
  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !words.empty()) {
    performLookup();
    return;
  }

  if (words.empty()) return;

  const bool hasNextWord = selected + 1 < static_cast<int>(words.size());
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) && selected > 0) {
    selected--;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) && hasNextWord) {
    selected++;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveVertical(1);
  }
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved (no
// buffer / oversize box) -- the highlight is drawn regardless, but the next
// cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  const WordBox& word = words[selected];
  // The box is the word's exact advance box, grown by 2px so the highlight has
  // a little air around the glyphs. Clamped to the panel so save, draw and
  // restore all use the same rectangle.
  int hx = word.x - 2;
  int hy = word.y - 2;
  int hw = word.width + 4;
  int hh = word.height + 4;
  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }
  if (hx + hw > renderer.getScreenWidth()) hw = renderer.getScreenWidth() - hx;
  if (hy + hh > renderer.getScreenHeight()) hh = renderer.getScreenHeight() - hy;

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  renderer.fillRect(hx, hy, hw, hh, true);
  if (word.scale == 1.0f) {
    renderer.drawText(word.fontId, word.x, word.y, word.text, false, word.style);
  } else {
    renderer.drawTextScaled(word.fontId, word.x, word.y, word.text, false, word.style, word.scale);
  }
  return saved;
}

// Front-button bar. Drawn last on every repaint path, including the
// differential highlight-only one, so it always ends up as the top layer even
// when a highlighted word's box falls under a hint's screen area. No
// side-button hints: the full-bleed reader page has no spare gutter for them,
// so a hint box there would hide text.
void DictionaryWordSelectActivity::drawHints() const {
  // No selectable word on this page: Confirm and navigation are all no-ops
  // (guarded by words.empty() in loop()), so only Back is hinted.
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path: only the highlight moved and the framebuffer still
  // holds a clean page (no popup or sub-activity since the last full repaint).
  // Restore the pixels under the old highlight, draw the new one, and push --
  // skipping the two-pass page render entirely.
  if (popup == Popup::None && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before drawing them white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        words[selected].fontId, words[selected].text,
        static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) -- fall through to a full repaint.
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card font glyphs
  // hit the in-RAM cache during the real draw -- and, on the first render, so
  // extractWords() measures against a warm cache instead of the card.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->renderTextOnly(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  if (!wordsExtracted) extractWords();
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!words.empty()) drawHighlightWithSnapshot();
  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer -- force the next render onto the full-repaint path.
    snapshotIdx = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
