#pragma once

#include <cstdint>

#include "FontBytes.h"
#include "TtfCff.h"

/// One TrueType or OpenType (CFF outlines) font file as the TTF engine reads it: where its tables are, its metrics,
/// and codepoint to glyph lookup. A Face holds offsets, never pointers, so it stays valid when the file's mapping
/// moves; the FontBytes is passed to every call.
///
/// Sizes and metrics reproduce FreeType 2.13 as the PC converter (fontconvert_sdcard.py) drives it:
/// FT_Set_Char_Size at 150 dpi, advances from linearHoriAdvance, line metrics from the size metrics. A TTF
/// at 14 pt therefore lays out like a .cpfont converted at 14 pt (spec 5.2). Integer only.
namespace ttf {

enum class FaceStatus : uint8_t {
  Ok,
  Unsupported,  ///< variable OpenType (a CFF2 table), a collection ('ttcf'), or no Unicode cmap of format 4 or 12
  Malformed,    ///< not a font, or a required table is missing, outside the file or impossible
  Transient,    ///< an SD read failed (Stage 2c): try again later
};

struct Face {
  enum class Outlines : uint8_t { TrueType, Cff };

  uint32_t glyf = 0;
  uint32_t glyfLength = 0;
  uint32_t loca = 0;
  uint32_t locaLength = 0;
  uint32_t hmtx = 0;
  uint32_t hmtxLength = 0;
  uint32_t cmapSubtable = 0;  ///< the subtable glyphIndex() reads
  uint32_t cmapEnd = 0;       ///< end of the cmap table: the subtable's reads stay below it
  uint32_t gpos = 0;          ///< the layout tables kerning and ligatures read (TtfOpenType); 0 when absent
  uint32_t gposLength = 0;
  uint32_t gsub = 0;
  uint32_t gsubLength = 0;
  uint32_t kern = 0;  ///< the legacy kerning table
  uint32_t kernLength = 0;
  uint16_t unitsPerEm = 0;
  uint16_t numGlyphs = 0;  ///< maxp's; for CFF outlines the CharStrings count, as FreeType's num_glyphs
  uint16_t numHMetrics = 0;
  uint16_t maxPoints = 0;    ///< the larger of maxp's maxPoints and maxCompositePoints; 0 for CFF outlines
  uint16_t maxContours = 0;  ///< the larger of maxp's maxContours and maxCompositeContours; 0 for CFF outlines
  int16_t xMin = 0;          ///< head: the box around every glyph, in font units
  int16_t yMin = 0;
  int16_t xMax = 0;
  int16_t yMax = 0;
  int16_t ascender = 0;  ///< FreeType's face metrics: hhea, or OS/2's when it asks (USE_TYPO_METRICS)
  int16_t descender = 0;
  int16_t height = 0;  ///< ascender - descender + line gap
  uint8_t cmapFormat = 0;
  bool longLoca = false;
  /// A bold face: head.macStyle says bold, or OS/2's weight class is 600 or more. Never emboldened (spec 7.1).
  bool bold = false;
  /// An italic face: head.macStyle says italic, OS/2's fsSelection says italic or oblique, or post's italicAngle is not
  /// 0. Never slanted (spec 7.4).
  bool italic = false;
  Outlines outlines = Outlines::TrueType;
  CffFace cff;  ///< Outlines::Cff: the CFF table (glyf and loca stay 0)
  /// Set by parseFace(), a number no other parse gives: with the Face's address, what a cache of what was read from the
  /// face (CffSourceCache) is keyed by, so a Face parsed again where another was is never taken for it.
  uint32_t stamp = 0;
};

/// Reads the table directory and the tables every glyph needs: glyf and loca for TrueType outlines ('true' or
/// 0x00010000), the CFF table for CFF ones ('OTTO'). Read errors set the FontBytes flags as well.
FaceStatus parseFace(FontBytes& bytes, Face* face);

/// The glyph for `codepoint`, or 0 (the font has none, or the cmap is broken: see bytes.outOfRange()).
uint16_t glyphIndex(FontBytes& bytes, const Face& face, uint32_t codepoint);

/// Whether the font's Unicode map, the one parseFace() would choose, maps `codepoint`, read from the table directory
/// and the cmap alone: what a font covers, asked of its file on the card before it is installed. False for a file of
/// another kind, or without such a map; bytes.ioError() tells a failed card read.
bool mapsCodepoint(FontBytes& bytes, uint32_t codepoint);

/// The post table's header, which FreeType reads (tt_face_load_post) and which holds italicAngle; SD mode copies only
/// it.
constexpr uint32_t kPostHeaderBytes = 32;

/// 26.6 pixels per em for a point size at the converter's 150 dpi, as FreeType derives it: 14 pt -> 1867.
constexpr uint32_t ppemFromPoints(const uint32_t points) { return (points * 64 * 150 + 36) / 72; }

/// The 16.16 factor from font units to 26.6 pixels at `ppem26_6` (FreeType's x_scale, equal to y_scale).
int32_t unitsToPixels(const Face& face, uint32_t ppem26_6);

/// hmtx for `glyph`, read as FreeType reads it: glyphs past numHMetrics share the last advance, and an entry
/// past the end of the table reads 0.
void horizontalMetrics(FontBytes& bytes, const Face& face, uint16_t glyph, uint16_t* advance, int16_t* lsb);

/// The glyph's advance in 12.4 pixels, as the converter derives advanceX from FreeType's linearHoriAdvance.
/// Unclipped: a value over 0xFFFF does not fit EpdGlyph::advanceX, and the caller refuses the glyph.
uint32_t advance12_4(FontBytes& bytes, const Face& face, uint16_t glyph, uint32_t ppem26_6);

/// Line metrics in whole pixels, as fontconvert_sdcard.py writes them: advanceY from the rounded height,
/// the ascender rounded up, the descender rounded down.
struct LineMetrics {
  int32_t advanceY = 0;
  int32_t ascender = 0;
  int32_t descender = 0;
};
LineMetrics lineMetrics(const Face& face, uint32_t ppem26_6);

}  // namespace ttf
