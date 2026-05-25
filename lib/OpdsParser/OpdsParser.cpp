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

// Returns an OpdsAcquisitionLink if the type and href correspond to a supported
// acquisition format, otherwise returns an empty OpdsAcquisitionLink.
OpdsAcquisitionLink supportedAcquisitionLink(const char* type, const char* href) {
  if (!type || !href || type[0] == '\0' || href[0] == '\0') {
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
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
  }
}

OpdsParser::~OpdsParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);
    XML_SetElementHandler(parser, nullptr, nullptr);
    XML_SetCharacterDataHandler(parser, nullptr);
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      XML_ParserFree(parser);
      parser = nullptr;
      return length;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      XML_ParserFree(parser);
      parser = nullptr;
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (!parser) {
    errorOccured = true;
    return;
  }
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    XML_ParserFree(parser);
    parser = nullptr;
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
  inEntry = inTitle = inAuthor = inAuthorName = inId = false;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
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
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        self->prevPageUrl = href;
      }

      if (self->inEntry) {
        if (rel && strstr(rel, "opds-spec.org/acquisition") != nullptr) {
          const auto acquisition = supportedAcquisitionLink(type, href);
          if (!acquisition.formatKey.empty() && !hasEquivalentAcquisitionLink(self->currentEntry, acquisition)) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            if (self->currentEntry.acquisitionLinks.empty()) {
              self->currentEntry.href = href;
            } else if (self->currentEntry.acquisitionLinks.size() == 1 &&
                       self->currentEntry.acquisitionLinks.capacity() < 3) {
              self->currentEntry.acquisitionLinks.reserve(3);
            }
            self->currentEntry.acquisitionLinks.push_back(acquisition);
          }
        } else if (rel && type && strstr(rel, "opds-spec.org/image") != nullptr &&
                   strstr(rel, "thumbnail") == nullptr && strncmp(type, "image/", 6) == 0 &&
                   self->currentEntry.imageHref.empty()) {
          self->currentEntry.imageHref = href;
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            self->currentEntry.href = href;
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
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
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
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
      self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->inTitle || self->inAuthorName || self->inId) {
    self->currentText.append(s, len);
  }
}
