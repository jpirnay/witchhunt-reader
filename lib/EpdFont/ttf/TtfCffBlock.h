#pragma once

#include <cstdint>

#include "FontBytes.h"
#include "TtfCff.h"

namespace ttf {

/// SD mode's CFF table (spec 9.4): an OpenType CFF font's CFF table rebuilt for its style block (SdBlock), so that the
/// charstring data, the one part of it left on the card, comes after everything else the engine reads. In order: the
/// header and the Name INDEX, the Top DICT INDEX, the String and Global Subr INDEXes, the charset, the Encoding, the
/// FDSelect, the FDArray, the Private DICTs with their local Subrs INDEXes, and the CharStrings INDEX's count, offset
/// size and offsets, its data following in the file. The Top DICT INDEX keeps the one Top DICT the engine reads.
///
/// An offset of a Top or font DICT whose part moved is written again as a 5-byte integer (operator 29), so each DICT's
/// size is known before where anything lands. The Private DICTs and their Subrs INDEXes are copied as one range, as
/// they lie in the file: a Subrs offset, from its Private DICT's start, holds. The table is rebuilt so whatever order
/// the font's has: one way, for every font.
///
/// Like SdBlock it is never held in memory: read() makes any range of it from the file, reading a DICT INDEX it
/// rewrites through a page cache made for that read.
class CffBlock {
 public:
  /// Plans the rebuilt table of `file`'s CFF table, reading the file through a page cache made for the plan. False
  /// when the engine cannot draw the font (parseFace()), a DICT written again would pass kCffMaxDictBytes, the table
  /// would pass 2^31 bytes, or there is no memory for the page cache.
  bool plan(const FileSource& file);
  /// The rebuilt table's bytes, its charstring data aside: the CharStrings INDEX's offsets end it.
  uint32_t bytes() const { return bytes_; }
  /// The charstring data, which follows the rebuilt table: where it lies in the file, and its length.
  uint32_t dataOffset() const { return dataOffset_; }
  uint32_t dataLength() const { return dataLength_; }
  /// Copies `len` bytes at `offset` of the rebuilt table to `dst`, from `file`, the one planned. False past bytes(),
  /// when a read fails, or when there is no memory to make a DICT INDEX with.
  bool read(const FileSource& file, uint32_t offset, uint8_t* dst, uint32_t len) const;

 private:
  enum class Part : uint8_t { Copy, TopDicts, FdArray };
  // A run of the rebuilt table: bytes of the file, from `from` on, or a DICT INDEX made again.
  struct Piece {
    uint32_t at = 0;  // in the rebuilt table
    uint32_t length = 0;
    uint32_t from = 0;
    Part part = Part::Copy;
    uint8_t offSize = 0;  // a DICT INDEX's
  };
  // An operand written again: where it is in the file, its bytes there, and the offset it now holds.
  struct Splice {
    uint32_t at;
    uint32_t bytes;
    uint32_t value;
  };
  struct Window;
  // Header and Name INDEX, Top DICT INDEX, String and Global Subr INDEXes, charset, Encoding, FDSelect, FDArray,
  // Private DICTs, CharStrings INDEX.
  static constexpr uint8_t kMaxPieces = 9;

  bool splices(const CffDictOffsets& dict, bool top, Splice (&out)[CffDictOffsets::kFields], uint8_t* count) const;
  bool rewrittenBytes(const CffDictOffsets& dict, bool top, uint32_t* length) const;
  bool writeIndex(FontBytes& bytes, const Piece& piece, Window& out) const;

  // Set by plan(); empty until then, so a block asked before it (or after it failed) holds nothing.
  Piece pieces_[kMaxPieces] = {};
  uint8_t pieceCount_ = 0;
  bool cid_ = false;
  CffIndex topDicts_;  // in the file
  CffIndex fdArray_;
  // Where the parts the Top DICT points at land in the rebuilt table.
  uint32_t charsetAt_ = 0;
  uint32_t encodingAt_ = 0;
  uint32_t fdSelectAt_ = 0;
  uint32_t fdArrayAt_ = 0;
  uint32_t charStringsAt_ = 0;
  // Where the first Private DICT or Subrs INDEX was, from the table's start, and is in the rebuilt table.
  uint32_t privatesFrom_ = 0;
  uint32_t privatesAt_ = 0;
  uint32_t bytes_ = 0;
  uint32_t dataOffset_ = 0;
  uint32_t dataLength_ = 0;
};

}  // namespace ttf
