#include "TextBlock.h"

#include <BuildArena.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../TextEmphasis.h"
#include "../VerticalDraw.h"

bool TextBlock::guideDotsEnabled = false;

// 32-bit layout: vptr 4, RenderStyle 12, five scalars and a pad byte 8, arena 4, arenaOwned_ and annotationBytes_ 4,
// five views 20.
static_assert(sizeof(void*) != 4 || sizeof(TextBlock) == 52, "a resident page holds ~28 TextBlocks");

namespace {
// Style and continuation share one arena byte: bits 0-6 are the EpdFontFamily::Style, bit 7
// says the word is glued to its predecessor. See TextBlock::wordContinues.
constexpr size_t kAnnotationRecordBytes = 10;  // u16 firstWord, u16 wordCount, s16 x, u16 width, u8 kind, u8 len
constexpr size_t kAnnotationHeaderBytes = 2;   // u16 count

bool knownKind(const uint8_t kind) {
  return kind == static_cast<uint8_t>(TextBlock::AnnotationKind::Ruby) ||
         kind == static_cast<uint8_t>(TextBlock::AnnotationKind::Emphasis) ||
         kind == static_cast<uint8_t>(TextBlock::AnnotationKind::Combine);
}

// Whether one of the `count` records at `record` is drawn in the band above the line: a ruby or an emphasis run. A
// combined word is drawn in its own cell.
bool anyBandRecord(const uint8_t* record, const uint16_t count) {
  for (uint16_t j = 0; j < count; j++, record += kAnnotationRecordBytes) {
    if (record[8] != static_cast<uint8_t>(TextBlock::AnnotationKind::Combine)) return true;
  }
  return false;
}

// The word the next Combine record at or after record index `from` names (`from` is left past it); UINT16_MAX when
// none is left. Records are in word order, so one walk serves a whole line.
uint16_t nextCombinedWord(const uint8_t* records, const uint16_t count, uint16_t& from) {
  for (; from < count; from++) {
    const uint8_t* record = records + from * kAnnotationRecordBytes;
    if (record[8] != static_cast<uint8_t>(TextBlock::AnnotationKind::Combine)) continue;
    uint16_t firstWord = 0;
    memcpy(&firstWord, record, 2);
    from++;
    return firstWord;
  }
  return UINT16_MAX;
}

uint8_t packStyle(const EpdFontFamily::Style style, const bool continues) {
  return static_cast<uint8_t>((static_cast<uint8_t>(style) & ~TextBlock::WORD_CONTINUES_BIT) |
                              (continues ? TextBlock::WORD_CONTINUES_BIT : 0));
}
}  // namespace

TextBlock::ArenaOffsets TextBlock::arenaOffsets(const uint16_t wordCount, const bool hasSizes, const uint8_t fontCount,
                                                const uint16_t textBytes) {
  // Layout documented in TextBlock.h: the word fonts (4-byte fields) first, then the 16-bit
  // arrays (textOff, xpos), then the 8-bit arrays (styles, optional sizes and font slots), then
  // the text blob. Without word fonts textOff sits at 0.
  const size_t wc = wordCount;
  ArenaOffsets o{};
  o.textOff = fontCount * sizeof(WordFont);
  o.xpos = o.textOff + wc * sizeof(uint16_t);
  o.styles = o.xpos + wc * sizeof(int16_t);
  o.sizes = o.styles + wc * sizeof(uint8_t);  // only meaningful when hasSizes
  o.fontSlots = o.sizes + (hasSizes ? wc * sizeof(uint8_t) : 0);
  o.text = o.fontSlots + (fontCount != 0 ? wc * sizeof(uint8_t) : 0);
  o.annotations = o.text + textBytes;
  return o;
}

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasSizes, const uint8_t fontCount,
                            const uint16_t textBytes, const uint16_t annotationBytes) {
  return arenaOffsets(wordCount, hasSizes, fontCount, textBytes).annotations + annotationBytes;
}

bool TextBlock::allocArena(const size_t size, BuildArena* scratch) {
  if (scratch != nullptr) {
    // A line's word fonts (4-byte fields) come first, then the two 16-bit arrays: the base
    // needs 4-byte alignment.
    arena = static_cast<uint8_t*>(scratch->alloc(size, 4));
    if (arena != nullptr) {
      arenaOwned_ = false;
      return true;
    }
    // Region full (a page beyond what it was sized for): fall through to the heap for this
    // line alone. A mixed page is fine -- ownership is per block.
  }
  arena = new (std::nothrow) uint8_t[size];
  arenaOwned_ = arena != nullptr;
  return arena != nullptr;
}

void TextBlock::bindArenaPointers() {
  const uint8_t* base = arena;
  const ArenaOffsets o = arenaOffsets(numWords, sizesPresent, wordFontCount, textBytes);
  textOffArr = reinterpret_cast<const uint16_t*>(base + o.textOff);
  xposArr = reinterpret_cast<const int16_t*>(base + o.xpos);
  stylesArr = base + o.styles;
  sizesArr = sizesPresent ? base + o.sizes : nullptr;
  textArr = reinterpret_cast<const char*>(base + o.text);
}

TextBlock::TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle,
                     std::vector<uint8_t> word_sizes, const std::vector<bool>& word_continues)
    // Narrow the parser's full BlockStyle to the render-only slice: the spacing
    // fields have already been consumed into word_xpos / the line's y by layout.
    : renderStyle{blockStyle.fontSizeMultiplier, blockStyle.headingFontId, blockStyle.alignment} {
  // Normalize the all-100% case to no sizes so uniform lines (the common case)
  // keep the zero-cost fast paths in render/serialize/line-height.
  if (std::all_of(word_sizes.begin(), word_sizes.end(), [](const uint8_t s) { return s == 100; })) {
    word_sizes.clear();
  }
  const bool hasSizes = !word_sizes.empty();

  if (words.size() != word_xpos.size() || words.size() != word_styles.size() || words.size() > 10000 ||
      (hasSizes && words.size() != word_sizes.size())) {
    LOG_ERR("TXB", "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, sizes=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(word_xpos.size()),
            static_cast<uint32_t>(word_styles.size()), static_cast<uint32_t>(word_sizes.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  sizesPresent = hasSizes;
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // Pass 1: total text size, one NUL per word. A line is at most a physical row
  // of the page, so uint16_t offsets are ample; reject anything larger.
  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    sizesPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, sizesPresent, 0, textBytes);
  if (!allocArena(size, nullptr)) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    sizesPresent = false;
    isValid = false;
    return;
  }

  // Pass 2: fill through mutable pointers derived from the same layout offsets
  // that bindArenaPointers() uses for the const views (bound below for reads).
  uint8_t* base = arena;
  const ArenaOffsets o = arenaOffsets(numWords, sizesPresent, 0, textBytes);
  auto* textOff = reinterpret_cast<uint16_t*>(base);
  auto* xpos = reinterpret_cast<int16_t*>(base + o.xpos);
  uint8_t* styles = base + o.styles;
  char* text = reinterpret_cast<char*>(base + o.text);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = word_xpos[i];
    styles[i] = packStyle(word_styles[i], i < word_continues.size() && word_continues[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
  if (sizesPresent) {
    uint8_t* sizes = base + o.sizes;
    for (uint16_t i = 0; i < numWords; i++) sizes[i] = word_sizes[i];
  }
  bindArenaPointers();
}

TextBlock::TextBlock(const WordRange& range, const std::vector<int16_t>& word_xpos, const BlockStyle& blockStyle,
                     BuildArena* scratch)
    : renderStyle{blockStyle.fontSizeMultiplier, blockStyle.headingFontId, blockStyle.alignment} {
  if (range.words == nullptr || range.styles == nullptr) {
    LOG_ERR("TXB", "Construction failed: null word range");
    isValid = false;
    return;
  }
  const std::vector<std::string>& words = *range.words;
  const std::vector<EpdFontFamily::Style>& styleSrc = *range.styles;
  const size_t first = range.first;
  const size_t count = range.count;

  // Every source array must actually span [first, first + count); xpos is per line so it is
  // indexed from 0 and only needs `count` entries.
  const bool hasSizeSrc = range.sizes != nullptr && !range.sizes->empty();
  // Optional, and only used when it actually spans the range -- a caller that does not track
  // continuation (or hands over a short vector) just produces space-separated words.
  const std::vector<TokenBoundary::Boundary>* boundariesSrc =
      (range.boundaries != nullptr && first + count <= range.boundaries->size()) ? range.boundaries : nullptr;
  if (count > 10000 || first + count > words.size() || first + count > styleSrc.size() || word_xpos.size() != count ||
      (hasSizeSrc && first + count > range.sizes->size())) {
    LOG_ERR("TXB", "Construction failed: range out of bounds (first=%u, count=%u, words=%u, xpos=%u)",
            static_cast<uint32_t>(first), static_cast<uint32_t>(count), static_cast<uint32_t>(words.size()),
            static_cast<uint32_t>(word_xpos.size()));
    isValid = false;
    return;
  }

  // The boxes over the line's words: each must name words of the line, and the region must fit its u16 size.
  const std::vector<Annotation>* annotationSrc =
      (range.annotations != nullptr && !range.annotations->empty()) ? range.annotations : nullptr;
  size_t annotationBytes = 0;
  if (annotationSrc != nullptr) {
    annotationBytes = kAnnotationHeaderBytes;
    bool annotationsOk = annotationSrc->size() <= UINT16_MAX;
    for (const Annotation& a : *annotationSrc) {
      if (a.wordCount == 0 || static_cast<size_t>(a.firstWord) + a.wordCount > count ||
          a.text.size() > kMaxAnnotationText || !knownKind(static_cast<uint8_t>(a.kind))) {
        annotationsOk = false;
        break;
      }
      annotationBytes += kAnnotationRecordBytes + (a.kind == AnnotationKind::Ruby ? a.text.size() : 0);
    }
    if (!annotationsOk || annotationBytes > UINT16_MAX) {
      LOG_ERR("TXB", "Construction failed: bad annotations (count=%u, words=%u)",
              static_cast<uint32_t>(annotationSrc->size()), static_cast<uint32_t>(count));
      isValid = false;
      return;
    }
  }

  // Normalize the all-100% case to no sizes, matching the vector constructor so both paths
  // produce byte-identical arenas (and therefore identical section-cache bytes).
  bool anyNon100 = false;
  if (hasSizeSrc) {
    const std::vector<WordSize>& sizeSrc = *range.sizes;
    for (size_t i = 0; i < count && !anyNon100; i++) anyNon100 = sizeSrc[first + i].pct != 100;
  }
  sizesPresent = anyNon100;

  numWords = static_cast<uint16_t>(count);
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // The line's word fonts: the fonts of the range's table its words use, in the order they first
  // appear; a line whose words are all in the block's font has none, and its arena is as before.
  uint8_t lineSlot[kMaxWordFonts] = {};  // the range's slot -> the line's, 1-based; 0: the block's font
  uint8_t tableSlot[kMaxWordFonts];      // the line's slot -> the range's
  const bool hasFonts = hasSizeSrc && range.fontTable != nullptr;
  for (size_t i = 0; hasFonts && i < count; i++) {
    const uint8_t slot = (*range.sizes)[first + i].fontSlot % kMaxWordFonts;
    if (range.fontTable[slot].fontId == 0 || lineSlot[slot] != 0) continue;
    tableSlot[wordFontCount] = slot;
    lineSlot[slot] = ++wordFontCount;
  }

  // Pass 1: total text size, one NUL per word, soft hyphens excluded — they are stripped
  // during the copy below, so reserving their bytes would leave a gap between words.
  size_t totalText = 0;
  for (size_t i = 0; i < count; i++) {
    const std::string& w = words[first + i];
    size_t stripped = 0;
    for (size_t pos = w.find(SOFT_HYPHEN_UTF8); pos != std::string::npos;
         pos = w.find(SOFT_HYPHEN_UTF8, pos + SOFT_HYPHEN_BYTES)) {
      stripped += SOFT_HYPHEN_BYTES;
    }
    totalText += w.size() - stripped + 1;
  }
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    sizesPresent = false;
    wordFontCount = 0;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size =
      arenaSize(numWords, sizesPresent, wordFontCount, textBytes, static_cast<uint16_t>(annotationBytes));
  if (!allocArena(size, scratch)) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    sizesPresent = false;
    wordFontCount = 0;
    isValid = false;
    return;
  }

  // Pass 2: fill straight from the caller's arrays — no intermediate per-line vectors.
  uint8_t* base = arena;
  const ArenaOffsets o = arenaOffsets(numWords, sizesPresent, wordFontCount, textBytes);
  auto* textOff = reinterpret_cast<uint16_t*>(base + o.textOff);
  auto* xpos = reinterpret_cast<int16_t*>(base + o.xpos);
  uint8_t* styles = base + o.styles;
  char* text = reinterpret_cast<char*>(base + o.text);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = word_xpos[i];
    styles[i] = packStyle(styleSrc[first + i], boundariesSrc != nullptr && (*boundariesSrc)[first + i].continues);
    // Copy the word one soft-hyphen-free run at a time.
    const std::string& w = words[first + i];
    size_t start = 0;
    for (size_t pos = w.find(SOFT_HYPHEN_UTF8); pos != std::string::npos; pos = w.find(SOFT_HYPHEN_UTF8, start)) {
      const size_t run = pos - start;
      memcpy(text + off, w.data() + start, run);
      off += static_cast<uint16_t>(run);
      start = pos + SOFT_HYPHEN_BYTES;
    }
    const size_t tail = w.size() - start;
    memcpy(text + off, w.data() + start, tail);
    off += static_cast<uint16_t>(tail);
    text[off++] = '\0';
  }
  if (sizesPresent) {
    uint8_t* sizes = base + o.sizes;
    const std::vector<WordSize>& sizeSrc = *range.sizes;
    for (uint16_t i = 0; i < numWords; i++) sizes[i] = sizeSrc[first + i].pct;
  }
  if (wordFontCount != 0) {
    auto* fonts = reinterpret_cast<WordFont*>(base);
    for (uint8_t f = 0; f < wordFontCount; f++) fonts[f] = range.fontTable[tableSlot[f]];
    uint8_t* slots = base + o.fontSlots;
    for (uint16_t i = 0; i < numWords; i++) slots[i] = lineSlot[(*range.sizes)[first + i].fontSlot % kMaxWordFonts];
  }
  if (annotationSrc != nullptr) {
    uint8_t* region = base + o.annotations;
    const uint16_t n = static_cast<uint16_t>(annotationSrc->size());
    memcpy(region, &n, sizeof(n));
    uint8_t* record = region + kAnnotationHeaderBytes;
    uint8_t* texts = record + n * kAnnotationRecordBytes;
    for (const Annotation& a : *annotationSrc) {
      const int16_t ax = a.x;
      memcpy(record, &a.firstWord, 2);
      memcpy(record + 2, &a.wordCount, 2);
      memcpy(record + 4, &ax, 2);
      memcpy(record + 6, &a.width, 2);
      record[8] = static_cast<uint8_t>(a.kind);
      if (a.kind == AnnotationKind::Ruby) {
        record[9] = static_cast<uint8_t>(a.text.size());
        memcpy(texts, a.text.data(), a.text.size());
        texts += a.text.size();
      } else {
        record[9] = a.mark;
      }
      record += kAnnotationRecordBytes;
    }
    annotationBytes_ = static_cast<uint16_t>(annotationBytes);
    annotationBand_ = anyBandRecord(base + o.annotations + kAnnotationHeaderBytes, n);
  }
  bindArenaPointers();
}

uint8_t TextBlock::maxSizePct() const {
  if (!sizesPresent) {
    return 100;
  }
  uint8_t m = 0;
  for (uint16_t i = 0; i < numWords; i++) m = std::max(m, sizesArr[i]);
  return m;
}

uint16_t TextBlock::annotationCount() const {
  if (annotationBytes_ == 0) return 0;
  uint16_t count = 0;
  memcpy(&count, annotationRegion(), sizeof(count));
  return count;
}

TextBlock::Annotation TextBlock::annotation(const uint16_t i) const {
  Annotation a;
  const uint16_t count = annotationCount();
  if (i >= count) return a;
  const uint8_t* record = annotationRegion() + kAnnotationHeaderBytes;
  const uint8_t* text = record + count * kAnnotationRecordBytes;
  for (uint16_t j = 0; j < i; j++, record += kAnnotationRecordBytes) {
    if (record[8] == static_cast<uint8_t>(AnnotationKind::Ruby)) text += record[9];
  }
  memcpy(&a.firstWord, record, 2);
  memcpy(&a.wordCount, record + 2, 2);
  memcpy(&a.x, record + 4, 2);
  memcpy(&a.width, record + 6, 2);
  a.kind = static_cast<AnnotationKind>(record[8]);
  if (a.kind == AnnotationKind::Ruby) {
    a.text = std::string_view(reinterpret_cast<const char*>(text), record[9]);
  } else {
    a.mark = record[9];
  }
  return a;
}

bool TextBlock::isCombined(const uint16_t wordIdx) const {
  if (annotationBytes_ == 0) return false;
  const uint8_t* records = annotationRegion() + kAnnotationHeaderBytes;
  const uint16_t count = annotationCount();
  uint16_t from = 0;
  for (uint16_t word = nextCombinedWord(records, count, from); word != UINT16_MAX;
       word = nextCombinedWord(records, count, from)) {
    if (word == wordIdx) return true;
  }
  return false;
}

bool TextBlock::annotationsValid() const {
  if (annotationBytes_ < kAnnotationHeaderBytes + kAnnotationRecordBytes) return false;
  const uint16_t count = annotationCount();
  const size_t recordsEnd = kAnnotationHeaderBytes + static_cast<size_t>(count) * kAnnotationRecordBytes;
  if (count == 0 || recordsEnd > annotationBytes_) return false;
  const uint8_t* record = annotationRegion() + kAnnotationHeaderBytes;
  size_t textBytesSum = 0;
  for (uint16_t j = 0; j < count; j++, record += kAnnotationRecordBytes) {
    uint16_t firstWord = 0;
    uint16_t wordCountOf = 0;
    memcpy(&firstWord, record, 2);
    memcpy(&wordCountOf, record + 2, 2);
    if (wordCountOf == 0 || static_cast<size_t>(firstWord) + wordCountOf > numWords || !knownKind(record[8])) {
      return false;
    }
    if (record[8] == static_cast<uint8_t>(AnnotationKind::Ruby)) textBytesSum += record[9];
  }
  return recordsEnd + textBytesSum == annotationBytes_;
}

int TextBlock::annotationBandFor(const GfxRenderer& renderer, const int effFontId, const float blockScale,
                                 const uint8_t maxSizePct) {
  const int ascender = (maxSizePct == 100 && blockScale == 1.0f)
                           ? renderer.getFontAscenderSize(effFontId)
                           : renderer.getFontAscenderSizeScaled(effFontId, blockScale * maxSizePct / 100.0f);
  return (ascender + 1) / 2;  // the ruby's own ascender is round(ascender * 0.5)
}

int TextBlock::annotationBand(const GfxRenderer& renderer, const int fontId) const {
  if (!annotationBand_) return 0;
  const int effFontId = renderStyle.headingFontId != 0 ? renderStyle.headingFontId : fontId;
  return annotationBandFor(renderer, effFontId, renderStyle.fontSizeMultiplier, maxSizePct());
}

// A vertical line is never unscaled: the ink-only fast path has not been exercised across a mid-line frame switch,
// and a wrong fast-path page costs more than a missed pre-render.
bool TextBlock::drawsUnscaled() const {
  if (!isValid) return true;  // draws nothing
  if (renderStyle.fontSizeMultiplier != 1.0f || renderStyle.headingFontId != 0 || sizesPresent || hasAnnotations() ||
      vertical_) {
    return false;
  }
  constexpr uint8_t scaledStyles = EpdFontFamily::SUP | EpdFontFamily::SUB | EpdFontFamily::SMALL_CAPS;
  for (uint16_t i = 0; i < numWords; i++) {
    if (wordStyle(i) & scaledStyles) return false;
  }
  return true;
}

// Mirrors the per-word geometry in render() below. Kept adjacent to it on
// purpose: the two must move together.
TextBlock::WordBox TextBlock::wordBox(const GfxRenderer& renderer, const uint16_t i, const int fontId, const int x,
                                      const int y) const {
  WordBox box;
  if (!isValid || i >= numWords) return box;

  const int effFontId = renderStyle.headingFontId != 0 ? renderStyle.headingFontId : fontId;
  const float blockScale = renderStyle.fontSizeMultiplier;
  const int blockAscender = (blockScale == 1.0f) ? renderer.getFontAscenderSize(effFontId)
                                                 : renderer.getFontAscenderSizeScaled(effFontId, blockScale);
  int lineAscender = blockAscender;
  if (sizesPresent) {
    lineAscender = renderer.getFontAscenderSizeScaled(effFontId, blockScale * (maxSizePct() / 100.0f));
  }
  const int baseY = y + annotationBand(renderer, fontId);  // the ruby's band is above the base

  const char* word = wordText(i);
  const EpdFontFamily::Style style = wordStyle(i);
  const int font = wordFontId(i, effFontId);
  const float scale = wordScale(i);
  const int ascender =
      (font == effFontId && scale == blockScale) ? blockAscender : renderer.getFontAscenderSizeScaled(font, scale);

  int wordY = baseY + (lineAscender - ascender);
  if ((style & EpdFontFamily::SUP) != 0) {
    wordY -= blockAscender * 2 / 5;
  } else if ((style & EpdFontFamily::SUB) != 0) {
    wordY += blockAscender / 4;
  }

  box.fontId = font;
  box.style = style;
  box.scale = scale;
  box.x = static_cast<int16_t>(xposArr[i] + x);
  box.y = static_cast<int16_t>(wordY);
  // Scaled measurement, not getTextAdvanceX: a book with publisher font sizes
  // gives words a per-word scale, and an unscaled width drifts wider or
  // narrower than the glyphs actually drawn.
  // A combined word is one em down the column, however many characters it holds.
  box.width = static_cast<int16_t>(vertical_ && isCombined(i) ? columnEm(renderer, font, style, scale)
                                   : vertical_                ? verticalTextAdvance(renderer, font, word, style, scale)
                                   : (scale == 1.0f)          ? renderer.getTextWidth(font, word, style)
                                                              : renderer.getTextWidthScaled(font, word, style, scale));
  box.height =
      static_cast<int16_t>((scale == 1.0f) ? renderer.getLineHeight(font) : renderer.getLineHeightScaled(font, scale));
  return box;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }

  const bool scanning = renderer.isFontCacheScanning();
  // Heading blocks may render with a taller real font (headingFontId) instead of scaling the
  // body font. In that case fontSizeMultiplier is a small residual (usually 1.0). Resolve the
  // effective (fontId, scale) pair once and use it for every measure/draw below.
  const int effFontId = renderStyle.headingFontId != 0 ? renderStyle.headingFontId : fontId;
  const float blockScale = renderStyle.fontSizeMultiplier;
  const int blockAscender = (blockScale == 1.0f) ? renderer.getFontAscenderSize(effFontId)
                                                 : renderer.getFontAscenderSizeScaled(effFontId, blockScale);
  // Mixed-size lines are baseline-aligned: every word's baseline sits at y + lineAscender,
  // where lineAscender is the tallest word's ascender. Uniform lines (no sizes) keep
  // lineAscender == blockAscender, i.e. the historical single-scale layout.
  int lineAscender = blockAscender;
  if (sizesPresent) {
    lineAscender = renderer.getFontAscenderSizeScaled(effFontId, blockScale * (maxSizePct() / 100.0f));
  }
  // The base sits below the band the line's ruby is drawn in.
  const int baseY = y + annotationBand(renderer, fontId);
  // Guide dots (see setGuideDots): one dot centered in each inter-word gap.
  // Sized and vertically anchored off the block's base metrics, not per-word
  // scales, so the dot row stays level across inline size changes.
  const bool guideDots = guideDotsEnabled && !scanning;
  const int dotSize = std::max(2, blockAscender / 8);
  int prevWordEndX = 0;  // right edge of the previous word; valid once i > 0
  // Cache per-word ascender calculations (typically 2-3 unique scales per block).
  // Avoids the per-word function call overhead on ESP32-C3 for typical books where
  // per-word font sizing is rare or uniform. A word in a font of its own (WordFont)
  // has its font's ascender.
  struct CachedAscender {
    int fontId;
    float scale;
    int ascender;
  };
  // A vertical line runs every draw down its column at the position the horizontal code computes (VerticalDraw.h).
  const bool vertical = vertical_;
  const bool centredMarks = centredMarks_;
  auto drawRun = [&renderer, vertical, centredMarks](const int font, const int px, const int py, const char* text,
                                                     const EpdFontFamily::Style style, const float scale,
                                                     const bool combined = false) {
    if (vertical && combined) {
      drawCombinedVertical(renderer, font, px, py, text, style, scale);
    } else if (vertical) {
      drawTextVertical(renderer, font, px, py, text, style, scale, centredMarks);
    } else if (scale == 1.0f) {
      renderer.drawText(font, px, py, text, true, style);
    } else {
      renderer.drawTextScaled(font, px, py, text, true, style, scale);
    }
  };
  // How far a piece of text runs along the line at `scale`.
  auto runWidth = [&renderer, vertical](const int font, const char* text, const EpdFontFamily::Style style,
                                        const float scale, const bool combined = false) {
    if (vertical && combined) return columnEm(renderer, font, style, scale);
    if (vertical) return verticalTextAdvance(renderer, font, text, style, scale);
    return scale == 1.0f ? renderer.getTextWidth(font, text, style)
                         : renderer.getTextWidthScaled(font, text, style, scale);
  };
  // Where the line's baseline-relative marks sit across the column: the over edge, the word's axis, the block's axis.
  const int baseline = baseY + lineAscender;
  const int blockEm = vertical ? columnEm(renderer, effFontId, EpdFontFamily::REGULAR, blockScale) : 0;
  std::vector<CachedAscender> ascenderCache;
  auto getOrCacheAscender = [&](const int font, const float s) -> int {
    if (font == effFontId && s == blockScale) return blockAscender;
    for (const CachedAscender& cached : ascenderCache) {
      if (cached.fontId == font && cached.scale == s) return cached.ascender;
    }
    const int ascender = renderer.getFontAscenderSizeScaled(font, s);
    ascenderCache.push_back({font, s, ascender});
    return ascender;
  };
  // The Combine records are in word order: one walk finds each combined word as the loop reaches it.
  const uint8_t* const records = annotationBytes_ != 0 ? annotationRegion() + kAnnotationHeaderBytes : nullptr;
  const uint16_t recordCount = annotationCount();
  uint16_t recordCursor = 0;
  uint16_t nextCombined = UINT16_MAX;
  if (vertical && records != nullptr) nextCombined = nextCombinedWord(records, recordCount, recordCursor);
  for (uint16_t i = 0; i < numWords; i++) {
    while (nextCombined < i) nextCombined = nextCombinedWord(records, recordCount, recordCursor);
    const bool combined = nextCombined == i;
    const char* word = wordText(i);
    const int wordX = xposArr[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const int font = wordFontId(i, effFontId);
    const float scale = wordScale(i);
    const int ascender = getOrCacheAscender(font, scale);
    // Baseline alignment: shift smaller words down so all baselines meet at y + lineAscender. A word in
    // another family meets it too: the line keeps its block font's baseline, whatever the word's ascender.
    int wordY = baseY + (lineAscender - ascender);
    // SUP/SUB shift the baseline only — the glyph shrink comes from the word's own size
    // percentage (parser default 50%, or the publisher's CSS). Offsets are anchored to the
    // BLOCK's full-size ascender, not the shrunken word's, so the raised/lowered positions
    // match the surrounding full-size text:
    //   SUP: raise by 40% of the block ascender — sits clearly above the cap-height
    //   SUB: lower by 25% of the block ascender — descends below baseline without clashing
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= blockAscender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += blockAscender / 4;
    }
    drawRun(font, wordX, wordY, word, currentStyle, scale, combined);

    const bool hasDecoration =
        !scanning && (currentStyle & (EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH)) != 0;
    int lineWidth = 0;
    if (guideDots || hasDecoration) {
      lineWidth = runWidth(font, word, currentStyle, scale, combined);
    }

    if (guideDots) {
      if (i > 0) {
        const int gap = wordX - prevWordEndX;
        // Skip cramped gaps (zero-width joins of adjacent styled runs): the dot
        // needs at least a pixel of clearance on each side to read as a dot.
        if (gap >= dotSize + 2) {
          const int dotX = prevWordEndX + (gap - dotSize) / 2;
          // A third of the ascender above the shared baseline -- roughly mid
          // x-height, like a typographic middle dot.
          const int dotY = vertical ? columnAxis(baseline, blockEm) - dotSize / 2
                                    : baseY + lineAscender - blockAscender / 3 - dotSize / 2;
          renderer.fillRect(dotX, dotY, dotSize, dotSize, true);
        }
      }
      prevWordEndX = wordX + lineWidth;
    }

    if (hasDecoration) {
      if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
        // Vertical: the side line lies on the over side of the column, off the em box's edge.
        const int underlineY = vertical ? columnOverEdge(baseline, blockEm) - 2 : baseline + 3;
        renderer.drawLine(wordX, underlineY, wordX + lineWidth, underlineY, 2, true);
      }

      if ((currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
        const int strikeY = vertical ? columnAxis(wordY + ascender, columnEm(renderer, font, currentStyle, scale))
                                     : wordY + ascender / 2 + 4;
        renderer.drawLine(wordX, strikeY, wordX + lineWidth, strikeY, 2, true);
      }
    }
  }

  // The ruby: one glyph style (REGULAR) per group, a call per character, half the base's size; the scanning pass draws
  // it too, so the glyph prewarm learns it. Nothing here allocates.
  const uint16_t annotations = annotationCount();
  for (uint16_t k = 0; k < annotations; k++) {
    const Annotation a = annotation(k);
    if (a.kind != AnnotationKind::Ruby || a.text.empty()) continue;
    const int font = wordFontId(a.firstWord, effFontId);
    const float rubyScale = 0.5f * wordScale(a.firstWord);
    const int rubyY = baseY - renderer.getFontAscenderSizeScaled(font, rubyScale);

    // Walk the ruby's characters (at most 4 bytes and a NUL each; the view is not NUL-terminated).
    auto nextChar = [&a](size_t& pos, char (&buf)[5]) -> uint32_t {
      const auto lead = static_cast<unsigned char>(a.text[pos]);
      size_t len = lead < 0x80             ? 1
                   : (lead & 0xE0) == 0xC0 ? 2
                   : (lead & 0xF0) == 0xE0 ? 3
                   : (lead & 0xF8) == 0xF0 ? 4
                                           : 1;
      len = std::min(len, a.text.size() - pos);
      memcpy(buf, a.text.data() + pos, len);
      buf[len] = '\0';
      pos += len;
      const unsigned char* q = reinterpret_cast<const unsigned char*>(buf);
      return utf8NextCodepoint(&q);
    };
    char buf[5];
    int chars = 0;
    int rawTotal = 0;    // the ruby's advance unscaled: rounded once, as layout measures the whole ruby
    bool spread = true;  // 1:2:1 only when every codepoint of the ruby and of its base is unspaced CJK
    for (size_t pos = 0; pos < a.text.size(); chars++) {
      const bool cjk = utf8IsUnspacedCjk(nextChar(pos, buf));
      spread = spread && cjk;
      rawTotal += renderer.getTextAdvanceX(font, buf, EpdFontFamily::REGULAR);
    }
    for (uint16_t w = a.firstWord; spread && w < a.firstWord + a.wordCount; w++) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(wordText(w));
      while (spread && *p != 0) spread = utf8IsUnspacedCjk(utf8NextCodepoint(&p));
    }
    const int slack = a.width - static_cast<int>(rawTotal * rubyScale + 0.5f);
    int rawBefore = 0;
    size_t pos = 0;
    for (int c = 0; c < chars; c++) {
      nextChar(pos, buf);
      const int offset = spread ? slack * (2 * c + 1) / (2 * chars) : slack / 2;
      const int advanced = static_cast<int>(rawBefore * rubyScale + 0.5f);
      drawRun(font, x + a.x + offset + advanced, rubyY, buf, EpdFontFamily::REGULAR, rubyScale);
      rawBefore += renderer.getTextAdvanceX(font, buf, EpdFontFamily::REGULAR);
    }
  }

  // The emphasis marks: each character that takes one (TextEmphasis::takesMark) gets the mark glyph at half its
  // word's size, centred on the character's advance, in the same band as the ruby; the scanning pass draws them too.
  // A face without the glyph gets a filled square instead. Nothing here allocates.
  for (uint16_t k = 0; k < annotations; k++) {
    const Annotation a = annotation(k);
    if (a.kind != AnnotationKind::Emphasis) continue;
    const uint32_t markCp = TextEmphasis::markCodepoint(a.mark);
    if (markCp == 0) continue;
    char markText[4];
    markText[0] = static_cast<char>(0xE0 | (markCp >> 12));  // every mark glyph is three bytes (U+0800 and up)
    markText[1] = static_cast<char>(0x80 | ((markCp >> 6) & 0x3F));
    markText[2] = static_cast<char>(0x80 | (markCp & 0x3F));
    markText[3] = '\0';
    for (uint16_t w = a.firstWord; w < a.firstWord + a.wordCount && w < numWords; w++) {
      const int font = wordFontId(w, effFontId);
      const float wordScl = wordScale(w);
      const float markScale = 0.5f * wordScl;
      const EpdFontFamily::Style style = wordStyle(w);
      const int markAscender = renderer.getFontAscenderSizeScaled(font, markScale);
      const int markY = baseY - markAscender;
      const bool drawGlyph = scanning || renderer.hasGlyph(font, markCp, EpdFontFamily::REGULAR);
      const int markWidth = drawGlyph ? runWidth(font, markText, EpdFontFamily::REGULAR, markScale) : 0;
      const int square = std::max(2, markAscender / 4);
      const int wordX = xposArr[w] + x;
      if (vertical && isCombined(w)) {
        // A combined word is one upright block in one em cell: one mark, centred on the cell, if any code point takes
        // it.
        bool takes = false;
        for (const unsigned char* q = reinterpret_cast<const unsigned char*>(wordText(w)); *q != 0 && !takes;) {
          takes = TextEmphasis::takesMark(utf8NextCodepoint(&q));
        }
        if (takes) {
          const int cell = columnEm(renderer, font, style, wordScl);
          if (drawGlyph) {
            drawRun(font, wordX + (cell - markWidth) / 2, markY, markText, EpdFontFamily::REGULAR, markScale);
          } else {
            renderer.fillRect(wordX + (cell - square) / 2, markY + (markAscender - square) / 2, square, square, true);
          }
        }
        continue;
      }
      const unsigned char* p = reinterpret_cast<const unsigned char*>(wordText(w));
      int rawBefore = 0;  // the word's advance so far, unscaled: rounded once per character like the ruby
      while (*p != 0) {
        const unsigned char* start = p;
        const uint32_t cp = utf8NextCodepoint(&p);
        char buf[5];
        const size_t len = std::min<size_t>(static_cast<size_t>(p - start), 4);
        memcpy(buf, start, len);
        buf[len] = '\0';
        const int raw = vertical ? verticalTextAdvance(renderer, font, buf, style, 1.0f)
                                 : renderer.getTextAdvanceX(font, buf, style);
        if (TextEmphasis::takesMark(cp)) {
          const int charX = wordX + (wordScl == 1.0f ? rawBefore : static_cast<int>(rawBefore * wordScl + 0.5f));
          const int advance = wordScl == 1.0f ? raw : static_cast<int>(raw * wordScl + 0.5f);
          if (drawGlyph) {
            drawRun(font, charX + (advance - markWidth) / 2, markY, markText, EpdFontFamily::REGULAR, markScale);
          } else {
            renderer.fillRect(charX + (advance - square) / 2, markY + (markAscender - square) / 2, square, square,
                              true);
          }
        }
        rawBefore += raw;
      }
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }

  // Word data: scalars, then the arena verbatim -- its in-memory layout is
  // exactly the on-disk layout (see TextBlock.h), so one write covers all
  // per-word arrays and the text blob. The flags byte holds the sizes in bit 0
  // bit 1 says the line has annotations (u16 annotationBytes follows textBytes), bit 2 that it is vertical,
  // bit 3 that its marks are centred, and the word-font count in bits 4-7: a line without word fonts or annotations
  // writes the 0 or 1 it always did.
  serialization::writePod(file, numWords);
  serialization::writePod(
      file, static_cast<uint8_t>((sizesPresent ? 1 : 0) | (annotationBytes_ != 0 ? 2 : 0) | (vertical_ ? 4 : 0) |
                                 (centredMarks_ ? 8 : 0) | wordFontCount << 4));
  serialization::writePod(file, textBytes);
  if (annotationBytes_ != 0) serialization::writePod(file, annotationBytes_);
  if (numWords > 0) {
    const size_t size = arenaSize(numWords, sizesPresent, wordFontCount, textBytes, annotationBytes_);
    if (file.write(arena, size) != size) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  // Style: only the render slice. The spacing fields (margins, padding, indent
  // and their "defined" flags) used to be written here, but layout consumes them
  // into the word xpos / line y before a TextBlock exists, and nothing ever read
  // them back -- so they were 19 dead bytes per line on disk as well as in RAM.
  // Dropping them is a format change; SECTION_FILE_VERSION is bumped so stale
  // caches are rejected and rebuilt.
  serialization::writePod(file, renderStyle.alignment);
  serialization::writePod(file, renderStyle.fontSizeMultiplier);
  serialization::writePod(file, renderStyle.headingFontId);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file, BuildArena* scratch) {
  uint16_t wc = 0;
  uint8_t flags = 0;
  uint16_t textBytes = 0;
  uint16_t annotationBytes = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&wc), sizeof(wc)) != static_cast<int>(sizeof(wc)) ||
      file.read(&flags, sizeof(flags)) != static_cast<int>(sizeof(flags)) ||
      file.read(reinterpret_cast<uint8_t*>(&textBytes), sizeof(textBytes)) != static_cast<int>(sizeof(textBytes))) {
    LOG_ERR("TXB", "Deserialization failed: incomplete arena header");
    return nullptr;
  }
  if ((flags & 0x02) != 0 && file.read(reinterpret_cast<uint8_t*>(&annotationBytes), sizeof(annotationBytes)) !=
                                 static_cast<int>(sizeof(annotationBytes))) {
    LOG_ERR("TXB", "Deserialization failed: incomplete annotation size");
    return nullptr;
  }

  // Sanity checks: cap the arena allocation and reject impossible geometry
  // (every word carries at least its NUL terminator).
  const uint8_t fontCount = flags >> 4;
  if (wc > 10000 || fontCount > kMaxWordFonts || (fontCount != 0 && wc == 0)) {
    LOG_ERR("TXB", "Deserialization failed: word count %u or flags 0x%02x out of range", wc, flags);
    return nullptr;
  }
  if ((wc == 0 && textBytes != 0) || (wc > 0 && textBytes < wc)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wc);
    return nullptr;
  }
  if ((flags & 0x02) != 0 && (wc == 0 || annotationBytes < kAnnotationHeaderBytes + kAnnotationRecordBytes)) {
    LOG_ERR("TXB", "Deserialization failed: bad annotation size %u for %u words", annotationBytes, wc);
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "OOM: TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;
  block->sizesPresent = (flags & 1) != 0;
  block->wordFontCount = fontCount;
  block->vertical_ = (flags & 0x04) != 0;
  block->centredMarks_ = (flags & 0x08) != 0;

  if (wc > 0) {
    const size_t size = arenaSize(wc, block->sizesPresent, fontCount, textBytes, annotationBytes);
    if (!block->allocArena(size, scratch)) {
      LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
      return nullptr;
    }
    if (file.read(block->arena, size) != static_cast<int>(size)) {
      LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(size));
      return nullptr;
    }
    block->bindArenaPointers();

    // Validate offsets before anything dereferences wordText(): offset 0 first,
    // strictly increasing, in bounds, and every word NUL-terminated (word i ends
    // at the byte before offset i+1; the last word at the last text byte).
    const uint16_t* textOff = block->textOffArr;
    const char* text = block->textArr;
    if (textOff[0] != 0 || text[textBytes - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
      return nullptr;
    }
    for (uint16_t i = 1; i < wc; i++) {
      if (textOff[i] <= textOff[i - 1] || textOff[i] >= textBytes || text[textOff[i] - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
        return nullptr;
      }
    }
    // Every word font names a font, and every slot one of them.
    bool fontsValid = true;
    for (uint8_t f = 0; f < fontCount; f++) fontsValid = fontsValid && block->wordFontsArr()[f].fontId != 0;
    for (uint16_t i = 0; fontCount != 0 && i < wc; i++)
      fontsValid = fontsValid && block->fontSlotsArr()[i] <= fontCount;
    if (!fontsValid) {
      LOG_ERR("TXB", "Deserialization failed: corrupt word fonts");
      return nullptr;
    }
    // The annotation region: its records fit, name words of the line, and the ruby lengths add up to the rest.
    block->annotationBytes_ = annotationBytes;
    if (annotationBytes != 0 && !block->annotationsValid()) {
      LOG_ERR("TXB", "Deserialization failed: corrupt annotations");
      return nullptr;
    }
    block->annotationBand_ = annotationBytes != 0 && anyBandRecord(block->annotationRegion() + kAnnotationHeaderBytes,
                                                                   block->annotationCount());
  }

  // Style: the render slice only (see serialize).
  RenderStyle& renderStyle = block->renderStyle;
  serialization::readPod(file, renderStyle.alignment);
  serialization::readPod(file, renderStyle.fontSizeMultiplier);
  serialization::readPod(file, renderStyle.headingFontId);

  return block;
}
