#include "BookSearch.h"

#include <Utf8.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "VisibleText.h"
#include "htmlEntities.h"

namespace BookSearch {
namespace {

bool isSpaceCodepoint(const uint32_t cp) { return cp < 0x80 && VisibleText::isSpace(static_cast<unsigned char>(cp)); }

// An element that starts a block of its own: text on either side of it is not one word.
bool isBlockElement(const char* name) {
  static constexpr const char* const kBlocks[] = {
      "p",   "div",    "br",         "hr",         "li",      "ul",      "ol",    "dl",     "dt",
      "dd",  "h1",     "h2",         "h3",         "h4",      "h5",      "h6",    "table",  "tr",
      "td",  "th",     "caption",    "blockquote", "section", "article", "aside", "header", "footer",
      "nav", "figure", "figcaption", "pre",        "address", "body",    "title"};
  for (const char* const block : kBlocks) {
    if (strcasecmp(name, block) == 0) return true;
  }
  return false;
}

}  // namespace

bool isWordCodepoint(const uint32_t cp) {
  if (cp < 0x80) return std::isalnum(static_cast<int>(cp)) != 0;
  if (cp >= 0xA0 && cp <= 0xBF) return cp == 0xAA || cp == 0xB5 || cp == 0xBA;  // Latin-1 punctuation and signs
  if (cp == 0xD7 || cp == 0xF7) return false;                                   // × ÷
  if (cp >= 0x2000 && cp <= 0x206F) return false;                               // general punctuation
  if (cp >= 0x3000 && cp <= 0x303F) return false;                               // CJK symbols and punctuation
  if (cp >= 0xFF00 && cp <= 0xFF0F) return false;                               // fullwidth punctuation
  return true;
}

Pattern::Pattern(const std::string& query) {
  const auto* p = reinterpret_cast<const unsigned char*>(query.c_str());
  bool lastWasSpace = true;
  while (*p != '\0' && length_ < kMaxCodepoints) {
    const uint32_t cp = utf8NextCodepoint(&p);
    uint32_t folded[2];
    size_t n = 0;
    if (isSpaceCodepoint(cp)) {
      folded[0] = ' ';
      n = 1;
    } else {
      n = utf8BookSearchFold(cp, folded);
    }
    for (size_t i = 0; i < n && length_ < kMaxCodepoints; ++i) {
      const bool space = folded[i] == ' ';
      if (space && lastWasSpace) continue;
      lastWasSpace = space;
      codepoints_[length_++] = folded[i];
    }
  }
  while (length_ > 0 && codepoints_[length_ - 1] == ' ') --length_;
  uint8_t k = 0;
  for (uint8_t i = 1; i < length_; ++i) {
    while (k > 0 && codepoints_[i] != codepoints_[k]) k = fail_[k - 1];
    if (codepoints_[i] == codepoints_[k]) ++k;
    fail_[i] = k;
  }
}

ChapterSearch::ChapterSearch(const Pattern& pattern, const bool wholeWords, HitSink& out, const size_t capacity,
                             size_t& count, const uint16_t spine, const uint32_t chapterBytes, const uint32_t from,
                             const uint32_t until)
    : pattern_(pattern),
      matcher_(pattern, wholeWords),
      out_(out),
      capacity_(capacity),
      count_(count),
      spine_(spine),
      chapterBytes_(chapterBytes),
      from_(from),
      until_(until) {}

bool ChapterSearch::begin(BuildArena* const arena) {
  if (pattern_.empty()) return false;
  if (arena != nullptr && arena->valid()) {
    const size_t bytes = SaxParser::stateBytes(SaxParser::Profile::Lean);
    if (void* const storage = arena->alloc(bytes)) parser_.setExternalState(storage, bytes);
  }
  // As the layout parser reads a chapter: HTML's bare void tags repaired. Lean: no attributes are wanted.
  return parser_.init(this, onStart, onEnd, onText, onDefault, /*htmlVoidTagRepair=*/true, SaxParser::Profile::Lean);
}

void ChapterSearch::onStart(void* self, const char* name, const char** /*atts*/) {
  auto* search = static_cast<ChapterSearch*>(self);
  search->partialLength_ = 0;  // a sequence cut by a tag is malformed: dropped
  if (VisibleText::isNonVisibleTag(name)) search->nonVisibleDepth_++;
  if (search->bodyDepth_ < 0 && strcasecmp(name, "body") == 0) search->bodyDepth_ = search->depth_;
  search->depth_++;
  if (isBlockElement(name)) search->separate();
}

void ChapterSearch::onEnd(void* self, const char* name) {
  auto* search = static_cast<ChapterSearch*>(self);
  search->partialLength_ = 0;
  search->depth_--;
  if (isBlockElement(name)) search->separate();
  if (VisibleText::isNonVisibleTag(name) && search->nonVisibleDepth_ > 0) search->nonVisibleDepth_--;
}

void ChapterSearch::onText(void* self, const char* text, const int len) {
  static_cast<ChapterSearch*>(self)->text(text, len);
}

// An entity the XML parser does not know, as the layout parser takes it (ChapterHtmlSlimParser::defaultHandlerExpand):
// a known HTML entity as its UTF-8 value, an unknown one as written; anything else ignored.
void ChapterSearch::onDefault(void* self, const char* text, const int len) {
  if (len < 3 || text[0] != '&' || text[len - 1] != ';') return;
  const char* const value = lookupHtmlEntity(text, static_cast<size_t>(len));
  if (value != nullptr) {
    static_cast<ChapterSearch*>(self)->text(value, static_cast<int>(strlen(value)));
  } else {
    static_cast<ChapterSearch*>(self)->text(text, len);
  }
}

void ChapterSearch::text(const char* text, const int len) {
  // VisibleText's rule, as the layout parser counts: text inside <body>, outside head, script and style.
  if (bodyDepth_ < 0 || nonVisibleDepth_ > 0 || len <= 0) return;
  for (int i = 0; i < len; ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (partialLength_ > 0 && (byte & 0xC0) != 0x80) partialLength_ = 0;  // a sequence that broke off: passed over
    // Every byte counts as VisibleText::visibleBytes counts it, a malformed one too, so the offsets stay the layout's.
    const uint32_t at = offset_;
    if (!VisibleText::isSpace(byte)) ++offset_;
    if (partialLength_ == 0) {
      if (utf8CodepointLen(byte) == 1) {
        // ASCII; or a continuation byte, or a lead byte that starts no sequence: one character, as it came.
        const char single = static_cast<char>(byte);
        codepoint(byte < 0x80 ? byte : 0xFFFD, &single, 1, at);
        continue;
      }
      partialOffset_ = at;
    }
    partial_[partialLength_++] = static_cast<char>(byte);
    if (partialLength_ == utf8CodepointLen(static_cast<unsigned char>(partial_[0]))) {
      char sequence[5] = {};
      std::memcpy(sequence, partial_, partialLength_);
      const auto* p = reinterpret_cast<const unsigned char*>(sequence);
      const uint8_t length = partialLength_;
      partialLength_ = 0;
      codepoint(utf8NextCodepoint(&p), sequence, length, partialOffset_);
    }
  }
}

void ChapterSearch::codepoint(const uint32_t cp, const char* bytes, const uint8_t length, const uint32_t offset) {
  cpOffset_ = offset;
  const bool space = length == 1 && VisibleText::isSpace(static_cast<unsigned char>(bytes[0]));
  if (space) {
    separate();
    return;
  }
  uint32_t folded[2];
  const size_t n = utf8BookSearchFold(cp, folded);
  if (n == 0) return;  // a soft hyphen, a zero-width character: not text to search, nor to show
  if (n == 1 && folded[0] == ' ') {
    separate();
    return;
  }
  cpSource_ = source_;
  ring(bytes, length);
  lastWasSpace_ = false;
  for (size_t i = 0; i < n; ++i) unit(folded[i]);
}

void ChapterSearch::separate() {
  if (lastWasSpace_) return;
  lastWasSpace_ = true;
  cpOffset_ = offset_;
  cpSource_ = source_;
  ring(" ", 1);
  unit(' ');
}

void ChapterSearch::ring(const char* bytes, const uint8_t length) {
  for (uint8_t b = 0; b < length; ++b) {
    ring_[source_ % kRing] = bytes[b];
    ++source_;
    for (uint8_t i = 0; i < queued_; ++i) {
      if (want_[i] == 0) continue;
      Hit& hit = queue_[i];
      hit.snippet[hit.snippetLength++] = bytes[b];
      if (--want_[i] == 0 || hit.snippetLength >= Hit::kSnippetBytes) want_[i] = 0;
    }
  }
  handOn(/*all=*/false);
  if (stopping_ && queued_ == 0) parser_.stop();
}

void ChapterSearch::handOn(const bool all) {
  uint8_t done = 0;
  while (done < queued_ && (all || want_[done] == 0)) {
    Hit& hit = queue_[done++];
    hit.snippetLength = static_cast<uint8_t>(utf8SafeTruncateBuffer(hit.snippet, hit.snippetLength));
    out_.add(hit);
  }
  if (done == 0) return;
  for (uint8_t i = done; i < queued_; ++i) {
    queue_[i - done] = queue_[i];
    want_[i - done] = want_[i];
  }
  queued_ = static_cast<uint8_t>(queued_ - done);
}

void ChapterSearch::unit(const uint32_t folded) {
  const uint8_t slot = static_cast<uint8_t>(units_ % kUnits);
  unitOffset_[slot] = cpOffset_;
  unitSource_[slot] = cpSource_;
  matcher_.push(folded, units_, [this](const uint32_t first, uint32_t) { accept(first); });
  ++units_;
}

void ChapterSearch::accept(const uint32_t firstUnit) {
  const uint32_t startOffset = unitOffset_[firstUnit % kUnits];
  const uint32_t startSource = unitSource_[firstUnit % kUnits];
  if (startOffset < from_ || startOffset >= until_ || count_ >= capacity_) return;
  if (queued_ == kOpenHits) {
    // The oldest snippet is closed as it stands, and handed on with any complete behind it.
    want_[0] = 0;
    handOn(/*all=*/false);
  }
  ++count_;
  Hit& hit = queue_[queued_];
  hit = Hit{};
  hit.spine = spine_;
  hit.offset = startOffset;
  // Where the parser is in the chapter's bytes: the end of the text run the match is in, close enough for a place
  // the list shows in percent of the book. finish() ends a word after the stream, at its end.
  const uint32_t at = parser_.byteOffset();
  hit.chapterPermille =
      chapterBytes_ == 0 ? 0 : static_cast<uint16_t>(std::min<uint64_t>(999, uint64_t{at} * 1000 / chapterBytes_));
  // Up to kBefore bytes before the match, as far as the ring still holds them, from a character's first byte; then
  // the match, and what follows as it comes.
  const uint32_t oldest = source_ > kRing ? source_ - kRing : 0;
  uint32_t first = startSource > kBefore ? startSource - kBefore : 0;
  if (first < oldest) first = oldest;
  while (first < startSource && (static_cast<uint8_t>(ring_[first % kRing]) & 0xC0) == 0x80) ++first;
  size_t n = 0;
  for (uint32_t p = first; p < source_ && n < Hit::kSnippetBytes; ++p) hit.snippet[n++] = ring_[p % kRing];
  hit.snippetLength = static_cast<uint8_t>(n);
  want_[queued_] = static_cast<uint8_t>(Hit::kSnippetBytes - n);
  ++queued_;
  handOn(/*all=*/false);
  if (count_ >= capacity_) {
    stopping_ = true;
    if (queued_ == 0) parser_.stop();
  }
}

bool ChapterSearch::finish() {
  partialLength_ = 0;
  const bool stoppedEarly = parser_.isStopped();
  const bool parsed = stoppedEarly || (!sink_.failed() && parser_.finalize());
  // The end of the chapter ends a word; then every hit goes, its snippet as far as the chapter took it.
  matcher_.end([this](const uint32_t first, uint32_t) { accept(first); });
  handOn(/*all=*/true);
  return parsed;
}

}  // namespace BookSearch
