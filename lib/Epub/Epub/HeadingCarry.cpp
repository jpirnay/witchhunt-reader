#include "HeadingCarry.h"

#include <BuildArena.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <ZipFile.h>

#include <cstddef>
#include <cstring>
#include <iterator>

#include "Epub.h"
#include "Section.h"

namespace HeadingCarry {
namespace {

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool isNameChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '-' ||
         c == '_' || c == '.';
}

char lower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool oneOf(const char* name, const char* const* list, const size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(name, list[i]) == 0) return true;
  }
  return false;
}

// What makes a page more than a title: a picture, a rule, a table, media, a form. A link is checked by its href.
constexpr const char* const kRejected[] = {
    "img",  "image",  "svg",     "video", "audio",  "object",   "embed",  "iframe", "table", "hr",
    "math", "canvas", "picture", "input", "select", "textarea", "button", "form",   "frame", "applet",
};
constexpr const char* const kVoid[] = {"br",   "hr",  "img", "meta",   "link",  "input", "area",
                                       "base", "col", "wbr", "source", "embed", "param", "track"};

bool isHeading(const char* name) { return name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0'; }

// The element name without a namespace prefix (svg:svg, m:math).
const char* localName(const char* name) {
  const char* colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}

}  // namespace

void Scanner::feed(const uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    step(static_cast<char>(data[i]));
    ++pos_;
  }
  if (pos_ > kMaxFileBytes) rejected_ = true;
}

size_t Scanner::feedThroughBodyStart(const uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (inBody_) return i;
    step(static_cast<char>(data[i]));
    ++pos_;
  }
  return len;
}

void Scanner::textByte(const char c) {
  if (!inBody_ || afterBody_ || skip_ > 0 || isSpace(c)) return;
  if (heading_ > 0) {
    ++headingText_;
  } else {
    textOutside_ = true;
  }
}

void Scanner::startTag() {
  const char* name = localName(name_);
  if (!inBody_) {
    if (strcmp(name, "body") == 0) {
      inBody_ = true;
      bodyBegin_ = pos_ + 1;  // pos_ is the tag's '>'
      if (selfClosing_) {
        bodyEnd_ = bodyBegin_;
        afterBody_ = true;
      }
    }
    return;
  }
  if (afterBody_) return;
  if (strcmp(name, "body") == 0 || oneOf(name, kRejected, std::size(kRejected)) ||
      (strcmp(name, "a") == 0 && hasHref_)) {
    rejected_ = true;
    return;
  }
  if (selfClosing_ || oneOf(name, kVoid, std::size(kVoid))) return;
  ++depth_;
  if (isHeading(name)) ++heading_;
  if (strcmp(name, "script") == 0 || strcmp(name, "style") == 0) ++skip_;
}

void Scanner::endTag() {
  if (!inBody_ || afterBody_) return;
  const char* name = localName(name_);
  if (strcmp(name, "body") == 0) {
    if (depth_ != 0) rejected_ = true;
    bodyEnd_ = tagStart_;
    afterBody_ = true;
    return;
  }
  if (oneOf(name, kVoid, std::size(kVoid))) return;
  if (depth_ == 0) {
    rejected_ = true;
    return;
  }
  --depth_;
  if (isHeading(name) && heading_ > 0) --heading_;
  if ((strcmp(name, "script") == 0 || strcmp(name, "style") == 0) && skip_ > 0) --skip_;
}

void Scanner::step(const char c) {
  switch (state_) {
    case State::Text:
      if (c == '<') {
        tagStart_ = pos_;
        state_ = State::Open;
      } else {
        textByte(c);
      }
      return;
    case State::Open:
      nameLen_ = 0;
      name_[0] = '\0';
      selfClosing_ = false;
      hasHref_ = false;
      if (c == '/') {
        closing_ = true;
        state_ = State::Name;
      } else if (c == '!') {
        attrLen_ = 0;
        state_ = State::Bang;
      } else if (c == '?') {
        tail1_ = 0;
        state_ = State::Pi;
      } else if (isNameChar(c)) {
        closing_ = false;
        name_[nameLen_++] = lower(c);
        name_[nameLen_] = '\0';
        state_ = State::Name;
      } else {
        rejected_ = true;  // a bare '<' in text: not well-formed, the layout parser stops there too
        state_ = State::Text;
      }
      return;
    case State::Name:
      if (isNameChar(c)) {
        // A name longer than the buffer is no element this scan knows; kept as its first bytes.
        if (static_cast<size_t>(nameLen_) + 1 < sizeof(name_)) {
          name_[nameLen_++] = lower(c);
          name_[nameLen_] = '\0';
        }
        return;
      }
      state_ = State::Attrs;
      step(c);
      return;
    case State::Attrs:
      if (c == '>') {
        if (closing_) {
          endTag();
        } else {
          startTag();
        }
        state_ = State::Text;
      } else if (c == '/') {
        selfClosing_ = true;
      } else if (c == '"' || c == '\'') {
        quote_ = c;
        state_ = State::AttrValue;
      } else if (isNameChar(c)) {
        selfClosing_ = false;
        attrLen_ = 0;
        attr_[attrLen_++] = lower(c);
        attr_[attrLen_] = '\0';
        state_ = State::AttrName;
      }
      return;
    case State::AttrName:
      if (isNameChar(c)) {
        if (static_cast<size_t>(attrLen_) + 1 < sizeof(attr_)) {
          attr_[attrLen_++] = lower(c);
          attr_[attrLen_] = '\0';
        } else {
          attr_[0] = '\0';  // longer than any attribute looked for
        }
        return;
      }
      if (strcmp(attr_, "href") == 0) hasHref_ = true;
      state_ = State::Attrs;
      step(c);
      return;
    case State::AttrValue:
      if (c == quote_) state_ = State::Attrs;
      return;
    case State::Comment:
      if (c == '>' && tail1_ == '-' && tail2_ == '-') state_ = State::Text;
      tail2_ = tail1_;
      tail1_ = c;
      return;
    case State::Bang:
      // <!-- a comment, <![CDATA[ text, else a declaration (DOCTYPE) up to its '>'.
      if (static_cast<size_t>(attrLen_) + 1 < sizeof(attr_)) {
        attr_[attrLen_++] = c;
        attr_[attrLen_] = '\0';
      }
      if (attrLen_ == 2 && attr_[0] == '-' && attr_[1] == '-') {
        tail1_ = tail2_ = 0;
        state_ = State::Comment;
      } else if (attrLen_ == 7 && strcmp(attr_, "[CDATA[") == 0) {
        tail1_ = tail2_ = 0;
        state_ = State::Cdata;
      } else if (c == '>') {
        state_ = State::Text;
      }
      return;
    case State::Cdata:
      if (c == '>' && tail1_ == ']' && tail2_ == ']') {
        state_ = State::Text;
      } else {
        textByte(c);
      }
      tail2_ = tail1_;
      tail1_ = c;
      return;
    case State::Pi:
      if (c == '>' && tail1_ == '?') state_ = State::Text;
      tail1_ = c;
      return;
  }
}

Shape Scanner::finish() const {
  if (rejected_ || !inBody_ || !afterBody_ || state_ != State::Text || textOutside_) return Shape::Other;
  if (headingText_ > kMaxHeadingTextBytes) return Shape::Other;
  return headingText_ == 0 ? Shape::Empty : Shape::Heading;
}

bool splitSiblings(const std::string_view first, const std::string_view second) {
  constexpr std::string_view kSplit = "_split_";
  const size_t at = first.rfind(kSplit);
  if (at == std::string_view::npos) return false;
  const size_t digits = at + kSplit.size();
  if (second.substr(0, digits) != first.substr(0, digits)) return false;
  // The number after "_split_" (calibre writes three digits), then the extension, which both share.
  const auto number = [digits](const std::string_view path, size_t& end) {
    unsigned long n = 0;
    for (end = digits; end < path.size() && end < digits + 6 && path[end] >= '0' && path[end] <= '9'; ++end) {
      n = n * 10 + static_cast<unsigned long>(path[end] - '0');
    }
    return n;
  };
  size_t endFirst = 0;
  size_t endSecond = 0;
  const unsigned long a = number(first, endFirst);
  const unsigned long b = number(second, endSecond);
  const std::string_view ext = first.substr(endFirst);
  return endFirst > digits && endSecond > digits && b == a + 1 && !ext.empty() && ext[0] == '.' &&
         ext.find('/') == std::string_view::npos && ext == second.substr(endSecond);
}

bool scanSpine(const Epub& epub, const int spine, const size_t inflatedSize, Scanner& scanner, BuildArena* scratch,
               const bool bankIt) {
  uint8_t window[256];
  size_t total = 0;
  // The banked copy a build of this spine left: no inflate, no heap. A plain open: no copy is no error.
  const std::string banked = Section::sectionHtmlCachePath(epub.getCachePath(), spine);
  FsFile file = Storage.open(banked.c_str());
  if (file && file.size() == inflatedSize) {
    for (int n = file.read(window, sizeof(window)); n > 0; n = file.read(window, sizeof(window))) {
      scanner.feed(window, static_cast<size_t>(n));
      total += static_cast<size_t>(n);
    }
    file.close();
    return total == inflatedSize;
  }
  if (file) file.close();

  // Else the entry itself.
  ZipFile::FileStatSlim stat = {};
  if (!epub.getSpineItemStat(spine, &stat)) return false;
  const size_t arenaBytes = sizeof(window) + InflateReader::ringSizeFor(inflatedSize) + 2 * alignof(std::max_align_t);
  std::unique_ptr<BuildArena> own;
  if (!scratch || scratch->capacity() - scratch->used() < arenaBytes) {
    own = makeUniqueNoThrow<BuildArena>(arenaBytes);
    if (!own || !own->valid()) {
      LOG_INF("HCA", "Spine %d: no %u B to read it", spine, static_cast<unsigned>(arenaBytes));
      return false;
    }
    scratch = own.get();
  }
  if (bankIt) {
    Storage.mkdir(Epub::spineCacheDir(epub.getCachePath(), spine).c_str());
    if (!Storage.openFileForWrite("HCA", banked, file)) return false;
  }
  ZipFile zip(epub.getPath());
  epub.primeZip(zip);
  bool ok = false;
  {
    ZipFile::EntryReader reader(zip, sizeof(window), scratch);
    ok = reader.open(stat);
    for (bool done = false; ok && !done;) {
      size_t produced = 0;
      ok = reader.step(window, sizeof(window), &produced, &done) &&
           (!file || produced == 0 || file.write(window, produced) == produced);
      scanner.feed(window, produced);
      total += produced;
    }
  }
  epub.adoptZipDetails(zip);
  ok = ok && total == inflatedSize;
  if (file) {
    file.close();
    if (!ok) Storage.remove(banked.c_str());
  }
  if (!ok) LOG_ERR("HCA", "Spine %d: XHTML not read", spine);
  return ok;
}

Verdict decide(const Epub& epub, const int spine, BuildArena* scratch) {
  if (spine < 0 || spine + 1 >= epub.getSpineItemsCount()) return Verdict::No;
  const auto head = epub.getSpineItem(spine);
  const size_t headBytes = head.zipUncompressedSize;
  if (headBytes == 0 || headBytes > kMaxFileBytes) return Verdict::No;
  const auto next = epub.getSpineItem(spine + 1);
  // The next file has a TOC entry of its own: it starts a chapter.
  if (next.tocIndex != head.tocIndex) return Verdict::No;
  bool oneChapter = splitSiblings(head.href, next.href);
  // A TOC that names too few files is replaced by one entry per file (Epub::getTocItem), which says nothing here.
  if (!oneChapter && head.tocIndex >= 0 && epub.hasReliableToc()) {
    const auto entry = epub.getTocItem(head.tocIndex);
    oneChapter = entry.spineIndex == spine && entry.anchor.empty();
  }
  if (!oneChapter) return Verdict::No;

  Scanner headScan;
  if (!scanSpine(epub, spine, headBytes, headScan, scratch)) return Verdict::Unknown;
  if (headScan.finish() == Shape::Other) return Verdict::No;

  const size_t nextBytes = next.zipUncompressedSize;
  if (nextBytes == 0) return Verdict::No;
  if (nextBytes <= kMaxFileBytes) {
    Scanner nextScan;
    if (!scanSpine(epub, spine + 1, nextBytes, nextScan, scratch)) return Verdict::Unknown;
    if (nextScan.finish() != Shape::Other) return Verdict::No;  // of two heading files only the last is carried
  }
  return Verdict::Yes;
}

}  // namespace HeadingCarry
