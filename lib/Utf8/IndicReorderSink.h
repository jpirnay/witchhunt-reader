#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "Utf8.h"

// Ours (this repo's, not upstream's): the bounded reads and the output types behind IndicReorder.cpp's reordering, kept
// here so that file's upstream lines stay as they were.
namespace indic_reorder {

using Cursor = const unsigned char*;

// The code point at `p` (0 at `end`), `p` moved past it. Every read is bounded by `end`, so a view that is not
// NUL-terminated is safe.
inline uint32_t next(Cursor& p, const Cursor end) {
  if (p >= end) return 0;
  unsigned char padded[5] = {};
  const size_t n = static_cast<size_t>(end - p) < 4 ? static_cast<size_t>(end - p) : 4;
  memcpy(padded, p, n);
  Cursor cursor = padded;
  const uint32_t cp = utf8NextCodepoint(&cursor);
  p += cursor == padded ? 1 : cursor - padded;
  return cp;
}

inline uint32_t peek(Cursor p, const Cursor end) { return next(p, end); }

// The output of a reordering into a std::string (upstream's form).
struct StringOut {
  std::string& s;
  void append(const char* data, size_t n) { s.append(data, n); }
  void appendCodepoint(uint32_t cp) { utf8AppendCodepoint(cp, s); }
};

// The output of a reordering into a bounded buffer: an append that does not fit sets `full` and writes nothing.
struct BoundedOut {
  char* buffer;
  size_t room;  // bytes, one of them kept for the NUL
  size_t used = 0;
  bool full = false;
  void append(const char* data, size_t n) {
    if (full || n > room - 1 - used) {
      full = true;
      return;
    }
    memcpy(buffer + used, data, n);
    used += n;
  }
  void appendCodepoint(uint32_t cp) {
    char encoded[4];
    size_t n = 0;
    if (cp < 0x80) {
      encoded[n++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
      encoded[n++] = static_cast<char>(0xC0 | (cp >> 6));
      encoded[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      encoded[n++] = static_cast<char>(0xE0 | (cp >> 12));
      encoded[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      encoded[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      encoded[n++] = static_cast<char>(0xF0 | (cp >> 18));
      encoded[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      encoded[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      encoded[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    append(encoded, n);
  }
};

}  // namespace indic_reorder
