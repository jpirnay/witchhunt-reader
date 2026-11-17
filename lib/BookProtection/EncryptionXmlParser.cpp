#include "EncryptionXmlParser.h"

#include <Logging.h>

#include <cstdlib>
#include <cstring>

namespace {
const char* localName(const char* name) {
  const char* colon = strrchr(name, ':');
  return colon != nullptr ? colon + 1 : name;
}

const char* attribute(const char** atts, const char* local) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(localName(atts[i]), local) == 0) return atts[i + 1];
  }
  return nullptr;
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// `in` percent-decoded into `out` (`cap` bytes with the NUL), without a leading '/': the archive's
// own name for the entry. False when an escape is malformed or the name does not fit.
bool decodeUri(const char* in, char* out, const size_t cap, size_t* len) {
  while (*in == '/') ++in;
  size_t n = 0;
  for (; *in != '\0'; ++in) {
    char c = *in;
    if (c == '%') {
      const int hi = hexValue(in[1]);
      const int lo = hi < 0 ? -1 : hexValue(in[2]);
      if (lo < 0) return false;
      c = static_cast<char>(hi << 4 | lo);
      in += 2;
    }
    if (n + 1 >= cap) return false;
    out[n++] = c;
  }
  out[n] = '\0';
  *len = n;
  return n > 0;
}
}  // namespace

bool EncryptionXmlParser::setup() {
  if (!sax_.init(this, startElement, endElement)) {
    LOG_ERR("PRT", "Couldn't allocate memory for parser");
    return false;
  }
  return true;
}

size_t EncryptionXmlParser::write(const uint8_t* buffer, const size_t size) {
  if (failed_ || !sax_.isActive()) return 0;
  if (!sax_.feed(buffer, size)) {
    if (!failed_) {
      LOG_ERR("PRT", "encryption.xml: %s (line %d)", sax_.errorString(), sax_.errorLine());
    }
    failed_ = true;
    return 0;
  }
  return failed_ ? 0 : size;
}

bool EncryptionXmlParser::finish() {
  if (failed_ || !sax_.isActive()) return false;
  if (!sax_.finalize()) {
    LOG_ERR("PRT", "encryption.xml: %s (line %d)", sax_.errorString(), sax_.errorLine());
    return false;
  }
  // A URI cut short would name another entry, or none, and leave the real one undecrypted.
  if ((sax_.truncationFlags() & SaxParser::kTruncAttrValue) != 0) {
    LOG_ERR("PRT", "encryption.xml: a value longer than the parser holds");
    return false;
  }
  return true;
}

void EncryptionXmlParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<EncryptionXmlParser*>(userData);
  const int depth = self->depth_++;
  if (self->failed_) return;
  const char* local = localName(name);
  if (strcmp(local, "EncryptedData") == 0) {
    self->dataDepth_ = depth;
    self->cipherBits_ = 0;
    self->deflated_ = true;
    self->sizeKnown_ = false;
    self->plainSize_ = 0;
    self->uriLen_ = 0;
    return;
  }
  if (self->dataDepth_ < 0) return;
  if (depth == self->dataDepth_ + 1 && strcmp(local, "EncryptionMethod") == 0) {
    // The algorithm URIs end "xmlenc#aes128-cbc" and "xmlenc#aes256-cbc"; matched as Free-Ink's
    // ProtectedBook matches them.
    const char* algorithm = attribute(atts, "Algorithm");
    self->cipherBits_ = 0;
    if (algorithm != nullptr && strstr(algorithm, "aes128-cbc") != nullptr) self->cipherBits_ = 128;
    if (algorithm != nullptr && strstr(algorithm, "aes256-cbc") != nullptr) self->cipherBits_ = 256;
    // Adobe's "http://ns.adobe.com/adept/xmlenc#aes128-cbc-uncompressed" encrypts an entry's bytes as they are, not
    // their DEFLATE stream (DeDRM's ineptepub.py decrypts it without inflating; it names video files).
    if (algorithm != nullptr && strstr(algorithm, "aes128-cbc-uncompressed") != nullptr) self->deflated_ = false;
  } else if (strcmp(local, "CipherReference") == 0) {
    const char* uri = attribute(atts, "URI");
    if (uri != nullptr && !decodeUri(uri, self->uri_, sizeof(self->uri_), &self->uriLen_)) {
      LOG_ERR("PRT", "encryption.xml: unusable URI %s", uri);
      self->failed_ = true;
    }
  } else if (strcmp(local, "Compression") == 0) {
    const char* method = attribute(atts, "Method");
    self->deflated_ = method == nullptr || strcmp(method, "0") != 0;
    const char* length = attribute(atts, "OriginalLength");
    if (length != nullptr && *length != '\0') {
      char* end = nullptr;
      const unsigned long value = strtoul(length, &end, 10);
      if (*end == '\0' && value <= UINT32_MAX) {
        self->plainSize_ = static_cast<uint32_t>(value);
        self->sizeKnown_ = true;
      }
    }
  }
}

void EncryptionXmlParser::endElement(void* userData, const char* /*name*/) {
  auto* self = static_cast<EncryptionXmlParser*>(userData);
  const int depth = --self->depth_;
  if (self->failed_ || depth != self->dataDepth_) return;
  self->dataDepth_ = -1;
  if (self->cipherBits_ == 0 || self->uriLen_ == 0) return;  // not content encryption
  const Item item{self->uri_, self->uriLen_, self->cipherBits_, self->deflated_, self->sizeKnown_, self->plainSize_};
  if (!self->visit_(self->ctx_, item)) {
    self->failed_ = true;
    self->sax_.stop();
  }
}
