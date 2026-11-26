#pragma once

#include <algorithm>
#include <cstdint>

#include "BitMatrix8.h"

// The glyph blitters: a glyph's bitmap (1 or 2 bits per pixel, rows packed tight) drawn into a 1-bit plane several
// pixels at a time, in each of the four orientations. GfxRenderer's text paths call them; GlyphBlitTest holds them to
// a pixel-at-a-time draw.
namespace glyph_blit {

// GfxRenderer::Orientation, in its order (GfxRenderer.cpp asserts it): this header needs nothing from GfxRenderer.h.
enum class Frame : uint8_t { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

// =============================================================================
// Fast-path glyph rendering helpers (1-bit BW fonts, TextRotation::None)
// =============================================================================
//
// OVERVIEW
// --------
// The legacy path called drawPixel() once per set glyph pixel.  drawPixel()
// invokes rotateCoordinates() (a switch), does a bounds check, logs on OOB,
// then writes one bit.  For a typical 10×14 UI glyph that is ~100 calls.
//
// This fast path eliminates drawPixel() entirely by writing directly to the
// framebuffer in up to 8-pixel chunks via writeRowBits().
//
// FRAMEBUFFER LAYOUT
// ------------------
// 1 bpp, MSB-first, DISPLAY_WIDTH (800) pixels per row stored in
// DISPLAY_WIDTH_BYTES (100) bytes.  Bit 7 of byte 0 = leftmost pixel of
// row 0.  "Physical row" phyY occupies bytes [phyY*100 .. phyY*100+99].
// A set bit (1) is WHITE; a cleared bit (0) is BLACK.
//
// LANDSCAPE ORIENTATIONS  (2.5–3.1× speedup vs legacy)
// -------------------------------------------------------
// phyX and phyY are both linear functions of glyphX/glyphY in these modes,
// so each glyph row maps directly to a physical framebuffer row.
//
//   LandscapeCounterClockwise:  phyX = screenXBase+glyphX,  phyY = screenYBase+glyphY
//   LandscapeClockwise:         phyX = W-1-screenXBase-glyphX, phyY = H-1-screenYBase-glyphY
//
// Strategy: outer loop over glyphY (one physical row per iteration), inner
// loop reads 8-pixel chunks of that glyph row with bitmapExtract() and writes
// them with writeRowBits().  Bitmap access is purely sequential — fastest.
// LandscapeClockwise iterates glyph chunks right-to-left and applies
// reverseBits8() to flip horizontal direction.
//
// PORTRAIT ORIENTATIONS  (~2× speedup vs legacy)
// -----------------------------------------------
// Portrait (90° CW panel rotation):
//   phyX = screenYBase+glyphY,  phyY = H-1-screenXBase-glyphX
// PortraitInverted (90° CCW panel rotation):
//   phyX = W-1-screenYBase-glyphY, phyY = screenXBase+glyphX
//
// Here glyph COLUMNS map to physical rows.  Naively iterating column-by-column
// reads the bitmap with stride glyphWidth — cache-unfriendly and one bit at a
// time.  Instead we use an 8×8 bit-matrix transpose:
//
//   For each 8-row × 8-column glyph block:
//     1. Read 8 consecutive glyph rows (sequential bitmap access) into the
//        rows of a bit matrix in two 32-bit words (one bitmapExtract per row).
//     2. bit_matrix::transpose() (BitMatrix8.h) — an O(log 8) butterfly
//        transform — swaps rows and columns in 3 passes of XOR-masking.
//     3. The transposed matrix holds 8 column bytes: byte k contains the
//        bits for glyph column glyphX+k, one per physical row, MSB-aligned.
//     4. Write each column byte with writeRowBits() to its physical row.
//
// For PortraitInverted the glyph rows are packed in reverse order (last row
// in the matrix's row 0) before transposing.  This ensures the post-transpose
// column bytes are already correctly ordered (MSB = leftmost phyX) without any
// per-column bit-reversal step.
//
// PARAMETERS
// ----------
//   screenXBase = cursorX + glyph.left  (logical X of glyph pixel [0,0])
//   screenYBase = cursorY - glyph.top   (logical Y of glyph pixel [0,0])

// Reverse all 8 bits of a byte (bit 7 ↔ bit 0).
inline uint8_t reverseBits8(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

// Extract up to 8 bits from a 1-bit MSB-first packed bitmap starting at bit
// position 'bitPos'.  Returns them MSB-aligned (bit 7 = first extracted bit);
// the lower (8-count) bits are zeroed.
// All 'count' bits must lie within the valid bitmap byte range.
inline uint8_t bitmapExtract(const uint8_t* bitmap, const int bitPos, const int count) {
  const int byteIdx = bitPos >> 3;
  const int bitOff = bitPos & 7;
  uint8_t result;
  if (bitOff == 0) {
    result = bitmap[byteIdx];
  } else if (count <= 8 - bitOff) {
    result = bitmap[byteIdx] << bitOff;  // all bits inside first byte
  } else {
    result = (uint8_t)(((uint16_t)bitmap[byteIdx] << 8 | bitmap[byteIdx + 1]) >> (8 - bitOff));
  }
  if (count < 8) result &= static_cast<uint8_t>(0xFF << (8 - count));
  return result;
}

// ---------------------------------------------------------------------------
// Fast glyph render pipeline
// ---------------------------------------------------------------------------
// Both 1-bit (BW) and 2-bit (antialiased) paths share the same structure:
//
//   gather → [reindex] → scatter
//
// The glyph bitmap is a row-major 2D tensor [glyphHeight][glyphWidth].
// The framebuffer is a row-major 2D tensor [DISPLAY_HEIGHT][DISPLAY_WIDTH_BYTES]
// (1 bpp) with a fixed row stride of DISPLAY_WIDTH_BYTES bytes.
//
// Non-rotated (Landscape): glyph rows map 1-to-1 to framebuffer rows.
// Reindex is a no-op; the pipeline is a tight per-row gather+scatter loop.
//
// Rotated 90° (Portrait): glyph rows become framebuffer columns.
// A row↔column axis swap (reindex) is required before scattering.
//
// 1-bit pipeline
//   gather  : extractGlyphBlock        reads an 8×8 glyph tile into the rows
//                                      of a bit matrix (two 32-bit words)
//                                      (≈ glyphTensor[tile].contiguous())
//   reindex : bit_matrix::transpose    swaps row↔column axes in the matrix;
//                                      pure index transform, no data movement
//   scatter : scatterBlockToFrameBuffer → writeRowBits
//                                      writes each column-byte to its row
//
// 2-bit pipeline (why it differs)
//   The glyph stores 4 gray levels (0–3). Rendering reduces these to a 1-bit
//   draw/skip decision via a render-mode threshold. That reduction is
//   information-lossy, so gather and threshold cannot be separated — there is
//   no contiguous 2-bit block to transpose. The two steps are fused:
//
//   gather+threshold : build2BitRowMask  Landscape — samples along glyph X
//                      build2BitColMask  Portrait  — samples along glyph Y
//                      both return a 1-bit mask ready for writeRowBits
//   scatter          : writeRowBits      same atom as the 1-bit path
// ---------------------------------------------------------------------------

// Scatter atom: merges 8 MSB-aligned bits into the framebuffer row at physical bit offset phyBitPos.
// Shared by both pipelines (1-bit: via scatterBlockToFrameBuffer; 2-bit: called directly).
//   bits      — MSB-aligned; bit 7 = pixel at phyBitPos, lower (8-count) bits are zero.
//   phyBitPos — physical X of the MSB pixel; may be negative for left-edge partial chunks.
//   pixelState true → black (clear bits to 0), false → white (set bits to 1).
inline void writeRowBits(uint8_t* const row, const int phyBitPos, const uint8_t bits, const bool pixelState,
                         const int widthBytes) {
  uint8_t effectiveBits = bits;
  int byteIdx;
  int shift;
  if (phyBitPos < 0) {
    // Chunk starts off-screen left: clip by shifting out the off-screen MSBs.
    // bits is MSB-aligned, so (bits << neg) discards the neg off-screen pixels
    // and leaves the on-screen pixels MSB-aligned starting at physical X=0.
    const int neg = -phyBitPos;
    if (neg >= 8) return;  // entire chunk is off-screen left
    effectiveBits = bits << neg;
    byteIdx = 0;
    shift = 0;
  } else {
    byteIdx = phyBitPos >> 3;
    shift = phyBitPos & 7;
  }
  if (pixelState) {
    row[byteIdx] &= ~(effectiveBits >> shift);
    if (shift > 0 && byteIdx + 1 < widthBytes) row[byteIdx + 1] &= ~(uint8_t)(effectiveBits << (8 - shift));
  } else {
    row[byteIdx] |= (effectiveBits >> shift);
    if (shift > 0 && byteIdx + 1 < widthBytes) row[byteIdx + 1] |= (uint8_t)(effectiveBits << (8 - shift));
  }
}

// 1-bit pipeline step 1 — gather: reads an up-to-8×8 tile from the glyph tensor
// ([glyphHeight][glyphWidth], 1 bpp, row stride = glyphWidth bits) into the rows of a bit matrix (BitMatrix8.h).
// Equivalent to glyphTensor[glyphY:+rowCount, glyphX:+colCount].contiguous().
// Row 0 = first source row (MSB-aligned). reverseRows implements a negative-stride gather along Y
// (reads rows bottom-to-top), needed for PortraitInverted.
// Full pipeline: extractGlyphBlock (gather) → bit_matrix::transpose (reindex) → scatterBlockToFrameBuffer (scatter).
inline void extractGlyphBlock(const uint8_t* const bitmap, const int stride, const int glyphX, const int glyphY,
                              const int rowCount, const int colCount, const bool reverseRows, uint32_t& hi,
                              uint32_t& lo) {
  hi = 0;
  lo = 0;
  int bitStart = glyphY * stride + glyphX;
  for (int n = 0; n < rowCount; n++, bitStart += stride) {
    const int slot = reverseRows ? (rowCount - 1 - n) : n;
    const uint32_t row = bitmapExtract(bitmap, bitStart, colCount);
    if (slot < 4) {
      hi |= row << (24 - 8 * slot);
    } else {
      lo |= row << (56 - 8 * slot);
    }
  }
}

// 1-bit pipeline step 3 — scatter: writes column-bytes of the transposed block into framebuffer rows.
// The framebuffer is a 2D tensor [DISPLAY_HEIGHT][DISPLAY_WIDTH_BYTES] with non-unit row stride;
// phyYStride=±1 selects the traversal direction along Y (positive = top-to-bottom, negative = inverted).
// Each column k maps to row (phyYBase + k*phyYStride) via writeRowBits.
inline void scatterBlockToFrameBuffer(uint8_t* const frameBuffer, const uint32_t hi, const uint32_t lo,
                                      const int colCount, const int phyYBase, const int phyYStride, const int phyBitPos,
                                      const bool pixelState, const int displayHeight, const int widthBytes) {
  for (int k = 0; k < colCount; k++) {
    const uint8_t cols_k = bit_matrix::byteAt(hi, lo, k);
    if (cols_k == 0) continue;
    const int phyY = phyYBase + k * phyYStride;
    if (phyY < 0 || phyY >= displayHeight) continue;
    writeRowBits(frameBuffer + phyY * widthBytes, phyBitPos, cols_k, pixelState, widthBytes);
  }
}

inline void draw1Bit(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                     const int glyphHeight, const int screenXBase, const int screenYBase, const bool pixelState,
                     const Frame orientation, const int displayWidth, const int displayHeight, const int widthBytes) {
  switch (orientation) {
    case Frame::LandscapeCounterClockwise: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = screenYBase + glyphY;
        if (phyY < 0 || phyY >= displayHeight) continue;
        uint8_t* const row = frameBuffer + phyY * widthBytes;
        const int rowBitStart = glyphY * glyphWidth;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int count = std::min(8, glyphWidth - glyphX);
          const uint8_t gbyte = bitmapExtract(bitmap, rowBitStart + glyphX, count);
          if (gbyte == 0) continue;
          const int phyBitPos = screenXBase + glyphX;
          if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
          writeRowBits(row, phyBitPos, gbyte, pixelState, widthBytes);
        }
      }
      break;
    }

    case Frame::LandscapeClockwise: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = displayHeight - 1 - (screenYBase + glyphY);
        if (phyY < 0 || phyY >= displayHeight) continue;
        uint8_t* const row = frameBuffer + phyY * widthBytes;
        const int rowBitStart = glyphY * glyphWidth;
        for (int chunkEnd = glyphWidth - 1; chunkEnd >= 0; chunkEnd -= 8) {
          const int chunkStart = std::max(0, chunkEnd - 7);
          const int count = chunkEnd - chunkStart + 1;
          const uint8_t gbyte_fwd = bitmapExtract(bitmap, rowBitStart + chunkStart, count);
          const uint8_t gbyte = reverseBits8(gbyte_fwd >> (8 - count));
          if (gbyte == 0) continue;
          const int phyBitPos = displayWidth - 1 - screenXBase - chunkEnd;
          if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
          writeRowBits(row, phyBitPos, gbyte, pixelState, widthBytes);
        }
      }
      break;
    }

    case Frame::Portrait: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
        const int rowCount = std::min(8, glyphHeight - glyphY);
        const int phyBitPos = screenYBase + glyphY;
        if (phyBitPos + rowCount <= 0 || phyBitPos >= displayWidth) continue;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int colCount = std::min(8, glyphWidth - glyphX);
          uint32_t hi;
          uint32_t lo;
          extractGlyphBlock(bitmap, glyphWidth, glyphX, glyphY, rowCount, colCount, false, hi, lo);
          bit_matrix::transpose(hi, lo);
          scatterBlockToFrameBuffer(frameBuffer, hi, lo, colCount, displayHeight - 1 - screenXBase - glyphX, -1,
                                    phyBitPos, pixelState, displayHeight, widthBytes);
        }
      }
      break;
    }

    case Frame::PortraitInverted: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
        const int rowCount = std::min(8, glyphHeight - glyphY);
        const int phyBitPos = displayWidth - 1 - screenYBase - (glyphY + rowCount - 1);
        if (phyBitPos + rowCount <= 0 || phyBitPos >= displayWidth) continue;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int colCount = std::min(8, glyphWidth - glyphX);
          uint32_t hi;
          uint32_t lo;
          extractGlyphBlock(bitmap, glyphWidth, glyphX, glyphY, rowCount, colCount, true, hi, lo);
          bit_matrix::transpose(hi, lo);
          scatterBlockToFrameBuffer(frameBuffer, hi, lo, colCount, screenXBase + glyphX, 1, phyBitPos, pixelState,
                                    displayHeight, widthBytes);
        }
      }
      break;
    }
  }
}

// Read one pixel from a tightly-packed 2-bit-per-pixel glyph bitmap.
// The bitmap is a row-major tensor [glyphHeight][glyphWidth] with no row padding;
// its pixel-row stride equals glyphWidth.  pixelPosition = row * glyphWidth + col.
// Returns the raw font value: 0=white, 1=light-gray, 2=dark-gray, 3=black.
inline uint8_t get2BitPixel(const uint8_t* const bitmap, const int pixelPosition) {
  return (bitmap[pixelPosition >> 2] >> ((3 - (pixelPosition & 3)) * 2)) & 0x3;
}

// Convenience overload using explicit row/col/stride (tensor element access).
inline uint8_t get2BitPixel(const uint8_t* const bitmap, const int stride, const int row, const int col) {
  return get2BitPixel(bitmap, row * stride + col);
}

// 2-bit pipeline — fused gather+threshold (X axis): the 2-bit analog of extractGlyphBlock, but
// gather and threshold are collapsed into one pass. The threshold (2-bit raw value → 1-bit on/off)
// is information-lossy, so no contiguous 2-bit intermediate block can be formed mid-pipeline.
// The resulting 1-bit mask feeds writeRowBits directly (scatter). build2BitColMask is the Y-axis counterpart.
//
// Templated on the drawMask byte (a non-type template parameter) so each render-mode/darkness
// combination compiles to its own specialization with the mask folded into a constant.
template <uint8_t drawMask>
inline uint8_t build2BitRowMask(const uint8_t* const bitmap, const int rowStartPixel, const int glyphXStartOrEnd,
                                const int count, const bool reverseXInChunk) {
  // drawMask uses raw 2-bit glyph values directly from font bitmaps:
  // raw 0=white, 1=light gray, 2=dark gray, 3=black.
  // Bit N set means: draw/update when raw==N.
  uint8_t mask = 0;
  for (int i = 0; i < count; i++) {
    const int logicalX = reverseXInChunk ? (glyphXStartOrEnd - i) : (glyphXStartOrEnd + i);
    const uint8_t raw = get2BitPixel(bitmap, rowStartPixel + logicalX);
    if ((drawMask >> raw) & 0x01) mask |= static_cast<uint8_t>(1u << (7 - i));
  }
  return mask;
}

// Fast-path 2-bit mask builder for 8 byte-aligned pixels.
//
// The 2-bit glyph bitmap stores 4 pixels per byte, MSB-first:
//   byte b = [p0.msb p0.lsb  p1.msb p1.lsb  p2.msb p2.lsb  p3.msb p3.lsb]
//
// For each drawMask the draw decision collapses to a two-bit boolean:
//   0x0E (raw ∈ {1,2,3}): msb | lsb
//   0x06 (raw ∈ {1,2}):   msb ^ lsb
//   0x04 (raw == 2):      msb & ~lsb
//   0x02 (raw == 1):      ~msb & lsb
//
// Derivation for one byte:
//   msb_bits = b & 0xAA  →  bits 7,5,3,1 hold p0.msb … p3.msb; bits 6,4,2,0 = 0
//   lsb_bits = (b & 0x55) << 1  →  same positions hold p0.lsb … p3.lsb
//   draw_bits = msb_bits OP lsb_bits  →  bits 7,5,3,1 are the per-pixel draw flags
//
// compact4: squeezes those 4 draw flags from bit positions 7,5,3,1
//   into the top nibble (bits 7,6,5,4 → pixels 0,1,2,3).
//
// Two bytes b0 (pixels 0–3) and b1 (pixels 4–7) are combined:
//   mask = compact4(draw(b0)) | (compact4(draw(b1)) >> 4)
//
// This avoids the 8-iteration per-pixel loop in build2BitRowMask and
// processes the full 8-pixel chunk in ~16 ALU ops instead of ~56.
// The caller is responsible for only calling this when pixelStart is
// 4-pixel (1-byte) aligned (pixelStart & 3 == 0) and count == 8.
template <uint8_t drawMask>
inline uint8_t build2BitRowMaskFromTwoBytes(const uint8_t b0, const uint8_t b1) {
  const uint8_t msb0 = b0 & 0xAA;
  const uint8_t lsb0 = (b0 & 0x55) << 1;
  const uint8_t msb1 = b1 & 0xAA;
  const uint8_t lsb1 = (b1 & 0x55) << 1;

  uint8_t draw0, draw1;
  if constexpr (drawMask == 0x0E) {  // glyph BW: raw ∈ {1,2,3}
    draw0 = msb0 | lsb0;
    draw1 = msb1 | lsb1;
  } else if constexpr (drawMask == 0x07) {  // image BW: raw ∈ {0,1,2} (not white)
    draw0 = ~(msb0 & lsb0) & 0xAA;
    draw1 = ~(msb1 & lsb1) & 0xAA;
  } else if constexpr (drawMask == 0x06) {  // raw ∈ {1,2}
    draw0 = msb0 ^ lsb0;
    draw1 = msb1 ^ lsb1;
  } else if constexpr (drawMask == 0x04) {  // raw == 2 (dark gray)
    draw0 = msb0 & ~lsb0;
    draw1 = msb1 & ~lsb1;
  } else {  // drawMask == 0x02, raw == 1 (light gray)
    static_assert(drawMask == 0x02, "unsupported drawMask in build2BitRowMaskFromTwoBytes");
    draw0 = ~msb0 & lsb0;
    draw1 = ~msb1 & lsb1;
  }

  // Compact each nibble's draw flags from bit positions 7,5,3,1 → 7,6,5,4.
  auto compact4 = [](const uint8_t d) -> uint8_t {
    return (d & 0x80) | ((d & 0x20) << 1) | ((d & 0x08) << 2) | ((d & 0x02) << 3);
  };
  return compact4(draw0) | (compact4(draw1) >> 4);
}

// 2-bit pipeline — fused gather+threshold (Y axis): column-direction counterpart to build2BitRowMask.
// Samples count pixels down glyph column glyphX starting at row glyphYStart; reverseRows implements
// a negative-stride view along Y (reads bottom-to-top), needed for PortraitInverted.
template <uint8_t drawMask>
inline uint8_t build2BitColMask(const uint8_t* const bitmap, const int glyphWidth, const int glyphX,
                                const int glyphYStart, const int count, const bool reverseRows) {
  uint8_t mask = 0;
  for (int i = 0; i < count; i++) {
    const int row = reverseRows ? (glyphYStart + count - 1 - i) : (glyphYStart + i);
    const uint8_t raw = get2BitPixel(bitmap, glyphWidth, row, glyphX);
    if ((drawMask >> raw) & 0x01) mask |= static_cast<uint8_t>(1u << (7 - i));
  }
  return mask;
}

// Shared body for Portrait and PortraitInverted 2-bit rendering.
// inverted=false → Portrait (phyY counts down, phyBitPos counts up).
// inverted=true  → PortraitInverted (phyY counts up, phyBitPos counts down).
// Both template params are compile-time constants; all ternaries fold away.
// `frameBuffer` may be a strip scratch covering only rows [fbOriginY, fbOriginY+fbRows);
// the writer subtracts fbOriginY when indexing and drops rows outside the band.
// In non-strip mode the caller passes fbOriginY=0, fbRows=displayHeight, so the
// translation is a no-op and the existing absolute-row indexing is preserved.
template <uint8_t drawMask, bool inverted>
inline void renderGlyphFast2BitPortrait(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                                        const int glyphHeight, const int screenXBase, const int screenYBase,
                                        const bool writeState, const int displayWidth, const int displayHeight,
                                        const int widthBytes, const int fbOriginY, const int fbRows) {
  for (int glyphX = 0; glyphX < glyphWidth; glyphX++) {
    const int phyY = inverted ? (screenXBase + glyphX) : (displayHeight - 1 - (screenXBase + glyphX));
    // Single unsigned compare drops both off-band rows (strip mode) and any
    // out-of-frame row (full-frame mode: fbOriginY=0, fbRows=displayHeight),
    // matching what the Landscape* cases above do.
    const int rowY = phyY - fbOriginY;
    if (static_cast<unsigned>(rowY) >= static_cast<unsigned>(fbRows)) continue;
    uint8_t* const row = frameBuffer + rowY * widthBytes;
    for (int glyphY = 0; glyphY < glyphHeight; glyphY += 8) {
      const int count = std::min(8, glyphHeight - glyphY);
      const uint8_t mask = build2BitColMask<drawMask>(bitmap, glyphWidth, glyphX, glyphY, count, inverted);
      if (mask == 0) continue;
      const int phyBitPos = inverted ? (displayWidth - 1 - screenYBase - (glyphY + count - 1)) : (screenYBase + glyphY);
      if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
      writeRowBits(row, phyBitPos, mask, writeState, widthBytes);
    }
  }
}

template <uint8_t drawMask>
inline void draw2Bit(uint8_t* const frameBuffer, const uint8_t* const bitmap, const int glyphWidth,
                     const int glyphHeight, const int screenXBase, const int screenYBase, const bool pixelState,
                     const Frame orientation, const int displayWidth, const int displayHeight, const int widthBytes,
                     const int fbOriginY, const int fbRows) {
  // Non-rotated text fast path for 2-bit glyphs. Writes compact masks directly to framebuffer rows.
  // TextRotation::Rotated90CW keeps the legacy per-pixel fallback path for safety and readability.
  // BW (drawMask 0x0E) honors the caller's pixelState; grayscale passes always clear the bit.
  //
  // Tiled grayscale: `frameBuffer` may be a strip scratch with origin fbOriginY
  // and fbRows; we subtract the origin when indexing and clip rows outside the
  // band. The unsigned compare drops both off-band rows (strip mode) and any
  // out-of-frame row (full-frame mode) in one branch.
  const bool writeState = (drawMask == 0x0E) ? pixelState : false;

  switch (orientation) {
    case Frame::LandscapeCounterClockwise: {
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = screenYBase + glyphY;
        const int rowY = phyY - fbOriginY;
        if (static_cast<unsigned>(rowY) >= static_cast<unsigned>(fbRows)) continue;
        uint8_t* const row = frameBuffer + rowY * widthBytes;
        const int rowStartPixel = glyphY * glyphWidth;
        for (int glyphX = 0; glyphX < glyphWidth; glyphX += 8) {
          const int count = std::min(8, glyphWidth - glyphX);
          const int pixelStart = rowStartPixel + glyphX;
          uint8_t mask;
          if (count == 8 && (pixelStart & 3) == 0) {
            const int srcByteIdx = pixelStart >> 2;
            mask = build2BitRowMaskFromTwoBytes<drawMask>(bitmap[srcByteIdx], bitmap[srcByteIdx + 1]);
          } else {
            mask = build2BitRowMask<drawMask>(bitmap, rowStartPixel, glyphX, count, false);
          }
          if (mask == 0) continue;
          const int phyBitPos = screenXBase + glyphX;
          if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
          writeRowBits(row, phyBitPos, mask, writeState, widthBytes);
        }
      }
      break;
    }

    case Frame::LandscapeClockwise: {
      // Row-outer/chunk-inner: framebuffer rows are written at stride widthBytes
      // (phyY decreases as glyphY increases). Keeping row-outer preserves sequential access
      // within each row, which is more cache-friendly than the chunk-outer alternative.
      for (int glyphY = 0; glyphY < glyphHeight; glyphY++) {
        const int phyY = displayHeight - 1 - (screenYBase + glyphY);
        const int rowY = phyY - fbOriginY;
        if (static_cast<unsigned>(rowY) >= static_cast<unsigned>(fbRows)) continue;
        uint8_t* const row = frameBuffer + rowY * widthBytes;
        const int rowStartPixel = glyphY * glyphWidth;
        for (int chunkEnd = glyphWidth - 1; chunkEnd >= 0; chunkEnd -= 8) {
          const int chunkStart = std::max(0, chunkEnd - 7);
          const int count = chunkEnd - chunkStart + 1;
          const int pixelStart = rowStartPixel + chunkStart;
          uint8_t mask;
          if (count == 8 && (pixelStart & 3) == 0) {
            const int srcByteIdx = pixelStart >> 2;
            mask = reverseBits8(build2BitRowMaskFromTwoBytes<drawMask>(bitmap[srcByteIdx], bitmap[srcByteIdx + 1]));
          } else {
            mask = build2BitRowMask<drawMask>(bitmap, rowStartPixel, chunkEnd, count, true);
          }
          if (mask == 0) continue;
          const int phyBitPos = displayWidth - 1 - screenXBase - chunkEnd;
          if (phyBitPos + count <= 0 || phyBitPos >= displayWidth) continue;
          writeRowBits(row, phyBitPos, mask, writeState, widthBytes);
        }
      }
      break;
    }

    case Frame::Portrait:
      renderGlyphFast2BitPortrait<drawMask, false>(frameBuffer, bitmap, glyphWidth, glyphHeight, screenXBase,
                                                   screenYBase, writeState, displayWidth, displayHeight, widthBytes,
                                                   fbOriginY, fbRows);
      break;

    case Frame::PortraitInverted:
      renderGlyphFast2BitPortrait<drawMask, true>(frameBuffer, bitmap, glyphWidth, glyphHeight, screenXBase,
                                                  screenYBase, writeState, displayWidth, displayHeight, widthBytes,
                                                  fbOriginY, fbRows);
      break;
  }
}

}  // namespace glyph_blit
