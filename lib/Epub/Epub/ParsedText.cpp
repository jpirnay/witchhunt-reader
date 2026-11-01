#include "ParsedText.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <vector>

#include "CjkLineBreak.h"
#include "CjkPunctuation.h"
#include "Epub/VisibleText.h"
#include "TextEmphasis.h"
#include "VerticalDraw.h"
#include "hyphenation/HyphenationCommon.h"
#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Closing punctuation that should not have extra space inserted before it during justification.
// Includes common closing brackets/quotes and sentence-ending marks. En/em dashes
// are also treated as inline separators here to avoid justification stretch
// immediately before them.
bool isClosingPunctuation(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case '!':
    case '?':
    case ':':
    case ';':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x203A:  // ›
    case 0x2019:  // '  right single quotation mark
    case 0x201D:  // "  right double quotation mark
    case 0x2026:  // … ellipsis
    case 0x2013:  // – en dash
    case 0x2014:  // — em dash
      return true;
    default:
      return false;
  }
}

// SOFT_HYPHEN_UTF8 / SOFT_HYPHEN_BYTES come from TextBlock.h — the arena fill strips the same
// pattern this file measures around, so both sides share one definition.

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// True when every codepoint of `text` but a soft hyphen (TextBlock strips them) is unspaced CJK: TextBlock's test for
// spreading a ruby 1:2:1 rather than centring it (Ruling R4).
bool allUnspacedCjk(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*p != 0) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp != 0x00AD && !utf8IsUnspacedCjk(cp)) return false;
  }
  return true;
}

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false, const float scale = 1.0f,
                          const bool vertical = false) {
  int raw = 0;
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    raw = renderer.getSpaceWidth(fontId, style);
  } else {
    const bool hasSoftHyphen = containsSoftHyphen(word);
    if (!hasSoftHyphen && !appendHyphen) {
      raw = vertical ? verticalTextAdvance(renderer, fontId, word.c_str(), style, 1.0f)
                     : renderer.getTextAdvanceX(fontId, word.c_str(), style);
    } else {
      std::string sanitized = word;
      if (hasSoftHyphen) stripSoftHyphensInPlace(sanitized);
      if (appendHyphen) sanitized.push_back('-');
      raw = vertical ? verticalTextAdvance(renderer, fontId, sanitized.c_str(), style, 1.0f)
                     : renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
    }
  }
  if (scale == 1.0f) return static_cast<uint16_t>(raw);
  return static_cast<uint16_t>(raw * scale + 0.5f);
}

// The em of CJK text in this face and size: the advance of 一. 0 when the face has no 一 of its own: a Latin font
// draws CJK characters as its replacement glyph, whose advance says nothing about CJK widths, so no mark is taken
// for full-width punctuation and no grid is made from it.
int cjkEm(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style, const float scale) {
  if (!renderer.hasGlyph(fontId, 0x4E00, style)) return 0;
  return measureWordWidth(renderer, fontId, "\xE4\xB8\x80", style, false, scale);  // 一
}

// An item's part of what a push-in tier gives (`tierNeed` of the tier's `tierTotal`), shared in line order by
// cumulative rounding: `cumulativeBefore` is the sum of the amounts of the tier's items before it. The parts of a
// tier's items add up to tierNeed exactly, and none is more than its own amount.
int pushInShare(const int cumulativeBefore, const int amount, const int tierNeed, const int tierTotal) {
  if (tierTotal == 0) return 0;
  return ((cumulativeBefore + amount) * tierNeed) / tierTotal - (cumulativeBefore * tierNeed) / tierTotal;
}

// The DP breaker's Knuth-Plass style badness of a line `amount` px off its width (what a stretched line lacks, what
// a pushed-in line overran by, never negative): (amount / width)³ × 10000 in integers, at most 10000. Cubic, so it is
// lenient on a line a little off and strongly penalises a very loose one.
int cubicBadness(const int amount, const int width) {
  const long long num = static_cast<long long>(amount) * amount * amount;
  const long long den = static_cast<long long>(width) * width * width;
  return den > 0 ? static_cast<int>(std::min(num * 10000LL / den, 10000LL)) : 10000;
}

// What a line that fits only pushed in costs beyond the demerits of its overrun. Against a line stretched with the
// same badness, or a paragraph with the same demerits, the one without push-in wins; against a looser one, push-in
// does. So push-in is taken where it avoids a loose line, not by default (spec 5.3).
constexpr long long kPushInPenalty = 1;

std::string buildLinePreview(const std::vector<std::string>& words,
                             const std::vector<TokenBoundary::Boundary>& boundaries, const size_t start,
                             const size_t endExclusive, const size_t maxLen = 120) {
  // Build a readable line preview while preserving continuation semantics
  // (no synthetic spaces before attached tokens or at CJK gaps).
  std::string preview;
  for (size_t idx = start; idx < endExclusive; ++idx) {
    if (idx > start && idx < boundaries.size() && TokenBoundary::hasSpace(boundaries[idx])) {
      preview.push_back(' ');
    }
    preview += words[idx];
    if (preview.size() >= maxLen) {
      preview.resize(maxLen);
      preview += "...";
      break;
    }
  }
  return preview;
}

constexpr int kBionicReadingMinCodepoints = 4;
constexpr int kBionicReadingMinBoldPrefix = 1;
constexpr int kBionicReadingBoldPrefixNumerator = 1;
constexpr int kBionicReadingBoldPrefixDenominator = 2;

struct TokenSpan {
  size_t start;
  size_t end;
  bool isWord;
};

static int computeBionicBoldPrefixCount(const int codepointCount) {
  return std::max(kBionicReadingMinBoldPrefix,
                  (codepointCount * kBionicReadingBoldPrefixNumerator + kBionicReadingBoldPrefixDenominator - 1) /
                      kBionicReadingBoldPrefixDenominator);
}

static bool isBionicWordCodepoint(const uint32_t cp) {
  if (cp == 0) {
    return false;
  }
  if (utf8IsCombiningMark(cp)) {
    return true;
  }
  return isAlphabetic(cp) || isAsciiDigit(cp) || isApostrophe(cp);
}

// Split a word token into contiguous spans of "word-like" characters and non-word characters.
// This avoids applying bionic bolding to punctuation, digits-only runs, or other separators.
// Only spans marked as word-like are eligible for the bionic prefix transform.
static std::vector<TokenSpan> tokenizeBionicWord(const std::string& word) {
  std::vector<TokenSpan> spans;
  spans.reserve(2);

  const unsigned char* base = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* ptr = base;
  const unsigned char* segmentStart = ptr;
  bool currentIsWord = false;
  bool haveCurrent = false;

  while (true) {
    const unsigned char* cpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) {
      break;
    }

    bool cpIsWord = isBionicWordCodepoint(cp);
    if (!haveCurrent) {
      currentIsWord = cpIsWord;
      haveCurrent = true;
    } else if (!utf8IsCombiningMark(cp) && cpIsWord != currentIsWord) {
      spans.push_back({static_cast<size_t>(segmentStart - base), static_cast<size_t>(cpStart - base), currentIsWord});
      segmentStart = cpStart;
      currentIsWord = cpIsWord;
    }
  }

  if (haveCurrent) {
    spans.push_back({static_cast<size_t>(segmentStart - base), word.size(), currentIsWord});
  }

  return spans;
}

}  // namespace

bool ParsedText::reserveWords(const size_t additional) {
  const size_t requiredSize = words.size() + additional;
  if (words.capacity() >= requiredSize && wordStyles.capacity() >= requiredSize &&
      wordBoundaries.capacity() >= requiredSize && wordSizes.capacity() >= requiredSize &&
      wordVisibleOffsets.capacity() >= requiredSize) {
    return true;
  }
  size_t newCapacity = std::max<size_t>(16, words.capacity());
  while (newCapacity < requiredSize) {
    newCapacity *= 2;
  }
  // The five reserves below are unchecked heap growth (abort() on failure under
  // -fno-exceptions). Require the largest free block to hold all five, plus a header
  // each -- conservative, since they need not share a block, but this only bites when the
  // heap is nearly gone, and then a partial-cache abort beats a crash.
  {
    constexpr size_t ALLOC_HEADER_SLACK = 16;
    const size_t needed =
        newCapacity * (sizeof(std::string) + sizeof(EpdFontFamily::Style) + sizeof(TextBlock::WordSize) +
                       sizeof(uint32_t) + sizeof(TokenBoundary::Boundary)) +
        5 * ALLOC_HEADER_SLACK;
    if (ESP.getMaxAllocHeap() < needed) {
      wordGrowthRefused_ = true;
      return false;
    }
  }
  words.reserve(newCapacity);
  wordStyles.reserve(newCapacity);
  wordBoundaries.reserve(newCapacity);
  wordSizes.reserve(newCapacity);
  wordVisibleOffsets.reserve(newCapacity);
  return true;
}

void ParsedText::pushToken(std::string token, const EpdFontFamily::Style style, const TokenBoundary::Boundary boundary,
                           const uint8_t sizePct, const uint32_t visibleOffset, const uint8_t fontSlot) {
  words.push_back(std::move(token));
  wordStyles.push_back(style);
  wordBoundaries.push_back(boundary);
  wordSizes.push_back({sizePct, fontSlot});
  fontSlotsUsed_ |= static_cast<uint8_t>(1u << fontSlot);
  wordVisibleOffsets.push_back(visibleOffset);
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const uint8_t sizePct, const uint32_t visibleOffset,
                         uint8_t fontSlot, const uint8_t textEmphasis) {
  if (word.empty()) return;
  if (fontSlot >= TextBlock::kMaxWordFonts) fontSlot = 0;
  if (wordGrowthRefused_) return;  // the parse is being aborted; see wordGrowthRefused()

  word = utf8NfcNorm(std::move(word));

  // Chinese and Japanese are written without spaces: a word with CJK characters becomes one token
  // per break the CJK rules allow, so a line can end between its characters. Latin words get no
  // offsets and stay whole. Our implementation of crosspoint-reader PR #2288 ("feat: Support for
  // Korean line breaks and glyph spacing", Justin Mitchell, 22f35750).
  //
  // Every CJK mark whose width the punctuation table can change is a token of its own as well, glued
  // where kinsoku forbids a break (字。 stays together, but 。 gets its own x position): a token is
  // drawn at its font's advances, so only a mark that is a token can be drawn narrower (spec 5.2).
  const std::vector<size_t> cjkBreaks = CjkLineBreak::breakByteOffsets(word);
  // Only in a word with CJK text: a Latin word keeps its curly quotes inside it.
  std::vector<size_t> markEdges;  // byte offsets around each adjustable mark, glued unless a break is allowed
  bool hasCjk = false;
  for (const auto* q = reinterpret_cast<const unsigned char*>(word.c_str()); *q && !hasCjk;) {
    hasCjk = utf8IsCjkBreakable(utf8NextCodepoint(&q));
  }
  if (hasCjk) {
    const auto* const begin = reinterpret_cast<const unsigned char*>(word.c_str());
    const auto* p = begin;
    while (*p) {
      const size_t at = static_cast<size_t>(p - begin);
      const uint32_t cp = utf8NextCodepoint(&p);
      if (cp == 0) break;
      const CjkPunctuation::Class c = CjkPunctuation::classOf(cp);
      if (c == CjkPunctuation::Class::None || c == CjkPunctuation::Class::IdeographicSpace ||
          c == CjkPunctuation::Class::AmbiguousQuote) {
        continue;
      }
      const size_t after = static_cast<size_t>(p - begin);
      if (at > 0) markEdges.push_back(at);
      if (after < word.size()) markEdges.push_back(after);
    }
  }
  // The token boundaries: the CJK breaks, then each mark edge that is not already one (glued).
  struct Cut {
    size_t at;
    bool breakable;
  };
  std::vector<Cut> cuts;
  cuts.reserve(cjkBreaks.size() + markEdges.size());
  for (const size_t at : cjkBreaks) cuts.push_back({at, true});
  for (const size_t at : markEdges) {
    if (std::find(cjkBreaks.begin(), cjkBreaks.end(), at) == cjkBreaks.end()) cuts.push_back({at, false});
  }
  std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.at < b.at; });
  cuts.erase(std::unique(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.at == b.at; }),
             cuts.end());
  if (!reserveWords(cuts.size() + 1)) return;

  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  const uint8_t size = std::min(std::max(sizePct, MIN_WORD_SIZE_PCT), MAX_WORD_SIZE_PCT);

  // Glued to the previous token in the source (no whitespace between them): where the CJK rules
  // allow a break there, the two meet at a breakable zero-width gap. Where the source had
  // whitespace, the space is content and stays. From crosspoint-reader PR #2768 ("fix: restore
  // spaces between CJK words", Uri Tauber, 9163149a). A glued Latin "foo—" + "bar" stays glued: the gap needs
  // a CJK character on one side, since the dash rule of upstream PR #3883 is not ported.
  TokenBoundary::Boundary boundary{attachToPrevious, false};
  // A glued " " is the parser's no-break space (U+00A0 / U+202F): no break on either side of it.
  if (attachToPrevious && !words.empty() && words.back() != " " && word != " ") {
    const uint32_t left = lastCodepoint(words.back());
    const uint32_t right = firstCodepoint(word);
    if ((utf8IsCjkBreakable(left) || utf8IsCjkBreakable(right)) && CjkLineBreak::isGapBetween(left, right)) {
      boundary = TokenBoundary::Boundary{false, true};
    }
  }

  // Every token of the word carries the mark: it travels with the token through splits and erases.
  if (cuts.empty()) {
    pushToken(std::move(word), combinedStyle, TokenBoundary::withEmphasis(boundary, textEmphasis), size, visibleOffset,
              fontSlot);
    return;
  }
  size_t start = 0;
  uint32_t offset = visibleOffset;
  for (size_t i = 0; i <= cuts.size(); ++i) {
    const size_t end = i < cuts.size() ? cuts[i].at : word.size();
    pushToken(word.substr(start, end - start), combinedStyle, TokenBoundary::withEmphasis(boundary, textEmphasis), size,
              offset, fontSlot);
    // The next token starts where this one's visible bytes end (KOSync positions).
    offset += static_cast<uint32_t>(VisibleText::visibleBytes(word.data() + start, static_cast<int>(end - start)));
    if (i < cuts.size()) {
      // A CJK break is a breakable zero-width gap; a mark's other edges are glued (no space, no break).
      boundary = cuts[i].breakable ? TokenBoundary::Boundary{false, true} : TokenBoundary::Boundary{true, false};
    }
    start = end;
  }
}

bool ParsedText::foldUniformWordSizes() {
  if (words.empty() || wordSizes.empty()) return false;
  const uint8_t first = wordSizes[0].pct;
  for (const TextBlock::WordSize& sz : wordSizes) {
    if (sz.pct != first) return false;
  }
  if (first == DEFAULT_WORD_SIZE_PCT) return false;  // all-100% — nothing to fold

  blockStyle.fontSizeMultiplier *= first / 100.0f;
  for (TextBlock::WordSize& sz : wordSizes) sz.pct = DEFAULT_WORD_SIZE_PCT;
  return true;
}

// Consumes data to minimize memory usage
int ParsedText::widthForLine(const int lineIndex, const int lineHeight, const int16_t blockStartY,
                             const int pageWidth) const {
  if (lineHeight == 0 || blockStyle.floatZoneCount == 0) return pageWidth;
  const int lineTop = blockStartY + lineIndex * lineHeight;
  int indent = 0;
  for (int i = 0; i < blockStyle.floatZoneCount; ++i) {
    const auto& z = blockStyle.floatZones[i];
    if (lineTop < z.bottom && lineTop + lineHeight > z.top) {
      indent += z.width;
    }
  }
  return std::max(1, pageWidth - indent);
}

int ParsedText::spaceBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  const size_t source = spaceSource(fontId, j);
  return renderer.getSpaceAdvance(wordFontId(source, fontId), lastCodepoint(words[j - 1]), firstCodepoint(words[j]),
                                  wordStyles[source]);
}

int ParsedText::kernBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  const int left = wordFontId(j - 1, fontId);
  if (left != wordFontId(j, fontId)) return 0;
  return renderer.getKerning(left, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
}

int ParsedText::naturalGapBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  const TokenBoundary::Boundary& boundary = wordBoundaries[j];
  if (boundary.continues) return kernBefore(renderer, fontId, j);
  if (boundary.noSpaceBefore) return cjkLatinGapBefore(renderer, fontId, j);
  return spaceBefore(renderer, fontId, j);
}

int ParsedText::cjkLatinGapBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  if (!typography_.cjkLatinGap) return 0;
  const TokenBoundary::Boundary& boundary = wordBoundaries[j];
  if (boundary.continues || !boundary.noSpaceBefore) return 0;
  const uint32_t left = lastCodepoint(words[j - 1]);
  const uint32_t right = firstCodepoint(words[j]);
  if (!CjkPunctuation::takesCjkLatinGap(left, right)) return 0;
  // The gap is a quarter of the CJK character's em, in its own face and size.
  const size_t k = CjkPunctuation::isHanOrKana(left) ? j - 1 : j;
  const int em = cjkEm(renderer, wordFontId(k, fontId),
                       static_cast<EpdFontFamily::Style>(wordStyles[k] & EpdFontFamily::BOLD_ITALIC), wordScale(k));
  if (em <= 0) return 0;
  return (em * CjkPunctuation::kCjkLatinGap + 4) / 8;
}

void ParsedText::layoutAndExtractLines(
    const GfxRenderer& renderer, const int bodyFontId, const uint16_t viewportWidth,
    const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
    const bool includeLastLine, const int16_t blockStartY, const int lineHeight, const bool preserveSource) {
  if (words.empty()) {
    return;
  }

  // preserveSource: take a snapshot now and put it back on the way out, so the caller sees the
  // text exactly as it handed it over. Suppressing the trailing erase alone is not enough --
  // computeLineBreaks force-splits any word too wide for the line (inserting a hyphen), and
  // applyParagraphIndent/applyBionicReadingTransform rewrite words too. A cell measured against a
  // 47px column and then re-laid out as a full-width paragraph would otherwise keep the narrow
  // column's breaks: "Wordy0001" coming back as "Word-" + "y0001".
  //
  // The copy is bounded by the caller: only the table grid path sets this, and a cell is size-
  // bounded before it gets here. It lives for one cell's layout and is released on return.
  std::vector<std::string> savedWords;
  std::vector<EpdFontFamily::Style> savedStyles;
  std::vector<TextBlock::WordSize> savedSizes;
  std::vector<uint32_t> savedOffsets;
  std::vector<TokenBoundary::Boundary> savedBoundaries;
  std::vector<std::string> savedRubyTexts;
  bool savedIsContinuation = false;
  size_t savedBionicWatermark = 0;
  if (preserveSource) {
    savedWords = words;
    savedStyles = wordStyles;
    savedSizes = wordSizes;
    savedOffsets = wordVisibleOffsets;
    savedBoundaries = wordBoundaries;
    savedRubyTexts = rubyTexts_;
    savedIsContinuation = isContinuation_;
    savedBionicWatermark = bionicTransformedUpTo_;
  }

  // Heading blocks lay out (and below render) with a taller real font instead of scaling the
  // body font. Resolve the effective fontId once; all measurement helpers below take it as
  // their fontId argument, so widths/kerning/indent are computed in the heading font and stay
  // consistent with TextBlock::render(). For the scale-fallback path headingFontId==0 and this
  // is just the body font (fontSizeMultiplier still applies inside the helpers).
  const int fontId = blockStyle.headingFontId != 0 ? blockStyle.headingFontId : bodyFontId;

  // Apply fixed transforms before any per-line layout work.
  // Paragraph indent only applies to the first layout pass; skip on continuations.
  if (!isContinuation_) {
    applyParagraphIndent(renderer, fontId);
  }
  // Bionic transform is incremental: applyBionicReadingTransform() is a no-op
  // for already-transformed words (bionicTransformedUpTo_ == words.size()) and
  // only processes raw words appended since the last flush, so it is always safe
  // to call regardless of isContinuation_.
  if (bionicReadingEnabled) {
    applyBionicReadingTransform();
  }

  // Ensure font glyph metrics are loaded before measuring word widths.
  // For built-in fonts this is a no-op (the map lookup finds nothing and returns
  // immediately). For SD card fonts it reads glyph metadata (advanceX, no bitmaps)
  // for all codepoints in this paragraph so calculateWordWidths() needs no SD I/O.
  // Each font gets its own words: the block's font, then each font slot in use (a TTF
  // family renders the glyphs it is handed, so not the other families' text). A ruby is
  // drawn in its leader's font: its text follows that font's words, after a space.
  size_t totalSize = 1;  // reserve room for a possible hyphen fallback
  bool hasEmphasis = false;
  for (size_t i = 0; i < words.size(); i++) {
    if (i > 0 && TokenBoundary::hasSpace(wordBoundaries[i])) totalSize += 1;
    totalSize += words[i].size();
    if (TokenBoundary::emphasisOf(wordBoundaries[i]) != 0) hasEmphasis = true;
  }
  if (hasEmphasis) totalSize += 3 * 15;  // one glyph of each mark, 3 bytes
  for (const std::string& ruby : rubyTexts_) totalSize += 1 + ruby.size();
  for (int slot = -1; slot < TextBlock::kMaxWordFonts; slot++) {
    const int32_t font = slot < 0 ? 0 : fontSlots_[slot].fontId;  // 0: the block's
    if (slot >= 0 && (font == 0 || (fontSlotsUsed_ & (1u << slot)) == 0)) continue;
    std::string& allText = allText_;
    allText.clear();
    allText.reserve(totalSize);
    for (size_t i = 0; i < words.size(); i++) {
      if (fontSlots_[wordSizes[i].fontSlot].fontId != font) continue;
      if (!allText.empty() && TokenBoundary::hasSpace(wordBoundaries[i])) allText += ' ';
      allText += words[i];
    }
    // The mark glyphs of the words in this font, one of each mark.
    uint32_t marksSeen = 0;
    for (size_t i = 0; i < words.size(); i++) {
      const uint8_t mark = TokenBoundary::emphasisOf(wordBoundaries[i]);
      if (mark == 0 || (marksSeen & (1u << mark)) != 0 || fontSlots_[wordSizes[i].fontSlot].fontId != font) continue;
      marksSeen |= 1u << mark;
      const uint32_t cp = TextEmphasis::markCodepoint(mark);
      if (cp == 0) continue;
      // The mark glyphs are all in the BMP above U+0800: three bytes.
      allText += static_cast<char>(0xE0 | (cp >> 12));
      allText += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      allText += static_cast<char>(0x80 | (cp & 0x3F));
    }
    for (size_t i = 0, ordinal = 0; i < words.size() && ordinal < rubyTexts_.size(); i++) {
      if (TokenBoundary::rubyRole(wordBoundaries[i]) != TokenBoundary::RubyRole::Leader) continue;
      const std::string& ruby = rubyTexts_[ordinal++];
      if (fontSlots_[wordSizes[i].fontSlot].fontId != font) continue;
      allText += ' ';
      allText += ruby;
    }
    allText += '-';
    renderer.ensureFontReady(font != 0 ? font : fontId, allText.c_str());
  }

  int pageWidth = viewportWidth;

  // Compute firstLineIndent once here so all layout helpers use the same value.
  // On a continuation flush the remaining words are mid-paragraph, so no indent.
  // firstLineExtraIndent adds extra indent on the first line (on top of CSS text-indent).
  // A negative text-indent is the hanging-indent pattern (margin-left:3em; text-indent:-1em),
  // where the inset is what the first line hangs out of. It may pull left by at most that inset:
  // the block's own left edge is where the CSS containing block starts, and anything past it
  // renders off the panel and silently loses its first glyphs (issue #198).
  const int cssTextIndent =
      !isContinuation_ && blockStyle.textIndentDefined &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? std::min(std::max<int>(static_cast<int>(blockStyle.textIndent), -static_cast<int>(blockStyle.leftInset())),
                     pageWidth - 1)
          : 0;
  int firstLineIndent = cssTextIndent + (isContinuation_ ? 0 : static_cast<int>(blockStyle.firstLineExtraIndent));

  // The CJK grid (spec 5.5): a Chinese or Japanese book's text width rounds down to whole ems of the block's font,
  // half the cut on each side, so pure CJK lines fill exactly and characters line up in columns. A first-line
  // indent rounds down to whole ems, at least one; a block with none keeps none. Floats keep their own width.
  gridInset_ = 0;
  if (typography_.wholeEmGrid && blockStyle.floatZoneCount == 0) {
    const int em = cjkEm(renderer, fontId, EpdFontFamily::REGULAR, blockStyle.fontSizeMultiplier);
    if (em > 0 && pageWidth >= em) {
      const int gridWidth = pageWidth - pageWidth % em;
      gridInset_ = static_cast<int16_t>((pageWidth - gridWidth) / 2);
      pageWidth = gridWidth;
      if (firstLineIndent > 0) firstLineIndent = std::max(em, firstLineIndent - firstLineIndent % em);
    }
  }

  std::vector<uint16_t>& wordWidths = wordWidths_;
  calculateWordWidths(renderer, fontId, wordWidths);

  // A span indent gives way before the line does. Poems draw their shape with margin-left on each
  // line's span (Alice's mouse tale steps out to 10em and back); inside a block that is already
  // inset, the deepest lines no longer fit, and wrapping put the rest of the line back at the
  // column's left edge -- the shape broke wherever that happened. A block whose words fit the
  // column on one line now keeps to one line, its indent shortened by what it lacks. Only the
  // span-indent channel: a paragraph's own text-indent is typography, not shape, and wraps as
  // before.
  if (blockStyle.textIndentYields && cssTextIndent > 0 && includeLastLine && !words.empty()) {
    int natural = wordWidths[0];
    for (size_t j = 1; j < words.size(); ++j) {
      natural += wordWidths[j] + naturalGapBefore(renderer, fontId, j);
    }
    const int available = widthForLine(0, lineHeight, blockStartY, pageWidth);
    if (natural <= available && natural + firstLineIndent > available) {
      firstLineIndent = std::max(0, available - natural);
    }
  }

  std::vector<size_t>& lineBreakIndices = lineBreakIndices_;
  std::vector<bool>& lineEndsWithHyphenatedWord = lineEndsWithHyphenatedWord_;
  std::vector<int>& splitPrefixWordIndexes = splitPrefixWordIndexes_;
  std::vector<bool>& splitInsertedHyphen = splitInsertedHyphen_;
  lineEndsWithHyphenatedWord.clear();
  splitPrefixWordIndexes.clear();
  splitInsertedHyphen.clear();
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordBoundaries, lineEndsWithHyphenatedWord,
                                splitPrefixWordIndexes, splitInsertedHyphen, firstLineIndent, blockStartY, lineHeight,
                                lineBreakIndices);
  } else {
    computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordBoundaries, firstLineIndent, blockStartY, lineHeight,
                      lineBreakIndices);
    lineEndsWithHyphenatedWord.assign(lineBreakIndices.size(), false);
    splitPrefixWordIndexes.assign(lineBreakIndices.size(), -1);
    splitInsertedHyphen.assign(lineBreakIndices.size(), false);
  }
  size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    const bool lineEndedWithHyphenation = i < lineEndsWithHyphenatedWord.size() ? lineEndsWithHyphenatedWord[i] : false;
    // A retried line below keeps this place: it is the same line, laid out again.
    linePlace_.paragraphStart = !isContinuation_ && i == 0;
    linePlace_.linesLeft =
        includeLastLine ? static_cast<uint16_t>(std::min<size_t>(lineCount - i, std::numeric_limits<uint16_t>::max()))
                        : 0;
    const auto result = extractLine(i, pageWidth, wordWidths, wordBoundaries, lineBreakIndices, processLine, renderer,
                                    fontId, lineEndedWithHyphenation, false, firstLineIndent, blockStartY, lineHeight);

    if (result == LineProcessResult::RetryWithoutHyphenation && lineEndedWithHyphenation) {
      const size_t lineStart = i > 0 ? lineBreakIndices[i - 1] : 0;
      const size_t lineEnd = i < lineBreakIndices.size() ? lineBreakIndices[i] : lineStart;
      const std::string firstAttemptPreview = buildLinePreview(words, wordBoundaries, lineStart, lineEnd);
      LOG_TRC("PTX", "Line %u requested rerender without hyphenation, first attempt: %s", static_cast<unsigned>(i),
              firstAttemptPreview.c_str());
      // Undo precomputed splits from this line onward so the retry starts from
      // clean, unsplit tokens and cannot inherit future hyphenation artifacts.
      std::set<int, std::greater<int>> splitIndexesToUndo;
      for (size_t lineIdx = i; lineIdx < splitPrefixWordIndexes.size(); ++lineIdx) {
        const int splitIndex = splitPrefixWordIndexes[lineIdx];
        if (splitIndex >= 0) {
          splitIndexesToUndo.insert(splitIndex);
        }
      }
      for (const int splitIndex : splitIndexesToUndo) {
        if (splitIndex < 0 || static_cast<size_t>(splitIndex + 1) >= words.size()) {
          continue;
        }
        bool removeInsertedHyphen = false;
        for (size_t lineIdx = i; lineIdx < splitPrefixWordIndexes.size(); ++lineIdx) {
          if (splitPrefixWordIndexes[lineIdx] == splitIndex && lineIdx < splitInsertedHyphen.size()) {
            removeInsertedHyphen = splitInsertedHyphen[lineIdx];
            break;
          }
        }

        std::string merged = words[splitIndex];
        if (removeInsertedHyphen && !merged.empty() && merged.back() == '-') {
          merged.pop_back();
        }
        merged += words[splitIndex + 1];
        words[splitIndex] = std::move(merged);
        words.erase(words.begin() + splitIndex + 1);
        wordStyles.erase(wordStyles.begin() + splitIndex + 1);
        wordBoundaries.erase(wordBoundaries.begin() + splitIndex + 1);
        wordSizes.erase(wordSizes.begin() + splitIndex + 1);
        wordVisibleOffsets.erase(wordVisibleOffsets.begin() + splitIndex + 1);
      }

      // Recompute widths after restoring unsplit words.
      calculateWordWidths(renderer, fontId, wordWidths);

      // Keep previous lines fixed; recompute only this specific line without hyphenation.
      // Suppression is intentionally line-local.
      const size_t retryBreak = computeSingleLineBreakNoHyphen(renderer, fontId, pageWidth, wordWidths, wordBoundaries,
                                                               lineStart, firstLineIndent, blockStartY, lineHeight);

      lineBreakIndices.resize(i + 1);
      lineEndsWithHyphenatedWord.resize(i + 1);
      splitPrefixWordIndexes.resize(i + 1);
      splitInsertedHyphen.resize(i + 1);

      lineBreakIndices[i] = retryBreak;
      lineEndsWithHyphenatedWord[i] = false;
      splitPrefixWordIndexes[i] = -1;
      splitInsertedHyphen[i] = false;
      lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

      if (i < lineBreakIndices.size()) {
        const size_t retryLineStart = i > 0 ? lineBreakIndices[i - 1] : 0;
        const size_t retryLineEnd = i < lineBreakIndices.size() ? lineBreakIndices[i] : retryLineStart;
        const std::string retryPreview = buildLinePreview(words, wordBoundaries, retryLineStart, retryLineEnd);
        LOG_TRC("PTX", "Rerendering line %u with hyphenation suppressed, retry attempt: %s", static_cast<unsigned>(i),
                retryPreview.c_str());
        extractLine(i, pageWidth, wordWidths, wordBoundaries, lineBreakIndices, processLine, renderer, fontId, false,
                    true, firstLineIndent, blockStartY, lineHeight);

        // Resume regular hyphenation from the first word after the retried line.
        const size_t resumeIndex = lineBreakIndices[i];
        std::vector<bool>& suffixLineEndsWithHyphenatedWord = suffixLineEndsWithHyphenatedWord_;
        std::vector<int>& suffixSplitPrefixWordIndexes = suffixSplitPrefixWordIndexes_;
        std::vector<bool>& suffixSplitInsertedHyphen = suffixSplitInsertedHyphen_;
        suffixLineEndsWithHyphenatedWord.clear();
        suffixSplitPrefixWordIndexes.clear();
        suffixSplitInsertedHyphen.clear();
        const std::vector<size_t>& hyphenatedSuffixBreaks = suffixBreaks_;
        computeHyphenatedLineBreaksFromIndex(renderer, fontId, pageWidth, wordWidths, wordBoundaries, resumeIndex,
                                             suffixLineEndsWithHyphenatedWord, suffixSplitPrefixWordIndexes,
                                             suffixSplitInsertedHyphen, blockStartY, lineHeight,
                                             static_cast<int>(i) + 1, suffixBreaks_);

        lineBreakIndices.insert(lineBreakIndices.end(), hyphenatedSuffixBreaks.begin(), hyphenatedSuffixBreaks.end());
        lineEndsWithHyphenatedWord.insert(lineEndsWithHyphenatedWord.end(), suffixLineEndsWithHyphenatedWord.begin(),
                                          suffixLineEndsWithHyphenatedWord.end());
        splitPrefixWordIndexes.insert(splitPrefixWordIndexes.end(), suffixSplitPrefixWordIndexes.begin(),
                                      suffixSplitPrefixWordIndexes.end());
        splitInsertedHyphen.insert(splitInsertedHyphen.end(), suffixSplitInsertedHyphen.begin(),
                                   suffixSplitInsertedHyphen.end());

        lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;
        LOG_TRC("PTX", "Resumed regular hyphenation after rerendered line %u", static_cast<unsigned>(i));
      }
    }
  }

  // Remove consumed words so size() reflects only remaining words. The capacity is kept on
  // purpose: the parser reuses this object for the next paragraph (see reset()), and a block is
  // split at 96 words (ChapterHtmlSlimParser::flushPartWordBuffer), so the vectors settle at
  // 128 entries and never grow again. Shrinking here used to hand the next paragraph an empty
  // vector to regrow through 16/32/64/128 -- the churn reset() exists to remove.
  //
  // preserveSource skips this: a caller that may have to lay the same text out a second time (the
  // table grid path, which can only discover a cell does not fit by laying it out) needs the words
  // to still be there. It pays for that in residency, so it is opt-in.
  if (lineCount > 0 && !preserveSource) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    // The ruby texts of the groups that led the consumed tokens go with them.
    const size_t consumedRuby = leadersBefore(consumed);
    rubyTexts_.erase(rubyTexts_.begin(), rubyTexts_.begin() + std::min(consumedRuby, rubyTexts_.size()));
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordBoundaries.erase(wordBoundaries.begin(), wordBoundaries.begin() + consumed);
    wordSizes.erase(wordSizes.begin(), wordSizes.begin() + consumed);
    wordVisibleOffsets.erase(wordVisibleOffsets.begin(), wordVisibleOffsets.begin() + consumed);
    // All remaining words were already transformed before the flush; reset the
    // watermark so that words appended by addWord() are processed next time.
    bionicTransformedUpTo_ = words.size();
  }
  isContinuation_ = !includeLastLine;

  if (preserveSource) {
    words = std::move(savedWords);
    wordStyles = std::move(savedStyles);
    wordSizes = std::move(savedSizes);
    wordVisibleOffsets = std::move(savedOffsets);
    wordBoundaries = std::move(savedBoundaries);
    rubyTexts_ = std::move(savedRubyTexts);
    isContinuation_ = savedIsContinuation;
    bionicTransformedUpTo_ = savedBionicWatermark;
  }
}

size_t ParsedText::leadersBefore(const size_t tokenIndex) const {
  size_t leaders = 0;
  for (size_t i = 0; i < tokenIndex && i < wordBoundaries.size(); ++i) {
    if (TokenBoundary::rubyRole(wordBoundaries[i]) == TokenBoundary::RubyRole::Leader) ++leaders;
  }
  return leaders;
}

// The longest prefix of `text` of at most `room` bytes that ends on a character boundary.
static size_t rubyBytesThatFit(const std::string_view text, const size_t room) {
  if (text.size() <= room) return text.size();
  size_t n = room;
  while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) --n;
  return n;
}

bool ParsedText::addRubyGroup(const size_t firstToken, const size_t tokenCount, std::string text) {
  // A ruby over TextBlock::kMaxAnnotationText bytes would make its whole line invalid: cap it where it enters,
  // at a character boundary.
  text.resize(rubyBytesThatFit(text, TextBlock::kMaxAnnotationText));
  if (tokenCount == 0 || firstToken >= words.size() || tokenCount > words.size() - firstToken) return false;
  for (size_t i = firstToken; i < firstToken + tokenCount; ++i) {
    if (TokenBoundary::rubyRole(wordBoundaries[i]) != TokenBoundary::RubyRole::None) return false;
  }
  const size_t ordinal = leadersBefore(firstToken);
  if (ordinal > rubyTexts_.size()) return false;
  // A space inside the base stays, as a glued space token (the form a no-break space takes): the group never breaks,
  // and the space is drawn and stretched like any other.
  size_t spaces = 0;
  for (size_t i = firstToken + 1; i < firstToken + tokenCount; ++i) {
    if (TokenBoundary::hasSpace(wordBoundaries[i])) ++spaces;
  }
  if (spaces != 0 && !reserveWords(spaces)) return false;
  rubyTexts_.insert(rubyTexts_.begin() + ordinal, std::move(text));
  wordBoundaries[firstToken] = TokenBoundary::withRubyRole(wordBoundaries[firstToken], TokenBoundary::RubyRole::Leader);
  constexpr TokenBoundary::Boundary kGluedFollower =
      TokenBoundary::withRubyRole(TokenBoundary::Boundary{true, false}, TokenBoundary::RubyRole::Follower);
  size_t end = firstToken + tokenCount;
  for (size_t i = firstToken + 1; i < end; ++i) {
    // Every token of the base keeps its word's emphasis mark, the inserted space too.
    const TokenBoundary::Boundary glued =
        TokenBoundary::withEmphasis(kGluedFollower, TokenBoundary::emphasisOf(wordBoundaries[i]));
    if (TokenBoundary::hasSpace(wordBoundaries[i])) {
      words.insert(words.begin() + i, " ");
      wordStyles.insert(wordStyles.begin() + i, wordStyles[i]);
      wordSizes.insert(wordSizes.begin() + i, wordSizes[i]);
      wordVisibleOffsets.insert(wordVisibleOffsets.begin() + i, wordVisibleOffsets[i]);
      wordBoundaries.insert(wordBoundaries.begin() + i, glued);
      ++i;
      ++end;
    }
    wordBoundaries[i] = glued;
  }
  return true;
}

bool ParsedText::appendRubyText(const std::string_view text) {
  if (rubyTexts_.empty()) return false;
  // Capped like addRubyGroup's text: only what whole characters leave room for is appended.
  std::string& last = rubyTexts_.back();
  if (last.size() >= TextBlock::kMaxAnnotationText) return true;
  const size_t room = TextBlock::kMaxAnnotationText - last.size();
  last.append(text.data(), rubyBytesThatFit(text, room));
  return true;
}

void ParsedText::releaseLayoutScratch() {
  // Pure scratch: recomputed by every layout, so safe to drop at any point between feeds.
  std::vector<uint16_t>().swap(wordWidths_);
  std::vector<uint8_t>().swap(markClass_);
  std::vector<int8_t>().swap(edgeTrim_);
  std::vector<uint8_t>().swap(shrink_);
  std::vector<uint8_t>().swap(edgeShrink_);
  std::vector<RubyEdge>().swap(rubyEdge_);
  std::vector<size_t>().swap(lineBreakIndices_);
  std::vector<bool>().swap(lineEndsWithHyphenatedWord_);
  std::vector<int>().swap(splitPrefixWordIndexes_);
  std::vector<bool>().swap(splitInsertedHyphen_);
  std::vector<size_t>().swap(suffixBreaks_);
  std::vector<bool>().swap(suffixLineEndsWithHyphenatedWord_);
  std::vector<int>().swap(suffixSplitPrefixWordIndexes_);
  std::vector<bool>().swap(suffixSplitInsertedHyphen_);
  std::vector<int>().swap(interWordGaps_);
  std::vector<int>().swap(lineIndexForWord_);
  std::vector<int>().swap(dp_);
  std::vector<size_t>().swap(ans_);
  std::vector<int16_t>().swap(lineXPosScratch_);
  std::string().swap(allText_);
  std::vector<TextBlock::Annotation>().swap(lineAnnotations_);
  // The word vectors carry the paragraph in progress: an empty block gives them back whole; a
  // block mid-paragraph keeps them AS THEY ARE. This used to shrink_to_fit them, and a slice
  // yields inside a paragraph almost always (the 1 KB feed chunk ends mid-<p>), so every yield
  // reallocated the vectors to their exact size and the next words regrew them by doubling
  // from there -- 40 -> 80 -> 160 entries, a 3,840 B request above the 128-entry steady state,
  // and a free/alloc pair per slice that left the heap at 3,956 B contiguous with 14 KB free
  // (device run 10). The capacity kept here is at most 128 entries, ~3.6 KB, reached once.
  if (words.empty()) {
    std::vector<std::string>().swap(words);
    std::vector<EpdFontFamily::Style>().swap(wordStyles);
    std::vector<TokenBoundary::Boundary>().swap(wordBoundaries);
    std::vector<TextBlock::WordSize>().swap(wordSizes);
    std::vector<uint32_t>().swap(wordVisibleOffsets);
    std::vector<std::string>().swap(rubyTexts_);
  }
}

void ParsedText::reset(const BlockStyle& newBlockStyle) {
  // clear() keeps each vector's capacity; the strings inside `words` are destroyed here.
  words.clear();
  wordStyles.clear();
  wordBoundaries.clear();
  wordSizes.clear();
  wordVisibleOffsets.clear();
  rubyTexts_.clear();
  for (TextBlock::WordFont& slot : fontSlots_) slot = {};
  fontSlotsUsed_ = 0;
  blockStyle = newBlockStyle;
  isContinuation_ = false;
  bionicTransformedUpTo_ = 0;
}

void ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId, std::vector<uint16_t>& out) {
  out.clear();
  out.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    out.push_back(
        measureWordWidth(renderer, wordFontId(i, fontId), words[i], wordStyles[i], false, wordScale(i), vertical_));
  }
  applyPunctuationWidths(renderer, fontId, out);
  // After the marks are narrowed: a narrowed mark lends a ruby only its narrowed width.
  applyRubyWidths(renderer, fontId, out);
}

int ParsedText::rubyWidth(const GfxRenderer& renderer, const int fontId, const size_t leader,
                          const std::string& text) const {
  return measureWordWidth(renderer, wordFontId(leader, fontId), text, EpdFontFamily::REGULAR, false,
                          0.5f * wordScale(leader));
}

// Ruby widths after crosspoint-reader PR #2665 ("feat(epub): native <ruby> support for Chinese, Japanese", Phạm Bình
// An / @brianhuster) and #2781 (ruby inside the margins). Theirs: a longer ruby overhangs its neighbours, a Han
// neighbour is pushed away, a line edge keeps the overhang inside. Ours: overhang only over kana and CJK punctuation,
// the 1:2:1 spreading JLREQ asks for, and the line-edge reserves as edge trims, so all four breakers see them.
void ParsedText::applyRubyWidths(const GfxRenderer& renderer, const int fontId, std::vector<uint16_t>& widths) {
  using TokenBoundary::RubyRole;
  rubyEdge_.clear();
  if (rubyTexts_.empty()) return;
  const size_t count = words.size();
  rubyEdge_.assign(count, RubyEdge{});

  // How far a ruby may overhang neighbour n: half of n's character next to the group (its last when n is left of the
  // group, else its first) when that is a kana or a CJK mark, n has no ruby of its own and no space separates them.
  // At most half of n's width here, so a narrowed mark lends half of its narrowed width.
  const auto room = [&](const size_t n, const size_t boundaryIndex, const bool leftOfGroup) {
    if (n >= count || TokenBoundary::hasSpace(wordBoundaries[boundaryIndex]) ||
        TokenBoundary::rubyRole(wordBoundaries[n]) != RubyRole::None) {
      return 0;
    }
    const uint32_t cp = leftOfGroup ? lastCodepoint(words[n]) : firstCodepoint(words[n]);
    if (!utf8IsKana(cp) && CjkPunctuation::classOf(cp) == CjkPunctuation::Class::None) return 0;
    std::string glyph;  // at most 4 bytes: no allocation
    utf8AppendCodepoint(glyph, cp);
    const int advance = measureWordWidth(renderer, wordFontId(n, fontId), glyph, wordStyles[n], false, wordScale(n));
    return std::min<int>(widths[n], advance) / 2;
  };

  size_t ordinal = 0;  // the n-th leader owns the n-th ruby text
  for (size_t i = 0; i < count && ordinal < rubyTexts_.size(); ++i) {
    if (TokenBoundary::rubyRole(wordBoundaries[i]) != RubyRole::Leader) continue;
    const std::string& text = rubyTexts_[ordinal++];
    size_t last = i;
    int base = widths[i];
    while (last + 1 < count && TokenBoundary::rubyRole(wordBoundaries[last + 1]) == RubyRole::Follower) {
      base += widths[++last];
    }
    const int ruby = rubyWidth(renderer, fontId, i, text);
    if (ruby <= base) continue;  // TextBlock spreads a shorter ruby over its base when it draws it

    // The ruby is centred over its base (spec 7.2): it passes each side by half of what it is longer. Beside a kana or
    // a CJK mark, up to `room` of that half overhangs the neighbour; the rest of the half, and all of it beside
    // anything else, pushes the neighbour away.
    const int excess = ruby - base;
    const int halfLeft = excess / 2;
    const int halfRight = excess - halfLeft;
    const int left = std::min(i > 0 ? room(i - 1, i, true) : 0, halfLeft);
    const int right = std::min(room(last + 1, last + 1, false), halfRight);
    const int pushLeft = halfLeft - left;
    const int pushRight = halfRight - right;

    // The base widens by each push on its own side, so it stays under the ruby's centre. Where ruby and base are all
    // unspaced CJK (Ruling R4, as TextBlock spreads the ruby), what the two pushes have in common spreads the base
    // 1:2:1 instead: its characters share it evenly, each drawn in the middle of its share (half a share before the
    // first character, a whole one between two, half after the last). Anything else stays one block, so no gap opens
    // inside a word split into tokens (bionic reading).
    const int n = static_cast<int>(last - i + 1);
    bool cjk = allUnspacedCjk(text);
    for (size_t k = i; cjk && k <= last; ++k) cjk = allUnspacedCjk(words[k]);
    const int spread = cjk ? 2 * std::min(pushLeft, pushRight) : 0;
    for (int k = 0; k < n; ++k) {
      const int e = spread * (k + 1) / n - spread * k / n;
      widths[i + k] = static_cast<uint16_t>(widths[i + k] + e);
      rubyEdge_[i + k].lead = static_cast<int16_t>(e / 2);
    }
    widths[i] = static_cast<uint16_t>(widths[i] + pushLeft - spread / 2);
    rubyEdge_[i].lead = static_cast<int16_t>(rubyEdge_[i].lead + pushLeft - spread / 2);
    widths[last] = static_cast<uint16_t>(widths[last] + pushRight - spread / 2);
    // The overhang on each side lies over the neighbour mid-line. A group starting or ending a line has no neighbour
    // there: the line holds that overhang inside the leader's or the last token's box (lineStartTrim, lineEndTrim).
    rubyEdge_[i].start = static_cast<int16_t>(left);
    rubyEdge_[last].end = static_cast<int16_t>(right);
  }

  // Push-in takes nothing a ruby needs. A mark inside a group gives nothing: the group's ruby box is measured on its
  // base's mid-line widths (addRubyBox). A mark a ruby overhangs keeps the overhangs from both sides mid-line. At a
  // line edge a ruby overhangs a mark from one side only, by at most half of its mid-line width (room), and no
  // line-edge minimum is under half a mark, so edgeShrink_ needs no cap.
  for (size_t n = 0; n < count; ++n) {
    if (shrink_[n] == 0 && edgeShrink_[n] == 0) continue;
    if (TokenBoundary::rubyRole(wordBoundaries[n]) != RubyRole::None) {
      shrink_[n] = 0;
      edgeShrink_[n] = 0;
      continue;
    }
    int uncovered = widths[n];
    if (n > 0) uncovered -= rubyEdge_[n - 1].end;
    if (n + 1 < count) uncovered -= rubyEdge_[n + 1].start;
    shrink_[n] = static_cast<uint8_t>(std::max(0, std::min<int>(shrink_[n], uncovered)));
  }
}

void ParsedText::applyPunctuationWidths(const GfxRenderer& renderer, const int fontId, std::vector<uint16_t>& widths) {
  using CjkPunctuation::Class;
  const size_t count = words.size();
  edgeTrim_.assign(count, 0);
  shrink_.assign(count, 0);
  edgeShrink_.assign(count, 0);

  // A mark is CJK punctuation only where its font draws it full-width, at least 4/5 of the em of 一 in the same
  // face and size: a curly quote in a Latin font is narrow, and a font without CJK glyphs has no em (cjkEm). Every
  // token is judged on its natural width before any is narrowed, as a narrowed neighbour (” at 6/8 before “) would no
  // longer pass.
  int emFont = -1;
  EpdFontFamily::Style emStyle = EpdFontFamily::REGULAR;
  float emScale = 0.0f;
  int em = 0;
  const auto fullWidth = [&](const size_t i) {
    const EpdFontFamily::Style style = static_cast<EpdFontFamily::Style>(wordStyles[i] & EpdFontFamily::BOLD_ITALIC);
    if (wordFontId(i, fontId) != emFont || style != emStyle || wordScale(i) != emScale) {
      emFont = wordFontId(i, fontId);
      emStyle = style;
      emScale = wordScale(i);
      em = cjkEm(renderer, emFont, emStyle, emScale);
    }
    return em > 0 && widths[i] * 5 >= em * 4;
  };
  const auto singleCodepoint = [&](const size_t i) {
    const auto* p = reinterpret_cast<const unsigned char*>(words[i].c_str());
    const uint32_t cp = utf8NextCodepoint(&p);
    return *p == 0 ? cp : 0;
  };

  // The class of each token that is one full-width mark (addWord makes every adjustable mark a token).
  std::vector<uint8_t>& marks = markClass_;
  marks.assign(count, static_cast<uint8_t>(Class::None));
  for (size_t i = 0; i < count; ++i) {
    const uint32_t cp = singleCodepoint(i);
    const Class c = cp != 0 ? CjkPunctuation::classOf(cp) : Class::None;
    if (c != Class::None && fullWidth(i)) marks[i] = static_cast<uint8_t>(c);
  }
  // The class of token j's character next to the mark: a mark token's own, else the edge codepoint's (a curly quote
  // inside a word is Latin).
  const auto neighbourClass = [&](const size_t j, const uint32_t edge) {
    if (singleCodepoint(j) != 0) return static_cast<Class>(marks[j]);
    return CjkPunctuation::isCurlyQuote(edge) ? Class::None : CjkPunctuation::classOf(edge);
  };

  const auto table = typography_.punctuation;
  for (size_t i = 0; i < count; ++i) {
    const auto mark = static_cast<Class>(marks[i]);
    if (mark == Class::None || mark == Class::IdeographicSpace || mark == Class::AmbiguousQuote) continue;
    // An opening mark looks at the character before it, every other mark at the one after it; across a space there
    // is no neighbour.
    Class neighbour = Class::None;
    if (mark == Class::Opening) {
      if (i > 0 && !TokenBoundary::hasSpace(wordBoundaries[i])) {
        neighbour = neighbourClass(i - 1, lastCodepoint(words[i - 1]));
      }
    } else if (i + 1 < count && !TokenBoundary::hasSpace(wordBoundaries[i + 1])) {
      neighbour = neighbourClass(i + 1, firstCodepoint(words[i + 1]));
    }
    const int full = widths[i];
    const CjkPunctuation::Width midLine = CjkPunctuation::widthOf(table, mark, neighbour, false);
    const CjkPunctuation::Width lineEdge = CjkPunctuation::widthOf(table, mark, Class::None, true);
    const int mid = (full * midLine.forced + 4) / 8;
    const int edge = (full * lineEdge.forced + 4) / 8;
    widths[i] = static_cast<uint16_t>(mid);
    // Half an em at most, so a byte holds it at any size a page can show; clamped all the same.
    edgeTrim_[i] = static_cast<int8_t>(std::clamp(edge - mid, -127, 127));
    // What push-in may take off the mark, down to the minimum each place allows; half an em at most, as above.
    shrink_[i] = static_cast<uint8_t>(std::clamp(mid - (full * midLine.minimum + 4) / 8, 0, 255));
    edgeShrink_[i] = static_cast<uint8_t>(std::clamp(edge - (full * lineEdge.minimum + 4) / 8, 0, 255));
  }
}

void ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                   std::vector<uint16_t>& wordWidths,
                                   const std::vector<TokenBoundary::Boundary>& boundaries, const int firstLineIndent,
                                   const int16_t blockStartY, const int lineHeight,
                                   std::vector<size_t>& lineBreakIndices) {
  lineBreakIndices.clear();
  if (words.empty()) {
    return;
  }

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = widthForLine(0, lineHeight, blockStartY, pageWidth) - (i == 0 ? firstLineIndent : 0);
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // Pre-compute inter-word gaps once so the O(n²) DP inner loop avoids repeated
  // codepoint scanning and renderer calls for every (i,j) pair.
  // interWordGaps[j] = the spacing between words[j-1] and words[j] (0 for j==0).
  // The four tables below are members reused across paragraphs (see the header): as locals
  // they cost four allocations per paragraph, the last per-block churn on a plain layout.
  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(totalWordCount, 0);
  for (size_t j = 1; j < totalWordCount; ++j) {
    interWordGaps[j] = naturalGapBefore(renderer, fontId, j);
  }

  // Greedy pre-pass: map each word-start index to its line number so the DP can
  // call widthForLine(lineIdx, ...) per-word-start in O(1).
  std::vector<int>& lineIndexForWord = lineIndexForWord_;
  lineIndexForWord.assign(totalWordCount, 0);
  if (lineHeight > 0 && blockStyle.floatZoneCount > 0) {
    int lineIdx = 0;
    size_t cur = 0;
    while (cur < totalWordCount) {
      const int lineW =
          widthForLine(lineIdx, lineHeight, blockStartY, pageWidth) - (lineIdx == 0 ? firstLineIndent : 0);
      int w = 0;
      size_t start = cur;
      while (cur < totalWordCount) {
        const int gap = (cur > start) ? interWordGaps[cur] : 0;
        if (w + gap + wordWidths[cur] > lineW && cur > start) break;
        lineIndexForWord[cur] = lineIdx;
        w += gap + wordWidths[cur];
        ++cur;
      }
      // Guard: if no progress (single oversized word), advance anyway
      if (cur == start) {
        lineIndexForWord[cur] = lineIdx;
        ++cur;
      }
      ++lineIdx;
    }
  }

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int>& dp = dp_;
  dp.assign(totalWordCount, 0);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t>& ans = ans_;
  ans.assign(totalWordCount, 0);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    int shrink = 0;  // what the line's marks and gaps may give mid-line (push-in)
    dp[i] = MAX_COST;

    const int lineIdx = lineIndexForWord[i];
    // The indent belongs to the line that starts the paragraph (word 0), not to every line planned at lineIdx 0:
    // without floats lineIndexForWord is all zeros, and beside a float a line can start mid-way through the greedy
    // first line.
    const int effectivePageWidth =
        widthForLine(lineIdx, lineHeight, blockStartY, pageWidth) - (i == 0 ? firstLineIndent : 0);

    const int startTrim = lineStartTrim(i);

    for (size_t j = i; j < totalWordCount; ++j) {
      const bool firstToken = j == static_cast<size_t>(i);
      const int gap = firstToken ? 0 : interWordGaps[j];
      currlen += wordWidths[j] + gap;
      shrink += shrink_[j] + (gap > 0 ? gapShrinkBefore(j, gap) : 0);
      // The line as drawn: its first and last token at their line-edge widths (CJK punctuation, spec 5.2), with the
      // overhang of a ruby at either edge held inside. A line wider than that may still fit pushed in, its marks and
      // CJK-Latin gaps at their minimum widths (spec 5.3), as the greedy breakers measure it and extractLine lays it
      // out. That pushed-in width only grows with j, so the break below holds: no mark's mid-line minimum is below
      // its line-edge minimum (CjkPunctuation.NoMarkIsNarrowerMidLineThanAtALineEdge), a CJK-Latin gap keeps half of
      // itself, and a ruby's end reserve on token j is at most half of token j + 1, the neighbour it overhangs when
      // the line goes on, which push-in never takes below half (no minimum is under half a mark, and a mark under two
      // overhangs keeps them). The exception is a mark inside a ruby group: it gives nothing and keeps its table
      // widths, so a base ending in 」 (narrower before 。 than at a line end) whose ruby overhangs a following 。 can
      // narrow the line when 。 joins it. The break then stops one token early: a looser line, never an overfull one.
      const int lineLen = currlen + startTrim + lineEndTrim(j);
      const bool pushedIn = lineLen > effectivePageWidth;

      if (pushedIn && pushedInWidth(currlen, shrink, i, j) > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && !TokenBoundary::allowsBreak(boundaries[j + 1])) {
        continue;
      }

      // Knuth-Plass style demerits, (1 + badness)², of what the line lacks; the last line has none, however loose.
      // A line that fits only pushed in, the last one too, pays the demerits of what it overran by and the push-in
      // penalty: an exact line costs 1, a pushed-in one at least 2, a stretched one with any badness at least 4. A
      // line with nothing that can give way is never pushed in, so it costs what it did without push-in.
      const bool lastLine = j == totalWordCount - 1;
      int cost;
      if (lastLine && !pushedIn) {
        cost = 0;
      } else {
        const int badness = pushedIn ? cubicBadness(lineLen - effectivePageWidth, effectivePageWidth)
                                     : cubicBadness(effectivePageWidth - lineLen, effectivePageWidth);
        const long long demerits =
            static_cast<long long>(1 + badness) * (1 + badness) + (pushedIn ? kPushInPenalty : 0);
        const long long cost_ll = demerits + (lastLine ? 0 : dp[j + 1]);

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return;
}

size_t ParsedText::computeSingleLineBreakNoHyphen(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const std::vector<uint16_t>& wordWidths,
                                                  const std::vector<TokenBoundary::Boundary>& boundaries,
                                                  const size_t lineStartIndex, const int firstLineIndent,
                                                  const int16_t blockStartY, const int lineHeight) const {
  // One-line non-hyphenating breaker used by the page-boundary retry path.
  if (lineStartIndex >= wordWidths.size()) {
    return lineStartIndex;
  }

  // lineStartIndex == 0 means this is the first line of the block.
  const int effectivePageWidth = widthForLine(lineStartIndex == 0 ? 0 : 1, lineHeight, blockStartY, pageWidth) -
                                 (lineStartIndex == 0 ? firstLineIndent : 0);

  size_t currentIndex = lineStartIndex;
  int lineWidth = 0;
  int lineShrink = 0;  // what the line's marks and gaps may give mid-line (push-in)

  while (currentIndex < wordWidths.size()) {
    const bool isFirstWord = currentIndex == lineStartIndex;
    const int spacing = isFirstWord ? 0 : naturalGapBefore(renderer, fontId, currentIndex);

    const int candidateWidth = spacing + wordWidths[currentIndex];
    const int candidateShrink = shrink_[currentIndex] + (spacing > 0 ? gapShrinkBefore(currentIndex, spacing) : 0);
    const int candidateLen = withEdgeTrims(lineWidth + candidateWidth, lineStartIndex, currentIndex);
    if (candidateLen <= effectivePageWidth || pushedInWidth(lineWidth + candidateWidth, lineShrink + candidateShrink,
                                                            lineStartIndex, currentIndex) <= effectivePageWidth) {
      lineWidth += candidateWidth;
      lineShrink += candidateShrink;
      ++currentIndex;
      continue;
    }

    if (currentIndex == lineStartIndex) {
      ++currentIndex;
    }
    break;
  }

  while (currentIndex > lineStartIndex + 1 && currentIndex < wordWidths.size() &&
         !TokenBoundary::allowsBreak(boundaries[currentIndex])) {
    --currentIndex;
  }

  return currentIndex;
}

void ParsedText::applyParagraphIndent(const GfxRenderer& renderer, const int fontId) {
  if (words.empty()) {
    return;
  }

  if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) — handled by extractLine() via firstLineIndent.
  } else if (blockStyle.fromBrElement) {
    // A block created by an inline <br> is the next line of the SAME paragraph, so it gets no
    // first-line indent. (A <br> standing between two paragraphs leaves its block empty; that
    // block is merged into the following one, which brings its own indent.)
  } else if (!extraParagraphSpacing && blockStyle.floatZoneCount == 0 &&
             (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)) {
    // No CSS text-indent defined — apply a font-size-relative pixel indent so paragraph
    // boundaries are visually clear. Using getFontAscenderSize() gives one em in pixels,
    // matching the typographic convention for a paragraph indent. This avoids injecting
    // U+2003 (em-space) as a character, which caused glyph-miss overhead on every page
    // when the active font doesn't contain that codepoint.
    // Skip when a float zone is active: the image already provides visual separation.
    const int oneEm = static_cast<int>(renderer.getFontAscenderSize(fontId) * blockStyle.fontSizeMultiplier + 0.5f);
    blockStyle.textIndent = oneEm;
    blockStyle.textIndentDefined = true;
  }
}

void ParsedText::applyBionicReadingTransform() {
  // Only transform words that haven't been processed yet.  On a fresh block
  // bionicTransformedUpTo_ == 0 so all words are processed.  After an
  // intermediate flush, only the new raw words appended since the last flush
  // (indices bionicTransformedUpTo_..words.size()-1) need transformation.
  if (words.empty() || bionicTransformedUpTo_ >= words.size()) {
    return;
  }

  const size_t suffixStart = bionicTransformedUpTo_;
  std::vector<std::string> transformedSuffix;
  std::vector<EpdFontFamily::Style> transformedSuffixStyles;
  std::vector<TokenBoundary::Boundary> transformedSuffixBoundaries;
  std::vector<TextBlock::WordSize> transformedSuffixSizes;
  // Each original word became one or two transformed words; both halves keep the word's offset.
  std::vector<uint32_t> transformedSuffixOffsets;
  transformedSuffix.reserve((words.size() - suffixStart) * 2);
  transformedSuffixStyles.reserve(transformedSuffix.capacity());
  transformedSuffixBoundaries.reserve(transformedSuffix.capacity());
  transformedSuffixSizes.reserve(transformedSuffix.capacity());
  transformedSuffixOffsets.reserve(transformedSuffix.capacity());

  for (size_t i = suffixStart; i < words.size(); ++i) {
    std::string source = std::move(words[i]);
    const auto originalStyle = wordStyles[i];
    const TokenBoundary::Boundary originalBoundary = wordBoundaries[i];
    const TextBlock::WordSize originalSize = wordSizes[i];
    const uint32_t originalOffset = wordVisibleOffsets[i];

    const auto spans = tokenizeBionicWord(source);
    if (spans.empty()) {
      continue;
    }

    // The first span keeps the word's own boundary; every span after it is glued to the one before.
    // A span of a ruby base after the first belongs to the base's group.
    // Every span keeps the word's emphasis mark.
    TokenBoundary::Boundary kGlued =
        TokenBoundary::withEmphasis(TokenBoundary::Boundary{true, false}, TokenBoundary::emphasisOf(originalBoundary));
    if (TokenBoundary::rubyRole(originalBoundary) != TokenBoundary::RubyRole::None) {
      kGlued = TokenBoundary::withRubyRole(kGlued, TokenBoundary::RubyRole::Follower);
    }
    TokenBoundary::Boundary boundary = originalBoundary;
    for (size_t spanIndex = 0; spanIndex < spans.size(); ++spanIndex) {
      const TokenSpan span = spans[spanIndex];
      const size_t spanLength = span.end - span.start;
      std::string token = source.substr(span.start, spanLength);

      if (span.isWord) {
        const unsigned char* ptr = reinterpret_cast<const unsigned char*>(token.c_str());
        int codepointCount = 0;
        while (utf8NextCodepoint(&ptr)) {
          codepointCount++;
        }

        if (codepointCount >= kBionicReadingMinCodepoints) {
          const int boldPrefixCount = computeBionicBoldPrefixCount(codepointCount);
          ptr = reinterpret_cast<const unsigned char*>(token.c_str());
          const unsigned char* prefixEnd = ptr;
          for (int j = 0; j < boldPrefixCount && *prefixEnd; ++j) {
            utf8NextCodepoint(&prefixEnd);
          }
          const size_t prefixByteCount =
              static_cast<size_t>(prefixEnd - reinterpret_cast<const unsigned char*>(token.c_str()));
          if (prefixByteCount < token.size()) {
            std::string suffix(reinterpret_cast<const char*>(prefixEnd), token.size() - prefixByteCount);
            token.resize(prefixByteCount);
            const auto boldStyle = static_cast<EpdFontFamily::Style>(originalStyle | EpdFontFamily::BOLD);
            transformedSuffix.push_back(std::move(token));
            transformedSuffixStyles.push_back(boldStyle);
            transformedSuffixBoundaries.push_back(boundary);
            transformedSuffixSizes.push_back(originalSize);
            transformedSuffixOffsets.push_back(originalOffset);

            transformedSuffix.push_back(std::move(suffix));
            transformedSuffixStyles.push_back(originalStyle);
            transformedSuffixBoundaries.push_back(kGlued);
            transformedSuffixSizes.push_back(originalSize);
            transformedSuffixOffsets.push_back(originalOffset);
            boundary = kGlued;
            continue;
          }
        }
      }

      transformedSuffix.push_back(std::move(token));
      transformedSuffixStyles.push_back(originalStyle);
      transformedSuffixBoundaries.push_back(boundary);
      transformedSuffixSizes.push_back(originalSize);
      transformedSuffixOffsets.push_back(originalOffset);
      boundary = kGlued;
    }
  }

  // Replace the (now move-emptied) suffix with the transformed version.
  words.resize(suffixStart);
  wordStyles.resize(suffixStart);
  wordBoundaries.resize(suffixStart);
  wordSizes.resize(suffixStart);
  wordVisibleOffsets.resize(suffixStart);
  words.insert(words.end(), std::make_move_iterator(transformedSuffix.begin()),
               std::make_move_iterator(transformedSuffix.end()));
  wordStyles.insert(wordStyles.end(), transformedSuffixStyles.begin(), transformedSuffixStyles.end());
  wordBoundaries.insert(wordBoundaries.end(), transformedSuffixBoundaries.begin(), transformedSuffixBoundaries.end());
  wordSizes.insert(wordSizes.end(), transformedSuffixSizes.begin(), transformedSuffixSizes.end());
  wordVisibleOffsets.insert(wordVisibleOffsets.end(), transformedSuffixOffsets.begin(), transformedSuffixOffsets.end());
  bionicTransformedUpTo_ = words.size();
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
void ParsedText::computeHyphenatedLineBreaks(
    const GfxRenderer& renderer, const int fontId, const int pageWidth, std::vector<uint16_t>& wordWidths,
    const std::vector<TokenBoundary::Boundary>& boundaries, std::vector<bool>& lineEndsWithHyphenatedWord,
    std::vector<int>& splitPrefixWordIndexes, std::vector<bool>& splitInsertedHyphen, const int firstLineIndent,
    const int16_t blockStartY, const int lineHeight, std::vector<size_t>& lineBreakIndices) {
  lineBreakIndices.clear();
  // Pre-compute inter-word gaps to avoid repeated codepoint scanning and renderer
  // calls in the inner loop. When hyphenateWordAtIndex inserts a new word, we insert
  // a placeholder gap (0) at that position to keep the vector in sync; the remainder
  // is always the first word on the next line so its spacing is never used.
  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(wordWidths.size(), 0);
  for (size_t j = 1; j < wordWidths.size(); ++j) {
    interWordGaps[j] = naturalGapBefore(renderer, fontId, j);
  }

  lineEndsWithHyphenatedWord.clear();
  splitPrefixWordIndexes.clear();
  splitInsertedHyphen.clear();
  size_t currentIndex = 0;
  int lineIdx = 0;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    int lineShrink = 0;  // what the line's marks and gaps may give mid-line (push-in)
    bool lineEndedWithHyphenation = false;
    int splitPrefixIndex = -1;
    bool splitNeedsInsertedHyphen = false;

    const int effectivePageWidth =
        widthForLine(lineIdx, lineHeight, blockStartY, pageWidth) - (lineIdx == 0 ? firstLineIndent : 0);

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord ? 0 : interWordGaps[currentIndex];
      const int candidateWidth = spacing + wordWidths[currentIndex];
      const int candidateShrink = shrink_[currentIndex] + (spacing > 0 ? gapShrinkBefore(currentIndex, spacing) : 0);

      // Word fits on current line, pushed in if need be
      const int candidateLen = withEdgeTrims(lineWidth + candidateWidth, lineStart, currentIndex);
      if (candidateLen <= effectivePageWidth || pushedInWidth(lineWidth + candidateWidth, lineShrink + candidateShrink,
                                                              lineStart, currentIndex) <= effectivePageWidth) {
        lineWidth += candidateWidth;
        lineShrink += candidateShrink;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points. What the line's first token grows by at the
      // line start (a ruby's overhang held inside it) is no room for the prefix. The prefix is measured against the
      // line at its natural width: a hyphenated line never relies on push-in.
      const int availableWidth = effectivePageWidth - lineWidth - spacing - std::max(0, lineStartTrim(lineStart));
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      bool insertedHyphen = false;
      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths,
                                                     allowFallbackBreaks, &insertedHyphen)) {
        // Keep interWordGaps in sync: insert placeholder for the new remainder word.
        // The remainder is always the first word on the next line so this slot is never read.
        interWordGaps.insert(interWordGaps.begin() + currentIndex + 1, 0);
        lineEndedWithHyphenation = true;
        splitPrefixIndex = static_cast<int>(currentIndex);
        splitNeedsInsertedHyphen = insertedHyphen;
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() &&
           !TokenBoundary::allowsBreak(boundaries[currentIndex])) {
      --currentIndex;
    }

    if (lineEndedWithHyphenation &&
        (splitPrefixIndex < static_cast<int>(lineStart) || splitPrefixIndex >= static_cast<int>(currentIndex))) {
      lineEndedWithHyphenation = false;
      splitPrefixIndex = -1;
      splitNeedsInsertedHyphen = false;
    }

    lineBreakIndices.push_back(currentIndex);
    lineEndsWithHyphenatedWord.push_back(lineEndedWithHyphenation);
    splitPrefixWordIndexes.push_back(splitPrefixIndex);
    splitInsertedHyphen.push_back(splitNeedsInsertedHyphen);
    ++lineIdx;
  }

  return;
}

void ParsedText::computeHyphenatedLineBreaksFromIndex(
    const GfxRenderer& renderer, const int fontId, const int pageWidth, std::vector<uint16_t>& wordWidths,
    const std::vector<TokenBoundary::Boundary>& boundaries, const size_t startIndex,
    std::vector<bool>& lineEndsWithHyphenatedWord, std::vector<int>& splitPrefixWordIndexes,
    std::vector<bool>& splitInsertedHyphen, const int16_t blockStartY, const int lineHeight, const int startLineIdx,
    std::vector<size_t>& lineBreakIndices) {
  // Same greedy hyphenating breaker as the full pass, but scoped to a suffix.
  lineBreakIndices.clear();
  if (startIndex >= wordWidths.size()) {
    lineEndsWithHyphenatedWord.clear();
    splitPrefixWordIndexes.clear();
    splitInsertedHyphen.clear();
    return;
  }

  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(wordWidths.size(), 0);
  for (size_t j = 1; j < wordWidths.size(); ++j) {
    interWordGaps[j] = naturalGapBefore(renderer, fontId, j);
  }

  lineEndsWithHyphenatedWord.clear();
  splitPrefixWordIndexes.clear();
  splitInsertedHyphen.clear();

  size_t currentIndex = startIndex;
  int lineIdx = startLineIdx;
  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    int lineShrink = 0;  // what the line's marks and gaps may give mid-line (push-in)
    bool lineEndedWithHyphenation = false;
    int splitPrefixIndex = -1;
    bool splitNeedsInsertedHyphen = false;
    const int effectivePageWidth = widthForLine(lineIdx, lineHeight, blockStartY, pageWidth);

    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord ? 0 : interWordGaps[currentIndex];
      const int candidateWidth = spacing + wordWidths[currentIndex];
      const int candidateShrink = shrink_[currentIndex] + (spacing > 0 ? gapShrinkBefore(currentIndex, spacing) : 0);

      const int candidateLen = withEdgeTrims(lineWidth + candidateWidth, lineStart, currentIndex);
      if (candidateLen <= effectivePageWidth || pushedInWidth(lineWidth + candidateWidth, lineShrink + candidateShrink,
                                                              lineStart, currentIndex) <= effectivePageWidth) {
        lineWidth += candidateWidth;
        lineShrink += candidateShrink;
        ++currentIndex;
        continue;
      }

      const int availableWidth = effectivePageWidth - lineWidth - spacing - std::max(0, lineStartTrim(lineStart));
      const bool allowFallbackBreaks = isFirstWord;

      bool insertedHyphen = false;
      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths,
                                                     allowFallbackBreaks, &insertedHyphen)) {
        interWordGaps.insert(interWordGaps.begin() + currentIndex + 1, 0);
        lineEndedWithHyphenation = true;
        splitPrefixIndex = static_cast<int>(currentIndex);
        splitNeedsInsertedHyphen = insertedHyphen;
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() &&
           !TokenBoundary::allowsBreak(boundaries[currentIndex])) {
      --currentIndex;
    }

    if (lineEndedWithHyphenation &&
        (splitPrefixIndex < static_cast<int>(lineStart) || splitPrefixIndex >= static_cast<int>(currentIndex))) {
      lineEndedWithHyphenation = false;
      splitPrefixIndex = -1;
      splitNeedsInsertedHyphen = false;
    }

    lineBreakIndices.push_back(currentIndex);
    lineEndsWithHyphenatedWord.push_back(lineEndedWithHyphenation);
    splitPrefixWordIndexes.push_back(splitPrefixIndex);
    splitInsertedHyphen.push_back(splitNeedsInsertedHyphen);
    ++lineIdx;
  }

  return;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks, bool* outInsertedHyphen) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }
  // A ruby base is never split: its ruby sits over the whole of it.
  if (TokenBoundary::rubyRole(wordBoundaries[wordIndex]) != TokenBoundary::RubyRole::None) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];
  const float scale = wordScale(wordIndex);
  const int font = wordFontId(wordIndex, fontId);

  // Collect candidate breakpoints (byte offsets and hyphen requirements). A Hangul word may split
  // between syllables at a line end, without a hyphen; those breaks come first, so they win a width
  // tie. From crosspoint-reader PR #3700 ("fix: justify Korean text by word spaces only", Sung-jin
  // Brian Hong).
  std::vector<Hyphenator::BreakInfo> breakInfos = CjkLineBreak::hangulLineEndBreaks(word);
  const std::vector<Hyphenator::BreakInfo> hyphenatorBreaks = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  breakInfos.insert(breakInfos.end(), hyphenatorBreaks.begin(), hyphenatorBreaks.end());
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth =
        measureWordWidth(renderer, font, word.substr(0, offset), style, needsHyphen, scale, vertical_);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style, size and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  wordSizes.insert(wordSizes.begin() + wordIndex + 1, wordSizes[wordIndex]);
  // The remainder starts where the prefix's visible bytes end (the hyphen is not source text).
  wordVisibleOffsets.insert(
      wordVisibleOffsets.begin() + wordIndex + 1,
      wordVisibleOffsets[wordIndex] +
          static_cast<uint32_t>(
              VisibleText::visibleBytes(words[wordIndex].data(), static_cast<int>(words[wordIndex].size())) -
              (chosenNeedsHyphen ? 1 : 0)));

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordBoundaries[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  // The remainder keeps the word's emphasis mark.
  wordBoundaries.insert(
      wordBoundaries.begin() + wordIndex + 1,
      TokenBoundary::withEmphasis(TokenBoundary::Boundary{}, TokenBoundary::emphasisOf(wordBoundaries[wordIndex])));

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, font, remainder, style, false, scale, vertical_);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  markClass_.insert(markClass_.begin() + wordIndex + 1, static_cast<uint8_t>(CjkPunctuation::Class::None));
  edgeTrim_.insert(edgeTrim_.begin() + wordIndex + 1, 0);
  shrink_.insert(shrink_.begin() + wordIndex + 1, 0);
  edgeShrink_.insert(edgeShrink_.begin() + wordIndex + 1, 0);
  // A split word is never a ruby base (refused above): its remainder holds no ruby reserve either.
  if (!rubyEdge_.empty()) rubyEdge_.insert(rubyEdge_.begin() + wordIndex + 1, RubyEdge{});
  if (outInsertedHyphen) {
    *outInsertedHyphen = chosenNeedsHyphen;
  }
  return true;
}

ParsedText::LineProcessResult ParsedText::extractLine(
    const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
    const std::vector<TokenBoundary::Boundary>& boundaries, const std::vector<size_t>& lineBreakIndices,
    const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
    const GfxRenderer& renderer, const int fontId, const bool lineEndsWithHyphenatedWord,
    const bool suppressHyphenationRetry, const int firstLineIndent, const int16_t blockStartY, const int lineHeight) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  lastLineVisibleOffset_ = lastBreakAt < wordVisibleOffsets.size() ? wordVisibleOffsets[lastBreakAt] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Apply indent only to line 0 of the layout pass; firstLineIndent is already
  // 0 for continuation flushes (computed once in layoutAndExtractLines).
  const int lineIndent = (breakIndex == 0) ? firstLineIndent : 0;

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  // A token's width on this line: mid-line, but the first and last at their line-edge widths (CJK punctuation).
  const auto boxWidth = [&](const size_t wordIdx) {
    int width = wordWidths[lastBreakAt + wordIdx];
    if (wordIdx == 0) width += lineStartTrim(lastBreakAt);
    if (wordIdx + 1 == lineWordCount) width += lineEndTrim(lastBreakAt + wordIdx);
    return width;
  };

  // A mark the line's edge acts on: the first token when it opens the line, the last when it closes it.
  const auto atLineEdge = [&](const size_t wordIdx) {
    const size_t i = lastBreakAt + wordIdx;
    return (wordIdx == 0 && opensLine(i)) || (wordIdx + 1 == lineWordCount && closesLine(i));
  };

  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  bool cjkGapOnLine = false;
  int totalNaturalGaps = 0;
  // What push-in may take off the line (spec 5.3), in the order it is taken: the line-edge marks, the other marks, the
  // CJK-Latin gaps.
  int edgeShrinkTotal = 0;
  int markShrinkTotal = 0;
  int gapShrinkTotal = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += boxWidth(wordIdx);
    if (atLineEdge(wordIdx)) {
      edgeShrinkTotal += edgeShrink_[lastBreakAt + wordIdx];
    } else {
      markShrinkTotal += shrink_[lastBreakAt + wordIdx];
    }
    // Count gaps: each word after the first creates a gap, unless it's a continuation.
    // Gaps before closing punctuation (. , ) » etc.) are excluded from justification
    // distribution so they stay at natural space width.
    const uint32_t firstCp = firstCodepoint(words[lastBreakAt + wordIdx]);
    const TokenBoundary::Boundary boundary = boundaries[lastBreakAt + wordIdx];
    if (wordIdx > 0 && !boundary.continues) {
      // A word gap or a CJK gap: both stretch when justified. A word gap is a space wide, a CJK gap has no width but
      // the CJK-Latin gap's.
      const bool beforeClosing = isClosingPunctuation(firstCp);
      if (!beforeClosing) actualGapCount++;
      if (boundary.noSpaceBefore) cjkGapOnLine = true;
      if (TokenBoundary::hasSpace(boundary)) {
        // Inter-word gaps scale with the effective size of the word whose space it is (spaceSource).
        const size_t j = lastBreakAt + wordIdx;
        totalNaturalGaps +=
            static_cast<int>(spaceBefore(renderer, fontId, j) * wordScale(spaceSource(fontId, j)) + 0.5f);
      } else {
        // The CJK-Latin gap is already scaled to its CJK side.
        const int gap = cjkLatinGapBefore(renderer, fontId, lastBreakAt + wordIdx);
        totalNaturalGaps += gap;
        gapShrinkTotal += gapShrinkBefore(lastBreakAt + wordIdx, gap);
      }
    } else if (wordIdx > 0) {
      // Non-breaking space tokens (" " with continues=true) are visible, stretchable spaces —
      // count them as justifiable gaps so justifyExtra is distributed to them too.
      if (words[lastBreakAt + wordIdx] == " ") {
        actualGapCount++;
      }
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps += static_cast<int>(
          kernBefore(renderer, fontId, lastBreakAt + wordIdx) * wordScale(lastBreakAt + wordIdx - 1) + 0.5f);
    }
  }

  // Calculate spacing (account for float zone narrowing + first-line indent)
  const int effectivePageWidth =
      widthForLine(static_cast<int>(breakIndex), lineHeight, blockStartY, pageWidth) - lineIndent;
  // A line is only truly last when it consumes all paragraph words.
  // During single-line retry we may temporarily pass a truncated break vector,
  // so relying only on breakIndex would incorrectly disable justification.
  const bool isLastLine = lineBreak == words.size();

  // Push-in: the line gives what its natural width overruns the measure by, as far as it can (an overfull line that
  // cannot be saved still gives everything). The line-edge marks are half or full, nothing in between (spec 5.2), so
  // they give all of theirs as soon as the line needs anything; the rest comes from the other marks, then from the
  // CJK-Latin gaps (JLREQ 3.8.3). What the line-edge marks give beyond the need is spread by justification.
  const int naturalWidth = lineWordWidthSum + totalNaturalGaps;
  const int need = std::clamp(naturalWidth - effectivePageWidth, 0, edgeShrinkTotal + markShrinkTotal + gapShrinkTotal);
  const int edgeGives = need > 0 ? edgeShrinkTotal : 0;
  const int marksGive = std::min(std::max(0, need - edgeGives), markShrinkTotal);
  const int gapsGive = std::min(std::max(0, need - edgeGives - marksGive), gapShrinkTotal);
  const int lineWidth = naturalWidth - edgeGives - marksGive - gapsGive;

  // For justified text, compute per-gap extra to distribute remaining space evenly.
  // Cap stretch to effectivePageWidth/8 per gap to prevent rivers in sparse lines.
  const int spareSpace = effectivePageWidth - lineWidth;
  const bool justified = blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1;
  const int justifyExtra =
      justified ? std::min(spareSpace / static_cast<int>(actualGapCount), effectivePageWidth / 8) : 0;
  // A line with CJK gaps ends flush: what the even share leaves over goes 1 px each to evenly spaced gaps
  // (spec 5.1). Latin lines keep the old rounding, so their layout does not move.
  const int justifyRemainder = justified && cjkGapOnLine && justifyExtra < effectivePageWidth / 8 && spareSpace > 0
                                   ? spareSpace - justifyExtra * static_cast<int>(actualGapCount)
                                   : 0;
  int stretchedGaps = 0;
  const auto gapExtra = [&]() {
    const int g = stretchedGaps++;
    const int gaps = static_cast<int>(actualGapCount);
    if (gaps == 0) return justifyExtra;
    return justifyExtra + ((g + 1) * justifyRemainder / gaps - g * justifyRemainder / gaps);
  };

  // Calculate initial x position (first line starts at indent for left/justified text;
  // may be negative for hanging indents, e.g. margin-left:3em; text-indent:-1em).
  auto xpos = static_cast<int16_t>(lineIndent + gridInset_);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = gridInset_ + effectivePageWidth - lineWidth;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = gridInset_ + (effectivePageWidth - lineWidth) / 2;
  }

  // What push-in takes off token wordIdx's box and off the CJK gap before it, called in line order by the x loop
  // below: a line-edge mark gives all it can, the other marks and the gaps their shares of what their tier gives.
  int marksBefore = 0;  // the shrinks of the other marks before this one
  int gapsBefore = 0;   // and of the gaps
  const auto markGive = [&](const size_t wordIdx) {
    if (need == 0) return 0;
    const size_t i = lastBreakAt + wordIdx;
    if (atLineEdge(wordIdx)) return static_cast<int>(edgeShrink_[i]);
    const int give = pushInShare(marksBefore, shrink_[i], marksGive, markShrinkTotal);
    marksBefore += shrink_[i];
    return give;
  };
  const auto gapGive = [&](const size_t wordIdx, const int gap) {
    if (need == 0) return 0;
    const int amount = gapShrinkBefore(lastBreakAt + wordIdx, gap);
    const int give = pushInShare(gapsBefore, amount, gapsGive, gapShrinkTotal);
    gapsBefore += amount;
    return give;
  };

  // A narrowed CJK mark is drawn where its ink sits: ink in the right half of its em (an opening bracket) moves
  // left by what the mark lost, centred ink by half of it, ink in the left half stays (spec 5.2). Down a column the
  // ink of an upright mark is where the draw places it (columnInk: a 、。 in its cell's corner, centred in Traditional
  // Chinese); a sideways one is turned, so its ink across is its ink along the column.
  const bool centredMarks = typography_.punctuation == TypographyProfile::Punctuation::TraditionalChinese;
  const auto inkShift = [&](const size_t wordIdx, const int box) {
    const size_t i = lastBreakAt + wordIdx;
    const auto* p = reinterpret_cast<const unsigned char*>(words[i].c_str());
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0 || *p != 0 || CjkPunctuation::classOf(cp) == CjkPunctuation::Class::None) return 0;
    const int font = wordFontId(i, fontId);
    const int natural = measureWordWidth(renderer, font, words[i], wordStyles[i], false, wordScale(i), vertical_);
    if (box >= natural) return 0;
    int inkStart = 0;
    int inkSize = 0;
    int cell = 0;
    if (!vertical_ || !columnInk(renderer, font, cp, wordStyles[i], centredMarks, inkStart, inkSize, cell)) {
      GlyphInk ink;
      if (!renderer.getGlyphInk(font, cp, wordStyles[i], ink)) return 0;
      inkStart = ink.left;
      inkSize = ink.width;
      cell = ink.advance;
    }
    if (cell <= 0) return 0;
    const int removed = natural - box;
    if (2 * inkStart >= cell) return removed;
    if (2 * (inkStart + inkSize) <= cell) return 0;
    return removed / 2;
  };

  // One ruby box per group leader on the line, over the leader and the followers the line holds. A follower with no
  // leader on its line (a group the breaker had to split) has nothing to hang its ruby on and is skipped. The box is
  // the group's base, or the whole ruby where that is longer, overhanging the base's left by the leader's `start`:
  // over the neighbour mid-line, inside the leader's own box when the group opens the line.
  lineAnnotations_.clear();
  size_t rubyOrdinal = rubyTexts_.empty() ? 0 : leadersBefore(lastBreakAt);
  const auto addRubyBox = [&](const size_t wordIdx, const int boxLeft) {
    if (rubyOrdinal >= rubyTexts_.size()) return;
    const std::string& text = rubyTexts_[rubyOrdinal++];
    const size_t leader = lastBreakAt + wordIdx;
    const int start = rubyEdge_.empty() ? 0 : rubyEdge_[leader].start;
    size_t groupCount = 1;
    int base = wordWidths[leader];
    while (wordIdx + groupCount < lineWordCount &&
           TokenBoundary::rubyRole(boundaries[leader + groupCount]) == TokenBoundary::RubyRole::Follower) {
      base += wordWidths[leader + groupCount];
      ++groupCount;
    }
    const int baseLeft = boxLeft + (wordIdx == 0 ? start : 0);
    const int ruby = rubyWidth(renderer, fontId, leader, text);
    TextBlock::Annotation& a = lineAnnotations_.emplace_back();
    a.firstWord = static_cast<uint16_t>(wordIdx);
    a.wordCount = static_cast<uint16_t>(groupCount);
    a.x = static_cast<int16_t>(ruby <= base ? baseLeft : baseLeft - start);
    a.width = static_cast<uint16_t>(std::max(base, ruby));
    a.kind = TextBlock::AnnotationKind::Ruby;
    a.text = text;
  };

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::vector<int16_t>& lineXPos = lineXPosScratch_;
  lineXPos.clear();  // keeps the capacity: the point is to stop reallocating per line
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    const int box = boxWidth(wordIdx) - markGive(wordIdx);
    // A base character widened by its ruby is drawn into its share (lead); the line's first token moves in by the
    // ruby overhang its box holds (start).
    int rubyShift = 0;
    if (!rubyEdge_.empty()) {
      const RubyEdge& edge = rubyEdge_[lastBreakAt + wordIdx];
      rubyShift = edge.lead + (wordIdx == 0 ? edge.start : 0);
    }
    if (!rubyTexts_.empty() &&
        TokenBoundary::rubyRole(boundaries[lastBreakAt + wordIdx]) == TokenBoundary::RubyRole::Leader) {
      addRubyBox(wordIdx, xpos);
    }
    lineXPos.push_back(static_cast<int16_t>(xpos - inkShift(wordIdx, box) + rubyShift));

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && boundaries[lastBreakAt + wordIdx + 1].continues;
    if (nextIsContinuation) {
      int advance = box;
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      advance += static_cast<int>(
          kernBefore(renderer, fontId, lastBreakAt + wordIdx + 1) * wordScale(lastBreakAt + wordIdx) + 0.5f);
      // Non-breaking space tokens are stretchable — expand them during justification like normal spaces.
      if (words[lastBreakAt + wordIdx] == " " && boundaries[lastBreakAt + wordIdx].continues &&
          blockStyle.alignment == CssTextAlign::Justify && !isLastLine) {
        advance += gapExtra();
      }
      xpos += advance;
    } else {
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
        const uint32_t nextFirstCp = firstCodepoint(words[lastBreakAt + wordIdx + 1]);
        // A CJK gap has no natural width but the CJK-Latin gap; it takes the justification share below.
        if (TokenBoundary::hasSpace(boundaries[lastBreakAt + wordIdx + 1])) {
          const size_t j = lastBreakAt + wordIdx + 1;
          gap = static_cast<int>(spaceBefore(renderer, fontId, j) * wordScale(spaceSource(fontId, j)) + 0.5f);
        } else {
          gap = gapShrinkTotal > 0 ? cjkLatinGapBefore(renderer, fontId, lastBreakAt + wordIdx + 1) : 0;
          gap -= gapGive(wordIdx + 1, gap);
        }
        // Don't stretch the gap before closing punctuation — it looks wrong with
        // extra space before ".", ")", "»" etc.
        const bool nextIsClosing = isClosingPunctuation(nextFirstCp);
        if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && !nextIsClosing) {
          gap += gapExtra();
        }
      }
      xpos += box + gap;
    }
  }

  // One Emphasis record per run of consecutive tokens with the same mark, outside ruby groups (ruby wins over a mark
  // on the same base). The records join the ruby boxes in firstWord order; the runs and boxes never overlap.
  const size_t rubyRecords = lineAnnotations_.size();
  for (size_t wordIdx = 0; wordIdx < lineWordCount;) {
    const TokenBoundary::Boundary b = boundaries[lastBreakAt + wordIdx];
    const uint8_t mark = TokenBoundary::emphasisOf(b);
    if (mark == 0 || TokenBoundary::rubyRole(b) != TokenBoundary::RubyRole::None) {
      ++wordIdx;
      continue;
    }
    size_t end = wordIdx + 1;
    while (end < lineWordCount && TokenBoundary::emphasisOf(boundaries[lastBreakAt + end]) == mark &&
           TokenBoundary::rubyRole(boundaries[lastBreakAt + end]) == TokenBoundary::RubyRole::None) {
      ++end;
    }
    // A run with no character that takes a mark (only punctuation or spaces) draws nothing: no record, so no band.
    bool markable = false;
    for (size_t t = wordIdx; t < end && !markable; ++t) {
      for (const auto* q = reinterpret_cast<const unsigned char*>(words[lastBreakAt + t].c_str()); *q && !markable;) {
        markable = TextEmphasis::takesMark(utf8NextCodepoint(&q));
      }
    }
    if (!markable) {
      wordIdx = end;
      continue;
    }
    TextBlock::Annotation& a = lineAnnotations_.emplace_back();
    a.firstWord = static_cast<uint16_t>(wordIdx);
    a.wordCount = static_cast<uint16_t>(end - wordIdx);
    a.kind = TextBlock::AnnotationKind::Emphasis;
    a.mark = mark;
    wordIdx = end;
  }
  if (rubyRecords != 0 && lineAnnotations_.size() > rubyRecords) {
    std::sort(lineAnnotations_.begin(), lineAnnotations_.end(),
              [](const TextBlock::Annotation& l, const TextBlock::Annotation& r) { return l.firstWord < r.firstWord; });
  }

  // Address the line's words in place. The source is left intact so retry paths can still
  // inspect/merge tokens, and TextBlock copies straight from here into its arena — stripping
  // soft hyphens as it goes — so a line costs one allocation instead of four.
  TextBlock::WordRange range;
  range.words = &words;
  range.styles = &wordStyles;
  range.sizes = &wordSizes;
  // Carried into the block so a reader-side consumer can tell one logical word from the
  // fragments layout split it into (bionic-reading halves, attached punctuation). Nothing on
  // the render path reads it; the xpos above already encodes the spacing.
  range.boundaries = &boundaries;
  range.fontTable = fontSlots_;
  range.first = lastBreakAt;
  range.count = lineWordCount;
  range.annotations = &lineAnnotations_;

  // The page-fit decision runs before the line exists (see beforeLine_): the parser may emit
  // the current page here and open the next one's arena block, so the block built just below
  // is allocated from the page it will actually land on.
  if (beforeLine_) {
    uint8_t maxPct = 100;
    for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
      maxPct = std::max(maxPct, wordSizes[lastBreakAt + wordIdx].pct);
    }
    beforeLine_(maxPct, !lineAnnotations_.empty());
  }

  // TextBlock flattens the range into its arena on construct; on arena OOM the
  // block is invalid, so drop the line rather than render/serialize garbage.
  auto block = makeUniqueNoThrow<TextBlock>(range, lineXPos, blockStyle, lineArena_);
  if (!block || !block->valid()) {
    LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
    return LineProcessResult::Accepted;
  }
  return processLine(std::move(block), lineEndsWithHyphenatedWord, suppressHyphenationRetry);
}
