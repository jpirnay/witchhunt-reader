#pragma once

#include <cstdint>

// An 8 x 8 bit matrix held in two 32-bit words: rows 0..3 in `hi` and rows 4..7 in `lo`, row 0 in hi's top byte, and
// within each row's byte column 0 in bit 7. The picture and glyph blitters build one from eight rows of pixels and
// transpose it into the eight framebuffer bytes the columns land in (PackedPlaneTiles.h, GlyphBlit.h).
//
// 32-bit words because the boards' cores are 32-bit: on a 64-bit word every shift costs them two instructions and a
// carry between the halves.
namespace bit_matrix {

// Swaps rows and columns in place: afterwards byte k holds column k, bit 7 from row 0 (Hacker's Delight 7-3,
// transpose8rS32).
inline void transpose(uint32_t& hi, uint32_t& lo) {
  uint32_t t = (hi ^ (hi >> 7)) & 0x00AA00AAu;
  hi = hi ^ t ^ (t << 7);
  t = (lo ^ (lo >> 7)) & 0x00AA00AAu;
  lo = lo ^ t ^ (t << 7);
  t = (hi ^ (hi >> 14)) & 0x0000CCCCu;
  hi = hi ^ t ^ (t << 14);
  t = (lo ^ (lo >> 14)) & 0x0000CCCCu;
  lo = lo ^ t ^ (t << 14);
  t = (hi & 0xF0F0F0F0u) | ((lo >> 4) & 0x0F0F0F0Fu);
  lo = ((hi << 4) & 0xF0F0F0F0u) | (lo & 0x0F0F0F0Fu);
  hi = t;
}

// Byte k (0..7) of the matrix: row k, or column k after transpose().
inline uint8_t byteAt(const uint32_t hi, const uint32_t lo, const int k) {
  return static_cast<uint8_t>(k < 4 ? hi >> (24 - k * 8) : lo >> (56 - k * 8));
}

// `f` holds a flag per pixel of two 16-bit words of 2-bit pixels (pixel k of a word in bits 15 - 2k and 14 - 2k), in
// each pixel's low bit, 14 - 2k. Gathers each word's eight flags onto one byte, pixel 0 in bit 7: bits 16..23 for the
// high word, 0..7 for the low one. The other bits of `f` are ignored.
inline uint32_t gatherPairs(uint32_t f) {
  f &= 0x55555555u;
  f = (f | (f >> 1)) & 0x33333333u;
  f = (f | (f >> 2)) & 0x0F0F0F0Fu;
  return (f | (f >> 4)) & 0x00FF00FFu;
}

// Four rows' bytes as gatherPairs() leaves them for rows (0, 1) in `a` and (2, 3) in `b`, as one word of the matrix:
// row 0 in the top byte.
inline uint32_t rowsFromPairs(const uint32_t a, const uint32_t b) {
  return ((a >> 16) << 24) | ((a & 0xFFu) << 16) | ((b >> 16) << 8) | (b & 0xFFu);
}

}  // namespace bit_matrix
