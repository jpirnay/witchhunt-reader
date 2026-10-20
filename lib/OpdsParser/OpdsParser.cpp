#include "OpdsParser.h"

#include <FsHelpers.h>
#include <Logging.h>

#include <cstring>
#include <utility>

namespace {
// Returns the length of href after trimming trailing slashes.
size_t trimmedHrefLength(const char* href) {
  size_t len = strlen(href);
  while (len > 0 && href[len - 1] == '/') {
    len--;
  }
  return len;
}

std::string_view trimmedHrefView(const char* href) { return std::string_view{href, trimmedHrefLength(href)}; }

// The publication itself: rel="http://opds-spec.org/acquisition", or its "/open-access" form. The
// other acquisition rels (buy, borrow, subscribe, sample) mostly lead to a shop or loan page.
bool isDirectAcquisitionRel(const char* rel) {
  static constexpr char kAcquisition[] = "opds-spec.org/acquisition";
  const char* at = strstr(rel, kAcquisition);
  if (at == nullptr) return false;
  const char* tail = at + sizeof(kAcquisition) - 1;
  return tail[0] == '\0' || strcmp(tail, "/open-access") == 0;
}

// What a file name says about a download whose link names no format. Longer suffixes first.
struct NamedFormat {
  const char* suffix;
  const char* mimeType;
  const char* formatKey;
  const char* fileExtension;
};
constexpr NamedFormat kNamedFormats[] = {
    {".kepub.epub", "application/epub+zip", "kepub", ".kepub.epub"},
    {".kepub", "application/epub+zip", "kepub", ".kepub.epub"},
    {".epub", "application/epub+zip", "epub", ".epub"},
    {".txt", "text/plain", "txt", ".txt"},
    {".md", "text/markdown", "md", ".md"},
    {".xtc", "application/vnd.xteink.xtc", "xtc", ".xtc"},
    {".xtch", "application/vnd.xteink.xtch", "xtch", ".xtch"},
    {".xth", "application/vnd.xteink.xtch", "xtch", ".xtch"},
};

// The format of a download whose link names none (no type, an empty one, or the generic
// application/octet-stream), read from the file name its href ends in.
OpdsAcquisitionLink acquisitionLinkFromFileName(const char* href) {
  std::string_view name{href};
  name = name.substr(0, name.find_first_of("?#"));
  while (!name.empty() && name.back() == '/') name.remove_suffix(1);
  for (const auto& format : kNamedFormats) {
    if (FsHelpers::checkFileExtension(name, format.suffix)) {
      return {href, format.mimeType, format.formatKey, format.fileExtension};
    }
  }
  return {};
}

// Returns an OpdsAcquisitionLink if the type and href correspond to a supported
// acquisition format, otherwise returns an empty OpdsAcquisitionLink. A direct
// download whose type names no format is judged by its file name instead.
OpdsAcquisitionLink supportedAcquisitionLink(const char* type, const char* href, const bool directDownload) {
  if (!href || href[0] == '\0') {
    return {};
  }
  const bool typed = type && type[0] != '\0';
  if (directDownload && (!typed || strcmp(type, "application/octet-stream") == 0)) {
    OpdsAcquisitionLink fromName = acquisitionLinkFromFileName(href);
    if (!fromName.formatKey.empty()) return fromName;
  }
  if (!typed) {
    return {};
  }

  // Some OPDS feeds append a trailing slash to format URLs like
  // `/opds/book/123/kepub/`. Trim it so suffix checks work on the final segment.
  const std::string_view trimmedHref = trimmedHrefView(href);

  if (strcmp(type, "application/epub+zip") == 0) {
    if (FsHelpers::checkFileExtension(trimmedHref, ".kepub.epub")) {
      return {href, type, "kepub", ".kepub.epub"};
    }

    // Calibre-Web-Automated uses trailing path segments like `/kepub/` instead of
    // filename extensions, so match `/kepub` after trimming trailing slashes.
    if (FsHelpers::checkFileExtension(trimmedHref, ".kepub") || FsHelpers::checkFileExtension(trimmedHref, "/kepub")) {
      // Save bare KePub downloads with an `.epub` suffix so the existing ePub
      // reader can open them.
      return {href, type, "kepub", ".kepub.epub"};
    }
    return {href, type, "epub", ".epub"};
  }

  if (strcmp(type, "text/plain") == 0) {
    return {href, type, "txt", ".txt"};
  }

  if (strcmp(type, "text/markdown") == 0 || strcmp(type, "text/x-markdown") == 0) {
    return {href, type, "md", ".md"};
  }

  if (FsHelpers::checkFileExtension(trimmedHref, ".xtc")) {
    return {href, "application/vnd.xteink.xtc", "xtc", ".xtc"};
  }

  if (FsHelpers::checkFileExtension(trimmedHref, ".xth") || FsHelpers::checkFileExtension(trimmedHref, ".xtch")) {
    return {href, "application/vnd.xteink.xtch", "xtch", ".xtch"};
  }

  return {};
}

// Determine if the given OpdsEntry's acquisition link href is already present,
// to prevent duplicate download targets.
bool hasEquivalentAcquisitionLink(const OpdsEntry& entry, const OpdsAcquisitionLink& candidate) {
  const std::string_view normalizedCandidateHref = trimmedHrefView(candidate.href.c_str());
  for (const auto& link : entry.acquisitionLinks) {
    if (trimmedHrefView(link.href.c_str()) == normalizedCandidateHref) {
      return true;
    }
  }

  return false;
}
}  // namespace

OpdsParser::OpdsParser() {
  if (!saxParser_.init(this, startElement, endElement, characterData)) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
  }
}

OpdsParser::~OpdsParser() = default;

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  if (!saxParser_.feed(xmlData, length)) {
    errorOccured = true;
  }
  return length;
}

void OpdsParser::flush() {
  if (errorOccured) return;
  if (!saxParser_.finalize()) {
    errorOccured = true;
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  searchTemplate.clear();
  osdUrl.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = inSummary = inContent = false;
  hrefIsUnsupportedDownload = false;
}

const char* OpdsParser::findAttribute(const char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void OpdsParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        std::string sHref(href);
        if (sHref.find("{searchTerms}") != std::string::npos) {
          self->searchTemplate = sHref;
        } else if (type && strcmp(type, "application/opensearchdescription+xml") == 0) {
          self->osdUrl = sHref;
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        self->nextPageUrl = href;
      } else if (rel && (strcmp(rel, "previous") == 0 || strcmp(rel, "prev") == 0) && !self->inEntry) {
        // The IANA link relations list "prev" beside "previous", and some catalogs use only that.
        // From witchling-reader commit 18e5f3b1 (syfq91).
        self->prevPageUrl = href;
      }

      if (self->inEntry) {
        if (rel && strstr(rel, "opds-spec.org/acquisition") != nullptr) {
          const bool directDownload = isDirectAcquisitionRel(rel);
          const auto acquisition = supportedAcquisitionLink(type, href, directDownload);
          if (!acquisition.formatKey.empty() && !hasEquivalentAcquisitionLink(self->currentEntry, acquisition)) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            if (self->currentEntry.acquisitionLinks.empty()) {
              self->currentEntry.href = href;
              self->hrefIsUnsupportedDownload = false;
            } else if (self->currentEntry.acquisitionLinks.size() == 1 &&
                       self->currentEntry.acquisitionLinks.capacity() < 3) {
              self->currentEntry.acquisitionLinks.reserve(3);
            }
            self->currentEntry.acquisitionLinks.push_back(acquisition);
          } else if (acquisition.formatKey.empty() && directDownload && self->currentEntry.href.empty()) {
            // A download in a format the reader cannot open (PDF, MOBI, ...). Its href holds the
            // entry's place until a link the reader can follow turns up; endElement() decides.
            self->currentEntry.href = href;
            self->hrefIsUnsupportedDownload = true;
          }
        } else if (rel && strstr(rel, "opds-spec.org/image") != nullptr && strstr(rel, "thumbnail") == nullptr &&
                   (!type || type[0] == '\0' || strncmp(type, "image/", 6) == 0) &&
                   self->currentEntry.imageHref.empty()) {
          // Some servers leave type= off their cover links; only a type that is not an image rules
          // one out. From witchling-reader commit 111f6ecc (syfq91).
          self->currentEntry.imageHref = href;
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            self->currentEntry.href = href;
            self->hrefIsUnsupportedDownload = false;
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    self->hrefIsUnsupportedDownload = false;
    return;
  }

  if (!self->inEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  } else if (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr) {
    self->inSummary = true;
    self->currentText.clear();
  } else if (strcmp(name, "content") == 0 || strstr(name, ":content") != nullptr) {
    self->inContent = true;
    self->currentText.clear();
  }
}

void OpdsParser::endElement(void* userData, const char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (self->hrefIsUnsupportedDownload) {
      // Its only downloads are in formats the reader cannot open, and it links nowhere else. It
      // stays in the list as a book without acquisition links, which the browser shows as "Format
      // not supported", instead of vanishing from the catalog. A navigation link always wins: a
      // series entry with a shop link must stay a series. After witchling-reader commit 111f6ecc
      // (syfq91), whose version also turned such navigation entries into books.
      self->currentEntry.type = OpdsEntryType::BOOK;
      self->hrefIsUnsupportedDownload = false;
    }
    if (!self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      if (self->onEntryParsed) {
        self->onEntryParsed(std::move(self->currentEntry));
        self->currentEntry = OpdsEntry{};
      }
    }
    self->inEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      // The first <author> is the book's author; catalogs list a translator or illustrator after it.
      // From CrossInk commit 234675e5c ("keep first OPDS author", Julia Nguyen).
      if (self->currentEntry.author.empty()) self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    } else if (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr) {
      // <summary> wins over <content>: first non-empty element encountered is kept.
      if (self->inSummary && self->currentEntry.summary.empty()) {
        self->currentEntry.summary = self->currentText;
      }
      self->inSummary = false;
    } else if (strcmp(name, "content") == 0 || strstr(name, ":content") != nullptr) {
      // <content> is a fallback when no <summary> was present in this entry.
      if (self->inContent && self->currentEntry.summary.empty()) {
        self->currentEntry.summary = self->currentText;
      }
      self->inContent = false;
    }
  }
}

void OpdsParser::characterData(void* userData, const char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  // Hard caps prevent a malicious or pathological feed from exhausting heap.
  // Summaries/content get a larger budget than short metadata fields.
  if (self->inSummary || self->inContent) {
    constexpr size_t kSummaryMax = 2048;
    const size_t space = kSummaryMax > self->currentText.size() ? kSummaryMax - self->currentText.size() : 0;
    if (space > 0) self->currentText.append(s, std::min<size_t>(len, space));
  } else if (self->inTitle || self->inAuthorName || self->inId) {
    constexpr size_t kFieldMax = 512;
    const size_t space = kFieldMax > self->currentText.size() ? kFieldMax - self->currentText.size() : 0;
    if (space > 0) self->currentText.append(s, std::min<size_t>(len, space));
  }
}
