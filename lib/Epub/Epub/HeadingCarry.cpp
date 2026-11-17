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
  // Splits a path into everything before "_split_", the number after it and the extension.
  struct Parts {
    std::string_view stem;
    std::string_view ext;
    unsigned long number = 0;
    bool ok = false;
  };
  const auto parse = [](const std::string_view path) {
    Parts parts;
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return parts;
    size_t digits = dot;
    while (digits > 0 && path[digits - 1] >= '0' && path[digits - 1] <= '9') --digits;
    constexpr std::string_view kSplit = "_split_";
    if (digits == dot || digits < kSplit.size() || path.substr(digits - kSplit.size(), kSplit.size()) != kSplit) {
      return parts;
    }
    if (dot - digits > 6) return parts;  // calibre writes three digits
    for (size_t i = digits; i < dot; ++i) parts.number = parts.number * 10 + static_cast<unsigned long>(path[i] - '0');
    parts.stem = path.substr(0, digits - kSplit.size());
    parts.ext = path.substr(dot);
    parts.ok = true;
    return parts;
  };
  const Parts a = parse(first);
  const Parts b = parse(second);
  return a.ok && b.ok && a.stem == b.stem && a.ext == b.ext && b.number == a.number + 1;
}

bool scanSpine(const Epub& epub, const int spine, const size_t inflatedSize, Scanner& scanner) {
  uint8_t window[256];
  // The banked copy a build of this spine left: no inflate, no heap.
  const std::string banked = Section::sectionHtmlCachePath(epub.getCachePath(), spine);
  if (Storage.exists(banked.c_str())) {
    FsFile file;
    if (Storage.openFileForRead("HCA", banked, file)) {
      if (file.size() == inflatedSize) {
        size_t total = 0;
        for (int n = file.read(window, sizeof(window)); n > 0; n = file.read(window, sizeof(window))) {
          scanner.feed(window, static_cast<size_t>(n));
          total += static_cast<size_t>(n);
        }
        file.close();
        if (total == inflatedSize) return true;
        LOG_ERR("HCA", "Spine %d: banked XHTML read short (%u of %u bytes)", spine, static_cast<unsigned>(total),
                static_cast<unsigned>(inflatedSize));
        return false;
      }
      file.close();
    }
  }

  // Else the entry itself. The ring only has to cover the file, which is at most kMaxFileBytes.
  ZipFile::FileStatSlim stat = {};
  if (!epub.getSpineItemStat(spine, &stat)) return false;
  const size_t arenaBytes = sizeof(window) + InflateReader::ringSizeFor(inflatedSize) + 2 * alignof(std::max_align_t);
  const auto arena = makeUniqueNoThrow<BuildArena>(arenaBytes);
  if (!arena || !arena->valid()) {
    LOG_INF("HCA", "Spine %d: no %u bytes to read it; asked again later", spine, static_cast<unsigned>(arenaBytes));
    return false;
  }
  ZipFile zip(epub.getPath());
  epub.primeZip(zip);
  bool ok = false;
  {
    ZipFile::EntryReader reader(zip, sizeof(window), arena.get());
    if (reader.open(stat)) {
      bool done = false;
      ok = true;
      while (!done) {
        size_t produced = 0;
        if (!reader.step(window, sizeof(window), &produced, &done)) {
          ok = false;
          break;
        }
        scanner.feed(window, produced);
      }
      ok = ok && reader.bytesProduced() == inflatedSize;
    }
  }
  epub.adoptZipDetails(zip);
  if (!ok) LOG_ERR("HCA", "Spine %d: could not inflate its XHTML", spine);
  return ok;
}

Verdict decide(const Epub& epub, const int spine) {
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
  if (!scanSpine(epub, spine, headBytes, headScan)) return Verdict::Unknown;
  if (headScan.finish() == Shape::Other) return Verdict::No;

  const size_t nextBytes = next.zipUncompressedSize;
  if (nextBytes == 0) return Verdict::No;
  if (nextBytes <= kMaxFileBytes) {
    Scanner nextScan;
    if (!scanSpine(epub, spine + 1, nextBytes, nextScan)) return Verdict::Unknown;
    if (nextScan.finish() != Shape::Other) return Verdict::No;  // of two heading files only the last is carried
  }
  return Verdict::Yes;
}

}  // namespace HeadingCarry
