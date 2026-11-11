#pragma once

#include <cstdint>
#include <cstring>

#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct FootnoteEntry {
  char number[FOOTNOTE_NUMBER_LEN];
  char href[FOOTNOTE_HREF_LEN];

  // Zero the WHOLE fixed-length arrays, not just the leading NUL: writeBlock serializes the full
  // FOOTNOTE_NUMBER_LEN/HREF_LEN bytes, so uninitialized tail bytes after the terminator made
  // content.bin non-deterministic (the same book compiled twice differed in that padding — harmless
  // to readers, which stop at the NUL, but it broke byte-identity / cache fingerprinting).
  FootnoteEntry() {
    std::memset(number, 0, sizeof(number));
    std::memset(href, 0, sizeof(href));
  }
};

// The link an inline image is inside, as its page image keeps it (PageImage::linkHash): FNV-1a over the href, never 0,
// which is none. The reader finds the image's entry among the page's links by it.
inline uint32_t linkHrefHash(const char* href) {
  uint32_t hash = 2166136261u;
  for (const char* p = href; *p != '\0'; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619u;
  }
  return hash != 0 ? hash : 1;
}
