#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

class BuildArena;
class Epub;

// A converter can leave a chapter's heading in a spine file of its own and its text in the next: calibre's split cuts a
// file where the source asked for a page break, so a chapter number ends up alone in `<stem>_split_000` and the text
// in `<stem>_split_001` (seen in 29 of 81 spine files of a calibre book). Every spine file starts a page, so the
// heading stood alone on one. Such a file is "carried": it gets no page of its own, and the next file's build lays its
// blocks out at the top of its first page (Section).
//
// A file is carried when all of these hold, a function of the book alone (never stored; Epub::isCarriedSpine
// remembers the last few answers):
//   1. it is not the last spine item, and its XHTML is at most kMaxFileBytes;
//   2. the next file has no TOC entry of its own (it inherits this one's), and either this file's TOC entry starts at
//      it with no fragment, or the two are calibre split siblings (splitSiblings);
//   3. its body holds headings only (Shape::Heading), at most kMaxHeadingTextBytes of text, or nothing (Shape::Empty);
//   4. the next file is not such a file itself: of two heading files in a row only the last is carried.
// A book laid out vertically joins nothing (Section decides, it knows the request).
namespace HeadingCarry {

constexpr size_t kMaxFileBytes = 4096;
constexpr size_t kMaxHeadingTextBytes = 200;

enum class Shape : uint8_t {
  Other,    // anything else, or a document the scan could not follow
  Heading,  // visible text inside h1-h6 only, and none of the elements that make a page more than a title
  Empty,    // no visible text and none of those elements
};

// What a spine file's <body> holds, from its XHTML fed in pieces. Allocation-free, so it answers the same whatever
// the heap. Rejects (Other) a link, a picture, a rule, a table, media and forms; text outside a heading; a body whose
// elements do not close, or that has no end tag; a document past kMaxFileBytes.
class Scanner {
 public:
  void feed(const uint8_t* data, size_t len);
  Shape finish() const;
  // Where the body's content lies in the document: from just after the <body ...> start tag to the '<' of </body>.
  // Valid when finish() is not Other.
  uint32_t bodyBegin() const { return bodyBegin_; }
  uint32_t bodyEnd() const { return bodyEnd_; }
  // For a file of any size (the one a heading file is carried into): consumes `data` up to and including the '>' of
  // its <body ...> start tag and returns how many bytes that took, all of `len` while the tag has not come.
  size_t feedThroughBodyStart(const uint8_t* data, size_t len);
  bool bodyStarted() const { return inBody_; }

 private:
  enum class State : uint8_t { Text, Open, Name, Attrs, AttrName, AttrValue, Comment, Bang, Cdata, Pi };
  void step(char c);
  void startTag();
  void endTag();
  void textByte(char c);

  State state_ = State::Text;
  uint32_t pos_ = 0;
  uint32_t tagStart_ = 0;
  uint32_t bodyBegin_ = 0;
  uint32_t bodyEnd_ = 0;
  uint32_t headingText_ = 0;
  char name_[12] = {};
  uint8_t nameLen_ = 0;
  char attr_[8] = {};
  uint8_t attrLen_ = 0;
  char quote_ = 0;
  // Comment / CDATA / PI end detection: the last bytes seen.
  char tail1_ = 0;
  char tail2_ = 0;
  uint8_t bangLen_ = 0;
  uint16_t depth_ = 0;  // elements open inside the body
  uint8_t heading_ = 0;
  uint8_t skip_ = 0;  // script or style open: their text is not shown
  bool closing_ = false;
  bool selfClosing_ = false;
  bool hasHref_ = false;
  bool inBody_ = false;
  bool afterBody_ = false;
  bool textOutside_ = false;
  bool rejected_ = false;
};

// True for calibre's split names: `<stem>_split_<n><ext>` and `<stem>_split_<n + 1><ext>`, the same stem and
// extension (paths compared whole).
bool splitSiblings(std::string_view first, std::string_view second);

enum class Verdict : uint8_t { No, Yes, Unknown };

// The rules above for spine item `spine` of `epub`. Unknown when a file it had to read could not be read (no memory
// for the inflate, a card error): the caller treats it as No and asks again later. `scratch`: as for scanSpine.
Verdict decide(const Epub& epub, int spine, BuildArena* scratch = nullptr);

// Feeds spine item `spine`'s XHTML into `scanner`: from its banked copy (Section::sectionHtmlCachePath) when that is
// whole, through a 256 B window on the stack and nothing else; else inflated from the book, and with `bankIt` banked
// as it is read. A deflated file needs a ring as long as itself (kMaxFileBytes at most: a back-reference reaches as far
// back as the bytes produced so far), taken from `scratch` when it has room (a build's lent region), else from the heap
// for the call; the inflater's own state (EntryReader, about 1.3 KB) is from the heap. False when it could not be read.
bool scanSpine(const Epub& epub, int spine, size_t inflatedSize, Scanner& scanner, BuildArena* scratch = nullptr,
               bool bankIt = false);

}  // namespace HeadingCarry
