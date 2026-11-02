#include "DictionaryWordSelectActivity.h"

#include <CooperativeAbort.h>
#include <Epub/PageFrameScope.h>
#include <Epub/TurnedFrame.h>
#include <Epub/VerticalDraw.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HeapFit.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <variant>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "util/SerialCommandGrammar.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
bool isSelectableToken(const char* text, const size_t length) {
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  for (size_t i = 0; i < length; i++) {
    if (p[i] < 0x80) {
      if (std::isalnum(p[i])) return true;
    } else if (p[i] == 0xE2 && i + 2 < length && (p[i + 1] == 0x80 || p[i + 1] == 0x81)) {
      i += 2;  // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

// Word bytes for the purpose of finding a dash that joins two words.
bool isWordByte(const uint8_t c) { return c >= 0x80 || std::isalnum(c) != 0; }

// Byte offset just past the next em or en dash (U+2014 / U+2013) that has a word on both sides,
// scanning from `from`; 0 when there is none.
//
// A dash written without spaces fuses two words into one layout token -- "swiftness-a",
// "truth-that" -- and the reader looking one of them up means one of them. This is the cut. The
// dash stays with the LEFT piece so the pieces tile the token exactly (no gap to position) and
// the dictionary's own edge-stripping takes it off the lookup.
// A token with no word content of its own that carries an em or en dash separates the words on
// either side of it exactly as a space does. Bionic reading produces these: it tokenizes
// "swiftness-a" into a word span, the dash, and another word span, so the dash arrives as its
// own token rather than inside one.
bool isDashSeparatorToken(const char* text, const size_t length) {
  if (isSelectableToken(text, length)) return false;
  const auto* b = reinterpret_cast<const uint8_t*>(text);
  for (size_t i = 0; i + 2 < length; i++) {
    if (b[i] == 0xE2 && b[i + 1] == 0x80 && (b[i + 2] == 0x93 || b[i + 2] == 0x94)) return true;
  }
  return false;
}

size_t nextDashSplit(const char* text, const size_t length, const size_t from) {
  const auto* b = reinterpret_cast<const uint8_t*>(text);
  for (size_t i = from; i + 3 < length; i++) {
    if (b[i] != 0xE2 || b[i + 1] != 0x80) continue;
    if (b[i + 2] != 0x93 && b[i + 2] != 0x94) continue;  // en dash, em dash
    if (i == 0 || !isWordByte(b[i - 1]) || !isWordByte(b[i + 3])) continue;
    return i + 3;
  }
  return 0;
}

// A word broken across two lines ends on an ASCII hyphen -- either the one layout inserted at
// the break, or the compound's own. A token that is nothing but a dash is not a broken word.
bool endsOnHyphen(const char* text, const size_t length) { return length >= 2 && text[length - 1] == '-'; }

// The other half of a broken word starts on a word character, never on punctuation.
bool startsOnWordByte(const char* text) {
  const auto first = static_cast<uint8_t>(text[0]);
  return first >= 0x80 || std::isalnum(first) != 0;
}

// Fed to the watchdog while the index pass streams a multi-megabyte .idx.
void indexBuildYield(void*) { vTaskDelay(1); }

// DrawStyle::flags: how a vertical line draws a piece (VerticalDraw.h), as TextBlock::render draws it.
constexpr uint8_t DRAW_VERTICAL = 0x01;       // down its column
constexpr uint8_t DRAW_CENTRED_MARKS = 0x02;  // the line's 、。 centred in their cells (Traditional Chinese)
constexpr uint8_t DRAW_COMBINED = 0x04;       // the word set upright as one block (tate-chu-yoko)

// The flags of a piece of `word`, from its line and from the word.
uint8_t drawFlags(const TextBlock& block, const uint16_t word) {
  if (!block.isVertical()) return 0;
  return static_cast<uint8_t>(DRAW_VERTICAL | (block.centredMarks() ? DRAW_CENTRED_MARKS : 0) |
                              (block.isCombined(word) ? DRAW_COMBINED : 0));
}

// Where the page's lines are measured from: the reader's margins, or on a vertical page the turned frame's origin, the
// one Page::render draws them from (PageFrameScope).
turnedframe::Offsets pageOrigin(const GfxRenderer& renderer, const Page& page, const int marginLeft,
                                const int marginTop) {
  if (!page.isVertical()) return {marginLeft, marginTop};
  const int readerWidth = renderer.getScreenWidth(renderer.getHeldOrientation());
  return turnedframe::contentOffsets(readerWidth, marginLeft, marginTop, page.verticalBlockExtent);
}

// How far a piece cut out of a token runs along its line: its width, or down the column on a vertical line.
int pieceWidth(const GfxRenderer& renderer, const TextBlock::WordBox& geometry, const char* piece,
               const bool vertical) {
  if (vertical) return verticalTextAdvance(renderer, geometry.fontId, piece, geometry.style, geometry.scale);
  return (geometry.scale == 1.0f) ? renderer.getTextWidth(geometry.fontId, piece, geometry.style)
                                  : renderer.getTextWidthScaled(geometry.fontId, piece, geometry.style, geometry.scale);
}

// How far the pen moves past that piece: its advance, not its ink width. A vertical line's run down the column is both.
int pieceAdvance(const GfxRenderer& renderer, const TextBlock::WordBox& geometry, const char* piece,
                 const bool vertical) {
  if (vertical) return verticalTextAdvance(renderer, geometry.fontId, piece, geometry.style, geometry.scale);
  const int advance = renderer.getTextAdvanceX(geometry.fontId, piece, geometry.style);
  return (geometry.scale == 1.0f) ? advance : static_cast<int>(advance * geometry.scale + 0.5f);
}

// The middle of a row, where the first selection starts: across the screen, or down it on a vertical page, whose rows
// are columns (a content x is a reader y).
int rowMiddle(const GfxRenderer& renderer, const bool vertical) {
  return vertical ? renderer.getScreenHeight(renderer.getHeldOrientation()) / 2 : renderer.getScreenWidth() / 2;
}

// The logical directions that move the cursor. A vertical page reads down the column and on into the next one, which
// lies to the left: Up and Down step through the words, Left goes to the next column and Right to the previous one.
struct CursorKeys {
  MappedInputManager::Direction previousWord;
  MappedInputManager::Direction nextWord;
  MappedInputManager::Direction previousRow;
  MappedInputManager::Direction nextRow;
};

CursorKeys cursorKeys(const bool vertical) {
  using Direction = MappedInputManager::Direction;
  if (vertical) return {Direction::Up, Direction::Down, Direction::Right, Direction::Left};
  return {Direction::Left, Direction::Right, Direction::Up, Direction::Down};
}

// The fragments' point under a reader point: the point itself, or on a vertical page the turned frame's.
turnedframe::Point contentPoint(const bool vertical, const int readerWidth, const int x, const int y) {
  return vertical ? turnedframe::toContentPoint(readerWidth, x, y) : turnedframe::Point{x, y};
}

// The reader's rectangle over a box of fragments: the box itself, or on a vertical page the turned frame's box turned
// back.
turnedframe::Rect readerBox(const bool vertical, const int readerWidth, const turnedframe::Rect box) {
  return vertical ? turnedframe::toReader(readerWidth, box) : box;
}

// Draws a piece as the page draws it, in `black` or white. Called in the page's frame: on a vertical line (x, y) is the
// pen down the column and the line's top, as drawTextVertical takes them.
void drawPiece(const GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text,
               const EpdFontFamily::Style style, const float scale, const uint8_t flags, const bool black) {
  if ((flags & DRAW_COMBINED) != 0) {
    drawCombinedVertical(renderer, fontId, x, y, text, style, scale, black);
  } else if ((flags & DRAW_VERTICAL) != 0) {
    drawTextVertical(renderer, fontId, x, y, text, style, scale, (flags & DRAW_CENTRED_MARKS) != 0, black);
  } else if (scale == 1.0f) {
    renderer.drawText(fontId, x, y, text, black, style);
  } else {
    renderer.drawTextScaled(fontId, x, y, text, black, style, scale);
  }
}

// A vertical piece measures its column against 一 in its own style (VerticalDraw.cpp), which the page's scan records
// beside the text when the font has it. Its UTF-8, for the highlight's prewarm to load too; "" when not needed.
const char* columnEmText(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style,
                         const uint8_t flags) {
  constexpr uint32_t IDEOGRAPH_ONE = 0x4E00;
  if ((flags & DRAW_VERTICAL) == 0 || !renderer.hasGlyph(fontId, IDEOGRAPH_ONE, style)) return "";
  return "\xE4\xB8\x80";
}

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void DictionaryWordSelectActivity::releaseWordArrays() {
  // Swapped with empty vectors, not clear()ed: clear() keeps the capacity, which is the whole
  // block this exists to give back.
  std::vector<Fragment>().swap(fragments);
  std::vector<Word>().swap(words);
  std::vector<DrawStyle>().swap(drawStyles);
  snapshot.reset();
  snapshotIdx = -1;
  snapshotRunCount = 0;
  wordsExtracted = false;
}

// Collect the selectable words and their screen boxes. Called from render()
// after the font cache has been prewarmed for this page: the boxes come from
// TextBlock::wordBox, which measures the text, and measuring an SD-card font
// before the prewarm loads every glyph from the card one overflow slot at a time.
// Index of `geometry`'s (font, scale, line height) and `flags` in the side table, adding it if new. The
// table has one or two entries on a normal page, so the linear scan never runs long; the 255th
// distinct style reuses the last slot rather than overflowing the one-byte index.
uint8_t DictionaryWordSelectActivity::internDrawStyle(const TextBlock::WordBox& geometry, const uint8_t flags) {
  for (size_t i = 0; i < drawStyles.size(); i++) {
    if (drawStyles[i].fontId == geometry.fontId && drawStyles[i].scale == geometry.scale &&
        drawStyles[i].height == geometry.height && drawStyles[i].flags == flags) {
      return static_cast<uint8_t>(i);
    }
  }
  if (drawStyles.size() >= UINT8_MAX) return static_cast<uint8_t>(drawStyles.size() - 1);
  drawStyles.push_back({geometry.scale, geometry.fontId, geometry.height, flags});
  return static_cast<uint8_t>(drawStyles.size() - 1);
}

size_t DictionaryWordSelectActivity::countPageTokens(size_t& wordEstimate) const {
  size_t tokens = 0;
  wordEstimate = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine*>(element.get())->getBlock();
    if (!block || !block->valid()) continue;
    const uint16_t count = block->wordCount();
    tokens += count;
    // One group per token that does not continue the one before it. Reading the continuation
    // bits costs a byte each -- no text scan, no measurement.
    for (uint16_t i = 0; i < count; i++) {
      if (i == 0 || !block->wordContinues(i)) wordEstimate++;
    }
  }
  return tokens;
}

void DictionaryWordSelectActivity::extractWords() {
  const unsigned long startMs = millis();
  const bool rebuild = wordsBuiltOnce;
  // Reserve once, up front, from a real count. A dense page runs to ~280 tokens and bionic
  // reading roughly doubles that, so a fixed guess is either far too small -- and then the
  // vector doubles repeatedly, each growth holding the old block alongside the new -- or far too
  // large. Cutting a token at an em dash adds a piece here and there, hence the small slack.
  size_t wordEstimate = 0;
  const size_t tokenCount = countPageTokens(wordEstimate);
  const size_t fragmentCapacity = tokenCount + tokenCount / 16 + 4;
  const size_t wordCapacity = wordEstimate + wordEstimate / 16 + 4;
  drawStyles.clear();
  drawStyles.reserve(4);
  fragments.clear();
  words.clear();
  rowCount = 0;
  // Each array is one contiguous block, and a failed reserve aborts the device. A CJK page is one
  // token per character -- 600 and more on a dense page -- and its glyphs fill the font cache with
  // ~29 KB of bitmaps (X3, Hong Lou Meng at 14pt), leaving no block over 4-7 KB. The page is on
  // screen by now, so that cache can go: glyphs load on demand afterwards, which slows only this
  // fallback. If the arrays still do not fit, nothing is selectable and the popup says why.
  const size_t fragmentBytes = fragmentCapacity * sizeof(Fragment);
  const size_t wordBytes = wordCapacity * sizeof(Word);
  // Larger first; the words are checked against what the fragments left.
  const auto reserveArrays = [&] {
    if (!heapHasBlockFor(fragmentBytes)) return false;
    fragments.reserve(fragmentCapacity);
    if (!heapHasBlockFor(wordBytes)) {
      std::vector<Fragment>().swap(fragments);
      return false;
    }
    words.reserve(wordCapacity);
    return true;
  };
  if (!reserveArrays()) {
    renderer.releaseFontCaches();
    LOG_INF("DICT", "Released the font caches for %u fragments / %u words", static_cast<unsigned>(tokenCount),
            static_cast<unsigned>(wordEstimate));
    if (!reserveArrays()) {
      LOG_ERR("DICT", "No block for %u fragments / %u words: word selection refused", static_cast<unsigned>(tokenCount),
              static_cast<unsigned>(wordEstimate));
      wordsExtracted = true;
      popup = Popup::Error;
      popupMsg = StrId::STR_DICT_LOW_MEMORY;
      popupTime = millis();
      return;
    }
  }

  // The boxes are in the frame the page draws in, so a vertical page's are the turned frame's.
  const turnedframe::Offsets origin = pageOrigin(renderer, *page, marginLeft, marginTop);
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;
    const bool vertical = block->isVertical();

    bool rowHasWords = false;
    bool openGroup = false;           // words.back() is a group still being extended
    bool openSelectable = false;      // ...and it holds at least one selectable token
    bool previousEmitted = false;     // the previous block word produced a fragment
    uint16_t fragmentSourceWord = 0;  // block word the piece being added came from

    // A group with nothing selectable in it -- a lone dash, a run of quotes -- is dropped.
    // Both it and its fragments are the tail of their vectors, so this is a pop, not a gap.
    auto closeGroup = [&] {
      if (!openGroup) return;
      if (openSelectable) {
        rowHasWords = true;
      } else {
        fragments.resize(words.back().firstFragment);
        words.pop_back();
      }
      openGroup = false;
      openSelectable = false;
    };

    // Adds one piece of a token. `mayGlue` is false for every piece after a dash cut: a dash
    // between two words is a word boundary even though layout kept the pair in one token.
    auto addFragment = [&](const char* text, const size_t length, const int x, const int width,
                           const TextBlock::WordBox& geometry, const uint8_t flags, const bool mayGlue) {
      const bool glued = mayGlue && openGroup && previousEmitted && fragmentSourceWord > 0 &&
                         block->wordContinues(fragmentSourceWord) && words.back().fragmentCount < UINT8_MAX;
      if (!glued) closeGroup();

      Fragment fragment;
      fragment.x = static_cast<int16_t>(x);
      fragment.y = geometry.y;
      fragment.width = static_cast<int16_t>(width);
      fragment.text = text;
      fragment.length = static_cast<uint16_t>(length);
      fragment.style = geometry.style;
      fragment.drawStyle = internDrawStyle(geometry, flags);
      fragments.push_back(fragment);
      previousEmitted = true;

      if (glued) {
        Word& word = words.back();
        word.fragmentCount++;
        word.width = static_cast<int16_t>(fragment.x + fragment.width - word.x);
      } else {
        Word word;
        word.firstFragment = static_cast<uint16_t>(fragments.size() - 1);
        word.fragmentCount = 1;
        word.hyphenFragment = NO_HYPHEN;
        word.row = rowCount;
        word.x = fragment.x;
        word.width = fragment.width;
        words.push_back(word);
        openGroup = true;
      }
      // Punctuation glued to a word rides along -- the dictionary strips a word's edges
      // anyway -- but it never makes a group selectable on its own.
      openSelectable = openSelectable || isSelectableToken(text, length);
    };

    for (uint16_t i = 0; i < block->wordCount(); i++) {
      fragmentSourceWord = i;
      const char* text = block->wordText(i);
      const size_t textLength = block->wordTextLen(i);
      const TextBlock::WordBox geometry =
          block->wordBox(renderer, i, fontId, line->xPos + origin.x, line->yPos + origin.y);
      // A token with no ink of its own -- the visible space that holds a no-break group
      // together -- is not part of any word, and it separates the words on either side of it.
      if (geometry.width <= 0 || geometry.height <= 0) {
        previousEmitted = false;
        continue;
      }
      // A bare dash between two words is a boundary, not a piece of either of them.
      if (isDashSeparatorToken(text, textLength)) {
        previousEmitted = false;
        continue;
      }

      const uint8_t flags = drawFlags(*block, i);
      // The common case: one whole token, measured once by wordBox and used in place.
      // Cutting needs a NUL-terminated copy to measure, so a token too long for the scratch
      // buffer is left whole -- those are never the dash-joined pairs this is for. Nor is a
      // combined word cut: it is one cell, drawn as one block.
      if ((flags & DRAW_COMBINED) != 0 || textLength >= WORD_TEXT_CAPACITY || nextDashSplit(text, textLength, 0) == 0) {
        addFragment(text, textLength, geometry.x, geometry.width, geometry, flags, /*mayGlue=*/true);
        continue;
      }

      size_t partStart = 0;
      int partX = geometry.x;
      bool firstPart = true;
      while (partStart < textLength) {
        size_t partEnd = nextDashSplit(text, textLength, partStart);
        if (partEnd == 0) partEnd = textLength;
        const size_t partLength = partEnd - partStart;

        char part[WORD_TEXT_CAPACITY];
        memcpy(part, text + partStart, partLength);
        part[partLength] = '\0';
        const int partWidth = pieceWidth(renderer, geometry, part, vertical);
        if (partWidth > 0) {
          addFragment(text + partStart, partLength, partX, partWidth, geometry, flags, firstPart);
          firstPart = false;
        }

        // The next piece starts where this one's pen ends.
        if (partEnd < textLength) partX += pieceAdvance(renderer, geometry, part, vertical);
        partStart = partEnd;
      }
    }
    closeGroup();
    if (rowHasWords) rowCount++;
  }

  joinHyphenatedWords();

  // After the two arrays, so they are the first claim on the heap. No null check: a failed
  // allocation just disables the differential fast path (drawHighlightWithSnapshot skips the
  // read), leaving the full repaint.
  if (!snapshot) snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);

  if (!words.empty()) {
    if (!rebuild) {
      // Start on the middle row's word nearest mid-screen instead of top-left: any
      // word on the page is then at most half a page of moves away.
      const int initial = closestInRow(rowCount / 2, rowMiddle(renderer, page->isVertical()));
      if (initial >= 0) selected = initial;
    } else if (selected >= static_cast<int>(words.size())) {
      // Same page, same metrics: the rebuild gives the same words, so this only guards a drift.
      selected = static_cast<int>(words.size()) - 1;
      speculation = Speculation::Unknown;
      speculationIdx = -1;
    }
  }
  wordsBuiltOnce = true;
  if (rebuild) {
    LOG_DBG("DICT", "Rebuilt %u words in %lu ms", static_cast<unsigned>(words.size()), millis() - startMs);
  }
  // Last: the render task reads this flag, and the loop task acts on the words once it is set.
  wordsExtracted = true;
}

void DictionaryWordSelectActivity::joinHyphenatedWords() {
  for (size_t i = 0; i + 1 < words.size(); i++) {
    Word& prefix = words[i];
    const Word& suffix = words[i + 1];
    // Words are in reading order, so a suffix one row down is by construction the last word of
    // its line meeting the first word of the next.
    if (suffix.row != prefix.row + 1) continue;
    // The fragments have to be adjacent for the joined word to stay one contiguous run.
    if (suffix.firstFragment != prefix.firstFragment + prefix.fragmentCount) continue;
    if (prefix.fragmentCount + suffix.fragmentCount > UINT8_MAX) continue;
    const Fragment& tail = fragments[suffix.firstFragment - 1];
    if (!endsOnHyphen(tail.text, tail.length)) continue;
    if (!startsOnWordByte(fragments[suffix.firstFragment].text)) continue;

    prefix.hyphenFragment = static_cast<uint8_t>(prefix.fragmentCount - 1);
    prefix.fragmentCount = static_cast<uint8_t>(prefix.fragmentCount + suffix.fragmentCount);
    words.erase(words.begin() + static_cast<long>(i) + 1);
  }
}

const char* DictionaryWordSelectActivity::fragmentText(const Fragment& fragment, char* buf, const size_t capacity) {
  if (fragment.text[fragment.length] == '\0') return fragment.text;  // whole token: no copy needed
  const size_t take = std::min(static_cast<size_t>(fragment.length), capacity - 1);
  memcpy(buf, fragment.text, take);
  buf[take] = '\0';
  return buf;
}

size_t DictionaryWordSelectActivity::buildWordText(const int index, char* buf, const size_t capacity,
                                                   const bool dropJoinHyphen) const {
  if (capacity == 0) return 0;
  const Word& word = words[index];
  size_t length = 0;
  for (uint8_t f = 0; f < word.fragmentCount && length + 1 < capacity; f++) {
    const Fragment& piece = fragments[word.firstFragment + f];
    const char* source = piece.text;
    size_t take = piece.length;
    if (dropJoinHyphen && f == word.hyphenFragment && take > 0 && source[take - 1] == '-') take--;
    take = std::min(take, capacity - 1 - length);
    memcpy(buf + length, source, take);
    length += take;
  }
  buf[length] = '\0';
  return length;
}

// Index of the word in `row` whose center along the row is closest to centerX;
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

int DictionaryWordSelectActivity::wordAt(const int px, const int py) const {
  for (int index = 0; index < static_cast<int>(words.size()); index++) {
    const Word& word = words[index];
    for (uint8_t f = 0; f < word.fragmentCount; f++) {
      const Fragment& piece = fragments[word.firstFragment + f];
      if (px < piece.x || px >= piece.x + piece.width) continue;
      if (py >= piece.y && py < piece.y + drawStyleOf(piece).height) return index;
    }
  }
  return -1;
}

DictionaryWordSelectActivity::WordTouch DictionaryWordSelectActivity::consumeWordTouch() {
  if (!mappedInput.hasTouch() || words.empty()) return WordTouch::None;

  int x = 0;
  int y = 0;
  int index = -1;
  // A touch is in the reader's frame; the boxes are in the page's.
  const bool vertical = page->isVertical();
  const int readerWidth = renderer.getScreenWidth(renderer.getHeldOrientation());
  const auto hit = [&](const int px, const int py) {
    const turnedframe::Point at = contentPoint(vertical, readerWidth, px, py);
    index = wordAt(at.x, at.y);
    return index >= 0;
  };

  // Claimed but not acted on: moving the cursor mid-contact would defeat the two-step below.
  if (mappedInput.wasScreenTouchDown(x, y) && hit(x, y)) return WordTouch::Consumed;
  if (!mappedInput.wasScreenTapped(x, y) || !hit(x, y)) return WordTouch::None;

  // The lit match is one target: a tap on any of its characters is a tap on the word it starts
  // from, which is what a lookup there reads (spec 5.1). A tap elsewhere moves the cursor.
  if (index > selected && index < selected + spanWordCount()) index = selected;

  switch (ListRowTap::apply(index, static_cast<int>(words.size()), selected)) {
    case ListRowTap::Result::Rejected:
      return WordTouch::Consumed;
    case ListRowTap::Result::Selected:
      return WordTouch::Moved;
    case ListRowTap::Result::Activate:
      return WordTouch::Activate;
  }
  return WordTouch::Consumed;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const Word& current = words[selected];
  const int centerX = current.x + current.width / 2;
  // Rows can be empty: joining a hyphenated word pulls the only word off the row that carried
  // its tail. Keep going rather than letting one such row wall the cursor in.
  for (int row = static_cast<int>(current.row) + direction; row >= 0 && row < static_cast<int>(rowCount);
       row += direction) {
    const int best = closestInRow(static_cast<uint16_t>(row), centerX);
    if (best < 0) continue;
    if (best != selected) {
      selected = best;
      requestUpdate();
    }
    return;
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

  if (scriptAt(selected) != CjkLookupRun::Script::Other) {
    // CJK text has no spaces to say where a word ends, so the run from here is searched for the
    // longest headword it starts with. The search gives way to a press between probes; the word
    // is then left unresolved, and the next idle tick searches it again once the cursor rests.
    CjkLookupRun run;
    buildRun(selected, run);
    Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
    if (!resolveRun(run, &CooperativeAbort::shouldAbortLongTask, result)) return;
  } else {
    char text[WORD_TEXT_CAPACITY];
    buildWordText(selected, text, sizeof(text), true);
    Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
    bool found = dict.resolve(text, speculationLocation, speculationHeadword, &result);
    // A word broken at a hyphen it already had ("US-Satellitensystems") is a real headword WITH
    // the hyphen, so the joined form gets a second chance before the word is called absent.
    if (!found && result == Dictionary::LookupResult::NotFound && isHyphenJoined(selected)) {
      buildWordText(selected, text, sizeof(text), false);
      found = dict.resolve(text, speculationLocation, speculationHeadword, &result);
    }
    Speculation verdict;
    if (found) {
      verdict = Speculation::Found;
    } else if (result == Dictionary::LookupResult::NotFound) {
      verdict = Speculation::Missing;
    } else {
      // The search did not reach a verdict (IO error). Say nothing rather than
      // marking a word absent, and let Confirm report the real failure.
      verdict = Speculation::Unknown;
    }
    setSpeculation(selected, verdict, 1);
  }
  // The verdict redraws the highlight only when it changes the box: an outline for a word known
  // to be absent, or the box grown over the words a CJK match runs on into. Either is this cursor
  // stop's second refresh (spec 4).
  if (speculation == Speculation::Missing || spanWordCount() > 1) {
    {
      RenderLock lock(*this);
      speculationRepaintPending = true;
    }
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::setSpeculation(const int index, const Speculation verdict, const uint8_t span) {
  RenderLock lock(*this);
  speculationIdx = index;
  speculation = verdict;
  speculationSpan = span;
}

int DictionaryWordSelectActivity::spanWordCount() const {
  return speculationIdx == selected && speculation == Speculation::Found ? speculationSpan : 1;
}

CjkLookupRun::Script DictionaryWordSelectActivity::scriptAt(const int index) const {
  char text[WORD_TEXT_CAPACITY];
  const size_t length = buildWordText(index, text, sizeof(text), true);
  return CjkLookupRun::scriptOf(text, length);
}

void DictionaryWordSelectActivity::buildRun(const int index, CjkLookupRun& run) const {
  char text[WORD_TEXT_CAPACITY];
  for (int i = index; i < static_cast<int>(words.size()); i++) {
    const size_t length = buildWordText(i, text, sizeof(text), true);
    if (!run.append(text, length)) return;
  }
}

bool DictionaryWordSelectActivity::resolveRun(const CjkLookupRun& run, const Dictionary::AbortCheck abortCheck,
                                              Dictionary::LookupResult& result) {
  // The search empties speculationMatch before its first probe, so the word the old speculation
  // described has lost its hits: it must not count as resolved if the cursor goes back to it.
  setSpeculation(-1, Speculation::Unknown, 1);
  const bool found = dict.resolvePrefixes(run.text(), speculationMatch, &result, abortCheck);
  if (speculationMatch.aborted) return false;
  Speculation verdict;
  if (found) {
    verdict = Speculation::Found;
  } else if (result == Dictionary::LookupResult::NotFound) {
    verdict = Speculation::Missing;
  } else {
    // No verdict (no scan buffers, an IO error): as on the Latin path, Confirm reports it.
    verdict = Speculation::Unknown;
  }
  // A match that ends inside a word covers that word whole.
  const uint8_t covered = found ? run.wordsCovering(speculationMatch.hits[0].runBytes) : 0;
  setSpeculation(selected, verdict, covered > 0 ? covered : 1);
  return true;
}

bool DictionaryWordSelectActivity::lookUpRun(const bool ok, std::string& definition, Dictionary::LookupResult& result) {
  CjkLookupRun run;
  buildRun(selected, run);
  result = Dictionary::LookupResult::NotFound;
  bool found = false;
  if (ok) {
    // Searched here only when the cursor did not rest long enough for a verdict. No abort: the
    // reader is waiting for this one. Recorded as the speculation, so the span is there on return.
    if (speculationIdx != selected || speculation == Speculation::Unknown) resolveRun(run, nullptr, result);
    // A known miss needs no SD access at all.
    if (speculation == Speculation::Found) found = dict.readPrefixDefinitions(speculationMatch, definition, &result);
  }
  Dictionary::logLookup(SETTINGS.dictionaryName, run.text(),
                        ok ? Dictionary::resultName(result) : "open_or_index_failed",
                        found ? speculationMatch.hits[0].headword.c_str() : nullptr);
  return found;
}

int DictionaryWordSelectActivity::definitionFontId(const CjkLookupRun::Script script) const {
  return script == CjkLookupRun::Script::Other ? DICTIONARY_FONT_ID : fontId;
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
  setSpeculation(-1, Speculation::Unknown, 1);
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

  // A found word is inflated through dictzip's 32 KB window (DictZip::Scratch), one contiguous
  // block taken at the session's first hit. On a CJK page the font cache holds most of the heap;
  // the page and the busy popup are drawn, so let the cache go -- glyphs load on demand after.
  if (!heapHasBlockFor(LOOKUP_CONTIGUOUS_BYTES)) {
    renderer.releaseFontCaches();
    LOG_INF("DICT", "Released the font caches for the lookup");
  }

  // The dictionary switch arrives here with the arrays released: the repaint above rebuilt them.
  // A refused rebuild left the popup extractWords() set and nothing to look up.
  if (words.empty()) {
    if (popup == Popup::Busy) popup = Popup::None;
    requestUpdate();
    return;
  }

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    dictNeedsIndex = !ok;  // a successful build leaves the sidecar fresh; a failed one retries
    // speculateForSelection() switched itself off for the missing index. With it built, the idle
    // search runs again without the screen being reopened, and a CJK span can grow.
    if (ok) speculationDisabled = false;
  }

  std::string definition;
  // Stays empty for a CJK lookup, whose definition opens on its headword lines instead.
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  bool found = false;
  // Not before the busy repaint: after a dictionary switch, that repaint is what rebuilt the words.
  const CjkLookupRun::Script script = scriptAt(selected);
  if (script != CjkLookupRun::Script::Other) {
    found = lookUpRun(ok, definition, result);
  } else {
    char text[WORD_TEXT_CAPACITY];
    buildWordText(selected, text, sizeof(text), true);
    if (ok && speculationIdx == selected && speculation != Speculation::Unknown) {
      // The index search already ran while the cursor sat here. A hit only has to
      // read the definition; a known miss needs no SD access at all.
      if (speculation == Speculation::Found) {
        headword = speculationHeadword;
        found = dict.readResolved(speculationLocation, definition, &result);
      } else {
        result = Dictionary::LookupResult::NotFound;
      }
    } else if (ok) {
      found = dict.lookup(text, definition, headword, &result);
      if (!found && result == Dictionary::LookupResult::NotFound && isHyphenJoined(selected)) {
        buildWordText(selected, text, sizeof(text), false);
        found = dict.lookup(text, definition, headword, &result);
      }
    }
    Dictionary::logLookup(SETTINGS.dictionaryName, text, ok ? Dictionary::resultName(result) : "open_or_index_failed",
                          found ? headword.c_str() : nullptr);
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
    // The word arrays are the other big block held. The render task reads them, hence the lock,
    // and the release is the last step before the push: a render queued for this activity would
    // otherwise find them gone and rebuild them just before the definition opens. The push only
    // records a pending activity (ActivityManager::pushActivity), so it is safe under the lock.
    // The page is rebuilt from the same Page when the definition returns.
    RenderLock lock(*this);
    releaseWordArrays();
    startActivityForResult(
        std::make_unique<DictionaryDefinitionActivity>(
            renderer, mappedInput, std::move(headword), std::move(definition), html,
            multiple ? std::string(SETTINGS.dictionaryName) : std::string(), definitionFontId(script)),
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
  // Until the rebuild after a definition has been drawn there is nothing to move over or look up.
  if (!wordsExtracted || words.empty()) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    performLookup();
    return;
  }

  const bool hasNextWord = selected + 1 < static_cast<int>(words.size());
  const int before = selected;

  // A tap moves the cursor onto the word under the finger; a second tap on the word it is
  // already on looks that word up. Point-then-confirm, as everywhere else, and it earns its keep
  // here more than most: a lookup searches the dictionary index off the SD card and opens a
  // whole activity, while a mis-tap on a page of small print costs one more tap.
  //
  // Resolved before the buttons and, when it moves the cursor, deliberately falls THROUGH to the
  // shared tail below -- the debounce restart and speculateForSelection() are the same for a
  // cursor moved by a finger as for one moved by a button.
  const WordTouch touch = consumeWordTouch();
  if (touch == WordTouch::Activate) {
    performLookup();
    return;
  }
  if (touch == WordTouch::Moved) {
    requestUpdate();
  } else if (touch == WordTouch::None) {
    // The cursor travels over the page as the reader sees it, so it follows the logical
    // directions: whichever button pair lies along the line picks the neighbouring word, and
    // whichever runs across it changes line -- across the screen and up and down it, or on a
    // vertical page the other way round.
    const CursorKeys keys = cursorKeys(page->isVertical());
    if (mappedInput.wasLogicalPressed(keys.previousWord) && selected > 0) {
      selected--;
      requestUpdate();
    } else if (mappedInput.wasLogicalPressed(keys.nextWord) && hasNextWord) {
      selected++;
      requestUpdate();
    } else if (mappedInput.wasLogicalPressed(keys.previousRow)) {
      moveVertical(-1);
    } else if (mappedInput.wasLogicalPressed(keys.nextRow)) {
      moveVertical(1);
    }
  }
  // WordTouch::Consumed lands here having changed nothing, which is the point: a contact still
  // down claims the tap so nothing else reads it, and the tail is a no-op when the cursor has
  // not moved.
  //
  // Restart the debounce on every move, so holding a direction down resolves
  // once at the end instead of at every word passed through.
  if (selected != before) lastMoveMs = millis();

  speculateForSelection();
}

// Saves the pixels under the highlight, then draws it over words [selected, selected +
// spanWordCount()): the selected word, or the CJK match that starts on it. Returns false when
// the boxes could not all be saved (no buffer, more rows than MAX_SNAPSHOT_RUNS, more pixels
// than SNAPSHOT_CAPACITY) -- the highlight is drawn regardless, but the next cursor move must do
// a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  // `selected` read once, and the span clamped from it: the loop task moves the cursor without
  // the render lock, and the boxes saved here have to be the ones drawn.
  const int first = selected;
  const int spanWords = std::min(spanWordCount(), static_cast<int>(words.size()) - first);

  // A word the speculative resolve has already proved absent gets an outline instead of the
  // inverse fill: the cursor is still obviously here, but the reader can see there is nothing
  // to look up without pressing Confirm and waiting for a "Not found" popup. Anything not yet
  // resolved keeps the plain highlight -- absence is only ever claimed on a verdict.
  const bool knownMissing = speculationIdx == first && speculation == Speculation::Missing;

  // The boxes are saved, drawn and restored in the reader's frame; a vertical page's fragments are in the turned one.
  const bool vertical = page->isVertical();
  const int readerWidth = renderer.getScreenWidth(renderer.getHeldOrientation());

  snapshotRunCount = 0;
  size_t used = 0;
  bool saved = snapshot != nullptr;
  bool overflowed = false;  // a box found no run, or no room, left in the snapshot
  unsigned rows = 0;

  // One row's box over fragments [begin, end): `draw` false saves the pixels under it, true
  // draws it.
  const auto visitBox = [&](const uint16_t begin, const uint16_t end, const bool draw) {
    if (!draw) rows++;
    const Fragment& firstPiece = fragments[begin];
    int left = firstPiece.x;
    int top = firstPiece.y;
    int right = firstPiece.x + firstPiece.width;
    int bottom = firstPiece.y + drawStyleOf(firstPiece).height;
    for (uint16_t f = begin + 1; f < end; f++) {
      const Fragment& piece = fragments[f];
      left = std::min(left, static_cast<int>(piece.x));
      top = std::min(top, static_cast<int>(piece.y));
      right = std::max(right, piece.x + piece.width);
      bottom = std::max(bottom, piece.y + drawStyleOf(piece).height);
    }

    // The box is the row's exact advance box, in the reader's frame, grown by 2px so the highlight has
    // a little air around the glyphs. Clamped to the panel so save, draw and
    // restore all use the same rectangle.
    const turnedframe::Rect box = readerBox(vertical, readerWidth, {left, top, right - left, bottom - top});
    int hx = box.x - 2;
    int hy = box.y - 2;
    int hw = box.width + 4;
    int hh = box.height + 4;
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
    if (hw <= 0 || hh <= 0) return;

    if (!draw) {
      if (!saved) return;
      // Boxes share the one buffer back to back. One that finds no run or no room left is still
      // drawn, but gives up the differential path for the whole highlight rather than restoring
      // part of it.
      size_t written = 0;
      if (snapshotRunCount < MAX_SNAPSHOT_RUNS) {
        // cppcheck-suppress arithOperationsOnVoidPointer ; get() is uint8_t*, cppcheck
        // mis-deduces the concept-constrained makeUniqueNoThrow<uint8_t[]> return type
        written = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get() + used, SNAPSHOT_CAPACITY - used);
      }
      if (written == 0) {
        saved = false;
        overflowed = true;
        return;
      }
      snapshotRuns[snapshotRunCount] = {static_cast<int16_t>(hx), static_cast<int16_t>(hy), static_cast<int16_t>(hw),
                                        static_cast<int16_t>(hh), static_cast<uint16_t>(used)};
      snapshotRunCount++;
      used += written;
      return;
    }

    if (knownMissing) {
      renderer.drawRect(hx, hy, hw, hh, true);
    } else {
      renderer.fillRect(hx, hy, hw, hh, true);
    }
    // The text in the page's frame, the turned one on a vertical page: the scope switches to it and back. Its offsets
    // go unused; the fragments already hold where the page drew them.
    int frameX = marginLeft;
    int frameY = marginTop;
    const PageFrameScope frame(renderer, *page, frameX, frameY);
    for (uint16_t f = begin; f < end; f++) {
      const Fragment& piece = fragments[f];
      const DrawStyle& drawStyle = drawStyleOf(piece);
      char scratch[WORD_TEXT_CAPACITY];
      const char* text = fragmentText(piece, scratch, sizeof(scratch));
      drawPiece(renderer, drawStyle.fontId, piece.x, piece.y, text, piece.style, drawStyle.scale, drawStyle.flags,
                knownMissing);
    }
  };

  // The span's fragments are one contiguous range of the array, in reading order, so a row's box
  // closes where the next piece sits on another row. A fragment is on its word's row, except the
  // pieces of a hyphen-joined word past its hyphen, which are on the row below.
  const auto forEachRow = [&](const bool draw) {
    int boxRow = -1;
    uint16_t boxBegin = 0;
    uint16_t boxEnd = 0;
    for (int w = first; w < first + spanWords; w++) {
      const Word& word = words[w];
      for (uint8_t f = 0; f < word.fragmentCount; f++) {
        const int row = word.row + (word.hyphenFragment != NO_HYPHEN && f > word.hyphenFragment ? 1 : 0);
        const auto index = static_cast<uint16_t>(word.firstFragment + f);
        if (row != boxRow) {
          if (boxRow >= 0) visitBox(boxBegin, boxEnd, draw);
          boxRow = row;
          boxBegin = index;
        }
        boxEnd = static_cast<uint16_t>(index + 1);
      }
    }
    if (boxRow >= 0) visitBox(boxBegin, boxEnd, draw);
  };

  // Every box is saved before any is drawn: where line spacing is tight, a row's box can overlap
  // the one above, and saving it after that one was drawn would put its ink back on the restore.
  forEachRow(false);
  forEachRow(true);

  if (overflowed) {
    LOG_DBG("DICT", "Highlight over %u words takes the full repaint (%u rows)", static_cast<unsigned>(spanWords), rows);
  }
  if (!saved) snapshotRunCount = 0;
  snapshotIdx = saved ? first : -1;
  return saved;
}

// One prewarm per font and style, with every fragment of the span drawn in it: an SD-card font
// rebuilds a style's cache on each call, so a call per fragment would leave only the last one's
// glyphs warm. The span is a few words, so the scans stay short.
void DictionaryWordSelectActivity::prewarmHighlight(FontCacheManager& fcm) const {
  const int first = selected;
  const int spanWords = std::min(spanWordCount(), static_cast<int>(words.size()) - first);
  const Word& last = words[first + spanWords - 1];
  const uint16_t begin = words[first].firstFragment;
  const auto end = static_cast<uint16_t>(last.firstFragment + last.fragmentCount);
  // The prewarm's style bit is the regular/bold/italic index; the flags above it (underline,
  // sub/superscript, small caps) draw from the same glyphs.
  const auto styleIndex = [](const Fragment& piece) { return static_cast<uint8_t>(piece.style) & 0x03; };
  const auto sameFace = [&](const Fragment& a, const Fragment& b) {
    return drawStyleOf(a).fontId == drawStyleOf(b).fontId && styleIndex(a) == styleIndex(b);
  };

  for (uint16_t i = begin; i < end; i++) {
    const Fragment& lead = fragments[i];
    bool loaded = false;  // by an earlier fragment in the same face
    for (uint16_t j = begin; j < i && !loaded; j++) loaded = sameFace(fragments[j], lead);
    if (loaded) continue;

    // A word's whole text, and far more than a CJK run's 12 code points; a face whose text is cut
    // short loads the rest of its glyphs on demand. A vertical face's column em goes first, so it is never cut.
    char text[WORD_TEXT_CAPACITY];
    const char* em = columnEmText(renderer, drawStyleOf(lead).fontId, lead.style, drawStyleOf(lead).flags);
    size_t length = strlen(em);
    memcpy(text, em, length);
    for (uint16_t j = i; j < end; j++) {
      const Fragment& piece = fragments[j];
      if (!sameFace(piece, lead)) continue;
      const size_t take = std::min(static_cast<size_t>(piece.length), sizeof(text) - 1 - length);
      memcpy(text + length, piece.text, take);
      length += take;
    }
    text[length] = '\0';
    fcm.prewarmCache(drawStyleOf(lead).fontId, text, static_cast<uint8_t>(1u << styleIndex(lead)));
  }
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
  // which is what repaints a word as absent, or grows the box over a CJK match,
  // a moment after landing on it.
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
    for (uint8_t run = 0; run < snapshotRunCount; run++) {
      const SnapshotRun& saved = snapshotRuns[run];
      // cppcheck-suppress arithOperationsOnVoidPointer ; get() is uint8_t*, cppcheck
      // mis-deduces the concept-constrained makeUniqueNoThrow<uint8_t[]> return type
      renderer.writeFramebufferRegion(saved.x, saved.y, saved.width, saved.height, snapshot.get() + saved.offset);
    }
    // Batch-load just the highlight's glyphs before drawing them
    // white-on-black. clearCache() FIRST: prewarmCache() appends into the page
    // slots and deliberately never clears them -- that clear normally happens
    // once per page inside endScanAndPrewarm(), which this path skips. Without
    // it every cursor move took another ~4-5 KB slot and never gave it back,
    // walking the heap down from ~60 KB to ~14 KB in a dozen moves and making
    // the first lookup fail with "Low heap for N byte definition".
    //
    // Dropping the page's warmed glyphs costs this path nothing: it does not
    // re-render the page, only the two highlights and the hints.
    auto* fcm = renderer.getFontCacheManager();
    fcm->clearCache();
    prewarmHighlight(*fcm);
    speculationRepaintPending = false;
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (a highlight over too many rows or pixels) -- fall through to a full repaint.
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card font glyphs
  // hit the in-RAM cache during the real draw -- and, on the first render, so
  // extractWords() measures against a warm cache instead of the card.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->renderTextOnly(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, marginLeft, marginTop);
  // After the draw: extraction may release the font caches to make room for its arrays.
  if (!wordsExtracted) extractWords();

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

#if SERIAL_TEST_API
void DictionaryWordSelectActivity::appendDebugState(std::string& out) const {
  using SerialCommandGrammar::appendField;
  if (!wordsExtracted || words.empty()) return;
  appendField(out, "sel", static_cast<long>(selected));
  char text[WORD_TEXT_CAPACITY];
  buildWordText(selected, text, sizeof(text), true);
  appendField(out, "label", text);
  // The speculation describes the word under the cursor only when it was made for it.
  const Speculation verdict = speculationIdx == selected ? speculation : Speculation::Unknown;
  const bool found = verdict == Speculation::Found;
  appendField(out, "spec", found ? "found" : (verdict == Speculation::Missing ? "missing" : "unknown"));
  appendField(out, "span", static_cast<long>(spanWordCount()));
  const char* match = "";
  if (found) {
    match = scriptAt(selected) != CjkLookupRun::Script::Other ? speculationMatch.hits[0].headword.c_str()
                                                              : speculationHeadword.c_str();
  }
  appendField(out, "match", match);
}
#endif  // SERIAL_TEST_API
