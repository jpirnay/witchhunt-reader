#pragma once

#include <Utf8.h>

#include <cstddef>
#include <cstdint>

// Matching a page word against the display text of an internal link, so the reader can put a tap
// target on the marker a reader can see.
//
// The problem this solves: the parser records an internal link's DISPLAY TEXT
// (FootnoteEntry::number) and its href, but not where on the page the text ended up -- footnotes
// are attached to a page by word count, not geometry. The reader therefore has to find the text
// again among the words it is about to draw. Two things make that less trivial than strcmp:
//
//   1. The parser NORMALISES a noteref on the way in: "[12]" is stored as "12", but the word
//      actually painted is still "[12]". So the page word has to be normalised the same way
//      before comparing, or every bracketed footnote marker in the corpus misses.
//   2. Link text is not always one word. Every internal <a href> is collected, cross-references
//      included, so the stored text can be "turn to 256" -- three page words that together make
//      one tappable run.
//
// Hence a token walk rather than an equality test: the caller feeds page words in reading order
// and this consumes the marker one token at a time, so the caller can union the boxes of the
// words that matched.
namespace LinkMarkerMatch {

inline bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// The wrapper characters the parser strips from a noteref, plus whitespace. Kept in one place so
// the page-word side normalises by exactly the rule the stored side was normalised by.
inline bool isWrapper(const char c) { return isSpace(c) || c == '[' || c == ']'; }

// The bidi controls a paragraph that needs bidi keeps in its word text: U+200E, U+200F, U+061C, U+202A-202E and
// U+2066-2069 (BidiLine::isControl, which this header-only component cannot include). They are not part of what a
// reader sees as the marker, so both ends of a word and of a marker token skip them, as they skip a bracket. Each is a
// multi-byte UTF-8 sequence: these return its length at `at` (or ending at `end`), else 0.
inline size_t bidiControlLenAt(const char* text, const size_t at, const size_t end) {
  const auto b = [&](const size_t i) { return static_cast<unsigned char>(text[i]); };
  if (at + 2 <= end && b(at) == 0xD8 && b(at + 1) == 0x9C) return 2;  // U+061C
  if (at + 3 > end || b(at) != 0xE2) return 0;
  if (b(at + 1) == 0x80 && (b(at + 2) == 0x8E || b(at + 2) == 0x8F || (b(at + 2) >= 0xAA && b(at + 2) <= 0xAE))) {
    return 3;  // U+200E, U+200F, U+202A-202E
  }
  if (b(at + 1) == 0x81 && b(at + 2) >= 0xA6 && b(at + 2) <= 0xA9) return 3;  // U+2066-2069
  return 0;
}

inline size_t bidiControlLenBefore(const char* text, const size_t start, const size_t end) {
  for (size_t len = 2; len <= 3; ++len) {
    if (end >= start + len && bidiControlLenAt(text, end - len, end) == len) return len;
  }
  return 0;
}

// The interior of a page word: the span left after the wrappers (and bidi controls) come off both ends. Returns false
// when nothing is left, which is the right answer for a lone bracket or a run of spaces -- those
// are not a marker and must not match an empty tail of one.
inline bool core(const char* text, const size_t len, size_t& start, size_t& end) {
  if (text == nullptr) return false;
  size_t s = 0;
  size_t e = len;
  while (s < e) {
    if (isWrapper(text[s])) {
      ++s;
    } else if (const size_t n = bidiControlLenAt(text, s, e)) {
      s += n;
    } else {
      break;
    }
  }
  while (e > s) {
    if (isWrapper(text[e - 1])) {
      --e;
    } else if (const size_t n = bidiControlLenBefore(text, s, e)) {
      e -= n;
    } else {
      break;
    }
  }
  if (s >= e) return false;
  start = s;
  end = e;
  return true;
}

// The code point that starts at text[at] (at < end), from bytes the view holds; U+FFFD for a sequence it cuts short
// or a byte that starts none.
inline uint32_t codepointAt(const char* text, const size_t at, const size_t end) {
  const auto b = [&](const size_t i) { return static_cast<unsigned char>(text[i]); };
  const unsigned char lead = b(at);
  const size_t n = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
  if (n == 0 || at + n > end) return 0xFFFD;
  if (n == 1) return lead;
  uint32_t cp = lead & (0x7F >> n);
  for (size_t i = 1; i < n; ++i) cp = (cp << 6) | (b(at + i) & 0x3F);
  return cp;
}

// The code point that ends at text[end - 1] (start < end).
inline uint32_t codepointBefore(const char* text, const size_t start, const size_t end) {
  size_t at = end - 1;
  while (at > start && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
  return codepointAt(text, at, end);
}

// Whether the parser may have stored one marker token as two page words split between these code points: CJK text
// written without spaces (utf8IsUnspacedCjk), which gives a token per character. Hangul is kept whole between spaces
// (keep-all), so a boundary touching it is no such place.
inline bool cjkSplit(const uint32_t before, const uint32_t after) {
  return utf8IsUnspacedCjk(before) || utf8IsUnspacedCjk(after);
}

// Consume one token of `marker` with one page word.
//
// `markerPos` is how much of the marker earlier words already accounted for; it advances past the
// matched token and any spaces after it when the word matches. Returns false and leaves
// `markerPos` alone otherwise, so a caller matching a multi-word marker can abandon a partial run
// and start again at the next word.
//
// A word may also match the front of the token, and `markerPos` then stops inside it for the next word: CJK text is
// stored one page word per character, so a link "第一章" is three words, while its text is one token between
// spaces. That is allowed only where the parser could have split the token: at a CJK break (cjkSplit), or where the
// caller says the next page word is glued to this one with no space (`nextGlued`: a styled run that changes
// mid-word). So a "1" and a separate "2" never make a link "12".
inline bool consumeToken(const char* marker, const size_t markerLen, size_t& markerPos, const char* word,
                         const size_t wordLen, const bool nextGlued = false) {
  if (marker == nullptr) return false;

  size_t wordStart = 0;
  size_t wordEnd = 0;
  if (!core(word, wordLen, wordStart, wordEnd)) return false;

  size_t pos = markerPos;
  while (pos < markerLen && isSpace(marker[pos])) ++pos;
  if (pos >= markerLen) return false;

  const size_t tokenStart = pos;
  while (pos < markerLen && !isSpace(marker[pos])) ++pos;

  // The marker token is compared as stored, minus its own wrappers: "12" was already stripped on
  // the way in, but the multi-word case never was, so a bracketed token inside a longer link text
  // still has to come off here.
  size_t tokenCoreStart = 0;
  size_t tokenCoreEnd = 0;
  if (!core(marker + tokenStart, pos - tokenStart, tokenCoreStart, tokenCoreEnd)) return false;
  tokenCoreStart += tokenStart;
  tokenCoreEnd += tokenStart;

  const size_t tokenLen = tokenCoreEnd - tokenCoreStart;
  const size_t coreLen = wordEnd - wordStart;
  if (coreLen > tokenLen) return false;
  for (size_t i = 0; i < coreLen; ++i) {
    if (marker[tokenCoreStart + i] != word[wordStart + i]) return false;
  }
  if (coreLen == tokenLen) {
    markerPos = pos;
    return true;
  }
  // The front of the token: only where the token could have been split there (see above), and never inside a code
  // point.
  const size_t splitAt = tokenCoreStart + coreLen;
  if ((static_cast<unsigned char>(marker[splitAt]) & 0xC0) == 0x80) return false;
  if (!nextGlued &&
      !cjkSplit(codepointBefore(marker, tokenCoreStart, splitAt), codepointAt(marker, splitAt, tokenCoreEnd))) {
    return false;
  }
  markerPos = splitAt;
  return true;
}

// True when every token of the marker has been accounted for -- i.e. the run of page words that
// matched is the whole link text and its box is complete.
inline bool complete(const char* marker, const size_t markerLen, size_t markerPos) {
  if (marker == nullptr) return false;
  while (markerPos < markerLen && isSpace(marker[markerPos])) ++markerPos;
  return markerPos >= markerLen && markerLen > 0;
}

}  // namespace LinkMarkerMatch
