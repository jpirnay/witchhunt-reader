// From
// https://github.com/vroland/epdiy/blob/c61e9e923ce2418150d54f88cea5d196cdc40c54/src/epd_internals.h

#pragma once
#include <cstdint>

/// Font metrics use "fixed-point 4" (4 fractional bits, i.e. 1/16-pixel
/// resolution).  Both the 12.4 glyph advances (uint16_t) and the 4.4 kern
/// values (int8_t) share the same 4 fractional bits, so they can be freely
/// added before snapping to whole pixels.
///
/// Rendering and measurement use "differential rounding": each glyph step
/// (previous advance + current kern) is combined in fixed-point and snapped
/// to a pixel as one unit.  This guarantees identical character pairs always
/// produce the same pixel spacing, regardless of position on the line.
///
/// The helpers below eliminate the raw bit-shifts that would otherwise be
/// scattered across every layout / measurement call site.
namespace fp4 {
constexpr int FRAC_BITS = 4;
constexpr int32_t HALF = 1 << (FRAC_BITS - 1);  // 8, added before shift for round-to-nearest

/// Convert an integer pixel value to 12.4 fixed-point.
constexpr int32_t fromPixel(int px) { return static_cast<int32_t>(px) << FRAC_BITS; }

/// Snap a fixed-point value to the nearest integer pixel.
constexpr int toPixel(int32_t fp) { return static_cast<int>((fp + HALF) >> FRAC_BITS); }

/// Convert a fixed-point value to float (mainly useful for debug logging).
constexpr float toFloat(int32_t fp) { return fp / static_cast<float>(1 << FRAC_BITS); }
}  // namespace fp4

/// Helpers for positioning Unicode combining marks (U+0300 ff.) over a
/// preceding base glyph without GPOS anchor tables.
namespace combiningMark {

constexpr int MIN_GAP_PX = 1;

/// Compute the cursor-X at which to render a combining mark so its bitmap
/// is visually centered over the base glyph's bitmap.
constexpr int centerOver(int baseCursorPos, int baseLeft, int baseWidth, int markLeft, int markWidth) {
  return baseCursorPos + baseLeft + baseWidth / 2 - markWidth / 2 - markLeft;
}

/// Rotated-90CW variant of centerOver.  In the rotated coordinate system
/// renderCharImpl uses (cursorY - left) instead of (cursorX + left), so
/// every left/width term inverts sign.
constexpr int centerOverRotated90CW(int baseCursorPos, int baseLeft, int baseWidth, int markLeft, int markWidth) {
  return baseCursorPos - baseLeft - baseWidth / 2 + markWidth / 2 + markLeft;
}

/// For combining marks that sit entirely above the baseline, compute how many
/// pixels to raise the mark so there is at least MIN_GAP_PX between its bottom
/// edge and the top of the base glyph.  Returns 0 for marks that extend to or
/// below the baseline (e.g. cedilla, dot-below, ogonek).
constexpr int raiseAboveBase(int markTop, int markHeight, int baseTop) {
  if (markTop - markHeight <= 0) return 0;
  const int gap = markTop - markHeight - baseTop;
  return (gap < MIN_GAP_PX) ? (MIN_GAP_PX - gap) : 0;
}

}  // namespace combiningMark

/// Fixed-point conventions used by EpdGlyph and EpdFontData:
///   advanceX:   12.4 unsigned fixed-point in uint16_t  (use fp4::toPixel)
///   kernMatrix:  4.4 signed fixed-point in int8_t      (use fp4::toPixel)
/// Both share 4 fractional bits so they combine directly in an accumulator.

/// Font data stored PER GLYPH
typedef struct {
  uint8_t width;        ///< Bitmap dimensions in pixels
  uint8_t height;       ///< Bitmap dimensions in pixels
  uint16_t advanceX;    ///< Distance to advance cursor (x axis), 12.4 fixed-point in pixels
  int16_t left;         ///< X dist from cursor pos to UL corner
  int16_t top;          ///< Y dist from cursor pos to UL corner
  uint16_t dataLength;  ///< Size of the font data.
  uint32_t dataOffset;  ///< Pointer into EpdFont->bitmap (or within-group offset for compressed fonts)
} EpdGlyph;

/// Compressed font group: a DEFLATE-compressed block of glyph bitmaps
typedef struct {
  uint32_t compressedOffset;  ///< Byte offset into compressed data array
  uint32_t compressedSize;    ///< Compressed DEFLATE stream size
  uint32_t uncompressedSize;  ///< Decompressed size
  uint16_t glyphCount;        ///< Number of glyphs in this group
  /// Bytes of decoded history this group's DEFLATE stream can reach back into — i.e. the
  /// largest back-reference distance the encoder actually emitted, which fontconvert.py
  /// measures by parsing the finished stream.
  ///
  /// This is what the decoder has to hold in RAM, and it is NOT uncompressedSize: the group
  /// is streamed through a ring of exactly this size and each glyph is compacted straight out
  /// of the stream (FontDecompressor::GroupStream), so the group never exists in memory at
  /// once. Decoupling the two is the whole point — the group may be as large as compression
  /// likes while the transient allocation stays small and, unlike a malloc sized by the data,
  /// predictable.
  ///
  /// 0 means "not measured" and the decoder falls back to uncompressedSize, which is always
  /// safe (a reference can never outrun the bytes produced so far). That keeps fonts generated
  /// before this field was added decodable.
  ///
  /// Sits here rather than at the end on purpose: it fills the padding hole after glyphCount,
  /// so the struct is still 20 bytes and the table costs no extra flash (static_assert in
  /// FontDecompressor.cpp).
  uint16_t ringBytes;
  uint32_t firstGlyphIndex;  ///< First glyph index in the global glyph array
} EpdFontGroup;

/// Glyph interval structure
typedef struct {
  uint32_t first;   ///< The first unicode code point of the interval
  uint32_t last;    ///< The last unicode code point of the interval
  uint32_t offset;  ///< Index of the first code point into the glyph array
} EpdUnicodeInterval;

/// Maps a codepoint to a kerning class ID, sorted by codepoint for binary search.
/// Class IDs are 1-based; codepoints not in the table have implicit class 0 (no kerning).
typedef struct {
  uint16_t codepoint;  ///< Unicode codepoint
  uint8_t classId;     ///< 1-based kerning class ID
} __attribute__((packed)) EpdKernClassEntry;

/// Ligature substitution for a specific glyph pair, sorted by `pair` for binary search.
/// `pair` encodes (leftCodepoint << 16 | rightCodepoint) for single-key lookup.
typedef struct {
  uint32_t pair;        ///< Packed codepoint pair (left << 16 | right)
  uint32_t ligatureCp;  ///< Codepoint of the replacement ligature glyph
} __attribute__((packed)) EpdLigaturePair;

/// Data stored for FONT AS A WHOLE
typedef struct {
  const uint8_t* bitmap;                ///< Glyph bitmaps, concatenated
  const EpdGlyph* glyph;                ///< Glyph array
  const EpdUnicodeInterval* intervals;  ///< Valid unicode intervals for this font
  uint32_t intervalCount;               ///< Number of unicode intervals.
  uint8_t advanceY;                     ///< Newline distance (y axis)
  int ascender;                         ///< Maximal height of a glyph above the base line
  int descender;                        ///< Maximal height of a glyph below the base line
  bool is2Bit;
  const EpdFontGroup* groups;                 ///< NULL for uncompressed fonts
  uint16_t groupCount;                        ///< 0 for uncompressed fonts
  const uint16_t* glyphToGroup;               ///< Per-glyph group ID (nullptr for contiguous-group fonts)
  const EpdKernClassEntry* kernLeftClasses;   ///< Sorted left-side class map (nullptr if none)
  const EpdKernClassEntry* kernRightClasses;  ///< Sorted right-side class map (nullptr if none)
  /// Split form of the two class maps, used by the built-in fonts instead of the packed
  /// EpdKernClassEntry arrays above (which are left null for them). Same total size — 2 + 1 bytes
  /// per entry either way — but measurably faster: the class lookups are ~96% of getKerning(),
  /// and splitting them measured -13 to -14% on that path. Two reasons. The binary search only
  /// ever reads codepoints, so keeping the classId payload out of the searched array shrinks its
  /// footprint by a third; and a uint16 array is naturally aligned where a 3-byte packed struct
  /// leaves two of every three codepoint reads unaligned.
  ///
  /// SD-card fonts keep the packed form: the .cpfont format stores it verbatim and maps it in
  /// place (see the static_asserts in SdCardFont.cpp). Same coexistence rule as the kerning
  /// matrix below — whichever pointer is non-null selects the representation.
  const uint16_t* kernLeftCodepoints;   ///< nullptr when this font uses the packed form
  const uint8_t* kernLeftClassIds;      ///< parallel to kernLeftCodepoints
  const uint16_t* kernRightCodepoints;  ///< nullptr when this font uses the packed form
  const uint8_t* kernRightClassIds;     ///< parallel to kernRightCodepoints
  const int8_t* kernMatrix;             ///< Flat leftClassCount x rightClassCount matrix, 4.4 fixed-point in pixels
  /// Sparse (CSR) kerning — the built-in fonts use this instead of `kernMatrix`, which is left
  /// null for them. Measured across the built-in set, 504682 of 583049 dense entries are zero
  /// (86.6%), so storing only the non-zero ones costs ~165 KB where the dense matrices cost
  /// ~583 KB. Values are identical, so layout and pagination are unaffected.
  ///
  /// SD-card fonts keep using `kernMatrix`: the .cpfont format stores the dense matrix verbatim
  /// and is memory-mapped in place (see the static_asserts in SdCardFont.cpp), so switching them
  /// would break every font file already on a user's card. getKerning() reads whichever is
  /// present, so the two representations coexist with one branch.
  ///
  /// kernRowOffsets has kernLeftClassCount + 1 entries; row `l` occupies
  /// [kernRowOffsets[l], kernRowOffsets[l+1]) in the other two arrays, sorted ascending by
  /// column so the lookup can binary-search it.
  const uint16_t* kernRowOffsets;        ///< nullptr when this font uses the dense matrix
  const uint8_t* kernSparseCols;         ///< 0-based right class of each stored entry
  const int8_t* kernSparseValues;        ///< 4.4 fixed-point value of each stored entry
  uint16_t kernLeftEntryCount;           ///< Entries in kernLeftClasses
  uint16_t kernRightEntryCount;          ///< Entries in kernRightClasses
  uint8_t kernLeftClassCount;            ///< Number of distinct left classes (matrix rows)
  uint8_t kernRightClassCount;           ///< Number of distinct right classes (matrix cols)
  const EpdLigaturePair* ligaturePairs;  ///< Sorted ligature pair table (nullptr if none)
  uint32_t ligaturePairCount;            ///< Number of entries in ligaturePairs

  /// On-demand glyph loading for fonts that don't keep all glyphs in RAM (e.g. SD card fonts).
  /// Called by getGlyph() when a codepoint is not found in the interval table.
  /// Returns a valid EpdGlyph* with correct metadata, or nullptr to fall back to the
  /// replacement glyph.  The returned pointer is valid until the next glyphMissHandler
  /// call that causes a ring-buffer eviction — callers must consume it (measure or draw)
  /// before requesting another missed glyph.
  const EpdGlyph* (*glyphMissHandler)(void* ctx, uint32_t codepoint);

  /// Context pointer for glyphMissHandler (typically SdCardFont*).  Also used by
  /// GfxRenderer::getGlyphBitmap() to retrieve overflow bitmaps via SdCardFont.
  void* glyphMissCtx;
} EpdFontData;
