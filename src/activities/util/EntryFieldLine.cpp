#include "EntryFieldLine.h"

#include <Utf8.h>

namespace {

// Digits run left to right inside any line (UAX #9 W2, I2): ASCII, Arabic-Indic and Extended Arabic-Indic.
bool isDigit(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9);
}

// Writes `cp` as UTF-8 at `out` and returns its length; the code points come from a decoded line, so all are valid.
size_t putUtf8(char* out, const uint32_t cp) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

}  // namespace

bool EntryFieldLine::build(const char* line, const size_t len, const int paragraphLevel, const Measure measure,
                           const void* ctx) {
  count = 0;
  edge[0] = 0;
  if (!line || !measure) return false;

  size_t n = 0;
  const auto* const start = reinterpret_cast<const unsigned char*>(line);
  const auto* p = start;
  while (static_cast<size_t>(p - start) < len) {
    if (n == MAX) return false;
    byteAt[n] = static_cast<uint16_t>(p - start);
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp) break;
    cps[n++] = cp;
  }
  byteAt[n] = static_cast<uint16_t>(len);
  const int level = paragraphLevel & 1;
  if (n == 0 || !BidiUtils::orderUiLine(cps, n, level, visual, drawn)) return false;
  count = n;
  for (size_t v = 0; v < n; ++v) {
    charAt[v] = static_cast<uint8_t>(visual[v]);
    slotOf[visual[v]] = static_cast<uint8_t>(v);
  }

  // Which way each character runs. A letter says so itself and a digit always runs left to right; a mark goes with the
  // letter it sits on. A space or a punctuation mark takes its direction from the text around it (UAX #9 N1, N2): read
  // it off the order, from a neighbour drawn right next to it, else it is the paragraph's.
  for (size_t i = 0; i < n; ++i) {
    mark[i] = BidiUtils::isTransparentMark(cps[i]);
    if (mark[i]) {
      rtl[i] = i > 0 ? rtl[i - 1] : level != 0;
      continue;
    }
    const int strong = BidiUtils::strongLevel(cps[i]);
    if (strong >= 0 || isDigit(cps[i])) {
      rtl[i] = strong == 1;
      continue;
    }
    const auto adjacent = [&](const size_t a, const size_t b) {
      const int d = static_cast<int>(slotOf[a]) - static_cast<int>(slotOf[b]);
      return d == 1 || d == -1;
    };
    if (i + 1 < n && !BidiUtils::isTransparentMark(cps[i + 1]) && adjacent(i + 1, i)) {
      rtl[i] = slotOf[i + 1] < slotOf[i];
    } else if (i > 0 && !mark[i - 1] && adjacent(i, i - 1)) {
      rtl[i] = slotOf[i] < slotOf[i - 1];
    } else {
      rtl[i] = level != 0;
    }
  }

  // Where each drawn character starts: the advance of the line up to it, in drawn order. Zero-width joiners are left
  // out, as visualUiLine leaves them out. The whole line is measured first: a font that reads a string's glyphs from
  // the card when it is measured (the Arabic UI pack) then has them all, and each shorter measure reads none.
  size_t bytes = 0;
  for (size_t v = 0; v < n; ++v) {
    if (drawn[v] != 0x200C && drawn[v] != 0x200D) bytes += putUtf8(&form[bytes], drawn[v]);
  }
  form[bytes] = '\0';
  measure(ctx, form);
  bytes = 0;
  for (size_t v = 0; v < n; ++v) {
    if (drawn[v] == 0x200C || drawn[v] == 0x200D) {
      edge[v + 1] = edge[v];
      continue;
    }
    char tmp[4];
    bytes += putUtf8(tmp, drawn[v]);
    const char kept = form[bytes];
    form[bytes] = '\0';
    edge[v + 1] = static_cast<int16_t>(measure(ctx, form));
    form[bytes] = kept;
  }
  return true;
}

size_t EntryFieldLine::indexOf(const size_t pos) const {
  for (size_t i = 0; i < count; ++i) {
    if (byteAt[i] >= pos) return i;
  }
  return count;
}

size_t EntryFieldLine::baseOf(size_t i) const {
  while (i > 0 && mark[i]) --i;
  return i;
}

int EntryFieldLine::caretX(const size_t pos, bool* rtlOut) const {
  if (count == 0) {
    if (rtlOut) *rtlOut = false;
    return 0;
  }
  const size_t c = indexOf(pos);
  if (c > 0) {
    const size_t before = baseOf(c - 1);
    if (rtlOut) *rtlOut = rtl[before];
    return rtl[before] ? edge[slotOf[before]] : edge[slotOf[before] + 1];
  }
  if (rtlOut) *rtlOut = rtl[0];
  return rtl[0] ? edge[slotOf[0] + 1] : edge[slotOf[0]];
}

void EntryFieldLine::charSpan(const size_t pos, int& x, int& advance) const {
  const size_t c = indexOf(pos);
  if (c >= count) {
    x = caretX(pos);
    advance = 0;
    return;
  }
  x = edge[slotOf[c]];
  advance = edge[slotOf[c] + 1] - x;
}

size_t EntryFieldLine::positionAt(const int x) const {
  if (count == 0) return 0;
  // The drawn character under x, or the nearest one at either end. A mark is drawn over its letter and has no width.
  size_t slot = count;
  for (size_t v = 0; v < count; ++v) {
    if (edge[v + 1] == edge[v]) continue;
    slot = v;
    if (x < edge[v + 1]) break;
  }
  if (slot == count) return byteAt[count];  // nothing on the line has a width
  const size_t i = charAt[slot];
  size_t after = i + 1;
  while (after < count && mark[after]) ++after;
  const bool leftHalf = x < (edge[slot] + edge[slot + 1]) / 2;
  // Left of a left-to-right character is before it; left of a right-to-left one is after it.
  return byteAt[leftHalf != rtl[i] ? i : after];
}
