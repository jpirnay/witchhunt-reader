#pragma once

#include <cstddef>

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

// Consume one token of `marker` with one page word.
//
// `markerPos` is how much of the marker earlier words already accounted for; it advances past the
// matched token and any spaces after it when the word matches. Returns false and leaves
// `markerPos` alone otherwise, so a caller matching a multi-word marker can abandon a partial run
// and start again at the next word.
inline bool consumeToken(const char* marker, const size_t markerLen, size_t& markerPos, const char* word,
                         const size_t wordLen) {
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
  if (tokenLen != wordEnd - wordStart) return false;
  for (size_t i = 0; i < tokenLen; ++i) {
    if (marker[tokenCoreStart + i] != word[wordStart + i]) return false;
  }

  markerPos = pos;
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
