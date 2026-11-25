#pragma once

#include <cstddef>
#include <cstdint>

// 2-bit picture rows (MSB first, 0 black .. 3 white, the layout of a .pxc cache and of Bitmap::readNextRow) drawn
// into a 1-bit plane a whole framebuffer byte at a time.
//
// In portrait a picture row runs down one physical column of the frame, so the per-pixel writer touches one bit of
// every physical row it crosses, a whole frame stride apart. drawByteColumn() reads the eight rows that share a
// framebuffer byte together, turns each 8 x 8 block of pixels into the eight bytes it lands in with a bit-matrix
// transpose, and merges each byte once. In landscape a row runs along one physical row, and drawRowSpan() turns eight
// pixels into the byte they land in. Every bit is computed as DirectPixelWriter::writePixel() decides it, so the
// result is the same frame.
//
// Idea from crosspoint-reader PR #4575 (gkaindl, "accelerated image rendering"), which does the transpose with
// ESP32-S3 SIMD; this is the scalar form, the same on every board.
namespace packed_plane {

// What a plane does with a pixel, from its 2-bit value v.
enum class Op : uint8_t {
  None,              // the plane draws nothing
  BlackUnlessWhite,  // B/W: v < 3 clears the bit (black); white leaves it
  BlackWhereWhite,   // B/W in Dark Mode: every bit written, cleared where v == 3, set elsewhere
  SetDarkGray,       // GRAYSCALE_LSB: v == 1 sets the bit
  SetAnyGray,        // GRAYSCALE_MSB: v == 1 or 2 sets the bit
  // Absolute gray planes (GfxRenderer::getAbsoluteGrayPlanes): every bit written, set where the plane's bit of v is.
  WriteLowBit,   // the LSB plane: v == 1 or 3
  WriteHighBit,  // the MSB plane: v == 2 or 3
};

namespace detail {

// 16-bit words of eight pixels (pixel k in bits 15 - 2k and 14 - 2k), one in each half of `w`: for each pixel the bit
// the op tests, gathered onto one byte per word (pixel 0 in bit 7). Bits 16..23 hold the high word's, 0..7 the low's.
// The op is a template argument, so the loops below carry no switch on it.
template <Op op>
inline uint32_t laneBits(const uint32_t w) {
  const uint32_t hiOnLo = w >> 1;  // each pixel's high bit moved onto its low bit's place
  uint32_t f;
  if constexpr (op == Op::SetDarkGray) {
    f = w & ~hiOnLo;  // v == 1: low set, high clear
  } else if constexpr (op == Op::SetAnyGray) {
    f = w ^ hiOnLo;  // v == 1 or 2
  } else if constexpr (op == Op::WriteLowBit) {
    f = w;  // the low bit itself
  } else if constexpr (op == Op::WriteHighBit) {
    f = hiOnLo;  // the high bit
  } else {
    f = w & hiOnLo;  // v == 3, white
  }
  f &= 0x55555555u;
  f = (f | (f >> 1)) & 0x33333333u;
  f = (f | (f >> 2)) & 0x0F0F0F0Fu;
  return (f | (f >> 4)) & 0x00FF00FFu;
}

// One framebuffer byte: `bits` (laneBits() for the op) written where `valid` is set, as writePixel() writes each.
template <Op op>
inline void merge(uint8_t& dest, const uint8_t bits, const uint8_t valid) {
  if constexpr (op == Op::BlackUnlessWhite) {
    dest &= static_cast<uint8_t>(bits | ~valid);
  } else if constexpr (op == Op::BlackWhereWhite) {
    dest = static_cast<uint8_t>((dest & ~valid) | (~bits & valid));
  } else if constexpr (op == Op::WriteLowBit || op == Op::WriteHighBit) {
    dest = static_cast<uint8_t>((dest & ~valid) | (bits & valid));
  } else {
    dest |= static_cast<uint8_t>(bits & valid);
  }
}

// Eight pixels of a row from column `column` (a multiple of 8), as a 16-bit word; past the row's end reads as white.
inline uint32_t tileWord(const uint8_t* row, const int rowBytes, const int column) {
  const int b = column >> 2;
  const uint32_t first = row[b];
  const uint32_t second = b + 1 < rowBytes ? row[b + 1] : 0xFFu;
  return (first << 8) | second;
}

// Eight pixels of a row from any column, negative included, as a 16-bit word; outside the row they read as white.
inline uint32_t wordAt(const uint8_t* row, const int rowBytes, const int column) {
  const int b = column >> 2;  // floor, also below 0
  uint32_t w;
  if (b >= 0 && b + 2 < rowBytes) {
    w = (static_cast<uint32_t>(row[b]) << 16) | (static_cast<uint32_t>(row[b + 1]) << 8) | row[b + 2];
  } else {
    const auto byteAt = [&](const int i) -> uint32_t { return i >= 0 && i < rowBytes ? row[i] : 0xFFu; };
    w = (byteAt(b) << 16) | (byteAt(b + 1) << 8) | byteAt(b + 2);
  }
  return (w << ((column & 3) * 2) >> 8) & 0xFFFFu;
}

inline uint8_t reverse8(uint8_t b) {
  b = static_cast<uint8_t>((b >> 4) | (b << 4));
  b = static_cast<uint8_t>(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
  return static_cast<uint8_t>(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
}

template <Op op>
void drawByteColumn(const uint8_t* const (&rows)[8], const int rowBytes, const int first, const int end,
                    uint8_t* const firstByte, const ptrdiff_t columnStep) {
  uint8_t valid = 0;
  for (int lane = 0; lane < 8; lane++) {
    if (rows[lane]) valid |= static_cast<uint8_t>(0x80u >> lane);
  }
  if (!valid) return;

  for (int column = first & ~7; column < end; column += 8) {
    uint32_t words[8];
    for (int lane = 0; lane < 8; lane++) {
      words[lane] = rows[lane] ? tileWord(rows[lane], rowBytes, column) : 0xFFFFu;
    }
    // m[lane]: bit (7 - k) for pixel k of that lane.
    uint32_t x = 0;
    uint32_t y = 0;
    for (int pair = 0; pair < 2; pair++) {
      const uint32_t a = laneBits<op>((words[pair * 2] << 16) | words[pair * 2 + 1]);
      const uint32_t b = laneBits<op>((words[4 + pair * 2] << 16) | words[4 + pair * 2 + 1]);
      const int shift = 16 - pair * 16;  // lanes 0,1 (and 4,5) in the top two bytes
      x |= (((a >> 16) << 8) | (a & 0xFFu)) << shift;
      y |= (((b >> 16) << 8) | (b & 0xFFu)) << shift;
    }
    // 8 x 8 bit transpose (Hacker's Delight, transpose8rS32): byte k of the result, bit (7 - lane) = pixel k of lane.
    uint32_t t = (x ^ (x >> 7)) & 0x00AA00AAu;
    x = x ^ t ^ (t << 7);
    t = (y ^ (y >> 7)) & 0x00AA00AAu;
    y = y ^ t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCCu;
    x = x ^ t ^ (t << 14);
    t = (y ^ (y >> 14)) & 0x0000CCCCu;
    y = y ^ t ^ (t << 14);
    t = (x & 0xF0F0F0F0u) | ((y >> 4) & 0x0F0F0F0Fu);
    y = ((x << 4) & 0xF0F0F0F0u) | (y & 0x0F0F0F0Fu);
    x = t;

    const int from = column < first ? first - column : 0;
    const int to = end - column < 8 ? end - column : 8;
    for (int k = from; k < to; k++) {
      const uint8_t bits = static_cast<uint8_t>(k < 4 ? x >> (24 - k * 8) : y >> (56 - k * 8));
      merge<op>(firstByte[(column + k - first) * columnStep], bits, valid);
    }
  }
}

template <Op op>
void drawRowSpan(const uint8_t* row, const int rowBytes, uint8_t* const line, const int origin, const int step,
                 const int lo, const int hi) {
  for (int byte = lo >> 3; byte <= hi >> 3; byte++) {
    const int p0 = byte * 8;
    const int jFirst = lo > p0 ? lo - p0 : 0;
    const int jLast = hi < p0 + 7 ? hi - p0 : 7;
    const uint8_t valid = static_cast<uint8_t>((0xFFu >> jFirst) & (0xFFu << (7 - jLast)));
    // Physical bit p is column (p - origin) * step: ascending from p0 going right, descending going left.
    const uint8_t bits = step > 0
                             ? static_cast<uint8_t>(laneBits<op>(wordAt(row, rowBytes, p0 - origin)))
                             : reverse8(static_cast<uint8_t>(laneBits<op>(wordAt(row, rowBytes, origin - p0 - 7))));
    merge<op>(line[byte], bits, valid);
  }
}

}  // namespace detail

// rows[lane]: the row whose pixels go to bit (7 - lane) of each byte, or null for a lane this call leaves alone.
// Columns [first, end) of the rows are drawn; column c goes to firstByte[(c - first) * columnStep].
inline void drawByteColumn(const uint8_t* const (&rows)[8], const int rowBytes, const int first, const int end,
                           uint8_t* const firstByte, const ptrdiff_t columnStep, const Op op) {
  if (first >= end) return;
  switch (op) {
    case Op::BlackUnlessWhite:
      detail::drawByteColumn<Op::BlackUnlessWhite>(rows, rowBytes, first, end, firstByte, columnStep);
      break;
    case Op::BlackWhereWhite:
      detail::drawByteColumn<Op::BlackWhereWhite>(rows, rowBytes, first, end, firstByte, columnStep);
      break;
    case Op::SetDarkGray:
      detail::drawByteColumn<Op::SetDarkGray>(rows, rowBytes, first, end, firstByte, columnStep);
      break;
    case Op::SetAnyGray:
      detail::drawByteColumn<Op::SetAnyGray>(rows, rowBytes, first, end, firstByte, columnStep);
      break;
    case Op::WriteLowBit:
      detail::drawByteColumn<Op::WriteLowBit>(rows, rowBytes, first, end, firstByte, columnStep);
      break;
    case Op::WriteHighBit:
      detail::drawByteColumn<Op::WriteHighBit>(rows, rowBytes, first, end, firstByte, columnStep);
      break;
    case Op::None:
      break;
  }
}

// Columns [first, end) of one row along one physical row `line`: column c lands on physical bit origin + c * step
// (step +1 or -1). Bits outside [0, frameBits) are left alone, as writePixel()'s guard leaves them.
inline void drawRowSpan(const uint8_t* row, const int rowBytes, const int first, const int end, uint8_t* const line,
                        const int origin, const int step, const int frameBits, const Op op) {
  if (first >= end) return;
  int lo = origin + first * step;
  int hi = origin + (end - 1) * step;
  if (lo > hi) {
    const int t = lo;
    lo = hi;
    hi = t;
  }
  if (lo < 0) lo = 0;
  if (hi > frameBits - 1) hi = frameBits - 1;
  if (lo > hi) return;
  switch (op) {
    case Op::BlackUnlessWhite:
      detail::drawRowSpan<Op::BlackUnlessWhite>(row, rowBytes, line, origin, step, lo, hi);
      break;
    case Op::BlackWhereWhite:
      detail::drawRowSpan<Op::BlackWhereWhite>(row, rowBytes, line, origin, step, lo, hi);
      break;
    case Op::SetDarkGray:
      detail::drawRowSpan<Op::SetDarkGray>(row, rowBytes, line, origin, step, lo, hi);
      break;
    case Op::SetAnyGray:
      detail::drawRowSpan<Op::SetAnyGray>(row, rowBytes, line, origin, step, lo, hi);
      break;
    case Op::WriteLowBit:
      detail::drawRowSpan<Op::WriteLowBit>(row, rowBytes, line, origin, step, lo, hi);
      break;
    case Op::WriteHighBit:
      detail::drawRowSpan<Op::WriteHighBit>(row, rowBytes, line, origin, step, lo, hi);
      break;
    case Op::None:
      break;
  }
}

}  // namespace packed_plane
