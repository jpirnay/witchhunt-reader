#pragma once

#include <cstdint>

#include "FontBytes.h"
#include "TtfCff.h"

// ftimage.h's point type, whose header (through TtfGrays.h) must not be included from a header: the rasterizer's
// scratch arrays hold these.
struct FT_Vector_;
typedef struct FT_Vector_ FT_Vector;

/// Type 2 charstrings (the glyph programs of a CFF table) to outlines, as FreeType 2.13's cf2 engine runs them
/// unhinted (FT/psaux/psintrp.c, pshints.c, psft.c, psobjs.c): the same operators, the same operand types and limits,
/// and the same path building, so the outline is FreeType's point for point. Without hinting, no hint map is built,
/// but stems are counted and hint masks read as FreeType reads them. Integer only, no heap, no recursion.
namespace ttf {

struct Face;
enum class Status : uint8_t;  // TtfRasterizer.h

/// The most operators and operands one glyph may run, seac's components included. A hostile charstring (subroutines
/// calling each other in a tree) could otherwise run FreeType's 20,000,000 and stall the reader; real glyphs run a few
/// thousand at most.
constexpr uint32_t kCffMaxOperators = 65536;
constexpr uint32_t kCffMaxSubrDepth = 16;  ///< CF2_MAX_SUBR: subroutine calls nested below the glyph's charstring
constexpr uint32_t kCffStackSize = 48;     ///< CF2_OPERAND_STACK_SIZE
constexpr uint32_t kCffStorageSize = 32;   ///< CF2_STORAGE_SIZE: the transient array of put and get

/// The outline runCharString() leaves, in the rasterizer's scratch arrays: points in integer font units (cf2 works in
/// 16.16 at a scale of 1/64, and ps_builder_add_point keeps `>> 10` of each), tags FT_CURVE_TAG_ON (1) or
/// FT_CURVE_TAG_CUBIC (2), and the last point of each contour. The arrays must hold one point more than the outline
/// keeps: a contour's closing point is added, and checked against maxPoints, before the builder removes it again.
struct CffOutline {
  FT_Vector* points;
  unsigned char* tags;
  unsigned short* contours;
  uint16_t maxPoints;
  uint16_t maxContours;
  uint16_t numPoints = 0;
  uint16_t numContours = 0;
};

/// The interpreter's whole state, for one glyph at a time: it lives in the rasterizer's scratch block, not on the
/// stack. runCharString() sets every member before it reads one.
struct CffDecoder {
  /// Bytes [at, end) of a charstring or subroutine.
  struct Region {
    uint32_t at;
    uint32_t end;
  };
  /// A point in character space (16.16 font units), or in cf2's device space (16.16 at 1/64).
  struct Point {
    int32_t x;
    int32_t y;
  };

  int32_t operands[kCffStackSize];  ///< cf2_stack: each an integer or a 16.16 number
  /// Bit i % 32 of word i / 32: operands[i] is 16.16. Two words, not a uint64_t: a variable 64-bit shift is a libgcc
  /// call on the C3 (__ashldi3, __lshrdi3), and every operand pushed and read took one.
  uint32_t fixedOperands[2];
  int32_t storage[kCffStorageSize];
  Region region;  ///< where the interpreter reads
  /// Bytes [windowAt, windowAt + windowBytes) of the region, in one piece (FontBytes::window()): read straight from
  /// it, the font's own bytes as u8() reads them. Dropped whenever the region changes.
  const uint8_t* window;
  uint32_t windowAt;
  uint32_t windowBytes;
  Region callers[kCffMaxSubrDepth];  ///< where each subroutine call returns to
  uint32_t operators;                ///< operators and operands run so far, seac's components included
  uint32_t stems;                    ///< horizontal and vertical stem hints declared
  Point current;                     ///< the interpreter's current point (curX, curY)
  Point currentCS;                   ///< cf2_glyphpath: the path's current point
  Point start;                       ///< ... the current subpath's first point
  Point currentDS;                   ///< ... the last point passed to the builder, in device space
  Point queued[3];                   ///< ... the element held back: a line's end, or a curve's three points
  int32_t seacBase;                  ///< seac's base character, while its accent is drawn
  uint8_t operandCount;
  uint8_t depth;     ///< subroutine calls open
  uint8_t queuedOp;  ///< 0 none, 1 a line, 2 a curve
  uint8_t phase;     ///< 0 the glyph, 1 seac's accent, 2 seac's base
  bool maskValid;    ///< cf2_hintmask: a mask is in force
  bool maskNew;      ///< ... and no hint map was built for it yet
  bool mapValid;     ///< cf2_hintmap: a map was built
  bool pathOpen;     ///< cf2_glyphpath: a subpath has drawn
  bool pathClosing;  ///< ... and is being closed
  bool pathBegun;    ///< ps_builder: the builder's contour has its first point
};

/// What the scratch block reserves for a CffDecoder, in a region aligned to 8 bytes, which is all the decoder needs.
constexpr uint32_t kCffDecoderBytes = (sizeof(CffDecoder) + 7) & ~uint32_t{7};
static_assert(alignof(CffDecoder) <= 8, "the scratch block's regions are 8-aligned");

/// Runs `glyph`'s charstring into `out`, using `decoder` for all its state. Ok; Malformed where FreeType fails the
/// glyph (an invalid operator use, a stack overflow or underflow, a bad subroutine index or nesting past 16, return
/// from the glyph's charstring, a nested seac or one whose component the charset does not name, more than 96 stems);
/// Unsupported for `random` (FreeType seeds it from memory addresses), for more points or contours than `out` holds,
/// and past kCffMaxOperators; Transient when an SD read fails. On failure `out` holds no outline.
Status runCharString(FontBytes& bytes, const Face& face, const CffGlyphSource& glyph, CffDecoder* decoder,
                     CffOutline* out);

}  // namespace ttf
