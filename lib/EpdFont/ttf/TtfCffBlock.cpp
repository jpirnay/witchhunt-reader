#include "TtfCffBlock.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "TtfFace.h"

namespace ttf {

namespace {

// The file as the parser reads it, through a page cache of its own: while a block is planned, and while a read makes
// a DICT INDEX again. About 4 KB, on the heap for that time only. Never moved: the FontBytes points at the range.
struct FileParse {
  explicit FileParse(const FileSource& file) : range{file, 0, 0, file.size, &cache}, bytes(nullptr, 0, &range) {}
  SdPageCache cache;
  SdRange range;
  FontBytes bytes;
};

// What a plan reads: the file, and its face.
struct PlanWork {
  explicit PlanWork(const FileSource& file) : parse(file) {}
  FileParse parse;
  Face face;
};

// The offset size an INDEX needs for offsets up to `last`.
uint8_t offSizeFor(const uint32_t last) { return last <= 0xFF ? 1 : last <= 0xFFFF ? 2 : last <= 0xFFFFFF ? 3 : 4; }

}  // namespace

// The part of the rebuilt table a read asks for, [from, to), and where it goes; `at` is where the bytes made next
// land in the table.
struct CffBlock::Window {
  uint32_t from;
  uint32_t to;
  uint8_t* dst;
  uint32_t at;

  bool done() const { return at >= to; }
  // `n` bytes made in memory.
  void put(const uint8_t* src, const uint32_t n) {
    const uint32_t lo = std::max(at, from);
    const uint32_t hi = std::min(at + n, to);
    if (lo < hi) std::memcpy(dst + (lo - from), src + (lo - at), hi - lo);
    at += n;
  }
  // `n` bytes of the file from `source` on.
  bool copy(FontBytes& bytes, const uint32_t source, const uint32_t n) {
    const uint32_t lo = std::max(at, from);
    const uint32_t hi = std::min(at + n, to);
    const bool read = lo >= hi || bytes.read(source + (lo - at), hi - lo, dst + (lo - from));
    at += n;
    return read;
  }
};

bool CffBlock::plan(const FileSource& file) {
  *this = CffBlock{};
  const auto work = makeUniqueNoThrow<PlanWork>(file);
  if (!work) {
    LOG_ERR("TTF", "SD mode: no %u B to read a CFF table with", static_cast<unsigned>(sizeof(PlanWork)));
    return false;
  }
  FontBytes& bytes = work->parse.bytes;
  const CffFace& cff = work->face.cff;
  CffParts parts;
  if (parseFace(bytes, &work->face) != FaceStatus::Ok || work->face.outlines != Face::Outlines::Cff ||
      !readCffParts(bytes, cff, &parts)) {
    return false;
  }
  cid_ = cff.cid;
  topDicts_ = parts.topDicts;
  fdArray_ = cff.fdArray;
  if (parts.privatesEnd > parts.privates) privatesFrom_ = parts.privates - cff.table;

  // The DICTs written again: their sizes do not depend on where anything lands.
  CffDictOffsets dict;  // the Top DICT, then each font DICT
  uint32_t topBytes = 0;
  if (!readCffDictOffsets(bytes, topDicts_, 0, &dict) || !rewrittenBytes(dict, true, &topBytes)) return false;
  uint32_t fontDictBytes = 0;  // at most 256 DICTs of kCffMaxDictBytes
  for (uint32_t fd = 0; cid_ && fd < fdArray_.count; ++fd) {
    uint32_t length = 0;
    if (!readCffDictOffsets(bytes, fdArray_, fd, &dict) || !rewrittenBytes(dict, false, &length)) return false;
    fontDictBytes += length;
  }

  // Where everything lands, in order.
  uint64_t end = 0;
  const auto add = [&](const Part part, const uint32_t from, const uint32_t length, const uint8_t offSize = 0) {
    pieces_[pieceCount_++] = {static_cast<uint32_t>(end), length, from, part, offSize};
    const uint32_t at = static_cast<uint32_t>(end);
    end += length;
    return at;
  };
  // A DICT INDEX of `count` DICTs, `data` bytes together.
  const auto addIndex = [&](const Part part, const uint32_t count, const uint32_t data) {
    const uint8_t offSize = offSizeFor(data + 1);
    return add(part, 0, 3 + (count + 1) * offSize + data, offSize);
  };
  add(Part::Copy, cff.table, parts.topDicts.start - cff.table);
  addIndex(Part::TopDicts, 1, topBytes);
  add(Part::Copy, parts.topDicts.end, cff.globalSubrs.end - parts.topDicts.end);
  if (parts.charsetBytes != 0) charsetAt_ = add(Part::Copy, cff.charset, parts.charsetBytes);
  if (parts.encodingBytes != 0) encodingAt_ = add(Part::Copy, parts.encoding, parts.encodingBytes);
  if (cid_) {
    fdSelectAt_ = add(Part::Copy, cff.fdSelect, parts.fdSelectBytes);
    fdArrayAt_ = addIndex(Part::FdArray, fdArray_.count, fontDictBytes);
  }
  if (parts.privatesEnd > parts.privates) {
    privatesAt_ = add(Part::Copy, parts.privates, parts.privatesEnd - parts.privates);
  }
  charStringsAt_ = add(Part::Copy, cff.charStrings.start, cff.charStrings.dataStart - cff.charStrings.start);
  // Every offset is written as a 5-byte integer: a positive int32.
  if (end > INT32_MAX) return false;
  bytes_ = static_cast<uint32_t>(end);
  dataOffset_ = cff.charStrings.dataStart;
  dataLength_ = cff.charStrings.end - cff.charStrings.dataStart;
  return true;
}

bool CffBlock::read(const FileSource& file, const uint32_t offset, uint8_t* dst, const uint32_t len) const {
  if (offset > bytes_ || len > bytes_ - offset) return false;
  Window window{offset, offset + len, dst, 0};
  std::unique_ptr<FileParse> parse;  // for the first DICT INDEX the read reaches
  for (uint8_t i = 0; i < pieceCount_; ++i) {
    const Piece& piece = pieces_[i];
    const uint32_t lo = std::max(offset, piece.at);
    const uint32_t hi = std::min(offset + len, piece.at + piece.length);
    if (lo >= hi) continue;
    if (piece.part == Part::Copy) {
      if (!file.read(file.ctx, piece.from + (lo - piece.at), dst + (lo - offset), hi - lo)) return false;
      continue;
    }
    if (!parse) parse = makeUniqueNoThrow<FileParse>(file);
    if (!parse) {
      LOG_ERR("TTF", "SD mode: no %u B to make a CFF DICT INDEX with", static_cast<unsigned>(sizeof(FileParse)));
      return false;
    }
    if (!writeIndex(parse->bytes, piece, window)) return false;
  }
  return true;
}

// The operands of `dict` (the Top DICT when `top`, else a font DICT) whose part moved, in the DICT's order, with the
// offsets they now hold. False when the DICT does not set one of them itself (a CID-keyed font's FDArray offset left
// at its default of 0): there is no operand to write again.
bool CffBlock::splices(const CffDictOffsets& dict, const bool top, Splice (&out)[CffDictOffsets::kFields],
                       uint8_t* count) const {
  *count = 0;
  const auto move = [&](const uint8_t field, const uint32_t value) {
    if (dict.operandBytes[field] == 0) return false;
    uint8_t i = (*count)++;
    for (; i > 0 && out[i - 1].at > dict.operand[field]; --i) out[i] = out[i - 1];
    out[i] = {dict.operand[field], dict.operandBytes[field], value};
    return true;
  };
  if (top) {
    if (dict.value[CffDictOffsets::Charset] > 2 && !move(CffDictOffsets::Charset, charsetAt_)) return false;
    if (!cid_ && dict.value[CffDictOffsets::Encoding] > 1 && !move(CffDictOffsets::Encoding, encodingAt_)) {
      return false;
    }
    if (!move(CffDictOffsets::CharStrings, charStringsAt_)) return false;
    if (cid_ && (!move(CffDictOffsets::FdArray, fdArrayAt_) || !move(CffDictOffsets::FdSelect, fdSelectAt_))) {
      return false;
    }
  }
  if (dict.readsPrivate) {
    // Every Private DICT keeps its place among the others.
    const uint32_t offset = dict.value[CffDictOffsets::Private];
    if (offset < privatesFrom_ || !move(CffDictOffsets::Private, privatesAt_ + (offset - privatesFrom_))) return false;
  }
  return true;
}

// The bytes of `dict` written again: each operand moved takes 5 bytes. False when it would pass kCffMaxDictBytes,
// which readCffTable refuses.
bool CffBlock::rewrittenBytes(const CffDictOffsets& dict, const bool top, uint32_t* length) const {
  Splice splice[CffDictOffsets::kFields];
  uint8_t count = 0;
  if (!splices(dict, top, splice, &count)) return false;
  int64_t bytes = dict.length;
  for (uint8_t i = 0; i < count; ++i) bytes += 5 - int64_t{splice[i].bytes};
  *length = static_cast<uint32_t>(bytes);
  return bytes <= kCffMaxDictBytes;
}

// A DICT INDEX made again: its count, offset size and offsets, then each DICT with its moved operands written again.
bool CffBlock::writeIndex(FontBytes& bytes, const Piece& piece, Window& out) const {
  const bool top = piece.part == Part::TopDicts;
  const CffIndex& source = top ? topDicts_ : fdArray_;
  const uint32_t count = top ? 1 : fdArray_.count;
  const uint32_t data = piece.at + 3 + (count + 1) * piece.offSize;
  if (out.from < data) {
    out.at = piece.at;
    const uint8_t head[3] = {static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count), piece.offSize};
    out.put(head, sizeof head);
    uint32_t offset = 1;
    for (uint32_t i = 0; !out.done(); ++i) {
      uint8_t be[4];
      for (uint8_t k = 0; k < piece.offSize; ++k) be[k] = static_cast<uint8_t>(offset >> (8 * (piece.offSize - 1 - k)));
      out.put(be, piece.offSize);
      if (i == count) break;
      CffDictOffsets dict;
      uint32_t length = 0;
      if (!readCffDictOffsets(bytes, source, i, &dict) || !rewrittenBytes(dict, top, &length)) return false;
      offset += length;
    }
  }
  out.at = data;
  for (uint32_t i = 0; i < count && !out.done(); ++i) {
    CffDictOffsets dict;
    Splice splice[CffDictOffsets::kFields];
    uint8_t moved = 0;
    if (!readCffDictOffsets(bytes, source, i, &dict) || !splices(dict, top, splice, &moved)) return false;
    uint32_t from = dict.start;
    for (uint8_t k = 0; k < moved; ++k) {
      const uint32_t value = splice[k].value;
      const uint8_t number[5] = {29, static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                                 static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
      if (!out.copy(bytes, from, splice[k].at - from)) return false;
      out.put(number, sizeof number);
      from = splice[k].at + splice[k].bytes;
    }
    if (!out.copy(bytes, from, dict.start + dict.length - from)) return false;
  }
  return true;
}

}  // namespace ttf
