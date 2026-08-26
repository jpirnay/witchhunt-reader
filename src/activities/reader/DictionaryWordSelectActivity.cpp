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
#include <cstring>
#include <variant>

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

bool DictionaryWordSelectActivity::ensureDictionaryOpen() {
  if (dictOpenAttempted) return dictOpenOk;
  dictOpenAttempted = true;
  if (SETTINGS.dictionaryName[0] == '\0') return false;
  dictOpenOk = dict.open(SETTINGS.dictionaryName);
  // needsIndex() opens and validates the .qidx sidecar, so ask it once per open
  // rather than once per word: the answer only changes when we build the
  // sidecar ourselves, which only performLookup() does.
  dictNeedsIndex = dictOpenOk && dict.needsIndex();
  return dictOpenOk;
}

// Runs from loop() once the cursor has been still long enough, so the search is
// paid for while the user is still reading the page rather than after they
// commit. Deliberately does NOT build the index: that pass takes seconds on a
// large dictionary and belongs behind Confirm's popup, not in an idle tick.
void DictionaryWordSelectActivity::speculateForSelection() {
  if (speculationDisabled || popup != Popup::None || words.empty()) return;
  if (speculationIdx == selected) return;  // already resolved for this word
  if (millis() - lastMoveMs < SPECULATIVE_DEBOUNCE_MS) return;

  if (!ensureDictionaryOpen() || dictNeedsIndex) {
    speculationDisabled = true;
    return;
  }

  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  const bool found = dict.resolve(words[selected].text, speculationLocation, speculationHeadword, &result);
  speculationIdx = selected;
  if (found) {
    speculation = Speculation::Found;
  } else if (result == Dictionary::LookupResult::NotFound) {
    speculation = Speculation::Missing;
  } else {
    // The search did not reach a verdict (IO error). Say nothing rather than
    // marking a word absent, and let Confirm report the real failure.
    speculation = Speculation::Unknown;
  }
  // Only the highlight style changes, and only when the answer is Missing.
  if (speculation == Speculation::Missing) {
    speculationRepaintPending = true;
    requestUpdate();
  }
}

const std::vector<DictionaryEntry>& DictionaryWordSelectActivity::installedDictionaries() {
  if (!dictionariesDiscovered) {
    dictionariesDiscovered = true;
    DictionaryRegistry::discover(dictionaries);
  }
  return dictionaries;
}

void DictionaryWordSelectActivity::switchDictionary(const int direction) {
  const auto& list = installedDictionaries();
  if (list.size() < 2) return;

  int current = -1;
  for (size_t i = 0; i < list.size(); i++) {
    if (list[i].name == SETTINGS.dictionaryName) {
      current = static_cast<int>(i);
      break;
    }
  }
  // An unknown current entry (the setting names a folder no longer on the card)
  // starts the cycle at the beginning rather than refusing to move.
  const int count = static_cast<int>(list.size());
  const int next = (current < 0) ? 0 : ((current + direction) % count + count) % count;

  strncpy(SETTINGS.dictionaryName, list[next].name.c_str(), sizeof(SETTINGS.dictionaryName) - 1);
  SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
  SETTINGS.saveToFile();

  // Force the next lookup to open the new dictionary, and drop everything that
  // described the old one.
  dictOpenAttempted = false;
  dictOpenOk = false;
  dictNeedsIndex = false;
  dict.releaseCaches();
  speculation = Speculation::Unknown;
  speculationIdx = -1;
  speculationDisabled = false;

  performLookup();
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
  ensureDictionaryOpen();
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
  bool found = false;
  if (ok && speculationIdx == selected && speculation != Speculation::Unknown) {
    // The index search already ran while the cursor sat here. A hit only has to
    // read the definition; a known miss needs no SD access at all.
    if (speculation == Speculation::Found) {
      headword = speculationHeadword;
      found = dict.readResolved(speculationLocation, definition, &result);
    } else {
      result = Dictionary::LookupResult::NotFound;
    }
  } else {
    found = ok && dict.lookup(words[selected].text, definition, headword, &result);
  }

  if (found) {
    popup = Popup::None;
    const bool html = dict.definitionsAreHtml();
    // The definition viewer lays the text out and may take the styled path
    // through the chapter parser, which wants every contiguous byte it can get.
    // The session buffers are no use while it is on top, so hand them back.
    dict.releaseCaches();
    // Name the dictionary for the viewer only when there is another to switch
    // to; that is what turns its Confirm button on.
    const bool multiple = installedDictionaries().size() > 1;
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                               renderer, mappedInput, std::move(headword), std::move(definition), html,
                               multiple ? std::string(SETTINGS.dictionaryName) : std::string()),
                           [this](const ActivityResult& result) {
                             // The child overdrew the page; the snapshot no longer matches.
                             snapshotIdx = -1;
                             if (const auto* switchRequest = std::get_if<DictionarySwitchResult>(&result.data)) {
                               pendingDictionarySwitch = switchRequest->direction;
                             }
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
  // Acted on here rather than in the result handler that set it: that handler
  // runs while the activity stack is being popped, and starting another
  // activity from inside it would re-enter the manager mid-transition.
  if (pendingDictionarySwitch != 0) {
    const int direction = pendingDictionarySwitch;
    pendingDictionarySwitch = 0;
    switchDictionary(direction);
    return;
  }

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
  const int before = selected;
  // The cursor travels over the page as the reader sees it, so it follows the logical directions:
  // whichever button pair lies across the screen picks the neighbouring word, and whichever runs
  // up and down it changes line.
  if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Left) && selected > 0) {
    selected--;
    requestUpdate();
  } else if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Right) && hasNextWord) {
    selected++;
    requestUpdate();
  } else if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Up)) {
    moveVertical(-1);
  } else if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Down)) {
    moveVertical(1);
  }
  // Restart the debounce on every move, so holding a direction down resolves
  // once at the end instead of at every word passed through.
  if (selected != before) lastMoveMs = millis();

  speculateForSelection();
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

  // A word the speculative resolve has already proved absent gets an outline
  // instead of the inverse fill: the cursor is still obviously here, but the
  // reader can see there is nothing to look up without pressing Confirm and
  // waiting for a "Not found" popup. Anything not yet resolved keeps the plain
  // highlight -- absence is only ever claimed on a verdict.
  const bool known_missing = speculationIdx == selected && speculation == Speculation::Missing;
  if (known_missing) {
    renderer.drawRect(hx, hy, hw, hh, true);
  } else {
    renderer.fillRect(hx, hy, hw, hh, true);
  }
  const bool inkBlack = known_missing;
  if (word.scale == 1.0f) {
    renderer.drawText(word.fontId, word.x, word.y, word.text, inkBlack, word.style);
  } else {
    renderer.drawTextScaled(word.fontId, word.x, word.y, word.text, inkBlack, word.style, word.scale);
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
  // The cursor moves in all four directions; the front strip carries two of them and the side
  // buttons the other two, so the arrows have to be routed rather than fixed.
  const auto labels =
      mappedInput
          .mapHints(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
          .front;
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path: only the highlight moved and the framebuffer still
  // holds a clean page (no popup or sub-activity since the last full repaint).
  // Restore the pixels under the old highlight, draw the new one, and push --
  // skipping the two-pass page render entirely.
  // `selected != snapshotIdx` is the cursor-moved case; the snapshot is also
  // stale when the cursor stayed put and only the speculation verdict changed,
  // which is what repaints a word as absent a moment after landing on it.
  const bool highlightMoved = !words.empty() && (selected != snapshotIdx || speculationRepaintPending);
  if (popup == Popup::None && snapshotIdx >= 0 && highlightMoved) {
    // displayBuffer() ends with a buffer swap, so the write framebuffer holds
    // the frame from TWO refreshes ago -- the reader menu the overlay was
    // opened from, or an older cursor position. Patching two word boxes into
    // that and re-displaying it ships the stale frame back to the panel, which
    // is why the menu reappeared and old highlights piled up. Resync to what is
    // actually on screen first; the saved pixels below were read from that same
    // frame, so the restore only lines up after this.
    renderer.syncWriteBufferFromDisplayed();
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // Batch-load just the highlighted word's glyphs before drawing them
    // white-on-black. clearCache() FIRST: prewarmCache() appends into the page
    // slots and deliberately never clears them -- that clear normally happens
    // once per page inside endScanAndPrewarm(), which this path skips. Without
    // it every cursor move took another ~4-5 KB slot and never gave it back,
    // walking the heap down from ~60 KB to ~14 KB in a dozen moves and making
    // the first lookup fail with "Low heap for N byte definition".
    //
    // Dropping the page's warmed glyphs costs this path nothing: it does not
    // re-render the page, only the two word boxes and the hints.
    auto* fcm = renderer.getFontCacheManager();
    fcm->clearCache();
    fcm->prewarmCache(words[selected].fontId, words[selected].text,
                      static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    speculationRepaintPending = false;
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

  speculationRepaintPending = false;
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
