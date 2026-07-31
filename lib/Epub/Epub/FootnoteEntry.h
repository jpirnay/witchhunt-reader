#pragma once

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
