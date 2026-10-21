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
/// How FreeType reads a glyph's charstring. False for an item past the count.
bool cffIndexItem(FontBytes& bytes, const CffIndex& index, uint32_t item, uint32_t* offset, uint32_t* length);

/// Where subroutine `item` of a subroutine INDEX lies, from its two offsets: two reads, whatever the INDEX's size,
/// which is what lets the interpreter's operator budget bound its work. readCffTable accepts only plain subroutine
/// INDEXes (offsets from 1, never decreasing), and for those this is where cff_index_get_pointers puts each item.
/// False for an item past the count, or (never for an INDEX readCffTable accepted) offsets out of order.
bool cffSubroutine(FontBytes& bytes, const CffIndex& index, uint32_t item, uint32_t* offset, uint32_t* length);

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

/// What a Face holds of its CFF table. For a name-keyed font fdArray.count is 0 and localSubrs are the Top DICT's
/// Private DICT's; for a CID-keyed font localSubrs is empty, and each glyph's are found through the FDSelect.
/// `charset` is a predefined charset (0 to 2) or where the font's own lies.
struct CffFace {
  uint32_t table = 0;
  uint32_t tableLength = 0;
  CffIndex charStrings;
  CffIndex globalSubrs;
  CffIndex fdArray;
  uint32_t fdSelect = 0;
  uint8_t fdSelectFormat = 0;
  CffIndex localSubrs;
  uint32_t charset = 0;
  CffMatrix matrix;            ///< the Top DICT's
  bool hasFontMatrix = false;  ///< the Top DICT has a FontMatrix: a font DICT's matrix is concatenated with it
  bool cid = false;
  /// The Top DICT's CharstringType. FreeType runs every CFF charstring as Type 2 (cf2) whatever it says; 1 only makes
  /// the subroutine bias 0 (cff_compute_bias).
  int32_t charstringType = 2;
};

/// Reads the CFF table at `table` (`length` bytes, inside the file) as cff_font_load and cff_face_init do, refusing
/// what they refuse, and checks every structure a glyph reads lies inside the table. `unitsPerEm` is head's, the
/// Top DICT's units per em when it has no FontMatrix. False for a table the engine cannot read: broken, a CID-keyed
/// font with no font DICTs or more than 256, a Top, font or Private DICT over 4,096 bytes, a matrix out of 32-bit
/// range, or a subroutine INDEX (the global one, the Private DICT's, any font DICT's) that is not plain: its first
/// offset not 1, or an offset below the one before. FreeType clamps such offsets into order; only a broken or hostile
/// font has them, and refusing it lets cffSubroutine find any subroutine in two reads.
bool readCffTable(FontBytes& bytes, uint32_t table, uint32_t length, uint16_t unitsPerEm, CffFace* out);

/// A glyph's charstring and what interpreting it needs: its font DICT's local subroutines and matrix (for a CID-keyed
/// glyph the font DICT's concatenated with the Top DICT's), and the scale cff_slot_load applies when the font DICT's
/// units per em differ from the Top DICT's: scaleNum / scaleDen, 1 / 1 when they are equal. For an Ok glyph both are
/// non-zero and at most INT32_MAX.
struct CffGlyphSource {
  uint32_t offset = 0;
  uint32_t length = 0;
  CffIndex localSubrs;
  CffMatrix matrix;
  uint32_t scaleNum = 1;
  uint32_t scaleDen = 1;
};

/// What cffGlyphSource() last found for a CID-keyed face, so the next glyph of the same face skips what it would read
/// again: the FDSelect range its glyph fell in, and its font DICT's local subroutines, matrix and scale. Keyed by the
/// Face's address and stamp; only what a lookup found without a read error is kept.
struct CffSourceCache {
  const Face* face = nullptr;
  uint32_t stamp = 0;
  /// Glyphs [rangeFirst, rangeEnd) are in FD rangeFd: the FDSelect scan answers rangeFd for each of them.
  uint32_t rangeFirst = 0;
  uint32_t rangeEnd = 0;
  uint8_t rangeFd = 0;
  bool haveSource = false;
  uint8_t fd = 0;  ///< the FD the source below is of
  CffIndex localSubrs;
  CffMatrix matrix;
  uint32_t scaleNum = 1;
  uint32_t scaleDen = 1;
};

/// Malformed for a glyph past the CharStrings or one whose FDSelect names an FD past the FDArray (FreeType refuses
/// both: cff_slot_load, cff_decoder_prepare), and for one whose scale would be 0 or past 32 bits (the engine's own
/// rule); Transient when an SD read fails. With a `cache`, the same result, reading less.
Status cffGlyphSource(FontBytes& bytes, const Face& face, uint16_t glyph, CffGlyphSource* out,
                      CffSourceCache* cache = nullptr);

/// The glyph seac takes for a StandardEncoding `code` (cff_lookup_glyph_by_stdcharcode): the first whose charset entry
/// is the code's SID. A CID-keyed font's charset holds CIDs and is searched all the same, as FreeType does with an
/// OpenType font. -1 for a code outside 0-255 or a SID no glyph has. It cannot report a failed SD read (the charset's
/// entries read as 0): the caller checks bytes.ioError() after it.
int32_t cffStandardCharGlyph(FontBytes& bytes, const Face& face, int32_t code);

// --- What SD mode's rebuilt CFF table needs (TtfCffBlock.h) ---

/// The largest Top, font or Private DICT readCffTable accepts.
constexpr uint32_t kCffMaxDictBytes = 4096;

/// A Top DICT or font DICT as readCffTable reads it, with the operands that set the offsets SD mode's rebuilt table
/// moves: of each offset, the operand of the operator's last occurrence, the one whose value readCffTable keeps (for
/// Private, its second, the offset).
struct CffDictOffsets {
  enum Field : uint8_t { Charset, Encoding, CharStrings, Private, FdArray, FdSelect, kFields };
  uint32_t start = 0;  ///< the DICT, in the font
  uint32_t length = 0;
  uint32_t operand[kFields] = {};       ///< where the operand starts, in the font
  uint32_t operandBytes[kFields] = {};  ///< 0: the DICT does not set the offset
  uint32_t value[kFields] = {};         ///< from the table's start, as read (a negative number wraps)
  /// readCffTable reads the DICT's Private DICT: it has no ROS, and a Private offset and size that are not 0.
  bool readsPrivate = false;
};

/// Item `item` of a Top DICT INDEX or an FDArray. False when it does not parse.
bool readCffDictOffsets(FontBytes& bytes, const CffIndex& index, uint32_t item, CffDictOffsets* out);

/// What a rebuild needs to know of a CFF table readCffTable accepted into `cff` that CffFace does not keep: where the
/// parts it copies lie (offsets in the font), and their sizes.
struct CffParts {
  /// The Top DICT INDEX. The header and the Name INDEX end at its start; the String INDEX and then the Global Subr
  /// INDEX follow its end.
  CffIndex topDicts;
  uint32_t charsetBytes = 0;  ///< the font's own charset (at CffFace::charset); 0 for a predefined one
  /// A name-keyed font's own Encoding; none (0, 0 bytes) for a predefined one, and in a CID-keyed font, which never
  /// reads it.
  uint32_t encoding = 0;
  uint32_t encodingBytes = 0;
  uint32_t fdSelectBytes = 0;  ///< a CID-keyed font's FDSelect (at CffFace::fdSelect)
  /// Every Private DICT readCffTable reads and its local Subrs INDEX, from the first byte of any of them to past the
  /// last: privates == privatesEnd when there is none.
  uint32_t privates = 0;
  uint32_t privatesEnd = 0;
};
bool readCffParts(FontBytes& bytes, const CffFace& cff, CffParts* out);

}  // namespace ttf
