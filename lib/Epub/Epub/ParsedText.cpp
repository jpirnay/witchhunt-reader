#include "ParsedText.h"

#include <ArabicScripts.h>
#include <Arduino.h>
#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <IndicScripts.h>
#include <IndicShaper.h>
#include <Logging.h>
#include <Memory.h>
#include <TextShaping.h>
#include <Utf8.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <vector>

#include "CjkLineBreak.h"
#include "CjkPunctuation.h"
#include "Epub/VisibleText.h"
#include "HashUtils.h"
#include "HeapGrowth.h"
#include "ShapedStore.h"
#include "TextEmphasis.h"
#include "VerticalDraw.h"
#include "VerticalOrientation.h"
#include "WordShapeCache.h"
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

// May a line end between a dash and what follows it? Not inside a run of dashes, not before punctuation that closes
// the word, and not where a hyphen introduces a number (a phone number, a sign).
bool mayBreakAfterDash(const uint32_t dash, const uint32_t next) {
  if (!TokenBoundary::isBreakableDash(dash) || TokenBoundary::isBreakableDash(next)) return false;
  if (isClosingPunctuation(next)) return false;
  const bool digitFollows = next >= '0' && next <= '9';
  return !(digitFollows && (dash == '-' || dash == 0x2010));
}

bool isDashesOnly(const std::string_view word) {
  const auto* p = reinterpret_cast<const unsigned char*>(word.data());
  const auto* const end = p + word.size();
  while (p < end) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!TokenBoundary::isBreakableDash(cp) && !utf8IsDefaultIgnorable(cp)) return false;
  }
  return true;
}

// Where a line may end inside `word`: after a dash with text before it, whatever follows allowing. A dash that opens
// the word stays on it ("—Well"), unless the word is glued to the one before it. Most words hold neither a '-' nor a
// multi-byte dash, and only those that do are walked.
std::vector<size_t> dashBreakByteOffsets(const std::string& word, const bool gluedToPrevious) {
  std::vector<size_t> offsets;
  if (word.find('-') == std::string::npos && word.find('\xE2') == std::string::npos) return offsets;
  const auto* const begin = reinterpret_cast<const unsigned char*>(word.c_str());
  const auto* p = begin;
  bool textBefore = gluedToPrevious;
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (!TokenBoundary::isBreakableDash(cp)) {
      if (!utf8IsDefaultIgnorable(cp)) textBefore = true;
      continue;
    }
    if (!textBefore || *p == 0) continue;
    const auto* q = p;
    const uint32_t next = utf8NextCodepoint(&q);
    if (mayBreakAfterDash(cp, next)) offsets.push_back(static_cast<size_t>(p - begin));
  }
  return offsets;
}

// SOFT_HYPHEN_UTF8 / SOFT_HYPHEN_BYTES come from TextBlock.h — the arena fill strips the same
// pattern this file measures around, so both sides share one definition.

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const char* word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word);
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}
uint32_t firstCodepoint(const std::string& word) { return firstCodepoint(word.c_str()); }

// Where the last UTF-8 sequence of a non-empty word starts: UTF-8 continuation bytes start with 10xxxxxx, so the scan
// goes back over every one of them to the leading byte.
size_t lastSequenceStart(const std::string_view word) {
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  return i;
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string_view word) {
  if (word.empty()) return 0;
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.data() + lastSequenceStart(word));
  return utf8NextCodepoint(&ptr);
}

// firstCodepoint and lastCodepoint past the controls that count nowhere (BidiLine::isUncountedControl), for the reads
// addWord makes before the paragraph's pass: one that needs no bidi drops those controls, and what it lays out must
// not depend on them.
uint32_t firstCodepointPastControls(const char* word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word);
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD && !BidiLine::isUncountedControl(cp)) return cp;
  }
}

uint32_t lastCodepointPastControls(std::string_view word) {
  while (!word.empty()) {
    const uint32_t cp = lastCodepoint(word);
    if (!BidiLine::isUncountedControl(cp)) return cp;
    // Cut at the control's lead byte, where the scan found it: in malformed text the stray continuation bytes after a
    // control go with it, so what it spans is not a fixed 2 or 3 bytes.
    word = word.substr(0, lastSequenceStart(word));
  }
  return 0;
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Whether `text` is soft hyphens and nothing else: TextBlock strips them, so as a word of its own it would be empty.
bool onlySoftHyphens(std::string_view text) {
  while (text.starts_with(SOFT_HYPHEN_UTF8)) text.remove_prefix(SOFT_HYPHEN_BYTES);
  return text.empty();
}

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

// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
int textAdvance(const GfxRenderer& renderer, const int fontId, const char* text, const EpdFontFamily::Style style,
                const bool vertical) {
  return vertical ? verticalTextAdvance(renderer, fontId, text, style, 1.0f)
                  : renderer.getTextAdvanceX(fontId, text, style);
}

uint16_t scaledWidth(const int raw, const float scale) {
  if (scale == 1.0f) return static_cast<uint16_t>(raw);
  return static_cast<uint16_t>(raw * scale + 0.5f);
}

// The advance width of `text`, which holds no soft hyphen (a drawn form, a literal), measured in place: nothing is
// copied. A lone space is the font's space width.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const char* text,
                          const EpdFontFamily::Style style, const float scale, const bool vertical) {
  const bool space = text[0] == ' ' && text[1] == '\0';
  return scaledWidth(
      space ? renderer.getSpaceWidth(fontId, style) : textAdvance(renderer, fontId, text, style, vertical), scale);
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false, const float scale = 1.0f,
                          const bool vertical = false) {
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) return measureWordWidth(renderer, fontId, word.c_str(), style, scale, vertical);
  std::string sanitized = word;
  if (hasSoftHyphen) stripSoftHyphensInPlace(sanitized);
  if (appendHyphen) sanitized.push_back('-');
  return scaledWidth(textAdvance(renderer, fontId, sanitized.c_str(), style, vertical), scale);
}

uint16_t clampedWidth(const int width) { return static_cast<uint16_t>(std::clamp(width, 0, UINT16_MAX)); }

// Removes the joiners (U+200C, U+200D) from `text`.
void dropJoiners(std::string& text) {
  for (size_t pos = text.find("\xE2\x80"); pos != std::string::npos; pos = text.find("\xE2\x80", pos)) {
    if (pos + 2 < text.size() && (text[pos + 2] == '\x8C' || text[pos + 2] == '\x8D')) {
      text.erase(pos, 3);
    } else {
      pos += 2;
    }
  }
}

// What the line pass draws of a Text piece's bytes (BidiLine::Line::display): them without the bidi controls and the
// joiners (TextBlock strips the soft hyphens).
void dropUndrawn(std::string& text) {
  if (text.find_first_of("\xE2\xD8") == std::string::npos) return;  // every control and joiner leads with one
  BidiLine::dropControls(text);
  dropJoiners(text);
}

// A piece of a token (whose bytes are `text`) measured as the line draws it, in the token's font, style and scale: a
// glyph run by its records' pen (glyphRunWidth), any other piece as its code points are measured today, a Text piece
// of a paragraph that needs bidi (`bidi`) without the controls and joiners its line does not draw. `scratch` takes the
// piece's bytes, NUL-terminated.
uint16_t measurePiece(const GfxRenderer& renderer, const int font, const EpdFontFamily::Style style, const float scale,
                      const bool vertical, const ShapedStore& store, const ShapedStore::Piece& piece,
                      const std::string_view text, const bool bidi, std::string& scratch) {
  if (piece.kind == IndicShaper::PieceKind::Glyphs) {
    return clampedWidth(renderer.glyphRunWidth(font, store.glyphs(piece), scale));
  }
  if (piece.kind == IndicShaper::PieceKind::Reordered) {
    scratch.assign(store.form(piece));
  } else {
    scratch.assign(text.substr(piece.begin, piece.end - piece.begin));
    if (bidi) dropUndrawn(scratch);
  }
  return measureWordWidth(renderer, font, scratch, style, false, scale, vertical);
}

// The bytes measurePiece copies into its scratch for the longest of `pieces` (a glyph run copies none).
size_t scratchBytesFor(const ShapedStore& store, const std::span<const ShapedStore::Piece> pieces) {
  size_t bytes = 0;
  for (const ShapedStore::Piece& piece : pieces) {
    if (piece.kind == IndicShaper::PieceKind::Reordered) {
      bytes = std::max(bytes, store.form(piece).size());
    } else if (piece.kind == IndicShaper::PieceKind::Text) {
      bytes = std::max<size_t>(bytes, piece.end - piece.begin);
    }
  }
  return bytes;
}

// The shaped layout's containers grow behind a heap check, as ShapedStore's and the line pass's do (HeapGrowth.h):
// under -fno-exceptions a growth the heap refuses aborts.
using heap_growth::growthBytes;
using heap_growth::heapAllows;

// FNV-1a over a value's bytes, folded into a running key (`key` starts at kKeySeed).
constexpr uint32_t kKeySeed = 2166136261u;
template <typename T>
uint32_t mixKey(uint32_t key, const T& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  for (size_t i = 0; i < sizeof(T); ++i) {
    key ^= bytes[i];
    key *= 16777619u;
  }
  return key;
}

// Every bidi control is E2 80 xx, E2 81 xx or D8 9C in UTF-8: a token without those lead bytes holds none.
bool mayHoldBidiControl(const std::string_view token) { return token.find_first_of("\xE2\xD8") != std::string::npos; }

// VisibleText::visibleBytes of the `len` bytes at `text`, less those of every control that counts nowhere
// (BidiLine::isUncountedControl) and every joiner the parser kept (U+200C, U+200D: it keeps only one that stands lone
// after a letter, and no word's offset starts at it): the offsets of a split word's parts are those of the word without
// them (KOSync positions). A literal isolate counts as text.
size_t visibleBytesOutsideControls(const char* text, const int len) {
  size_t count = VisibleText::visibleBytes(text, len);
  if (!mayHoldBidiControl(std::string_view(text, static_cast<size_t>(len)))) return count;
  for (int i = 0; i < len; ++i) {
    const auto lead = static_cast<uint8_t>(text[i]);
    const int bytes = lead == 0xE2 ? 3 : lead == 0xD8 ? 2 : 0;
    if (bytes == 0 || i + bytes > len) continue;
    const auto* p = reinterpret_cast<const unsigned char*>(text + i);
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!BidiLine::isUncountedControl(cp) && cp != 0x200C && cp != 0x200D) continue;
    count -= static_cast<size_t>(bytes);  // no byte of a control is whitespace
    i += bytes - 1;
  }
  return count;
}

// The level (BidiUtils::strongLevel: 0 for a letter of class L, 1 for R or AL) of the first letter in `tokens`, read in
// order, that has a direction of its own, outside every isolate (UAX #9 P2: from an LRI, RLI or FSI to its matching
// PDI, or to the end); -1 when none has.
int firstStrongLevel(const std::vector<std::string>& tokens) {
  int isolates = 0;
  for (const std::string& token : tokens) {
    const auto* p = reinterpret_cast<const unsigned char*>(token.c_str());
    while (*p != 0) {
      const uint32_t cp = utf8NextCodepoint(&p);
      if (cp >= 0x2066 && cp <= 0x2068) {
        ++isolates;
      } else if (cp == 0x2069) {
        if (isolates > 0) --isolates;
      } else if (isolates == 0) {
        const int level = BidiUtils::strongLevel(cp);
        if (level >= 0) return level;
      }
    }
  }
  return -1;
}

// The level of a paragraph laid out from `tokens` (design decision 6). The markup's ltr or rtl decides; auto
// (dir="auto" or unicode-bidi: plaintext on the block) takes the first letter with a direction (UAX #9 P2/P3).
// Otherwise the book's language does (`bookLevel`), unless every letter with a direction runs the other way: a Hebrew
// book's English paragraph runs left to right, an English book's Hebrew one right to left, and a paragraph with letters
// of both directions, or with none, runs the book's way. `rtlClass`: whether a token has a code point of class R, AL,
// RLE, RLO, RLI or FSI (BidiUtils::hasRtlClass); without one no letter runs right to left.
uint8_t paragraphLevel(const std::vector<std::string>& tokens, const CssDirection direction, const uint8_t bookLevel,
                       const bool rtlClass) {
  if (direction == CssDirection::Ltr) return 0;
  if (direction == CssDirection::Rtl) return 1;
  if (direction == CssDirection::Auto) {
    const int first = firstStrongLevel(tokens);
    return first >= 0 ? static_cast<uint8_t>(first) : bookLevel;
  }
  // No right-to-left letter can turn a left-to-right book's paragraph: its letters are not read.
  if (bookLevel == 0 && !rtlClass) return 0;
  bool otherWay = false;
  for (const std::string& token : tokens) {
    const auto* p = reinterpret_cast<const unsigned char*>(token.c_str());
    while (*p != 0) {
      const int level = BidiUtils::strongLevel(utf8NextCodepoint(&p));
      if (level < 0) continue;
      if (level == bookLevel) return bookLevel;
      // A left-to-right letter in a right-to-left book where no right-to-left one can follow.
      if (!rtlClass) return 0;
      otherWay = true;
    }
  }
  return otherWay ? static_cast<uint8_t>(1 - bookLevel) : bookLevel;
}

// `token` without its bidi controls, into `out` (the caller's reused string). False, `out` unspecified, when it holds
// none.
bool withoutControls(const std::string& token, std::string& out) {
  if (!mayHoldBidiControl(token)) return false;
  out.assign(token);
  return BidiLine::dropControls(out);
}

// Whether `form` is `text` as TextBlock stores it, its soft hyphens left out.
bool formIsText(const std::string& text, const std::string_view form) {
  size_t at = 0;
  for (size_t pos = 0; pos < text.size();) {
    if (text.compare(pos, SOFT_HYPHEN_BYTES, SOFT_HYPHEN_UTF8) == 0) {
      pos += SOFT_HYPHEN_BYTES;
      continue;
    }
    if (at == form.size() || form[at] != text[pos]) return false;
    ++at;
    ++pos;
  }
  return at == form.size();
}

// Appends `bytes` to `out` without the bidi controls (BidiLine::isControl), and without the joiners (U+200C, U+200D)
// too when `joiners`.
void appendWithoutControls(std::string& out, const std::string_view bytes, const bool joiners) {
  for (size_t pos = 0; pos < bytes.size();) {
    const size_t at = pos;
    const uint32_t cp = TextShaping::decodeAt(bytes, pos);
    if (BidiLine::isControl(cp) || (joiners && (cp == 0x200C || cp == 0x200D))) continue;
    out.append(bytes, at, pos - at);
  }
}

// Points each of `forms` that holds a soft hyphen at a copy of it without them, in `copies` (cleared, then reserved for
// `bytes`, the forms' sizes, which the caller's heap check allowed: no reallocation, so the views stay valid).
void copyWithoutSoftHyphens(std::vector<std::string_view>& forms, std::string& copies, const size_t bytes) {
  copies.clear();
  copies.reserve(bytes);
  for (std::string_view& form : forms) {
    if (form.find(SOFT_HYPHEN_UTF8) == std::string_view::npos) continue;
    const size_t at = copies.size();
    copies.append(form);
    for (size_t pos = copies.find(SOFT_HYPHEN_UTF8, at); pos != std::string::npos;
         pos = copies.find(SOFT_HYPHEN_UTF8, pos)) {
      copies.erase(pos, SOFT_HYPHEN_BYTES);
    }
    form = std::string_view(copies).substr(at);
  }
}

// The most code points a combined word may have (tate-chu-yoko): a longer run stays sideways.
constexpr int kMaxCombinedCodepoints = 4;

// Whether text-combine-upright `mode` (a CssTextCombine) asks for `token` to combine: every token under `all`; under
// `digits N` a token of 1 to N ASCII digits and nothing else. The rest of the rule is layout's (ParsedText::combines).
bool combineRequestedFor(const uint8_t mode, const std::string_view token) {
  const auto combine = static_cast<CssTextCombine>(mode);
  if (combine == CssTextCombine::All) return true;
  if (combine != CssTextCombine::Digits2 && combine != CssTextCombine::Digits3 && combine != CssTextCombine::Digits4) {
    return false;
  }
  const size_t maxDigits = 2 + (mode - static_cast<uint8_t>(CssTextCombine::Digits2));
  return !token.empty() && token.size() <= maxDigits &&
         std::all_of(token.begin(), token.end(), [](const char c) { return c >= '0' && c <= '9'; });
}

// The em of CJK text in this face and size: the advance of 一. 0 when the face has no 一 of its own: a Latin font
// draws CJK characters as its replacement glyph, whose advance says nothing about CJK widths, so no mark is taken
// for full-width punctuation and no grid is made from it.
int cjkEm(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style, const float scale) {
  if (!renderer.hasGlyph(fontId, 0x4E00, style)) return 0;
  return measureWordWidth(renderer, fontId, "\xE4\xB8\x80", style, scale, false);  // 一
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

// What a paragraph that shapes keeps across its passes (see shapingState_): the store, and two strings that keep their
// capacity across tokens, the bytes of a piece being measured and a candidate prefix the breaker shapes. And the
// pass's face answers (shapingFaceFor), forgotten at each pass's start and end.
struct ParsedText::ShapingState {
  ShapedStore store;
  std::string measureScratch;
  std::string candidate;
  struct FaceAnswer {
    int fontId = 0;
    uint8_t style = 0;  // its BOLD_ITALIC bits
    ShapingFace* face = nullptr;
    uint32_t ppem26_6 = 0;  // the face's, before the font's base scale
    bool refused = false;   // no face: the font has one, but the heap refused it
  };
  // A paragraph's fonts and styles in a pass: more are asked of the renderer at each use.
  static constexpr uint8_t kFaceAnswers = 8;
  FaceAnswer faces[kFaceAnswers];
  uint8_t faceCount = 0;
  // The line being laid out as pieces (viewShapedPieces): their texts (views into `words`), forms and runs (views into
  // the store, or into formCopies), styles, sizes and boundaries, and each token's first piece, its last entry the
  // line's piece count. TextBlock copies the bytes into its arena; the one string built is formCopies, for a line with
  // a form that holds a soft hyphen. All keep their capacity across lines.
  std::vector<std::string_view> pieceWords;
  std::vector<std::string_view> pieceForms;
  std::vector<GlyphRun> pieceGlyphs;
  std::vector<EpdFontFamily::Style> pieceStyles;
  std::vector<TextBlock::WordSize> pieceSizes;
  std::vector<TokenBoundary::Boundary> pieceBoundaries;
  std::vector<uint16_t> tokenPieces;
  std::string formCopies;  // the line's Reordered forms that held a soft hyphen, without it

  // Token i of `text`'s pieces, valid for its text and key; none for a token that holds no shaped text, was not shaped
  // (a combined word among them: it is one em, whatever its glyphs) or has none. Valid as ShapedStore::pieces is.
  std::optional<std::span<const ShapedStore::Piece>> piecesOf(const ParsedText& text, const GfxRenderer& renderer,
                                                              const int fontId, const size_t i) const {
    if (!text.shapedText(text.words[i])) return std::nullopt;
    const std::optional<std::span<const ShapedStore::Piece>> pieces =
        store.pieces(i, text.words[i], text.shapeKey(renderer, fontId, i));
    if (!pieces || pieces->empty()) return std::nullopt;
    return pieces;
  }
  // Token j's piece that faces a glued neighbour (its last when `last`, else its first) when it is a glyph run with
  // records, the run a ligature across the glue sits in; null otherwise.
  const ShapedStore::Piece* edgeRun(const ParsedText& text, const GfxRenderer& renderer, const int fontId,
                                    const size_t j, const bool last) const {
    const auto pieces = piecesOf(text, renderer, fontId, j);
    if (!pieces) return nullptr;
    const ShapedStore::Piece& piece = last ? pieces->back() : pieces->front();
    return piece.kind == IndicShaper::PieceKind::Glyphs && piece.count > 0 ? &piece : nullptr;
  }
};

ParsedText::ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled, const BlockStyle& blockStyle,
                       const bool bionicReadingEnabled)
    : blockStyle(blockStyle),
      extraParagraphSpacing(extraParagraphSpacing),
      hyphenationEnabled(hyphenationEnabled),
      bionicReadingEnabled(bionicReadingEnabled) {}
ParsedText::~ParsedText() = default;
ParsedText::ParsedText(ParsedText&&) noexcept = default;
ParsedText& ParsedText::operator=(ParsedText&&) noexcept = default;

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
  // A block the parser splits never holds more than splitCapacity_ tokens, but for the few a ruby group may hold the
  // split up: no further than that, and past it by 16s. The split's kept line plus 48 is a threshold of 97 to 175 for
  // the lines a span-per-word paragraph makes, which doubling took to 256 entries (33 B each on the device).
  if (splitCapacity_ != 0 && newCapacity > splitCapacity_) {
    newCapacity = requiredSize <= splitCapacity_ ? splitCapacity_ : (requiredSize + 15) / 16 * 16;
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
                         uint8_t fontSlot, const uint8_t textEmphasis, const uint8_t textCombine) {
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
  // A line may end after a dash inside a Latin word, whether or not hyphenation is on.
  const std::vector<size_t> dashBreaks = hasCjk ? std::vector<size_t>{} : dashBreakByteOffsets(word, attachToPrevious);
  // The token boundaries: the CJK breaks, then each mark edge that is not already one (glued), and the dash breaks.
  // What each one puts before the token it starts: a CJK break is a breakable zero-width gap, a mark's other edges are
  // glued (no space, no break), and a dash is glued but breakable, the fourth state of TokenBoundary.
  struct Cut {
    size_t at;
    TokenBoundary::Boundary boundary;
  };
  std::vector<Cut> cuts;
  cuts.reserve(cjkBreaks.size() + markEdges.size() + dashBreaks.size());
  for (const size_t at : cjkBreaks) cuts.push_back({at, TokenBoundary::Boundary{false, true}});
  for (const size_t at : markEdges) {
    if (std::find(cjkBreaks.begin(), cjkBreaks.end(), at) == cjkBreaks.end()) {
      cuts.push_back({at, TokenBoundary::Boundary{true, false}});
    }
  }
  for (const size_t at : dashBreaks) cuts.push_back({at, TokenBoundary::Boundary{true, true}});
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
  // spaces between CJK words", Uri Tauber, 9163149a). A glued Latin "foo—" + "bar" stays glued with no gap, and a line
  // may end between them: ported from crosspoint-reader PR #3883 (Phạm Bình An) by way of CrossInk 59b53529 (Julia
  // Nguyen). The word after an inline tag used to get a real word gap here, for want of this state.
  TokenBoundary::Boundary boundary{attachToPrevious, false};
  // A glued " " is the parser's no-break space (U+00A0 / U+202F): no break on either side of it.
  if (attachToPrevious && !words.empty() && words.back() != " " && word != " ") {
    const uint32_t left = lastCodepointPastControls(words.back());
    const uint32_t right = firstCodepointPastControls(word.c_str());
    if ((utf8IsCjkBreakable(left) || utf8IsCjkBreakable(right)) && CjkLineBreak::isGapBetween(left, right)) {
      boundary = TokenBoundary::Boundary{false, true};
    } else if (mayBreakAfterDash(left, right) && !isDashesOnly(words.back())) {
      boundary = TokenBoundary::Boundary{true, true};
    }
  }

  // Every token of the word carries the mark, and the combine request where the word's mode asks for the token: both
  // travel with the token through splits and erases.
  const auto marked = [&](const TokenBoundary::Boundary b, const std::string_view token) {
    return TokenBoundary::withCombineRequested(TokenBoundary::withEmphasis(b, textEmphasis),
                                               combineRequestedFor(textCombine, token));
  };
  if (cuts.empty()) {
    const TokenBoundary::Boundary only = marked(boundary, word);
    pushToken(std::move(word), combinedStyle, only, size, visibleOffset, fontSlot);
    return;
  }
  size_t start = 0;
  uint32_t offset = visibleOffset;
  for (size_t i = 0; i <= cuts.size(); ++i) {
    const size_t end = i < cuts.size() ? cuts[i].at : word.size();
    std::string token = word.substr(start, end - start);
    const TokenBoundary::Boundary tokenBoundary = marked(boundary, token);
    pushToken(std::move(token), combinedStyle, tokenBoundary, size, offset, fontSlot);
    // The next token starts where this one's visible bytes end (KOSync positions).
    offset += static_cast<uint32_t>(visibleBytesOutsideControls(word.data() + start, static_cast<int>(end - start)));
    if (i < cuts.size()) {
      boundary = cuts[i].boundary;
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
  // The engine kerns by code point, and the shaper kerned inside its runs: a glued gap beside a glyph run takes none.
  if (shapes_ && (glyphEdge(renderer, fontId, j - 1, true) || glyphEdge(renderer, fontId, j, false))) return 0;
  return renderer.getKerning(left, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
}

int ParsedText::naturalGapBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  const TokenBoundary::Boundary& boundary = wordBoundaries[j];
  if (boundary.continues) return kernBefore(renderer, fontId, j);
  if (boundary.noSpaceBefore) return cjkLatinGapBefore(renderer, fontId, j);
  return spaceBefore(renderer, fontId, j);
}

bool ParsedText::stretchesGapBefore(const size_t j) const { return !isClosingPunctuation(firstCodepoint(words[j])); }

int ParsedText::cjkLatinGapBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  if (!typography_.cjkLatinGap) return 0;
  const TokenBoundary::Boundary& boundary = wordBoundaries[j];
  if (boundary.continues || !boundary.noSpaceBefore) return 0;
  // A combined word sets as one upright character, as unspaced CJK does: 一 stands for it, so it takes no gap beside
  // CJK, the only neighbour a combined run of Latin letters or digits meets across a CJK gap.
  constexpr uint32_t kIdeographOne = 0x4E00;
  const uint32_t left = combines(j - 1) ? kIdeographOne : lastCodepoint(words[j - 1]);
  const uint32_t right = combines(j) ? kIdeographOne : firstCodepoint(words[j]);
  if (!CjkPunctuation::takesCjkLatinGap(left, right)) return 0;
  // The gap is a quarter of the CJK character's em, in its own face and size.
  const size_t k = CjkPunctuation::isHanOrKana(left) ? j - 1 : j;
  const int em = cjkEm(renderer, wordFontId(k, fontId),
                       static_cast<EpdFontFamily::Style>(wordStyles[k] & EpdFontFamily::BOLD_ITALIC), wordScale(k));
  if (em <= 0) return 0;
  return (em * CjkPunctuation::kCjkLatinGap + 4) / 8;
}

void ParsedText::resolveAlignment(const bool rtl) {
  if (blockStyle.alignment == CssTextAlign::Start) {
    blockStyle.alignment = rtl ? CssTextAlign::Right : CssTextAlign::Left;
  } else if (blockStyle.alignment == CssTextAlign::End) {
    blockStyle.alignment = rtl ? CssTextAlign::Left : CssTextAlign::Right;
  }
}

void ParsedText::decideBidi() {
  // bidi_ is only set here, once the level is decided.
  if (!bidi_) {
    const bool rtlClass = std::any_of(words.begin(), words.end(),
                                      [](const std::string& w) { return BidiUtils::hasRtlClass(w.data(), w.size()); });
    if (!levelDecided_) {
      // On the paragraph's first pass, whether or not it needs bidi; the passes after the long-block split keep it.
      const uint8_t bookLevel = typography_.direction == TypographyProfile::Direction::RightToLeft ? 1 : 0;
      level_ = paragraphLevel(words, blockStyle.direction, bookLevel, rtlClass);
      levelDecided_ = true;
    }
    bidi_ = level_ == 1 || rtlClass;
    if (bidi_ && !bidiState_) {
      bidiState_ = makeUniqueNoThrow<BidiState>();
      if (!bidiState_) {
        // As when the line pass has no workspace: the text is laid out without bidi, and its controls are not drawn.
        LOG_ERR("BIDI", "OOM: bidi state (%u B): pass laid out without bidi", static_cast<unsigned>(sizeof(BidiState)));
        bidi_ = false;
      }
    }
  }
  if (!bidi_) dropBidiControls();
}

void ParsedText::forgetBidiParagraph() {
  bidi_ = false;
  level_ = 0;
  levelDecided_ = false;
  if (bidiState_) bidiState_->openControls = {};
}

void ParsedText::dropBidiControls() {
  for (size_t i = 0; i < words.size();) {
    std::string& word = words[i];
    if (!mayHoldBidiControl(word) || !BidiLine::dropControls(word) || !word.empty() ||
        TokenBoundary::rubyRole(wordBoundaries[i]) != TokenBoundary::RubyRole::None) {
      ++i;
      continue;
    }
    // The token held nothing else: it goes, and a token glued to it takes the gap it had before it.
    if (i + 1 < words.size() && !TokenBoundary::hasSpace(wordBoundaries[i + 1])) {
      wordBoundaries[i + 1].continues = wordBoundaries[i].continues;
      wordBoundaries[i + 1].noSpaceBefore = wordBoundaries[i].noSpaceBefore;
    }
    words.erase(words.begin() + i);
    wordStyles.erase(wordStyles.begin() + i);
    wordBoundaries.erase(wordBoundaries.begin() + i);
    wordSizes.erase(wordSizes.begin() + i);
    wordVisibleOffsets.erase(wordVisibleOffsets.begin() + i);
    if (bionicTransformedUpTo_ > i) --bionicTransformedUpTo_;
    if (shapingState_) shapingState_->store.erase(i);
  }
}

void ParsedText::decideShaping(const GfxRenderer& renderer, const int fontId) {
  // The pass's face answers start afresh: a face lives for the build, and a font set reload between passes frees it.
  forgetFaceAnswers();
  // Indic text is formed in any font (reordered without a face). Arabic text only in a paragraph that needs bidi (every
  // Arabic run starts at an AL letter, which makes it need bidi: a Latin paragraph's Arabic-Indic digits or Arabic
  // comma shape nowhere), where a token of it has a face that draws Arabic (maps beh), the first one found deciding (my
  // call 5): in a bitmap or built-in font, or a TrueType face without Arabic (whose every run would be Text), an Arabic
  // paragraph makes no ShapingState and merges nothing, so its lines are byte for byte those of the line pass alone.
  // Consecutive tokens in one font and style that has no such face ask it once.
  shapes_ = std::any_of(words.begin(), words.end(), [](const std::string& w) { return IndicShaper::holdsIndic(w); });
  int askedFont = 0;
  int askedStyle = -1;
  for (size_t i = 0; bidi_ && i < words.size() && !shapes_; ++i) {
    if (!arabic::holdsArabic(words[i].data(), words[i].size())) continue;
    const int font = wordFontId(i, fontId);
    const int style = wordStyles[i] & EpdFontFamily::BOLD_ITALIC;
    if (font == askedFont && style == askedStyle) continue;
    uint32_t ppem = 0;
    ShapingFace* const face = shapingFaceFor(renderer, fontId, i, &ppem);
    shapes_ = face != nullptr && IndicShaper::drawsArabic(*face);
    askedFont = font;
    askedStyle = style;
  }
  if (!shapes_) {
    // The store is in step with the passes that shape: a later one starts it again.
    if (shapingState_) shapingState_->store.reset();
    return;
  }
  if (!shapingState_) {
    // About 630 B on the device (the store's chunk table and vectors, the face answers, the line's piece vectors and
    // three strings): the paragraph's across its passes, so not on the stack; its chunks then survive the paragraphs.
    shapingState_ = makeUniqueNoThrow<ShapingState>();
    if (!shapingState_) {
      LOG_ERR("SHP", "OOM: shaping state (%u B): pass laid out unshaped", static_cast<unsigned>(sizeof(ShapingState)));
      shapes_ = false;
      latchShapingDegraded();
      return;
    }
  }
  ShapedStore& store = shapingState_->store;
  if (store.size() > words.size()) store.reset();  // entries past the words cannot be in step with them
  // The kept tokens' last was shaped as the paragraph's end: a token glued to it has arrived since.
  const size_t kept = store.size();
  if (kept > 0 && kept < words.size() && wordBoundaries[kept].continues) forgetShaping(kept - 1);
}

void ParsedText::forgetShaping(const size_t i) {
  if (!shapingState_ || i >= shapingState_->store.size()) return;
  // An opened entry is stale from the start, and abandoning it drops its pieces: the entry stays, empty and stale.
  ShapedStore& store = shapingState_->store;
  if (store.open(i, std::string_view(), 0)) store.abandon();
}

bool ParsedText::sameStyle(const int fontId, const size_t a, const size_t b) const {
  return wordFontId(a, fontId) == wordFontId(b, fontId) && wordStyles[a] == wordStyles[b] &&
         wordSizes[a].pct == wordSizes[b].pct && wordSizes[a].fontSlot == wordSizes[b].fontSlot &&
         TokenBoundary::emphasisOf(wordBoundaries[a]) == TokenBoundary::emphasisOf(wordBoundaries[b]);
}

// The most bytes a merged token may hold: past it, glued tokens stay apart, each shaped with the other as context. A
// shaping call's block and the breaker's candidates grow with the token, so a run of one style with no space in it
// (the parser cuts its words at 200 B) must not become one token of any length. 512 B is 170 Indic code points.
constexpr size_t kMaxMergedBytes = 512;
// Glue inside a syllable merges up to twice that: kept apart, a sign would be shaped without its consonant. A word
// of the cap and one of the parser's (200 B) fit; only a pathological chain of syllable-internal spans reaches it.
constexpr size_t kMaxSyllableMergeBytes = 2 * kMaxMergedBytes;

size_t ParsedText::sourceTokenCount() const {
  size_t count = words.size();
  for (const TokenBoundary::Boundary& b : wordBoundaries) count += b.merged;
  return count;
}

void ParsedText::mergeGluedGroups(const int fontId) {
  const auto mergeable = [&](const size_t k) {
    const TokenBoundary::Boundary b = wordBoundaries[k];
    return k < unmergedFrom_ && words[k] != " " && TokenBoundary::rubyRole(b) == TokenBoundary::RubyRole::None &&
           !TokenBoundary::combineRequested(b);
  };
  // Glue inside a syllable (indic::syllableBreakAllowed false across it), read as mergesWithPrevious reads it.
  const auto insideSyllable = [&](const size_t j) {
    if (words[j - 1].empty() || words[j].empty()) return false;
    size_t pos = 0;
    return !indic::syllableBreakAllowed(lastCodepoint(words[j - 1]), TextShaping::decodeAt(words[j], pos));
  };
  // Each maximal run of glued tokens, [start, end).
  for (size_t start = 0; start < words.size();) {
    size_t end = start + 1;
    while (end < words.size() && wordBoundaries[end].continues) ++end;
    // The group holds text the pass shapes (shapedText): only then do deviation 7's merges apply.
    bool shapedGroup = false;
    for (size_t k = start; shapes_ && end - start > 1 && k < end && !shapedGroup; ++k) {
      shapedGroup = shapedText(words[k]);
    }
    for (size_t j = start + 1; j < end;) {
      // words[j - 1] is the token merged so far: its style is its first part's.
      const TextShaping::GroupToken previous{words[j - 1], 0};
      const TextShaping::GroupToken token{words[j], sameStyle(fontId, j - 1, j) ? 0u : 1u};
      const size_t bytes = words[j - 1].size() + words[j].size();
      const size_t cap = shapedGroup && insideSyllable(j) ? kMaxSyllableMergeBytes : kMaxMergedBytes;
      // The merged count is a byte: a merge past it would lose the parser's count.
      const unsigned merged = wordBoundaries[j - 1].merged + 1u + wordBoundaries[j].merged;
      if (!mergeable(j - 1) || !mergeable(j) || bytes > cap || merged > UINT8_MAX ||
          !TextShaping::mergesWithPrevious(previous, token, shapedGroup)) {
        ++j;
        continue;
      }
      // The merged text grows the first part's string (up to 1 KB): refused, the two stay apart, as past the cap, and
      // that is not the merged token's form.
      if (!heapAllows(growthBytes(words[j - 1], bytes))) {
        LOG_ERR("SHP", "No room to merge a %u B glued group: its tokens stay apart", static_cast<unsigned>(bytes));
        latchShapingDegraded();
        ++j;
        continue;
      }
      wordBoundaries[j - 1].merged = static_cast<uint8_t>(merged);
      words[j - 1] += words[j];
      words.erase(words.begin() + j);
      wordStyles.erase(wordStyles.begin() + j);
      wordBoundaries.erase(wordBoundaries.begin() + j);
      wordSizes.erase(wordSizes.begin() + j);
      wordVisibleOffsets.erase(wordVisibleOffsets.begin() + j);
      if (bionicTransformedUpTo_ > j) --bionicTransformedUpTo_;
      if (shapingState_) {
        shapingState_->store.erase(j);
        // A neighbour shaped in an earlier pass with a part as its context is shaped again, with the whole.
        if (j - 1 > start) forgetShaping(j - 2);
        if (j + 1 < end) forgetShaping(j);
      }
      --end;
    }
    start = end;
  }
}

bool ParsedText::readyShapedStore() {
  ShapedStore& store = shapingState_->store;
  // The estimate, for the tokens this pass adds to the store: an entry each, and 2 B of records a byte of their Indic
  // text, 3 a byte of their Arabic text (2 B a letter; the kept tokens' records are in the chunks already). Refused,
  // the chunks are still asked for one at a time.
  const size_t added = words.size() > store.size() ? words.size() - store.size() : 0;
  size_t bytes = 0;
  for (size_t i = store.size(); i < words.size(); ++i) {
    if (IndicShaper::holdsIndic(words[i])) {
      bytes += 2 * words[i].size();
    } else if (shapedText(words[i])) {
      bytes += 3 * words[i].size();
    }
  }
  store.reserve(added, bytes);
  while (store.size() < words.size()) {
    if (!store.insert(store.size())) {
      LOG_ERR("SHP", "No room for %u shaped-store entries: pass laid out unshaped",
              static_cast<unsigned>(words.size()));
      store.reset();
      latchShapingDegraded();
      return false;
    }
  }
  return true;
}

ShapingFace* ParsedText::shapingFaceFor(const GfxRenderer& renderer, const int fontId, const size_t i,
                                        uint32_t* const drawnPpem) const {
  *drawnPpem = 0;
  // A vertical line refuses glyph forms on load (TextBlock::bidiValid): its Indic words take IndicReorder's form.
  if (vertical_) return nullptr;
  const int font = wordFontId(i, fontId);
  const auto style = static_cast<uint8_t>(wordStyles[i] & EpdFontFamily::BOLD_ITALIC);
  ShapingFace* face = nullptr;
  uint32_t ppem = 0;
  bool refused = false;
  // The pass's answer for this font and style; the first ask makes it (a cache: shapeKey and glyphEdge are const).
  ShapingState::FaceAnswer* answer = nullptr;
  if (shapingState_) {
    ShapingState& state = *shapingState_;
    for (uint8_t a = 0; a < state.faceCount && answer == nullptr; ++a) {
      if (state.faces[a].fontId == font && state.faces[a].style == style) answer = &state.faces[a];
    }
    if (answer == nullptr && state.faceCount < ShapingState::kFaceAnswers) {
      answer = &state.faces[state.faceCount++];
      answer->fontId = font;
      answer->style = style;
      answer->ppem26_6 = 0;
      answer->refused = false;
      answer->face =
          renderer.shapingFace(font, static_cast<EpdFontFamily::Style>(style), &answer->ppem26_6, &answer->refused);
    }
  }
  if (answer != nullptr) {
    face = answer->face;
    ppem = answer->ppem26_6;
    refused = answer->refused;
  } else {
    face = renderer.shapingFace(font, static_cast<EpdFontFamily::Style>(style), &ppem, &refused);
  }
  // The token is laid out as a font that cannot shape is, which is not what this font draws (invariant 6).
  if (refused) latchShapingDegraded();
  if (face == nullptr) return nullptr;
  // Records are in drawn pixels: the word is shaped at the size it is drawn at.
  *drawnPpem = static_cast<uint32_t>(static_cast<float>(ppem) * renderer.fontBaseScale(font) * wordScale(i) + 0.5f);
  return face;
}

void ParsedText::forgetFaceAnswers() {
  if (shapingState_) shapingState_->faceCount = 0;
}

uint32_t ParsedText::shapeKey(const GfxRenderer& renderer, const int fontId, const size_t i) const {
  uint32_t ppem = 0;
  const bool face = shapingFaceFor(renderer, fontId, i, &ppem) != nullptr;
  const int32_t font = wordFontId(i, fontId);
  uint32_t key = kKeySeed;
  key = mixKey(key, font);
  key = mixKey(key, static_cast<uint8_t>(wordStyles[i]));
  key = mixKey(key, wordSizes[i].pct);
  key = mixKey(key, wordSizes[i].fontSlot);
  key = mixKey(key, wordScale(i));
  key = mixKey(key, renderer.fontBaseScale(font));
  key = mixKey(key, ppem);
  key = mixKey(key, static_cast<uint8_t>((face ? 1 : 0) | (vertical_ ? 2 : 0)));
  return key;
}

IndicShaper::Outcome ParsedText::shapeInto(const GfxRenderer& renderer, const int fontId, const size_t i,
                                           const std::string_view text, const std::string_view before,
                                           const std::string_view after) {
  IndicShaper::Request request;
  request.face = shapingFaceFor(renderer, fontId, i, &request.ppem26_6);
  request.languageTags = typography_.shapingLanguage;
  request.before = before;
  request.after = after;
  // Without a face only Indic text takes a form (IndicReorder's): a token without it, Arabic text in a font that cannot
  // shape, is Plain, as the shaper would say, with no call block.
  if (request.face == nullptr && !IndicShaper::holdsIndic(text)) return IndicShaper::Outcome::Plain;
  // The call's block is the arena's for this call alone, released here, before any of the token's glyphs is looked up
  // (LIFO with the TrueType miss scope). Without a face the call only reorders, in a block of the form's size.
  IndicShaper::CallMemory memory(
      lineArena_, text.size(), request.face != nullptr ? IndicShaper::CallKind::Shape : IndicShaper::CallKind::Reorder);
  return IndicShaper::shapeToken(text, request, memory, shapingState_->store);
}

bool ParsedText::shapeToken(const GfxRenderer& renderer, const int fontId, const size_t i, uint16_t* const width) {
  const std::string& text = words[i];
  if (!shapedText(text)) return false;
  ShapedStore& store = shapingState_->store;
  const uint32_t key = shapeKey(renderer, fontId, i);
  std::optional<std::span<const ShapedStore::Piece>> pieces = store.pieces(i, text, key);
  if (!pieces) {
    if (i >= store.size() || !store.open(i, text, key)) {
      latchShapingDegraded();  // the store could not keep in step, for want of memory
      return false;
    }
    // The glued neighbours kept apart are its context (deviation 7).
    const std::string_view before =
        i > 0 && wordBoundaries[i].continues ? std::string_view(words[i - 1]) : std::string_view();
    const std::string_view after =
        i + 1 < words.size() && wordBoundaries[i + 1].continues ? std::string_view(words[i + 1]) : std::string_view();
    // A token with no glued neighbour is shaped without context, so its pieces are the same wherever it comes: the
    // build's word cache may hold them. A glued one is always shaped.
    const bool cacheable = wordCache_ != nullptr && before.empty() && after.empty() && text.size() <= UINT16_MAX;
    const uint64_t textHash = cacheable ? HashUtils::fnvHash64(text.data(), text.size()) : 0;
    const uint32_t cacheKey = cacheable ? WordShapeCache::keyFor(key, typography_.shapingLanguage) : 0;
    const auto length = static_cast<uint16_t>(text.size());
    if (cacheable && wordCache_->replay(textHash, length, cacheKey, store)) {
      store.commit();
    } else {
      if (!store.openPieces().empty()) {
        // The store refused the replay part-way: shaped afresh, as without the cache.
        store.abandon();
        if (!store.open(i, text, key)) {
          latchShapingDegraded();
          return false;
        }
      }
      ++tokensShaped_;
      const IndicShaper::Outcome outcome = shapeInto(renderer, fontId, i, text, before, after);
      if (outcome == IndicShaper::Outcome::Degraded) {
        store.abandon();
        latchShapingDegraded();
        return false;
      }
      store.commit();
      if (cacheable && outcome == IndicShaper::Outcome::Formed) {
        wordCache_->insert(textHash, length, cacheKey, store, i);
      }
    }
    pieces = store.pieces(i, text, key);
    if (!pieces || pieces->empty()) return false;
    // A piece that is not a glyph run is measured from a copy of its bytes: the scratch is grown for the longest here,
    // behind the heap check. Refused, the entry goes stale and the token is laid out as its code points.
    if (!heapAllows(growthBytes(shapingState_->measureScratch, scratchBytesFor(store, *pieces)))) {
      LOG_ERR("SHP", "No room to measure a shaped token: laid out as its code points");
      forgetShaping(i);
      latchShapingDegraded();
      return false;
    }
    // Grown once, to what was checked: pieces of rising length would otherwise grow it again, unchecked.
    shapingState_->measureScratch.reserve(scratchBytesFor(store, *pieces));
    // Warm: the glyph IDs are looked up once, here inside the build's miss scope, so a page drawn later needs no miss
    // memory for them. Then each piece's width.
    const int font = wordFontId(i, fontId);
    for (const ShapedStore::Piece& piece : *pieces) {
      if (piece.kind == IndicShaper::PieceKind::Glyphs) {
        renderer.prefetchGlyphRun(font, store.glyphs(piece), wordStyles[i]);
      }
    }
    for (size_t p = 0; p < pieces->size(); ++p) {
      store.setWidth(i, p,
                     measurePiece(renderer, font, wordStyles[i], wordScale(i), vertical_, store, (*pieces)[p], text,
                                  bidi_, shapingState_->measureScratch));
    }
  }
  if (pieces->empty()) return false;
  if (width != nullptr) {
    int sum = 0;
    for (const ShapedStore::Piece& piece : *pieces) sum += piece.width;
    *width = clampedWidth(sum);
  }
  return true;
}

bool ParsedText::shapedText(const std::string_view text) const {
  return IndicShaper::holdsIndic(text) || (bidi_ && arabic::holdsArabic(text.data(), text.size()));
}

bool ParsedText::storedWidth(const GfxRenderer& renderer, const int fontId, const size_t i,
                             uint16_t* const width) const {
  const auto pieces = shapingState_ ? shapingState_->piecesOf(*this, renderer, fontId, i) : std::nullopt;
  if (!pieces) return false;
  int sum = 0;
  for (const ShapedStore::Piece& piece : *pieces) sum += piece.width;
  *width = clampedWidth(sum);
  return true;
}

int ParsedText::shapedPrefixWidth(const GfxRenderer& renderer, const int fontId, const size_t i, const size_t bytes,
                                  const bool hyphen) {
  ShapingState& state = *shapingState_;
  std::string& candidate = state.candidate;
  // Never grows: hyphenateWordAtIndex reserved the word and its hyphen.
  candidate.assign(words[i], 0, bytes);
  if (hyphen) candidate.push_back('-');
  const int font = wordFontId(i, fontId);
  int width = -1;
  // The prefix ends its line: a token glued before it is its context, the rest of its word is not.
  const std::string_view before =
      i > 0 && wordBoundaries[i].continues ? std::string_view(words[i - 1]) : std::string_view();
  if (state.store.open(state.store.size(), candidate, 0)) {
    const IndicShaper::Outcome outcome = shapeInto(renderer, fontId, i, candidate, before, std::string_view());
    // Its pieces are measured as shapeToken measures a token's, the scratch grown behind the heap check.
    const bool measurable =
        outcome == IndicShaper::Outcome::Formed &&
        heapAllows(growthBytes(state.measureScratch, scratchBytesFor(state.store, state.store.openPieces())));
    if (measurable) {
      state.measureScratch.reserve(scratchBytesFor(state.store, state.store.openPieces()));  // once, as checked
      width = 0;
      for (const ShapedStore::Piece& piece : state.store.openPieces()) {
        width += measurePiece(renderer, font, wordStyles[i], wordScale(i), vertical_, state.store, piece, candidate,
                              bidi_, state.measureScratch);
      }
    } else if (outcome != IndicShaper::Outcome::Plain) {
      latchShapingDegraded();  // measured as code points below, as any run that could not be shaped is laid out
    }
    state.store.abandon();
  }
  return width >= 0 ? width
                    : measureWordWidth(renderer, font, candidate, wordStyles[i], false, wordScale(i), vertical_);
}

bool ParsedText::glyphEdge(const GfxRenderer& renderer, const int fontId, const size_t i, const bool last) const {
  if (!shapingState_) return false;
  // An empty glyph run (a letter its neighbour's ligature took) is a glyph edge too.
  const auto pieces = shapingState_->piecesOf(*this, renderer, fontId, i);
  return pieces && (last ? pieces->back() : pieces->front()).kind == IndicShaper::PieceKind::Glyphs;
}

void ParsedText::layoutAndExtractLines(
    const GfxRenderer& renderer, const int bodyFontId, const uint16_t viewportWidth,
    const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
    const bool includeLastLine, const int16_t blockStartY, const int lineHeight, const bool preserveSource) {
  if (words.empty()) {
    return;
  }
  // Whether the paragraph needs bidi and which way it runs, before anything reads its words or its alignment. A pass
  // without bidi drops its controls first, so the snapshot below is of the text that is laid out.
  decideBidi();
  controlsOnlyTokens_ = 0;
  if (words.empty()) {
    // The block held nothing but bidi controls. Its paragraph ends here as at the end of any last pass below: the
    // parser may merge the next one into the block, which then decides its own direction.
    if (includeLastLine && !preserveSource) forgetBidiParagraph();
    unmergedFrom_ = SIZE_MAX;
    return;
  }

  // Heading blocks lay out (and below render) with a taller real font instead of scaling the
  // body font. Resolve the effective fontId once; all measurement helpers below take it as
  // their fontId argument, so widths/kerning/indent are computed in the heading font and stay
  // consistent with TextBlock::render(). For the scale-fallback path headingFontId==0 and this
  // is just the body font (fontSizeMultiplier still applies inside the helpers).
  const int fontId = blockStyle.headingFontId != 0 ? blockStyle.headingFontId : bodyFontId;

  // Whether this pass forms its Indic and Arabic words, then each glued group resolved (deviation 7): the snapshot
  // below is of the tokens that are laid out, as it is of the text without the dropped controls.
  decideShaping(renderer, fontId);
  mergeGluedGroups(fontId);
  unmergedFrom_ = SIZE_MAX;

  resolveAlignment(rtl());
  // Upstream's flip: a right-to-left paragraph sets a left alignment the book did not ask for (the reader's) right.
  if (rtl() && blockStyle.alignment == CssTextAlign::Left && !blockStyle.textAlignDefined) {
    blockStyle.alignment = CssTextAlign::Right;
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
  BidiLine::OpenControls savedOpenControls;
  if (preserveSource) {
    savedWords = words;
    savedStyles = wordStyles;
    savedSizes = wordSizes;
    savedOffsets = wordVisibleOffsets;
    savedBoundaries = wordBoundaries;
    savedRubyTexts = rubyTexts_;
    savedIsContinuation = isContinuation_;
    savedBionicWatermark = bionicTransformedUpTo_;
    if (bidiState_) savedOpenControls = bidiState_->openControls;
  }

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
  // where the inset is what the first line hangs out of. It may pull out by at most that inset:
  // the block's own edge is where the CSS containing block starts, and anything past it
  // renders off the panel and silently loses its first glyphs (issue #198). The indent is on the
  // start side: the left, the right in a right-to-left paragraph, with its inset.
  const CssTextAlign startSide = rtl() ? CssTextAlign::Right : CssTextAlign::Left;
  const int startInset = rtl() ? blockStyle.rightInset() : blockStyle.leftInset();
  const int cssTextIndent =
      !isContinuation_ && blockStyle.textIndentDefined &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == startSide)
          ? std::min(std::max<int>(static_cast<int>(blockStyle.textIndent), -startInset), pageWidth - 1)
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
  // The bidi controls open at the pass's first token: the retry below measures from there again.
  const BidiLine::OpenControls passControls = bidiState_ ? bidiState_->openControls : BidiLine::OpenControls{};
  calculateWordWidths(renderer, fontId, wordWidths, passControls);

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
        // The rejoined word's entry is stale by its text, and shaped again with the widths below.
        if (shapingState_) shapingState_->store.erase(splitIndex + 1);
      }

      // Recompute widths after restoring unsplit words.
      calculateWordWidths(renderer, fontId, wordWidths, passControls);

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
    // The kept tokens keep their records for the next pass; the consumed ones' bytes are reclaimed.
    if (shapingState_) shapingState_->store.dropFront(consumed);
    // All remaining words were already transformed before the flush; reset the
    // watermark so that words appended by addWord() are processed next time.
    bionicTransformedUpTo_ = words.size();
  }
  isContinuation_ = !includeLastLine;
  // A pass that lays the last line out ends the paragraph. The parser may hand the drained block more words without a
  // reset (<hr> or a table drains it, and the next paragraph merges into the empty block): they are a new paragraph.
  if (includeLastLine && !preserveSource) forgetBidiParagraph();
  forgetFaceAnswers();

  if (preserveSource) {
    words = std::move(savedWords);
    wordStyles = std::move(savedStyles);
    wordSizes = std::move(savedSizes);
    wordVisibleOffsets = std::move(savedOffsets);
    wordBoundaries = std::move(savedBoundaries);
    rubyTexts_ = std::move(savedRubyTexts);
    isContinuation_ = savedIsContinuation;
    bionicTransformedUpTo_ = savedBionicWatermark;
    if (bidiState_) bidiState_->openControls = savedOpenControls;
    // The restored tokens are those before any split the pass made: the entries are in step with neither, so the
    // store starts again.
    if (shapingState_) shapingState_->store.reset();
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
    // Every token of the base keeps its word's emphasis mark and combine request, the inserted space too.
    const TokenBoundary::Boundary glued = TokenBoundary::withWordMarks(kGluedFollower, wordBoundaries[i]);
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

bool ParsedText::appendControlToLastToken(const std::string_view control) {
  if (words.empty() || words.back() == " ") return false;
  words.back().append(control.data(), control.size());
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

void ParsedText::releaseLineBuffers() {
  std::vector<int16_t>().swap(lineXPosScratch_);
  if (!bidiState_) return;
  BidiState& state = *bidiState_;
  std::vector<BidiLine::Piece>().swap(state.line.pieces);
  std::string().swap(state.line.display);
  std::vector<std::string_view>().swap(state.line.fallbackViews);
  std::vector<uint16_t>().swap(state.line.fallbackOrder);
  std::vector<BidiLine::Token>().swap(state.tokens);
  std::vector<uint16_t>().swap(state.tokenCutCount);
  std::vector<std::string_view>().swap(state.pieceWords);
  std::vector<std::string_view>().swap(state.pieceForms);
  std::vector<EpdFontFamily::Style>().swap(state.pieceStyles);
  std::vector<TextBlock::WordSize>().swap(state.pieceSizes);
  std::vector<TokenBoundary::Boundary>().swap(state.pieceBoundaries);
  std::vector<uint8_t>().swap(state.pieceOrder);
  std::vector<GlyphRun>().swap(state.pieceGlyphs);
  std::vector<uint16_t>().swap(state.tokenCuts);
  std::string().swap(state.tokenForm);
}

size_t ParsedText::keptLineBytes() const {
  // A string's bytes on the heap: none while it sits in its short-string buffer.
  const auto stringBytes = [](const std::string& str) {
    return str.capacity() > std::string().capacity() ? str.capacity() : 0;
  };
  size_t bytes = lineXPosScratch_.capacity() * sizeof(int16_t);
  if (!bidiState_) return bytes;
  const BidiState& s = *bidiState_;
  bytes +=
      s.line.pieces.capacity() * sizeof(BidiLine::Piece) + stringBytes(s.line.display) +
      s.line.fallbackViews.capacity() * sizeof(std::string_view) + s.line.fallbackOrder.capacity() * sizeof(uint16_t) +
      s.tokens.capacity() * sizeof(BidiLine::Token) + s.tokenCutCount.capacity() * sizeof(uint16_t) +
      s.pieceWords.capacity() * sizeof(std::string_view) + s.pieceForms.capacity() * sizeof(std::string_view) +
      s.pieceStyles.capacity() * sizeof(EpdFontFamily::Style) + s.pieceSizes.capacity() * sizeof(TextBlock::WordSize) +
      s.pieceBoundaries.capacity() * sizeof(TokenBoundary::Boundary) + s.pieceOrder.capacity() +
      s.pieceGlyphs.capacity() * sizeof(GlyphRun) + s.tokenCuts.capacity() * sizeof(uint16_t) +
      stringBytes(s.tokenForm);
  return bytes;
}

size_t ParsedText::keptStoreBytes() const {
  return shapingState_ ? shapingState_->store.indexBytes() + shapingState_->store.chunkBytes() : 0;
}

void ParsedText::releaseOversizedLayoutBuffers() {
  if (keptLineBytes() > kKeptLineBytes) releaseLineBuffers();
  // The next pass that shapes reserves the store again, to its own count (readyShapedStore).
  if (keptStoreBytes() > kKeptStoreBytes) shapingState_->store.release();
}

void ParsedText::releaseLayoutScratch() {
  // Pure scratch: recomputed by every layout, so safe to drop at any point between feeds.
  std::vector<uint16_t>().swap(wordWidths_);
  std::vector<uint8_t>().swap(markClass_);
  std::vector<uint8_t>().swap(plainForm_);
  std::vector<int8_t>().swap(edgeTrim_);
  std::vector<uint8_t>().swap(shrink_);
  std::vector<uint8_t>().swap(edgeShrink_);
  std::vector<uint8_t>().swap(combined_);
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
  // The line pass's: the line and its pieces. The paragraph's direction and its open controls are its state, not
  // scratch: a paragraph in progress that needs bidi keeps them, and otherwise the whole box goes. Its working memory
  // is the build's (BidiLine::heapScratch), not this text's.
  if (bidiState_ && !bidi_) {
    bidiState_.reset();
  } else if (bidiState_) {
    releaseLineBuffers();
  }
  // The shaped store's records are recomputed by the next pass that needs them (each token kept is shaped again); the
  // box goes too unless a paragraph in progress shapes.
  if (shapingState_ && (!shapes_ || words.empty())) {
    shapingState_.reset();
    shapes_ = false;
  } else if (shapingState_) {
    ShapingState& state = *shapingState_;
    state.store.release();
    std::string().swap(state.measureScratch);
    std::string().swap(state.candidate);
    std::vector<std::string_view>().swap(state.pieceWords);
    std::vector<std::string_view>().swap(state.pieceForms);
    std::vector<GlyphRun>().swap(state.pieceGlyphs);
    std::vector<EpdFontFamily::Style>().swap(state.pieceStyles);
    std::vector<TextBlock::WordSize>().swap(state.pieceSizes);
    std::vector<TokenBoundary::Boundary>().swap(state.pieceBoundaries);
    std::vector<uint16_t>().swap(state.tokenPieces);
    std::string().swap(state.formCopies);
  }
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
  forgetBidiParagraph();
  // Empty for the next paragraph, its chunks kept (rule 1).
  if (shapingState_) shapingState_->store.reset();
  shapes_ = false;
}

void ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId, std::vector<uint16_t>& out,
                                     const BidiLine::OpenControls& passControls) {
  out.clear();
  out.reserve(words.size());
  // An entry for every token before any is shaped; refused, the pass is laid out unshaped.
  if (shapes_ && !readyShapedStore()) shapes_ = false;

  decideCombined(renderer, fontId);
  // A paragraph that needs bidi shapes its tokens first, before the line pass's workspace is borrowed below: a shaping
  // call's block (about 6.3 KB on the C3) and the workspace (about 7.4 KB) are then never in the lent arena at once,
  // which would press the word cache's tail (risk 2). The loop below reads the widths the store kept, and shapes
  // nothing: a token that could not be shaped here is measured as its code points.
  const bool shapedFirst = shapes_ && bidi_;
  for (size_t i = 0; shapedFirst && i < words.size(); ++i) {
    if (!combines(i)) shapeToken(renderer, fontId, i, nullptr);
  }
  {
    // A paragraph that needs bidi breaks on each token's own drawn form (BidiLine::tokenForm), at the controls the text
    // before it leaves open. The workspace is borrowed from the build's lent arena for this loop alone.
    const BidiLine::ArenaWorkspace workspace(bidi_ ? lineArena_ : nullptr);
    BidiLine::Scratch* const scratch = bidi_ ? bidiScratchFor(workspace) : nullptr;
    BidiLine::OpenControls open = passControls;
    plainForm_.clear();
    for (size_t i = 0; i < words.size(); ++i) {
      const int font = wordFontId(i, fontId);
      uint16_t shapedWidth = 0;
      bool plainToken = false;  // set by tokenForm when it is asked: a width the placed line can take as it is
      if (combines(i)) {
        // A combined word is one em down its column, whatever its glyphs' advances.
        out.push_back(static_cast<uint16_t>(columnEm(renderer, font, wordStyles[i], wordScale(i))));
      } else if (shapes_ && (shapedFirst ? storedWidth(renderer, fontId, i, &shapedWidth)
                                         : shapeToken(renderer, fontId, i, &shapedWidth))) {
        // A token with pieces is as wide as they are: its runs' advances and its other code points measured as today.
        out.push_back(shapedWidth);
      } else if (scratch != nullptr &&
                 BidiLine::tokenForm(words[i], open, level_, *scratch, bidiState_->tokenForm, &plainToken)) {
        out.push_back(
            measureWordWidth(renderer, font, bidiState_->tokenForm, wordStyles[i], false, wordScale(i), vertical_));
      } else if (bidi_ && scratch == nullptr && withoutControls(words[i], bidiState_->tokenForm)) {
        // No scratch (logged): the block is laid out without bidi, and its controls are not drawn.
        out.push_back(
            measureWordWidth(renderer, font, bidiState_->tokenForm, wordStyles[i], false, wordScale(i), vertical_));
      } else {
        out.push_back(measureWordWidth(renderer, font, words[i], wordStyles[i], false, wordScale(i), vertical_));
      }
      if (scratch != nullptr) {
        plainForm_.push_back(plainToken ? 1 : 0);
        open.advance(words[i]);
      }
    }
  }
  applyPunctuationWidths(renderer, fontId, out);
  // After the marks are narrowed: a narrowed mark lends a ruby only its narrowed width.
  applyRubyWidths(renderer, fontId, out);
}

void ParsedText::decideCombined(const GfxRenderer& renderer, const int fontId) {
  combined_.clear();
  if (!vertical_) return;
  const auto flagged = std::find_if(wordBoundaries.begin(), wordBoundaries.end(),
                                    [](const TokenBoundary::Boundary b) { return TokenBoundary::combineRequested(b); });
  if (flagged == wordBoundaries.end()) return;
  combined_.assign(words.size(), 0);
  for (size_t i = static_cast<size_t>(flagged - wordBoundaries.begin()); i < words.size(); ++i) {
    combined_[i] = fitsCombined(renderer, fontId, i) ? 1 : 0;
  }
}

bool ParsedText::fitsCombined(const GfxRenderer& renderer, const int fontId, const size_t i) const {
  const auto flagged = [&](const size_t k) { return TokenBoundary::combineRequested(wordBoundaries[k]); };
  if (!flagged(i)) return false;
  // The flagged run is this token alone: a run the CJK rules cut into tokens (第1), or one spread over two elements,
  // is more than one block.
  if (i > 0 && flagged(i - 1) && !TokenBoundary::hasSpace(wordBoundaries[i])) return false;
  if (i + 1 < words.size() && flagged(i + 1) && !TokenBoundary::hasSpace(wordBoundaries[i + 1])) return false;
  int codepoints = 0;
  for (const auto* p = reinterpret_cast<const unsigned char*>(words[i].c_str()); *p;) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0x00AD) continue;  // a soft hyphen is not drawn
    if (++codepoints > kMaxCombinedCodepoints || drawsUpright(cp)) return false;
  }
  if (codepoints == 0) return false;
  // Set side by side across the column, the run fits the em at no less than kMinCombineScale.
  const int font = wordFontId(i, fontId);
  const int across = measureWordWidth(renderer, font, words[i], wordStyles[i], false, wordScale(i));
  const int em = columnEm(renderer, font, wordStyles[i], wordScale(i));
  return em > 0 && static_cast<float>(across) * kMinCombineScale <= static_cast<float>(em);
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
             (blockStyle.alignment == CssTextAlign::Justify ||
              blockStyle.alignment == (rtl() ? CssTextAlign::Right : CssTextAlign::Left))) {
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

    // A run the book asks to set upright down a column stays one token: a bold prefix would cut it.
    if (vertical_ && TokenBoundary::combineRequested(originalBoundary)) {
      transformedSuffix.push_back(std::move(source));
      transformedSuffixStyles.push_back(originalStyle);
      transformedSuffixBoundaries.push_back(originalBoundary);
      transformedSuffixSizes.push_back(originalSize);
      transformedSuffixOffsets.push_back(originalOffset);
      continue;
    }

    // Complex-script words never take a focus split: each half would shape on its own.
    // Ported from crosspoint-reader PR #3787 ("feat: add Indic language support without HarfBuzz", SSafayet /
    // @SSafayet).
    if (indic::containsIndic(source.c_str()) || arabic::holdsArabic(source.c_str(), source.size())) {
      transformedSuffix.push_back(std::move(source));
      transformedSuffixStyles.push_back(originalStyle);
      transformedSuffixBoundaries.push_back(originalBoundary);
      transformedSuffixSizes.push_back(originalSize);
      transformedSuffixOffsets.push_back(originalOffset);
      continue;
    }

    const auto spans = tokenizeBionicWord(source);
    if (spans.empty()) {
      continue;
    }

    // The first span keeps the word's own boundary; every span after it is glued to the one before.
    // A span of a ruby base after the first belongs to the base's group.
    // Every span keeps the word's emphasis mark and combine request.
    TokenBoundary::Boundary kGlued =
        TokenBoundary::withWordMarks(TokenBoundary::Boundary{true, false}, originalBoundary);
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
  // A ruby base is never split: its ruby sits over the whole of it. Nor, down a column, is a word the book asks to
  // combine: combined it is one em; not, a piece of it short enough to combine on a later pass would set it half
  // upright.
  if (TokenBoundary::rubyRole(wordBoundaries[wordIndex]) != TokenBoundary::RubyRole::None ||
      (vertical_ && TokenBoundary::combineRequested(wordBoundaries[wordIndex]))) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];
  const float scale = wordScale(wordIndex);
  const int font = wordFontId(wordIndex, fontId);
  // A word this pass shapes is measured as each part would be shaped. Only upstream's fallback breaks, between
  // syllables or Arabic clusters, reach one in practice: no language's patterns cut Indic letters, and an Arabic book
  // has no patterns.
  const bool shapedWord = shapes_ && shapedText(word);

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
  // A shaped word's candidates are copied into one string (shapedPrefixWidth), grown here once to the word and a
  // hyphen, behind the heap check: refused, the word is not split, as when its remainder's entry is refused below.
  if (shapedWord) {
    std::string& candidate = shapingState_->candidate;
    if (!heapAllows(growthBytes(candidate, word.size() + 1))) {
      LOG_ERR("SHP", "No room for a %u B word's split candidates: not split", static_cast<unsigned>(word.size()));
      latchShapingDegraded();
      return false;
    }
    candidate.reserve(word.size() + 1);
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
        shapedWord ? shapedPrefixWidth(renderer, fontId, wordIndex, offset, needsHyphen)
                   : measureWordWidth(renderer, font, word.substr(0, offset), style, needsHyphen, scale, vertical_);
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
  // The remainder's entry comes first, so the store stays in step with the words, for a split word that is not shaped
  // too (the retry that joins the parts again erases the entry): out of step, each token after it would look up its
  // neighbour's entry, and be laid out as its code points at its shaped width. Refused, the word is not split.
  if (shapes_ && !shapingState_->store.insert(wordIndex + 1)) {
    latchShapingDegraded();
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
              visibleBytesOutsideControls(words[wordIndex].data(), static_cast<int>(words[wordIndex].size())) -
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
  // The remainder keeps the word's emphasis mark and combine request.
  wordBoundaries.insert(wordBoundaries.begin() + wordIndex + 1,
                        TokenBoundary::withWordMarks(TokenBoundary::Boundary{}, wordBoundaries[wordIndex]));

  // Update cached widths to reflect the new prefix/remainder pairing. A shaped word's parts are shaped again, as the
  // tokens they now are; a part that is not shaped is measured as its code points.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  uint16_t remainderWidth = 0;
  if (shapedWord) {
    uint16_t shaped = 0;
    wordWidths[wordIndex] = shapeToken(renderer, fontId, wordIndex, &shaped)
                                ? shaped
                                : measureWordWidth(renderer, font, words[wordIndex], style, false, scale, vertical_);
    remainderWidth = shapeToken(renderer, fontId, wordIndex + 1, &shaped)
                         ? shaped
                         : measureWordWidth(renderer, font, remainder, style, false, scale, vertical_);
  } else {
    remainderWidth = measureWordWidth(renderer, font, remainder, style, false, scale, vertical_);
  }
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  if (wordIndex < plainForm_.size()) {
    plainForm_[wordIndex] = 0;  // the prefix's width now holds its hyphen: not the form the line pass places
    plainForm_.insert(plainForm_.begin() + wordIndex + 1, 0);
  }
  markClass_.insert(markClass_.begin() + wordIndex + 1, static_cast<uint8_t>(CjkPunctuation::Class::None));
  edgeTrim_.insert(edgeTrim_.begin() + wordIndex + 1, 0);
  shrink_.insert(shrink_.begin() + wordIndex + 1, 0);
  edgeShrink_.insert(edgeShrink_.begin() + wordIndex + 1, 0);
  if (!combined_.empty()) combined_.insert(combined_.begin() + wordIndex + 1, 0);  // a split word did not combine
  // A split word is never a ruby base (refused above): its remainder holds no ruby reserve either.
  if (!rubyEdge_.empty()) rubyEdge_.insert(rubyEdge_.begin() + wordIndex + 1, RubyEdge{});
  if (outInsertedHyphen) {
    *outInsertedHyphen = chosenNeedsHyphen;
  }
  return true;
}

BidiLine::Scratch* ParsedText::bidiScratchFor(const BidiLine::ArenaWorkspace& workspace) {
  return workspace.scratch() != nullptr ? workspace.scratch() : BidiLine::heapScratch();
}

bool ParsedText::layOutBidiLine(const GfxRenderer& renderer, const int fontId, const size_t first, const size_t count) {
  BidiState& state = *bidiState_;
  // The workspace is the pass's alone: its scope ends here, before the line is materialised (beforeLine_ may close the
  // page's arena block, and the line's TextBlock takes its bytes above it). No shaping call is made under it. The
  // line's tokens, 16 B each on the device, come with it: the pass is the last to need them (the materialisation keeps
  // their count and cut counts). Without a lent workspace they go to a heap vector kept across lines: a line longer
  // than any before grows it, behind the heap check the pass's other growths take. Refused, the line is laid out
  // without bidi, as when the pass has no scratch.
  const BidiLine::ArenaWorkspace workspace(lineArena_, count);
  BidiLine::Token* tokens = workspace.tokens();
  if (tokens == nullptr) {
    state.tokens.clear();
    if (!heapAllows(growthBytes(state.tokens, count))) {
      LOG_ERR("BIDI", "No room for a line's %u tokens: line laid out without bidi", static_cast<unsigned>(count));
      return false;
    }
    state.tokens.resize(count);
    tokens = state.tokens.data();
  }
  for (size_t t = 0; t < count; ++t) {
    new (tokens + t) BidiLine::Token{words[first + t], TokenBoundary::hasSpace(wordBoundaries[first + t])};
  }
  state.lineTokens = count;
  if (shapes_) cutShapedTokens(renderer, fontId, first, count, tokens);
  BidiLine::Scratch* const scratch = bidiScratchFor(workspace);
  if (scratch == nullptr) return false;
  if (BidiLine::layoutLine(tokens, count, state.openControls, level_, *scratch, state.line)) return true;
  LOG_ERR("BIDI", "No workspace for the line pass: line laid out without bidi");
  return false;
}

void ParsedText::cutShapedTokens(const GfxRenderer& renderer, const int fontId, const size_t first, const size_t count,
                                 BidiLine::Token* const tokens) {
  BidiState& state = *bidiState_;
  // The line's cuts are counted first and their room taken whole, so no growth moves the array under a pointer handed
  // out: 2 B a cut, and 2 B a token for the counts, kept across lines and freed at the slice's yield
  // (releaseLayoutScratch). No counts recorded (the heap refused them) reads as no cuts.
  size_t cuts = 0;
  for (size_t t = 0; t < count; ++t) {
    const auto pieces = shapingState_->piecesOf(*this, renderer, fontId, first + t);
    if (pieces) cuts += pieces->size() - 1;
  }
  state.tokenCuts.clear();
  state.tokenCutCount.clear();
  if (cuts == 0) return;
  if (!heapAllows(growthBytes(state.tokenCuts, cuts) + growthBytes(state.tokenCutCount, count))) {
    LOG_ERR("SHP", "No room for a line's %u cut points: its shaped tokens drawn as code points",
            static_cast<unsigned>(cuts));
    latchShapingDegraded();
    return;
  }
  state.tokenCuts.reserve(cuts);
  state.tokenCutCount.assign(count, 0);
  for (size_t t = 0; t < count; ++t) {
    const auto pieces = shapingState_->piecesOf(*this, renderer, fontId, first + t);
    if (!pieces || pieces->size() < 2) continue;
    const size_t at = state.tokenCuts.size();
    for (size_t p = 1; p < pieces->size(); ++p) state.tokenCuts.push_back((*pieces)[p].begin);
    BidiLine::Token& token = tokens[t];
    token.cutCount = static_cast<uint16_t>(pieces->size() - 1);
    token.cuts = state.tokenCuts.data() + at;
    // The pieces cover the token in order, so their edges keep the contract. A list that does not is a store out of
    // step with its token: the token is laid out uncut, drawn as its code points, and latched.
    if (!BidiLine::cutsAreValid(token)) {
      LOG_ERR("SHP", "Token %u's pieces break the cut contract: drawn as code points",
              static_cast<unsigned>(first + t));
      token.cutCount = 0;
      token.cuts = nullptr;
      latchShapingDegraded();
    }
    state.tokenCutCount[t] = token.cutCount;
  }
}

bool ParsedText::bidiLineIsPlain(const size_t first, const size_t count) const {
  const BidiState& state = *bidiState_;
  const std::vector<BidiLine::Piece>& pieces = state.line.pieces;
  if (rtl() || state.line.reordered || pieces.size() != count) return false;
  for (size_t v = 0; v < count; ++v) {
    const BidiLine::Piece& piece = pieces[v];
    if (piece.token != v ||
        !formIsText(words[first + v],
                    std::string_view(state.line.display).substr(piece.displayBegin, piece.displayLength))) {
      return false;
    }
  }
  return true;
}

bool ParsedText::formBidiPieces(const GfxRenderer& renderer, const int fontId, const size_t first) {
  BidiState& state = *bidiState_;
  const std::vector<BidiLine::Piece>& pieces = state.line.pieces;
  const size_t count = pieces.size();
  const char* const display = state.line.display.c_str();
  const auto displayForm = [&](const size_t v) {
    return std::string_view(display + pieces[v].displayBegin, pieces[v].displayLength);
  };
  // A display form's exact width, in its token's font, style and scale. A plain token's whole form is the one the
  // width pass measured (plainForm_): its width stands, and the piece is not measured a second time. The measure is
  // the TrueType glyph path on the device, and it ran twice for every token of a bidi paragraph.
  const auto measured = [&](const size_t v) {
    const BidiLine::Piece& piece = pieces[v];
    const size_t i = first + piece.token;
    if (i < plainForm_.size() && plainForm_[i] != 0 && piece.begin == 0 && piece.end == words[i].size() &&
        TokenBoundary::rubyRole(wordBoundaries[i]) == TokenBoundary::RubyRole::None) {
      return static_cast<int16_t>(wordWidths_[i]);
    }
    return static_cast<int16_t>(measureWordWidth(renderer, wordFontId(i, fontId), display + piece.displayBegin,
                                                 wordStyles[i], wordScale(i), vertical_));
  };
  // The store piece, not Text, that piece v draws, by Task 5's contract: one at most, the line cut at the store's
  // edges, a piece's bytes one segment's and the undrawn ones beside it (which are Text). Null: the piece is drawn as
  // the line pass's code points.
  const auto sourceOf = [&](const size_t v) -> const ShapedStore::Piece* {
    const BidiLine::Piece& piece = pieces[v];
    const auto stored = shapingState_->piecesOf(*this, renderer, fontId, first + piece.token);
    if (!stored) return nullptr;
    // A token of several pieces the line was not cut for (no room for the cuts, or a store out of step: latched).
    const size_t cutCount = piece.token < state.tokenCutCount.size() ? state.tokenCutCount[piece.token] : 0;
    if (stored->size() > 1 && cutCount + 1 != stored->size()) return nullptr;
    const ShapedStore::Piece* source = nullptr;
    for (const ShapedStore::Piece& sp : *stored) {
      if (sp.kind == IndicShaper::PieceKind::Text || sp.end <= piece.begin || sp.begin >= piece.end) continue;
      if (source != nullptr) return nullptr;  // two: not a piece the contract gives
      source = &sp;
    }
    // An empty run draws nothing wherever it lands: it is left off (below, where its ligature is drawn).
    if (source == nullptr || (source->kind == IndicShaper::PieceKind::Glyphs && source->count == 0)) return source;
    // The mandatory guard: a segment drawn as one form (glyphs, a Reordered form) must give one piece, or that form
    // would be drawn and measured on each (a mark across a cut drawn inside its base's segment, a Reordered segment
    // at two levels). Such a segment is drawn as the line pass's code points, which deal its characters out to them.
    return piece.segmentPieces > 1 ? nullptr : source;
  };

  std::vector<int16_t>& xs = lineXPosScratch_;
  xs.clear();
  xs.reserve(count);
  state.pieceForms.clear();
  state.pieceForms.reserve(count);
  state.pieceGlyphs.clear();
  // A paragraph that shapes keeps each piece's run beside its form, 8 B on the device, kept across lines: refused, a
  // piece that would be drawn from the store is its code points (latched).
  const bool runs = shapes_ && heapAllows(growthBytes(state.pieceGlyphs, count));
  if (runs) state.pieceGlyphs.reserve(count);
  bool glyphs = false;
  bool refusalLatched = false;
  for (size_t v = 0; v < count; ++v) {
    const ShapedStore::Piece* source = shapes_ ? sourceOf(v) : nullptr;
    if (source != nullptr && !runs) {
      if (!refusalLatched) {
        LOG_ERR("SHP", "No room for a line's %u glyph forms: drawn as code points", static_cast<unsigned>(count));
        latchShapingDegraded();
        refusalLatched = true;
      }
      source = nullptr;
    }
    GlyphRun run;
    if (source == nullptr) {
      state.pieceForms.push_back(displayForm(v));
      xs.push_back(measured(v));
    } else {
      // At the store's width, which broke the line. A glyph run's records are in visual order already; it has no form.
      const bool isRun = source->kind == IndicShaper::PieceKind::Glyphs;
      if (isRun) run = shapingState_->store.glyphs(*source);
      state.pieceForms.push_back(isRun ? std::string_view() : shapingState_->store.form(*source));
      xs.push_back(static_cast<int16_t>(source->width));
      glyphs = glyphs || isRun;
    }
    if (runs) state.pieceGlyphs.push_back(run);
  }
  // A run its context's ligature emptied is left off only where that ligature is drawn: in a glued neighbour's facing
  // run drawn from its glyphs on this line, or on an earlier line (a fallback break after a lam, accepted). Where the
  // neighbour is drawn as its code points (no room for its cuts, the guard, its own shaping refused), the letter is
  // drawn as its own code points, or nothing would draw it.
  const auto drawnFrom = [&](const size_t token, const ShapedStore::Piece* run) {
    for (size_t w = 0; run != nullptr && w < count; ++w) {
      if (pieces[w].token == token && state.pieceGlyphs[w].count != 0 && run->begin < pieces[w].end &&
          pieces[w].begin < run->end) {
        return true;
      }
    }
    return false;
  };
  const auto ligatureDrawn = [&](const size_t t) {
    const size_t i = first + t;
    if (wordBoundaries[i].continues) {
      if (i == 0) return true;  // the neighbour was an earlier pass's, drawn on its line
      const ShapedStore::Piece* run = shapingState_->edgeRun(*this, renderer, fontId, i - 1, true);
      if (run != nullptr && (t == 0 || drawnFrom(t - 1, run))) return true;
    }
    return t + 1 < state.lineTokens && wordBoundaries[i + 1].continues &&
           drawnFrom(t + 1, shapingState_->edgeRun(*this, renderer, fontId, i + 1, false));
  };
  for (size_t v = 0; glyphs && v < count; ++v) {
    if (!state.pieceForms[v].empty() || state.pieceGlyphs[v].count != 0 || ligatureDrawn(pieces[v].token)) continue;
    state.pieceForms[v] = displayForm(v);
    xs[v] = measured(v);
  }

  // A Reordered form keeps the token's soft hyphens (the line pass's forms hold none), which TextBlock strips from a
  // word's text and not from its form: it is drawn from a copy without them. Refused, the piece is its code points.
  size_t copyBytes = 0;
  for (const std::string_view form : state.pieceForms) {
    if (form.find(SOFT_HYPHEN_UTF8) != std::string_view::npos) copyBytes += form.size();
  }
  if (copyBytes == 0) return glyphs;
  std::string& copies = shapingState_->formCopies;
  if (!heapAllows(growthBytes(copies, copyBytes))) {
    LOG_ERR("SHP", "No %u B for a line's form copies: drawn as code points", static_cast<unsigned>(copyBytes));
    latchShapingDegraded();
    for (size_t v = 0; v < count; ++v) {
      if (state.pieceForms[v].find(SOFT_HYPHEN_UTF8) == std::string_view::npos) continue;
      state.pieceForms[v] = displayForm(v);
      xs[v] = measured(v);
    }
    return glyphs;
  }
  copyWithoutSoftHyphens(state.pieceForms, copies, copyBytes);
  return glyphs;
}

void ParsedText::dropEmptyPieces() {
  BidiState& state = *bidiState_;
  std::vector<BidiLine::Piece>& pieces = state.line.pieces;
  std::vector<int16_t>& xs = lineXPosScratch_;
  const auto off = [&](const size_t v) { return state.pieceForms[v].empty() && state.pieceGlyphs[v].count == 0; };
  // The reading positions left off, a bit each: a reading index is a byte.
  uint32_t dropped[8] = {};
  for (size_t v = 0; v < pieces.size(); ++v) {
    if (off(v)) dropped[pieces[v].readingIndex >> 5] |= 1u << (pieces[v].readingIndex & 31);
  }
  const auto droppedBelow = [&](const uint8_t index) {
    int n = 0;
    for (int word = 0; word < index >> 5; ++word) n += std::popcount(dropped[word]);
    return n + std::popcount(dropped[index >> 5] & ((1u << (index & 31)) - 1u));
  };
  size_t kept = 0;
  bool afterOff = false;  // a piece was left off since the last one kept
  bool spaceOff = false;  // and a word space came before it, which the next one kept takes
  uint16_t spaceToken = 0;
  for (size_t v = 0; v < pieces.size(); ++v) {
    BidiLine::Piece piece = pieces[v];
    if (off(v)) {
      if (kept > 0 && !spaceOff && piece.gapBefore == BidiLine::Gap::Space) {
        spaceOff = true;
        spaceToken = piece.spaceToken;
      }
      afterOff = true;
      continue;
    }
    if (kept == 0) {
      piece.gapBefore = BidiLine::Gap::None;  // the line's first
    } else if (afterOff) {
      // Past a run left off: a word space if one came before it or before this piece; else glued, and kerned by
      // nothing, as a glyph run's edge is (Gap::None after the first piece).
      if (spaceOff) {
        piece.gapBefore = BidiLine::Gap::Space;
        piece.spaceToken = spaceToken;
      } else if (piece.gapBefore != BidiLine::Gap::Space) {
        piece.gapBefore = BidiLine::Gap::None;
      }
    }
    piece.readingIndex = static_cast<uint8_t>(piece.readingIndex - droppedBelow(piece.readingIndex));
    pieces[kept] = piece;
    state.pieceForms[kept] = state.pieceForms[v];
    state.pieceGlyphs[kept] = state.pieceGlyphs[v];
    xs[kept] = xs[v];
    ++kept;
    afterOff = false;
    spaceOff = false;
  }
  pieces.resize(kept);
  state.pieceForms.resize(kept);
  state.pieceGlyphs.resize(kept);
  xs.resize(kept);
}

uint8_t ParsedText::placeBidiLine(const GfxRenderer& renderer, const int fontId, const size_t first,
                                  const int effectivePageWidth, const int lineIndent, const bool isLastLine,
                                  TextBlock::WordRange& range) {
  BidiState& state = *bidiState_;
  // A logical order numbers its words in a byte: a longer (fallback) line has none, and has none either once a piece
  // left off brings it under the limit (its reading indices are not exact).
  const bool ordered = state.line.reordered && state.line.pieces.size() <= TextBlock::kMaxOrderedWords;
  // Each piece's form and exact width (the x scratch holds the widths until the pieces are placed); a run a glued
  // neighbour's ligature emptied is left off.
  const bool runs = formBidiPieces(renderer, fontId, first);
  if (runs) dropEmptyPieces();
  const std::vector<BidiLine::Piece>& pieces = state.line.pieces;
  const size_t count = pieces.size();
  const auto form = [&](const size_t v) { return state.pieceForms[v]; };
  // The token whose word space comes before piece v (a Space gap): at least 1, since a fallback line names a word's own
  // token for a space between pieces that are not neighbours.
  const auto spaceToken = [&](const size_t v) { return std::max<size_t>(first + pieces[v].spaceToken, 1); };
  // The gap before piece v, unstretched: a word space is the space before its token, scaled as the space source's; a
  // glued pair kerns across the code points that face each other on the line, in the left piece's font and scale,
  // unless either is a glyph run, which the shaper kerned inside (step 8; a run has no form).
  const auto gapBefore = [&](const size_t v) {
    const BidiLine::Piece& piece = pieces[v];
    if (piece.gapBefore == BidiLine::Gap::Space) {
      const size_t j = spaceToken(v);
      return static_cast<int>(spaceBefore(renderer, fontId, j) * wordScale(spaceSource(fontId, j)) + 0.5f);
    }
    if (piece.gapBefore != BidiLine::Gap::Glued || form(v - 1).empty() || form(v).empty()) return 0;
    const size_t left = first + pieces[v - 1].token;
    const size_t right = first + piece.token;
    const int font = wordFontId(left, fontId);
    if (font != wordFontId(right, fontId)) return 0;
    const int kern =
        renderer.getKerning(font, lastCodepoint(form(v - 1)), firstCodepoint(form(v).data()), wordStyles[left]);
    return static_cast<int>(kern * wordScale(left) + 0.5f);
  };
  // A word space stretches when the line is justified, unless closing punctuation follows it, as on the ordinary path;
  // and so does a piece drawn as a space (a no-break space), between two pieces: at an end of the line its share would
  // only hold the line in from the margin.
  const auto stretchesBefore = [&](const size_t v) {
    return pieces[v].gapBefore == BidiLine::Gap::Space && stretchesGapBefore(spaceToken(v));
  };
  const auto stretchesAfter = [&](const size_t v) { return v > 0 && v + 1 < count && form(v) == " "; };

  std::vector<int16_t>& xs = lineXPosScratch_;
  int natural = 0;
  int gaps = 0;
  for (size_t v = 0; v < count; ++v) {
    natural += xs[v] + (v > 0 ? gapBefore(v) : 0);
    if (stretchesBefore(v)) ++gaps;
    if (stretchesAfter(v)) ++gaps;
  }

  // Justified, the gaps share what the line lacks, at most an eighth of the measure each; the remainder stays where the
  // line ends. The line then runs from its start side.
  const bool justified = blockStyle.alignment == CssTextAlign::Justify && !isLastLine && gaps >= 1;
  const int extra = justified ? std::min((effectivePageWidth - natural) / gaps, effectivePageWidth / 8) : 0;
  const int content = natural + extra * gaps;
  int x = lineIndent + gridInset_;
  if (rtl()) {
    if (blockStyle.alignment == CssTextAlign::Right || blockStyle.alignment == CssTextAlign::Justify) {
      x = gridInset_ + effectivePageWidth - content;
    } else if (blockStyle.alignment == CssTextAlign::Center) {
      x = gridInset_ + (effectivePageWidth - content) / 2;
    } else {
      x = gridInset_;
    }
  } else if (blockStyle.alignment == CssTextAlign::Right) {
    x = gridInset_ + effectivePageWidth - content;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    x = gridInset_ + (effectivePageWidth - content) / 2;
  }

  // The pieces as the line stores them, in visual order: each its token's bytes (the line piece's, any undrawn bytes
  // it took in included), style and size, glued unless a word space comes before it, drawn as its form or its run.
  state.pieceWords.clear();
  state.pieceStyles.clear();
  state.pieceSizes.clear();
  state.pieceBoundaries.clear();
  state.pieceOrder.clear();
  state.pieceWords.reserve(count);
  state.pieceStyles.reserve(count);
  state.pieceSizes.reserve(count);
  state.pieceBoundaries.reserve(count);
  state.pieceOrder.reserve(count);
  uint8_t maxPct = 100;
  for (size_t v = 0; v < count; ++v) {
    const BidiLine::Piece& piece = pieces[v];
    const size_t i = first + piece.token;
    if (v > 0) x += gapBefore(v) + (stretchesBefore(v) ? extra : 0);
    const int width = xs[v];
    xs[v] = static_cast<int16_t>(x);
    x += width + (stretchesAfter(v) ? extra : 0);

    state.pieceWords.push_back(std::string_view(words[i]).substr(piece.begin, piece.end - piece.begin));
    state.pieceStyles.push_back(wordStyles[i]);
    state.pieceSizes.push_back(wordSizes[i]);
    state.pieceBoundaries.push_back(TokenBoundary::Boundary{piece.gapBefore != BidiLine::Gap::Space, false});
    state.pieceOrder.push_back(piece.readingIndex);
    maxPct = std::max(maxPct, wordSizes[i].pct);
  }

  range = TextBlock::WordRange{};
  range.wordViews = state.pieceWords.data();
  range.displayForms = state.pieceForms.data();
  range.glyphForms = runs ? state.pieceGlyphs.data() : nullptr;
  range.logicalOrder = ordered ? state.pieceOrder.data() : nullptr;
  range.styles = &state.pieceStyles;
  range.sizes = &state.pieceSizes;
  range.boundaries = &state.pieceBoundaries;
  range.fontTable = fontSlots_;
  range.first = 0;
  range.count = count;
  return maxPct;
}

void ParsedText::viewWithoutControls(const size_t first, const size_t count, TextBlock::WordRange& range) {
  BidiState& state = *bidiState_;
  // The copies go to state.line.display, which the pass that did not run leaves free. Reserved whole first: no
  // reallocation below, so the views into it stay valid.
  std::string& copies = state.line.display;
  copies.clear();
  size_t bytes = 0;
  for (size_t t = 0; t < count; ++t) bytes += words[first + t].size() + 1;
  copies.reserve(bytes);
  state.pieceWords.clear();
  state.pieceStyles.clear();
  state.pieceSizes.clear();
  state.pieceBoundaries.clear();
  state.pieceWords.reserve(count);
  state.pieceStyles.reserve(count);
  state.pieceSizes.reserve(count);
  state.pieceBoundaries.reserve(count);
  for (size_t t = 0; t < count; ++t) {
    const size_t i = first + t;
    if (withoutControls(words[i], state.tokenForm)) {
      const size_t at = copies.size();
      copies += state.tokenForm;
      copies.push_back('\0');
      state.pieceWords.emplace_back(copies.data() + at, state.tokenForm.size());
    } else {
      state.pieceWords.emplace_back(words[i]);
    }
    state.pieceStyles.push_back(wordStyles[i]);
    state.pieceSizes.push_back(wordSizes[i]);
    state.pieceBoundaries.push_back(wordBoundaries[i]);
  }
  range.words = nullptr;
  range.wordViews = state.pieceWords.data();
  range.styles = &state.pieceStyles;
  range.sizes = &state.pieceSizes;
  range.boundaries = &state.pieceBoundaries;
  range.first = 0;
}

bool ParsedText::viewShapedPieces(const GfxRenderer& renderer, const int fontId, const size_t first, const size_t count,
                                  TextBlock::WordRange& range, const bool withoutControls) {
  if (!shapingState_) return false;
  ShapingState& state = *shapingState_;
  const ShapedStore& store = state.store;
  // Token i's pieces, which its width was measured from.
  const auto piecesOf = [&](const size_t i) { return state.piecesOf(*this, renderer, fontId, i); };
  // The line's pieces, at most: a token without pieces is one (a piece that draws nothing is counted, then left out).
  // And the bytes of the Reordered forms that hold a soft hyphen, copied without it below; and, without the controls,
  // the copies of every text placed, each with its NUL.
  bool anyPieces = false;
  size_t total = 0;
  size_t copyBytes = 0;
  size_t textBytes = 0;
  size_t emptiedBytes = 0;  // the longest run a ligature emptied, which may be measured as its code points below
  for (size_t t = 0; t < count; ++t) {
    const std::optional<std::span<const ShapedStore::Piece>> pieces = piecesOf(first + t);
    anyPieces = anyPieces || pieces.has_value();
    total += pieces ? pieces->size() : 1;
    textBytes += words[first + t].size() + (pieces ? pieces->size() : 1);
    if (!pieces) continue;
    for (const ShapedStore::Piece& piece : *pieces) {
      if (piece.kind == IndicShaper::PieceKind::Glyphs && piece.count == 0) {
        emptiedBytes = std::max<size_t>(emptiedBytes, piece.end - piece.begin);
      }
      if (piece.kind != IndicShaper::PieceKind::Reordered) continue;
      const std::string_view form = store.form(piece);
      if (form.find(SOFT_HYPHEN_UTF8) != std::string_view::npos) copyBytes += form.size();
    }
  }
  if (!anyPieces) return false;
  std::string* const texts = withoutControls ? &bidiState_->line.display : nullptr;

  // Every vector below is reserved whole, so none grows in the loop, and the copies too: on the device a piece takes
  // 2 * 8 B (two views) + 8 (a run) + 1 (a style) + 2 (a size) + 4 (a boundary) + 2 (an x) = 33 B and a token 2 B,
  // kept across lines (a line of 40 pieces is about 1.4 KB) and freed at the slice's yield (releaseLayoutScratch). One
  // check of the heap for all they ask, before any: refused, the line keeps its bytes, its tokens drawn as their code
  // points. The measuring scratch is grown for an emptied run's bytes under it too.
  // The tokens' positions are [0, count) of the x scratch: the pieces' go after them, and then take their place.
  std::vector<int16_t>& xs = lineXPosScratch_;
  const size_t asked = growthBytes(state.pieceWords, total) + growthBytes(state.pieceForms, total) +
                       growthBytes(state.pieceGlyphs, total) + growthBytes(state.pieceStyles, total) +
                       growthBytes(state.pieceSizes, total) + growthBytes(state.pieceBoundaries, total) +
                       growthBytes(state.tokenPieces, count + 1) + growthBytes(xs, count + total) +
                       growthBytes(state.formCopies, copyBytes) + (texts ? growthBytes(*texts, textBytes) : 0) +
                       growthBytes(state.measureScratch, emptiedBytes);
  if (!heapAllows(asked)) {
    LOG_ERR("SHP", "No %u B for a line's pieces: laid out as its tokens' code points", static_cast<unsigned>(asked));
    latchShapingDegraded();
    return false;
  }
  state.pieceWords.clear();
  state.pieceForms.clear();
  state.pieceGlyphs.clear();
  state.pieceStyles.clear();
  state.pieceSizes.clear();
  state.pieceBoundaries.clear();
  state.tokenPieces.clear();
  state.pieceWords.reserve(total);
  state.pieceForms.reserve(total);
  state.pieceGlyphs.reserve(total);
  state.pieceStyles.reserve(total);
  state.pieceSizes.reserve(total);
  state.pieceBoundaries.reserve(total);
  state.tokenPieces.reserve(count + 1);
  xs.reserve(count + total);
  if (texts != nullptr) {
    texts->clear();
    texts->reserve(textBytes);
  }
  if (state.measureScratch.capacity() < emptiedBytes) state.measureScratch.reserve(emptiedBytes);
  // The text a piece is drawn from: its bytes, or, without the controls, a copy without them (and a Text piece's
  // without its joiners, as it was measured) when it holds any.
  const auto drawn = [&](const std::string_view bytes, const bool joiners) {
    if (texts == nullptr || bytes.find_first_of("\xE2\xD8") == std::string_view::npos) return bytes;
    const size_t at = texts->size();
    appendWithoutControls(*texts, bytes, joiners);
    const std::string_view copy(texts->data() + at, texts->size() - at);
    texts->push_back('\0');
    return copy;
  };
  const auto place = [&](const size_t i, const std::string_view text, const std::string_view form, const GlyphRun run,
                         const TokenBoundary::Boundary boundary, const int x) {
    state.pieceWords.push_back(text);
    state.pieceForms.push_back(form);
    state.pieceGlyphs.push_back(run);
    state.pieceStyles.push_back(wordStyles[i]);
    state.pieceSizes.push_back(wordSizes[i]);
    state.pieceBoundaries.push_back(boundary);
    xs.push_back(static_cast<int16_t>(x));
  };
  // Whether the ligature that emptied token t's run is drawn: in a glued neighbour's facing glyph run, on this line or
  // an earlier one (a token's pieces are all placed here, so its runs are drawn from their glyphs).
  const auto ligatureDrawn = [&](const size_t t) {
    const size_t i = first + t;
    if (wordBoundaries[i].continues && (i == 0 || state.edgeRun(*this, renderer, fontId, i - 1, true) != nullptr)) {
      return true;
    }
    return t + 1 < count && wordBoundaries[i + 1].continues &&
           state.edgeRun(*this, renderer, fontId, i + 1, false) != nullptr;
  };
  for (size_t t = 0; t < count; ++t) {
    const size_t i = first + t;
    state.tokenPieces.push_back(static_cast<uint16_t>(state.pieceWords.size()));
    const int x = xs[t];
    const std::optional<std::span<const ShapedStore::Piece>> pieces = piecesOf(i);
    if (!pieces) {
      place(i, drawn(words[i], false), std::string_view(), GlyphRun{}, wordBoundaries[i], x);
      continue;
    }
    const std::string_view text = words[i];
    int before = 0;  // the widths of the token's pieces before this one
    bool placed = false;
    for (const ShapedStore::Piece& piece : *pieces) {
      const int at = x + before;
      before += piece.width;
      // A run its context's ligature emptied (deviation 7's lam-alef) draws nothing: placed, TextBlock would draw a
      // word without a form as its text, the letter again. Its bytes stay the token's. Where that ligature is not
      // drawn, the piece is placed, and drawn as its code points at their width (its own is 0), as on a bidi line.
      const bool emptied = piece.kind == IndicShaper::PieceKind::Glyphs && piece.count == 0;
      if (emptied && ligatureDrawn(t)) continue;
      std::string_view bytes = text.substr(piece.begin, piece.end - piece.begin);
      if (piece.kind == IndicShaper::PieceKind::Text) {
        bytes = drawn(bytes, true);
        if (bytes.empty() || onlySoftHyphens(bytes)) continue;
      }
      if (emptied) {
        // Copied for its NUL, within the room reserved above; a run holds no soft hyphen.
        state.measureScratch.assign(bytes);
        before += measureWordWidth(renderer, wordFontId(i, fontId), state.measureScratch.c_str(), wordStyles[i],
                                   wordScale(i), vertical_);
      }
      const std::string_view form =
          piece.kind == IndicShaper::PieceKind::Reordered ? store.form(piece) : std::string_view();
      place(i, bytes, form, piece.kind == IndicShaper::PieceKind::Glyphs ? store.glyphs(piece) : GlyphRun{},
            placed ? TokenBoundary::Boundary{true, false} : wordBoundaries[i], at);
      placed = true;
    }
  }
  state.tokenPieces.push_back(static_cast<uint16_t>(state.pieceWords.size()));
  xs.erase(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(count));

  // A Reordered form keeps the token's soft hyphens, which TextBlock strips from a word's text and not from its form:
  // such a form is drawn from a copy without them, reserved whole under the heap check above.
  if (copyBytes > 0) copyWithoutSoftHyphens(state.pieceForms, state.formCopies, copyBytes);

  // The annotations name pieces: a run over tokens covers all their pieces, and one over pieces all left out has
  // nothing to sit on.
  for (TextBlock::Annotation& a : lineAnnotations_) {
    const uint16_t end = state.tokenPieces[a.firstWord + a.wordCount];
    a.firstWord = state.tokenPieces[a.firstWord];
    a.wordCount = static_cast<uint16_t>(end - a.firstWord);
  }
  lineAnnotations_.erase(std::remove_if(lineAnnotations_.begin(), lineAnnotations_.end(),
                                        [](const TextBlock::Annotation& a) { return a.wordCount == 0; }),
                         lineAnnotations_.end());

  range.words = nullptr;
  range.wordViews = state.pieceWords.data();
  range.displayForms = state.pieceForms.data();
  range.glyphForms = state.pieceGlyphs.data();
  range.styles = &state.pieceStyles;
  range.sizes = &state.pieceSizes;
  range.boundaries = &state.pieceBoundaries;
  range.first = 0;
  range.count = state.pieceWords.size();
  return true;
}

void ParsedText::stepOpenControls(const size_t first, const size_t end) {
  if (!bidi_) return;
  for (size_t i = first; i < end && i < words.size(); ++i) bidiState_->openControls.advance(words[i]);
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
  // The parser's tokens: a merged token counts as its parts.
  uint32_t lineTokens = static_cast<uint32_t>(lineWordCount);
  for (size_t i = lastBreakAt; i < lineBreak; ++i) lineTokens += boundaries[i].merged;
  lastLineTokenCount_ = lineTokens + controlsOnlyTokens_;

  // Apply indent only to line 0 of the layout pass; firstLineIndent is already
  // 0 for continuation flushes (computed once in layoutAndExtractLines).
  const int lineIndent = (breakIndex == 0) ? firstLineIndent : 0;

  // A line of a paragraph that needs bidi: one UAX #9 pass over its tokens (decision 5). Its pieces are placed in
  // visual order, unless the line is its tokens as they stand in a left-to-right paragraph: that takes the path below.
  // So does a line the pass could not run on (no workspace or scratch, logged), without its bidi controls.
  bool withoutBidi = false;
  if (bidi_) {
    withoutBidi = !layOutBidiLine(renderer, fontId, lastBreakAt, lineWordCount);
    if (!withoutBidi && !bidiLineIsPlain(lastBreakAt, lineWordCount)) {
      const int effectivePageWidth =
          widthForLine(static_cast<int>(breakIndex), lineHeight, blockStartY, pageWidth) - lineIndent;
      TextBlock::WordRange range;
      const uint8_t maxPct = placeBidiLine(renderer, fontId, lastBreakAt, effectivePageWidth, lineIndent,
                                           lineBreak == words.size(), range);
      // Only controls, or only letters a glued neighbour's ligature took (a line the breaker's fallback starts after a
      // lam, at its alef: accepted, the letter is drawn in the previous line's ligature): nothing to draw.
      if (range.count == 0) {
        controlsOnlyTokens_ += lineTokens;
        stepOpenControls(lastBreakAt, lineBreak);
        return LineProcessResult::Accepted;
      }
      return emitLine(range, maxPct, false, lastBreakAt, lineBreak, processLine, lineEndsWithHyphenatedWord,
                      suppressHyphenationRetry);
    }
  }

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

  // A no-break space (a " " token glued to the word before it) stretches like a word gap, as long as a word is glued to
  // it. Otherwise a word gap follows it, which stretches for itself, or the line ends there and nothing stretches. The
  // gap count and the x loop below both ask here, so a justified line gives out exactly the shares it counted. Ported
  // from crosspoint-reader PR #4142 (Uri Tauber).
  const auto isStretchableSpace = [&](const size_t wordIdx) {
    const size_t i = lastBreakAt + wordIdx;
    return words[i] == " " && boundaries[i].continues && wordIdx + 1 < lineWordCount && boundaries[i + 1].continues;
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
    // distribution so they stay at natural space width (stretchesGapBefore).
    const TokenBoundary::Boundary boundary = boundaries[lastBreakAt + wordIdx];
    if (wordIdx > 0 && !boundary.continues) {
      // A word gap or a CJK gap: both stretch when justified. A word gap is a space wide, a CJK gap has no width but
      // the CJK-Latin gap's.
      if (stretchesGapBefore(lastBreakAt + wordIdx)) actualGapCount++;
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
      // Non-breaking space tokens are visible spaces: count them as justifiable gaps so justifyExtra reaches them too.
      if (isStretchableSpace(wordIdx)) actualGapCount++;
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
      if (isStretchableSpace(wordIdx) && blockStyle.alignment == CssTextAlign::Justify && !isLastLine) {
        advance += gapExtra();
      }
      xpos += advance;
    } else {
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
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
        if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine &&
            stretchesGapBefore(lastBreakAt + wordIdx + 1)) {
          gap += gapExtra();
        }
      }
      xpos += box + gap;
    }
  }

  // One Emphasis record per run of consecutive tokens with the same mark, outside ruby groups (ruby wins over a mark
  // on the same base). The runs and the ruby boxes never overlap.
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
  // One Combine record per combined word: its cell, one em from where it is drawn (TextBlock sets it upright there).
  for (size_t wordIdx = 0; !combined_.empty() && wordIdx < lineWordCount; ++wordIdx) {
    const size_t i = lastBreakAt + wordIdx;
    if (!combines(i)) continue;
    TextBlock::Annotation& a = lineAnnotations_.emplace_back();
    a.firstWord = static_cast<uint16_t>(wordIdx);
    a.wordCount = 1;
    a.x = lineXPos[wordIdx];
    a.width = static_cast<uint16_t>(columnEm(renderer, wordFontId(i, fontId), wordStyles[i], wordScale(i)));
    a.kind = TextBlock::AnnotationKind::Combine;
  }
  // The records go in firstWord order (a ruby box or an emphasis run and a combined word may start at the same word:
  // then by kind). Each kind is in order already.
  const auto inLineOrder = [](const TextBlock::Annotation& l, const TextBlock::Annotation& r) {
    return l.firstWord != r.firstWord ? l.firstWord < r.firstWord : l.kind < r.kind;
  };
  if (!std::is_sorted(lineAnnotations_.begin(), lineAnnotations_.end(), inLineOrder)) {
    std::sort(lineAnnotations_.begin(), lineAnnotations_.end(), inLineOrder);
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
  if (withoutBidi) {
    // A paragraph that shapes draws the line as its tokens' pieces all the same, in logical order, without the
    // controls: drawn as code points, a token would overrun the glyph runs' widths it was measured at.
    if (!shapes_ || !viewShapedPieces(renderer, fontId, lastBreakAt, lineWordCount, range, true)) {
      viewWithoutControls(lastBreakAt, lineWordCount, range);
    }
  } else if (shapes_) {
    // A token with a shaped run and other text is drawn as its pieces, cut at the run's edges (step 7); a line without
    // one keeps its bytes.
    viewShapedPieces(renderer, fontId, lastBreakAt, lineWordCount, range, false);
  }
  if (range.count == 0) {  // every piece left off (a letter its glued neighbour's ligature took): nothing to draw, as
                           // on a bidi line
    controlsOnlyTokens_ += lineTokens;
    stepOpenControls(lastBreakAt, lineBreak);
    return LineProcessResult::Accepted;
  }

  uint8_t maxPct = 100;
  bool band = false;
  if (beforeLine_) {
    for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
      maxPct = std::max(maxPct, wordSizes[lastBreakAt + wordIdx].pct);
    }
    // Only a ruby or an emphasis run is drawn in the band above the line: a combined word is drawn in its cell.
    band = std::any_of(lineAnnotations_.begin(), lineAnnotations_.end(),
                       [](const TextBlock::Annotation& a) { return a.kind != TextBlock::AnnotationKind::Combine; });
  }
  return emitLine(range, maxPct, band, lastBreakAt, lineBreak, processLine, lineEndsWithHyphenatedWord,
                  suppressHyphenationRetry);
}

ParsedText::LineProcessResult ParsedText::emitLine(
    const TextBlock::WordRange& range, const uint8_t maxSizePct, const bool band, const size_t first, const size_t end,
    const std::function<LineProcessResult(std::unique_ptr<TextBlock>, bool, bool)>& processLine,
    const bool lineEndsWithHyphenatedWord, const bool suppressHyphenationRetry) {
  // The page-fit decision runs before the line exists (see beforeLine_): the parser may emit
  // the current page here and open the next one's arena block, so the block built just below
  // is allocated from the page it will actually land on.
  if (beforeLine_) beforeLine_(maxSizePct, band);

  // TextBlock flattens the range into its arena on construct; on arena OOM the
  // block is invalid, so drop the line rather than render/serialize garbage.
  auto block = makeUniqueNoThrow<TextBlock>(range, lineXPosScratch_, blockStyle, lineArena_);
  if (!block || !block->valid()) {
    LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
    controlsOnlyTokens_ = 0;
    stepOpenControls(first, end);
    return LineProcessResult::Accepted;
  }
  const LineProcessResult result = processLine(std::move(block), lineEndsWithHyphenatedWord, suppressHyphenationRetry);
  // A line the parser asks to lay out again is the same text: the controls step when it is accepted.
  if (result == LineProcessResult::Accepted) {
    stepOpenControls(first, end);
    controlsOnlyTokens_ = 0;
  }
  return result;
}
