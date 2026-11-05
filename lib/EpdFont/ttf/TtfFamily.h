#pragma once

#include <cstddef>
#include <cstdint>

#include "FontBytes.h"
#include "GlyphStoreFormat.h"
#include "TtfCffBlock.h"

/// What the registry and the manager need to know about a TrueType family before loading it: which style a
/// file is, and the identity that tells an installed glyph store whether it holds this family.
namespace ttf {

/// A loaded family's native sizes have font IDs `base | px`: a window of 256 IDs, its base a multiple of 256 with
/// the top bit clear, so no negative ID is ever in it. FontSizeLadder makes the same IDs (its setNative()).
constexpr uint32_t kNativeIdMask = 0xFFFFFF00u;
inline bool isNativeId(const int32_t base, const int fontId) {
  return base != 0 && (static_cast<uint32_t>(fontId) & kNativeIdMask) == static_cast<uint32_t>(base);
}
inline uint8_t nativeIdPixels(const int fontId) { return static_cast<uint8_t>(fontId & 0xFF); }
/// A window base from `hash`: its bits 8-30. 0 (no window) when they are all clear.
inline int32_t nativeBaseFrom(const uint32_t hash) { return static_cast<int32_t>(hash & 0x7FFFFF00u); }

/// A FileSource over an open file with seekSet() and read() (HalFile on the device), `size` bytes long, with the
/// card's `stamp` of it (FileSource::stamp). The file must outlive the source.
template <typename File>
FileSource fileSource(File& file, const uint32_t size, const uint32_t stamp = 0) {
  return {size,
          [](void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
            auto* f = static_cast<File*>(ctx);
            return f->seekSet(offset) && f->read(dst, len) == static_cast<int>(len);
          },
          &file, stamp};
}

/// True for a name ending in ".ttf" or ".otf", in any case. In-progress copies ("x.ttf.tmp") do not count.
bool isOutlineFontFileName(const char* name);

/// The CPac table (the lookup filters ot::FilterTableWriter writes) is left out for a GSUB or GPOS past these: lookups
/// (MAX_PLANNED_LOOKUPS) or declared subtables, summed over its lookups. Counted and capped here, so no hostile table
/// makes the probe reserve gigabytes. The same figures as ot::FilterTableWriter::kMaxLookups and kMaxSubtables.
constexpr uint32_t kCpacMaxLookups = 4096;
constexpr uint32_t kCpacMaxSubtables = 16384;

/// GSUB's or GPOS's lookups and the subtables they declare, as the shaper's Face reads them.
struct LayoutCounts {
  uint32_t lookups = 0;
  uint32_t subtables = 0;
  /// The table is past kCpacMaxLookups or kCpacMaxSubtables: its font gets no CPac at all.
  bool capped = false;
};

/// Counts the lookups of the GSUB or GPOS table at `offset`, `length` bytes, of `file`, reading the LookupList in small
/// pieces: its offsets 64 at a time, then each lookup's 6-byte header. A table past the caps stops counting there
/// (`capped`); a major version other than 1 and a null or out-of-table offset count as the Face does (no lookups, a
/// lookup of no subtables). False: a read failed.
bool countLookups(const FileSource& file, uint32_t offset, uint32_t length, LayoutCounts* out);

/// The room a CPac takes: 16 + 28 a lookup + 24 a subtable; 0 without lookups, or when either table is capped. At
/// least what the writer writes.
constexpr uint32_t cpacBytesFor(const LayoutCounts& gsub, const LayoutCounts& gpos) {
  if (gsub.capped || gpos.capped) return 0;
  const uint64_t lookups = uint64_t{gsub.lookups} + gpos.lookups;
  if (lookups == 0) return 0;
  const uint64_t bytes = 16 + 28 * lookups + 24 * (uint64_t{gsub.subtables} + gpos.subtables);
  return bytes <= UINT32_MAX ? static_cast<uint32_t>(bytes) : 0;
}

/// A flash-mode block: the file, then its CPac at align4(fileBytes).
constexpr uint32_t flashBlockBytes(const uint32_t fileBytes, const uint32_t cpacBytes) {
  if (cpacBytes == 0) return fileBytes;
  const uint64_t bytes = ((uint64_t{fileBytes} + 3) & ~uint64_t{3}) + cpacBytes;
  return bytes <= UINT32_MAX ? static_cast<uint32_t>(bytes) : UINT32_MAX;
}

/// The CPac room of one file: its directory's GSUB and GPOS, counted. False: a read failed. The probe, SdBlock::plan
/// and the install's flash-mode sizes all count through it.
bool filterTableBytes(const FileSource& file, uint32_t* bytes);

/// What discovery learns from a TrueType or OpenType CFF file, in a few small reads.
struct FileProbe {
  /// head.macStyle: bit 0 bold, bit 1 italic, as EpdFontFamily::Style. -1 when it is not a font the engine
  /// draws: not TrueType or OpenType CFF (a collection, variable CFF2), a table it needs missing or outside the file,
  /// or a read error.
  int style = -1;
  /// OS/2 usWeightClass (400 regular, 700 bold). Without an OS/2 table, 700 for a bold style and 400 otherwise.
  uint16_t weight = 400;
  /// It has an fvar table: a variable font, drawn as its default instance only (spec 5.6).
  bool variable = false;
  /// What its SD-mode block would take (SdBlock::bytes()), GDEF and the CPac room in it. UINT32_MAX for a CFF file that
  /// can have none (SdPlan::NoBlock): its family installs in flash mode only. kSdBytesUnknown for one whose block could
  /// not be planned for now (SdPlan::Transient).
  uint32_t sdBytes = 0;
  /// The room of the CPac the install writes for this file (filterTableBytes()); 0: none.
  uint32_t cpacBytes = 0;
  /// What its flash-mode block takes: flashBlockBytes(file size, cpacBytes).
  uint32_t flashBytes = 0;
};
/// FileProbe::sdBytes of a CFF file whose block could not be planned for now (no memory, a card read failed): the
/// size is not known, and fitsStore() lets the load, which plans the block again, decide.
constexpr uint32_t kSdBytesUnknown = UINT32_MAX - 1;
FileProbe probeFile(const FileSource& file);

/// Which file serves each style of a directory's TrueType files, the files sorted by name. Each style takes, of
/// the files declaring it, a static one before a variable one, then the weight nearest 400 (regular, italic) or
/// 700 (bold, bold italic), then the first by name: a folder of static weights (Light, Regular, ExtraBold, ...,
/// most of them declaring the regular style) then serves Regular, not whichever sorts first. A file of weight 600 or
/// more declaring regular (italic) may serve bold (bold italic) too, unless it serves its own style. Without a regular
/// file, the italic, else the bold, else the bold italic one serves as the regular instead of its own style (every
/// missing style falls back to regular). `out[style]` is that file's index, or -1. False when no file can be drawn.
bool assignStyles(const FileProbe* files, size_t count, int (&out)[4]);

/// SD mode (spec 4.1): what a style's block holds. A font of its own, made of the file's tables the engine reads but
/// its outlines, each 4-byte aligned, in file order, behind a table directory; FontBytes reads the outlines past the
/// block from the file on the card. A TrueType file's: cmap, head, hhea, hmtx, loca, maxp, OS/2, GPOS, GSUB, kern, post
/// (its header) and GDEF, whichever it has, its glyf record pointing past the block. An OpenType CFF file's (spec 9.4):
/// cmap, head, hhea, hmtx, maxp, OS/2, GPOS, GSUB, kern, post (its header) and GDEF, then its CFF table rebuilt
/// (CffBlock), ending the block with the CharStrings INDEX's offsets; their data lies past it. A font with lookups also
/// has room for its CPac (filterTableBytes()) as a made table of the directory, after every copied table and before the
/// outline table; source() reads it erased (0xFF), the install writes it. The block is never held in memory:
/// source() serves any range of it, the directory made here and the tables read from the file, so an install copies it,
/// and compares it with what flash holds, as it does a whole file in flash mode.
class SdBlock {
 public:
  /// Reads `file`'s table directory, and a CFF file's CFF table (CffBlock::plan()). NoBlock when it is no TrueType or
  /// OpenType CFF file the engine draws, or a table it would copy lies outside the file; Transient when a read fails
  /// or, for a CFF file, there is no memory to plan with. `file` must outlive the block.
  SdPlan plan(const FileSource& file);
  /// The block's bytes: its directory, tables and CPac room.
  uint32_t bytes() const { return bytes_; }
  /// Where the CPac room is in the block, and its size (0: none, cpacAt() is then 0 as well).
  uint32_t cpacAt() const { return cpacAt_; }
  uint32_t cpacBytes() const { return cpacBytes_; }
  /// The outlines read from the card, glyf or the CFF table's charstring data: where they are in the file, and where
  /// the block's directory puts them, right past the block.
  glyphstore::SdOutlines outlines() const { return {outlinesAt_, bytes_, outlinesLength_}; }
  /// The block as a file, bytes() long. Valid while the block and its file are.
  FileSource source() { return {bytes_, &SdBlock::readSource, this}; }
  /// Whether `block`, `size` bytes, starts with this block's table directory: every table's tag, checksum, length and
  /// place the same, so the file is the one it was made from.
  bool sameAs(const uint8_t* block, uint32_t size) const;

 private:
  // TrueType: the twelve tables the engine reads, and glyf; CFF: eleven of them, and the CFF table; the CPac either
  // way.
  static constexpr uint8_t kMaxTables = 14;
  struct Table {
    uint32_t tag;
    uint32_t checksum;
    uint32_t fileOffset;
    uint32_t length;       // CFF: the rebuilt table's, its charstring data included
    uint32_t blockOffset;  // glyf: past the block; CFF: the last in the block, only its charstring data past it
  };
  bool addTable(const void* record);
  uint32_t directoryWord(uint32_t index) const;
  bool read(uint32_t offset, uint8_t* dst, uint32_t len) const;
  static bool readSource(void* ctx, uint32_t offset, void* dst, uint32_t len);

  // Set by plan(); empty until then, so a block asked before it (or after it failed) holds nothing.
  FileSource file_;
  uint32_t version_ = 0;
  Table tables_[kMaxTables] = {};  // in file order, the outline table among them
  CffBlock cff_;                   // a CFF file's CFF table
  uint32_t outlinesAt_ = 0;
  uint32_t outlinesLength_ = 0;
  uint32_t cpacAt_ = 0;
  uint32_t cpacBytes_ = 0;
  uint16_t seen_ = 0;  // the tags met in the directory
  uint8_t count_ = 0;
  uint32_t bytes_ = 0;
};

/// True when the glyph store can install a family with these styles: in flash mode, every style's block
/// (`fileBytes`, FileProbe::flashBytes: the file and its CPac room) copied into a font area of `usableBytes`, with the
/// largest style, the index and a minimal log mapped within `freeMapPages`; else in SD mode, every style's tables
/// (`sdBytes`, FileProbe::sdBytes) copied and mapped together (glyphstore::planLayout). When flash mode does not fit,
/// every style can have a block and one's size is kSdBytesUnknown, true: the load plans the blocks again and decides.
bool fitsStore(const uint32_t* fileBytes, const uint32_t* sdBytes, uint8_t count, uint32_t usableBytes,
               uint32_t freeMapPages);

/// Bump whenever the engine's output changes for the same font (the rasterizer, the 150 dpi sizing, the 2-bit
/// thresholds): the identity changes with it, so every installed store is rebuilt.
/// 2: kerning and GSUB ligatures (2c-3): U+FB00-FB04 now draw the glyphs GSUB names, so stores holding the cmap's
/// glyphs or a no-glyph marker for them must go.
/// 3: synthetic italic (spec 7.4): a family with neither a bold nor an italic file stored its bold italic emboldened
/// and upright under the bold-italic bits, which now draw slanted.
constexpr uint8_t kRenderVersion = 3;

/// One style's file as the card lists it.
struct FileStamp {
  const char* name = nullptr;  ///< nullptr: the family has no file for this style
  uint32_t size = 0;
  uint16_t date = 0;  ///< FAT modification date and time
  uint16_t time = 0;
};

/// The family identity (spec 4.2): FNV-1a over each style's file name, size and modification time, and
/// kRenderVersion, in four lanes with different seeds. Any change to a file gives a new identity.
void familyIdentity(const FileStamp (&styles)[4], uint8_t out[glyphstore::kIdentityBytes]);

/// A family set's identity, the glyph store's (spec 11.1): FNV-1a over the families' identities in the set's
/// order, in four lanes as familyIdentity(). Another family, another order or another count gives another identity,
/// and the store is installed again.
void setIdentity(const uint8_t (*families)[glyphstore::kIdentityBytes], uint8_t count,
                 uint8_t out[glyphstore::kIdentityBytes]);

/// The identity folded to 32 bits, to seed the family's font IDs.
inline uint32_t identityHash(const uint8_t identity[glyphstore::kIdentityBytes]) {
  return glyphstore::fnv1a(identity, glyphstore::kIdentityBytes);
}

}  // namespace ttf
