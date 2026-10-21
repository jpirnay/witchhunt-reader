#pragma once

#include <cstdint>

#include "FontBytes.h"

/// The 'CFF ' table of an OpenType font with PostScript outlines (sfnt 'OTTO'), read as FreeType 2.13's CFF driver
/// reads it: the INDEXes, the Top DICT, for a CID-keyed font the font DICTs (FDArray) and the FDSelect, the Private
/// DICTs and the font matrix. Like the rest of the engine it holds offsets into the FontBytes, never pointers, and
/// allocates nothing: a CID-keyed glyph's font DICT is read again for each glyph. Integer only.
namespace ttf {

struct Face;
enum class Status : uint8_t;  // TtfRasterizer.h

/// An INDEX at `start`: `count` items whose 1-based offsets (`offSize` bytes each) lead to data at `dataStart`, up to
/// `end`, the first byte past the INDEX. An empty INDEX (count 0) is two bytes.
struct CffIndex {
  uint32_t start = 0;
  uint16_t count = 0;
  uint8_t offSize = 0;
  uint32_t dataStart = 0;
  uint32_t end = 0;
};

/// The INDEX at `at`, which must end by `limit` (cff_index_init): an offset size of 1 to 4 and a last offset past 0.
/// False otherwise.
bool readCffIndex(FontBytes& bytes, uint32_t at, uint32_t limit, CffIndex* out);

/// Where `item` of `index` lies, as cff_index_access_element finds it: a zero end offset is passed over, an end past
/// the INDEX's data is clamped to it, and an item that does not start before its end is empty (offset and length 0).
/// For INDEXes with ascending offsets, as fonts have them, the subroutines lie where cff_index_get_pointers puts them.
/// False for an item past the count.
bool cffIndexItem(FontBytes& bytes, const CffIndex& index, uint32_t item, uint32_t* offset, uint32_t* length);

/// A font matrix as cff_face_init leaves it: normalized so yy (or yx, when yy is 0) is 1.0, the 2x2 part in 16.16,
/// the offset in whole font units, and the units per em that remain once the scale is taken out of the matrix.
struct CffMatrix {
  int32_t xx = 0x10000;
  int32_t xy = 0;
  int32_t yx = 0;
  int32_t yy = 0x10000;
  int32_t dx = 0;
  int32_t dy = 0;
  uint32_t upm = 0;

  /// cff_slot_load's test before it transforms the outline; the offset is applied separately.
  bool identity() const { return xx == 0x10000 && yy == 0x10000 && xy == 0 && yx == 0; }
};

/// What a Face holds of its CFF table. For a name-keyed font fdArray.count is 0, and privateDict, privateSize and
/// localSubrs are the Top DICT's; for a CID-keyed font they are 0 and empty, and each glyph's are found through the
/// FDSelect. `charset` is a predefined charset (0 to 2) or where the font's own lies.
struct CffFace {
  uint32_t table = 0;
  uint32_t tableLength = 0;
  CffIndex charStrings;
  CffIndex globalSubrs;
  CffIndex fdArray;
  uint32_t fdSelect = 0;
  uint8_t fdSelectFormat = 0;
  uint32_t privateDict = 0;
  uint32_t privateSize = 0;
  CffIndex localSubrs;
  uint32_t charset = 0;
  CffMatrix matrix;            ///< the Top DICT's
  bool hasFontMatrix = false;  ///< the Top DICT has a FontMatrix: a font DICT's matrix is concatenated with it
  bool cid = false;
};

/// Reads the CFF table at `table` (`length` bytes, inside the file) as cff_font_load and cff_face_init do, refusing
/// what they refuse, and checks every structure a glyph reads lies inside the table. `unitsPerEm` is head's, the
/// Top DICT's units per em when it has no FontMatrix. False for a table the engine cannot read: broken, a CID-keyed
/// font with no font DICTs or more than 256, or a matrix out of 32-bit range.
bool readCffTable(FontBytes& bytes, uint32_t table, uint32_t length, uint16_t unitsPerEm, CffFace* out);

/// A glyph's charstring and what interpreting it needs: its font DICT's local subroutines and matrix (for a CID-keyed
/// glyph the font DICT's concatenated with the Top DICT's), and the scale cff_slot_load applies when the font DICT's
/// units per em differ from the Top DICT's: scaleNum / scaleDen, 1 / 1 when they are equal.
struct CffGlyphSource {
  uint32_t offset = 0;
  uint32_t length = 0;
  CffIndex localSubrs;
  CffMatrix matrix;
  uint32_t scaleNum = 1;
  uint32_t scaleDen = 1;
};

/// Malformed for a glyph past the CharStrings or one whose FDSelect names an FD past the FDArray (FreeType refuses
/// both: cff_slot_load, cff_decoder_prepare); Transient when an SD read fails.
Status cffGlyphSource(FontBytes& bytes, const Face& face, uint16_t glyph, CffGlyphSource* out);

/// The glyph seac takes for a StandardEncoding `code` (cff_lookup_glyph_by_stdcharcode): the first whose charset entry
/// is the code's SID. A CID-keyed font's charset holds CIDs and is searched all the same, as FreeType does with an
/// OpenType font. -1 for a code outside 0-255 or a SID no glyph has.
int32_t cffStandardCharGlyph(FontBytes& bytes, const Face& face, int32_t code);

}  // namespace ttf
