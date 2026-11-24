#include "AsciiParse.h"

#include <cstring>

namespace ascii_parse {

int hexNibble(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hexBytes(const char* hex, uint8_t* out, const size_t count) {
  for (size_t i = 0; i < 2 * count; i++) {
    if (hexNibble(hex[i]) < 0) return false;
  }
  for (size_t i = 0; i < count; i++) {
    out[i] = static_cast<uint8_t>(hexNibble(hex[2 * i]) << 4 | hexNibble(hex[2 * i + 1]));
  }
  return true;
}

bool ipv4(const char* text, const size_t length, uint8_t out[4]) {
  uint8_t quad[4];
  size_t pos = 0;
  for (int part = 0; part < 4; part++) {
    if (part > 0) {
      if (pos >= length || text[pos] != '.') return false;
      pos++;
    }
    const size_t start = pos;
    unsigned value = 0;
    while (pos < length && text[pos] >= '0' && text[pos] <= '9') {
      value = value * 10 + static_cast<unsigned>(text[pos] - '0');
      if (value > 255) return false;
      pos++;
    }
    if (pos == start) return false;
    quad[part] = static_cast<uint8_t>(value);
  }
  if (pos != length) return false;
  memcpy(out, quad, sizeof(quad));
  return true;
}

}  // namespace ascii_parse
