#pragma once

#include <BuildArena.h>
#include <Print.h>
#include <SaxParser/SaxFeedSink.h>
#include <SaxParser/SaxParser.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Searching a book's text: a query, and a chapter's XHTML read through the SAX parser the layout reads it with, so a
// hit's offset is the visible-text offset (VisibleText.h) its page is found by (Section::getPageForVisibleTextOffset).
// Text and query are folded alike (utf8BookSearchFold): case, diacritics, curly quotes and dashes do not count, and a
// run of white space is one space. A block element's start or end is a space too, so a match never runs from the end
// of one paragraph into the next.
//
// Nothing here opens a book: the caller streams a chapter into sink() (Epub::readItemContentsToStream) and calls
// finish().
namespace BookSearch {

// The query, folded, at most kMaxCodepoints long; empty when it holds nothing to search for.
class Pattern {
 public:
  static constexpr uint8_t kMaxCodepoints = 48;

  explicit Pattern(const std::string& query);
  bool empty() const { return length_ == 0; }
  uint8_t length() const { return length_; }
  uint32_t at(const uint8_t i) const { return codepoints_[i]; }
  // KMP: the longest proper prefix of the first i+1 codepoints that is also their suffix.
  uint8_t fail(const uint8_t i) const { return fail_[i]; }

 private:
  uint32_t codepoints_[kMaxCodepoints]{};
  uint8_t fail_[kMaxCodepoints]{};
  uint8_t length_ = 0;
};

// A character a word is made of: an ASCII letter or digit, or any other script's character but punctuation.
bool isWordCodepoint(uint32_t cp);

// The query found in a stream of folded codepoints (KMP), each carrying a tag the caller gives it: a match is reported
// by the tags of its first and last codepoint. With whole words, a match with a word character beside it where the
// query has one at that end is none; the codepoint after a match decides it, or end().
class Matcher {
 public:
  Matcher(const Pattern& pattern, const bool wholeWords) : pattern_(pattern), wholeWords_(wholeWords) {}

  // The next codepoint: found(firstTag, lastTag) for each match it decides -- one it completes, or one it closes.
  template <typename Found>
  void push(const uint32_t cp, const uint32_t tag, Found&& found) {
    if (pending_) {
      pending_ = false;
      if (!isWordCodepoint(cp)) found(pendingFirst_, pendingLast_);
    }
    const uint8_t slot = static_cast<uint8_t>(units_ % kUnits);
    cp_[slot] = cp;
    tag_[slot] = tag;
    ++units_;
    const uint8_t length = pattern_.length();
    if (length == 0) return;
    while (state_ > 0 && cp != pattern_.at(state_)) state_ = pattern_.fail(static_cast<uint8_t>(state_ - 1));
    if (cp == pattern_.at(state_)) ++state_;
    if (state_ < length) return;
    state_ = pattern_.fail(static_cast<uint8_t>(length - 1));
    const uint32_t first = units_ - length;
    const uint32_t firstTag = tag_[first % kUnits];
    if (wholeWords_) {
      if (first > 0 && isWordCodepoint(pattern_.at(0)) && isWordCodepoint(cp_[(first - 1) % kUnits])) return;
      if (isWordCodepoint(pattern_.at(static_cast<uint8_t>(length - 1)))) {
        pending_ = true;
        pendingFirst_ = firstTag;
        pendingLast_ = tag;
        return;
      }
    }
    found(firstTag, tag);
  }
  // The stream's end, which ends a word: a match it left waiting.
  template <typename Found>
  void end(Found&& found) {
    if (pending_) found(pendingFirst_, pendingLast_);
    pending_ = false;
  }

 private:
  static constexpr uint8_t kUnits = Pattern::kMaxCodepoints + 1;
  const Pattern& pattern_;
  const bool wholeWords_;
  uint32_t cp_[kUnits]{};
  uint32_t tag_[kUnits]{};
  uint32_t units_ = 0;
  uint8_t state_ = 0;  // codepoints of the pattern matched
  bool pending_ = false;
  uint32_t pendingFirst_ = 0;
  uint32_t pendingLast_ = 0;
};

// One place the query was found: the chapter, the visible-text offset of the match's first character in it, how far
// through the chapter that is (per mille of its visible text), and the text around it.
struct Hit {
  static constexpr size_t kSnippetBytes = 80;
  uint16_t spine = 0;
  uint16_t chapterPermille = 0;
  uint32_t offset = 0;
  uint8_t snippetLength = 0;
  char snippet[kSnippetBytes]{};
};

// One chapter's search, its hits tagged `spine` and added to the caller's array until it is full: the parse then
// stops. Only hits at an offset in [from, until) are kept (a search begins and ends at the page it was started on).
// One object a chapter.
class ChapterSearch {
 public:
  // A chapter's place in the hits' order is unknown when its parse stopped before the end (the hits were full).
  static constexpr uint16_t kPermilleUnknown = UINT16_MAX;

  ChapterSearch(const Pattern& pattern, bool wholeWords, Hit* hits, size_t capacity, size_t& count, uint16_t spine,
                uint32_t from = 0, uint32_t until = UINT32_MAX);
  ChapterSearch(const ChapterSearch&) = delete;
  ChapterSearch& operator=(const ChapterSearch&) = delete;

  // False when the parser could not start (no memory for its state). `arena`: a lent region the parser's state goes
  // into rather than the heap, where it has room (the stream takes its own from it too).
  bool begin(BuildArena* arena = nullptr);
  // Where the chapter's XHTML goes, and the flag that stops its stream once the hits are full or the parse failed.
  Print& sink() { return sink_; }
  const bool* stopFlag() const { return sink_.stopFlag(); }
  // The chapter read: the last match closed, each hit's place in the chapter set. False on a parse error; the hits
  // found before it stand.
  bool finish();
  bool full() const { return count_ >= capacity_; }

 private:
  static constexpr size_t kRing = 128;     // bytes of recent text, for snippets
  static constexpr size_t kBefore = 28;    // of a snippet, at most, before the match
  static constexpr uint8_t kOpenHits = 4;  // snippets still taking text after their match

  static void onStart(void* self, const char* name, const char** atts);
  static void onEnd(void* self, const char* name);
  static void onText(void* self, const char* text, int len);
  static void onDefault(void* self, const char* text, int len);

  void text(const char* text, int len);
  void codepoint(uint32_t cp, const char* bytes, uint8_t length, uint32_t offset);
  void separate();
  void ring(const char* bytes, uint8_t length);
  void unit(uint32_t folded);
  void accept(uint32_t firstUnit);

  const Pattern& pattern_;
  Matcher matcher_;
  Hit* const hits_;
  const size_t capacity_;
  size_t& count_;

  const uint16_t spine_;
  const uint32_t from_;
  const uint32_t until_;
  const size_t firstHit_;
  SaxParser parser_;
  SaxFeedSink sink_{parser_};
  // The hits are full: the parse stops once the open snippets have their text.
  bool stopping_ = false;

  // The layout parser's rule (ChapterHtmlSlimParser::characterData): text counts from <body> on, outside head, script
  // and style.
  int depth_ = 0;
  int bodyDepth_ = -1;
  int nonVisibleDepth_ = 0;
  uint32_t offset_ = 0;
  // A UTF-8 sequence a chunk of text ended inside, and the offset of its first byte.
  char partial_[4]{};
  uint8_t partialLength_ = 0;
  uint32_t partialOffset_ = 0;

  // The units the matcher was given, tagged by their count: for each its offset and where its source begins in the
  // ring. As many as a match and the codepoint that decides it span.
  static constexpr uint8_t kUnits = Pattern::kMaxCodepoints + 2;
  uint32_t unitOffset_[kUnits]{};
  uint32_t unitSource_[kUnits]{};
  uint32_t units_ = 0;
  bool lastWasSpace_ = true;
  // The current codepoint's offset and source position, for the units it folds to.
  uint32_t cpOffset_ = 0;
  uint32_t cpSource_ = 0;

  // Recent text, white space collapsed, for snippets; `source_` counts every byte it has taken.
  char ring_[kRing]{};
  uint32_t source_ = 0;
  // Hits whose snippets still take text: index into the hits, and bytes still wanted.
  size_t openHit_[kOpenHits]{};
  uint8_t openWant_[kOpenHits]{};
  uint8_t openCount_ = 0;
};

}  // namespace BookSearch
