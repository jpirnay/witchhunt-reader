#pragma once
// Ours (writing modes step 2, decision 4), not upstream's: the compact glyph records a section stores for a shaped run,
// and the pen walk over them. Little-endian on every target; fields are read and written byte by byte, never through a
// cast pointer, because records sit unaligned in a buffer.

#include <EpdFontData.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

/// One glyph of a shaped run as a section stores it (writing modes step 2, decision 4): u16 glyph, then u16 whose
/// bit 15 says an offset follows and whose bits 0-14 are the advance in 12.4 pixels (two's complement: -1024 to
/// 1023.9375 px, 8.5 em at 120 px), then i8 dx, i8 dy (whole pixels, y down) when it says so. Little-endian, read
/// unaligned. Advances and offsets are in the pixels the run is drawn at.
struct GlyphRecord {
  uint16_t glyph = 0;
  int16_t advance = 0;  // 12.4 pixels
  int8_t dx = 0;
  int8_t dy = 0;
};

namespace glyph_run {

constexpr int32_t kMinAdvance = -16384;
constexpr int32_t kMaxAdvance = 16383;

constexpr size_t recordBytes(const GlyphRecord& r) { return (r.dx != 0 || r.dy != 0) ? 6 : 4; }

/// Writes `r` at `out` (`room` bytes); false, writing nothing, when it does not fit or its advance is outside the
/// field. A run with such a glyph is laid out unshaped (PR 3).
inline bool encode(const GlyphRecord& r, uint8_t* out, size_t room, size_t* written) {
  const size_t n = recordBytes(r);
  if (room < n || r.advance < kMinAdvance || r.advance > kMaxAdvance) return false;
  uint16_t field = static_cast<uint16_t>(static_cast<uint16_t>(r.advance) & 0x7FFF);
  if (n == 6) field |= 0x8000;
  out[0] = static_cast<uint8_t>(r.glyph & 0xFF);
  out[1] = static_cast<uint8_t>(r.glyph >> 8);
  out[2] = static_cast<uint8_t>(field & 0xFF);
  out[3] = static_cast<uint8_t>(field >> 8);
  if (n == 6) {
    out[4] = static_cast<uint8_t>(r.dx);
    out[5] = static_cast<uint8_t>(r.dy);
  }
  if (written) *written = n;
  return true;
}

/// Reads the record at `in` (`room` bytes left); false when it is cut off.
inline bool decode(const uint8_t* in, size_t room, GlyphRecord* r, size_t* read) {
  if (room < 4) return false;
  const uint16_t field = static_cast<uint16_t>(in[2] | (in[3] << 8));
  const bool hasOffset = (field & 0x8000) != 0;
  if (hasOffset && room < 6) return false;
  r->glyph = static_cast<uint16_t>(in[0] | (in[1] << 8));
  // Sign-extend the 15-bit two's complement advance.
  r->advance = static_cast<int16_t>((field & 0x4000) ? (field | 0x8000) : (field & 0x7FFF));
  r->dx = hasOffset ? static_cast<int8_t>(in[4]) : 0;
  r->dy = hasOffset ? static_cast<int8_t>(in[5]) : 0;
  if (read) *read = hasOffset ? 6 : 4;
  return true;
}

}  // namespace glyph_run

/// A run's records: `count` of them in `bytes` bytes.
struct GlyphRun {
  const uint8_t* records = nullptr;
  uint16_t count = 0;
  uint16_t bytes = 0;
};

/// Calls visit(record, penX) for each whole record in order. penX is the pen in whole pixels from `x`:
/// per record x + sum of toPixel(advance) when !accumulate (drawText's differential rounding), else
/// (16x + sum of advances + 8) >> 4 (drawTextAtScale's cursor). Stops at a record cut off by `bytes`.
template <typename Visit>
void forEachGlyph(const GlyphRun& run, int x, bool accumulate, Visit&& visit) {
  size_t pos = 0;
  int pen = x;                        // !accumulate: whole pixels
  int32_t penFP = fp4::fromPixel(x);  // accumulate: 12.4
  for (uint16_t i = 0; i < run.count; i++) {
    GlyphRecord rec;
    size_t n = 0;
    if (!glyph_run::decode(run.records + pos, run.bytes - pos, &rec, &n)) return;
    visit(rec, accumulate ? fp4::toPixel(penFP) : pen);
    pos += n;
    pen += fp4::toPixel(rec.advance);
    penFP += rec.advance;
  }
}

/// How far the pen moves over `run`, snapped as forEachGlyph snaps it.
inline int glyphRunAdvance(const GlyphRun& run, bool accumulate) {
  size_t pos = 0;
  int32_t sumFP = 0;
  int sum = 0;
  for (uint16_t i = 0; i < run.count; i++) {
    GlyphRecord rec;
    size_t n = 0;
    if (!glyph_run::decode(run.records + pos, run.bytes - pos, &rec, &n)) break;
    pos += n;
    sum += fp4::toPixel(rec.advance);
    sumFP += rec.advance;
  }
  return accumulate ? fp4::toPixel(sumFP) : sum;
}
