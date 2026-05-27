#include "ContainerParser.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

bool ContainerParser::setup() {
  if (!saxParser_.init(this, startElement, endElement)) {
    LOG_ERR("CTR", "Couldn't allocate memory for parser");
    return false;
  }
  return true;
}

ContainerParser::~ContainerParser() = default;

size_t ContainerParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContainerParser::write(const uint8_t* buffer, const size_t size) {
  if (!saxParser_.isActive()) return 0;

  remainingSize -= std::min(size, remainingSize);
  if (!saxParser_.feed(buffer, size)) {
    LOG_ERR("CTR", "Parse error: %s", saxParser_.errorString());
    return 0;
  }
  if (remainingSize == 0) {
    if (!saxParser_.finalize()) {
      LOG_ERR("CTR", "Parse error (finalize): %s", saxParser_.errorString());
      return 0;
    }
  }
  return size;
}

void ContainerParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<ContainerParser*>(userData);

  if (self->state == START && strcmp(name, "container") == 0) {
    self->state = IN_CONTAINER;
    return;
  }

  if (self->state == IN_CONTAINER && strcmp(name, "rootfiles") == 0) {
    self->state = IN_ROOTFILES;
    return;
  }

  if (self->state == IN_ROOTFILES && strcmp(name, "rootfile") == 0) {
    const char* mediaType = nullptr;
    const char* path = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "full-path") == 0) {
        path = atts[i + 1];
      }
    }

    if (mediaType && path && strcmp(mediaType, "application/oebps-package+xml") == 0) {
      self->fullPath = path;
    }
  }
}

void ContainerParser::endElement(void* userData, const char* name) {
  auto* self = static_cast<ContainerParser*>(userData);

  if (self->state == IN_ROOTFILES && strcmp(name, "rootfiles") == 0) {
    self->state = IN_CONTAINER;
  } else if (self->state == IN_CONTAINER && strcmp(name, "container") == 0) {
    self->state = START;
  }
}
