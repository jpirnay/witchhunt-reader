#include "PageMapParser.h"

#include <FsHelpers.h>
#include <Logging.h>

#include <algorithm>

#include "PageListSink.h"

bool PageMapParser::setup() {
  if (!saxParser_.init(this, startElement, nullptr)) {
    LOG_DBG("PMP", "Couldn't allocate memory for parser");
    return false;
  }
  return true;
}

PageMapParser::~PageMapParser() = default;

size_t PageMapParser::write(const uint8_t data) { return write(&data, 1); }

size_t PageMapParser::write(const uint8_t* buffer, const size_t size) {
  if (!saxParser_.isActive()) return 0;

  remainingSize -= std::min(size, remainingSize);
  if (!saxParser_.feed(buffer, size)) {
    LOG_DBG("PMP", "Parse error at line %d: %s", saxParser_.errorLine(), saxParser_.errorString());
    return 0;
  }
  if (remainingSize == 0) {
    if (!saxParser_.finalize()) {
      LOG_DBG("PMP", "Parse error (finalize): %s", saxParser_.errorString());
      return 0;
    }
  }
  return size;
}

void PageMapParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<PageMapParser*>(userData);

  // We only care about <page name="..." href="..."/> elements. The wrapping <page-map>
  // root is ignored (no need for a state machine — every page element carries its data).
  const char* localName = strrchr(name, ':');
  if (localName) {
    localName++;
  } else {
    localName = name;
  }
  if (strcmp(localName, "page") != 0) {
    return;
  }

  std::string label;
  std::string rawHref;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "name") == 0) {
      label = atts[i + 1];
    } else if (strcmp(atts[i], "href") == 0) {
      rawHref = atts[i + 1];
    }
  }

  if (!self->pageListSink || label.empty() || rawHref.empty()) {
    return;
  }

  const std::string rawTarget = self->baseContentPath + rawHref;
  const size_t pos = rawTarget.find('#');
  const std::string rawPath = pos == std::string::npos ? rawTarget : rawTarget.substr(0, pos);
  std::string href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawPath));
  std::string anchor;
  if (pos != std::string::npos) {
    anchor = FsHelpers::decodeUriEscapes(rawTarget.substr(pos + 1));
  }
  self->pageListSink->addEntry(href, anchor, label);
}
