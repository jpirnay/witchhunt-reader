#include "ProgressiveJpeg.h"

#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace ProgressiveJpeg {
namespace {

constexpr uint8_t MAX_COMPONENTS = 4;
constexpr uint8_t MAX_LUMA_SCANS = 32;
// Huffman tables live in a pool the scans point into. A DHT can redefine a slot between scans
// (optimised files emit one per scan), so a table a recorded scan still uses must survive; the
// pool only reclaims entries nobody references.
constexpr uint8_t TABLE_POOL = 16;
constexpr size_t INPUT_BUFFER = 512;

// Zigzag index -> natural (row * 8 + column) index.
constexpr uint8_t ZIGZAG[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

bool isStartOfFrame(const uint8_t marker) {
  return (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
         (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
}

// Random-access byte reader over the file. Every cursor reads through it; a cursor switch
// usually lands outside the buffer and costs one seek + read, which is the price of holding
// one 512-byte buffer instead of one per scan.
struct Source {
  FsFile* file = nullptr;
  uint8_t buffer[INPUT_BUFFER] = {};
  uint32_t start = 0;
  uint16_t length = 0;
  bool ioError = false;
  uint32_t reads = 0;
  uint32_t bytesRead = 0;

  // -1 past the end of the file (or on a read error, which sets ioError). A seek past the end
  // fails on SdFat: that is end of data too, which the bit reader pads with zeros like libjpeg.
  int at(const uint32_t pos) {
    if (pos - start >= length) {
      if (!file->seek(pos)) return -1;
      const int count = file->read(buffer, sizeof(buffer));
      ++reads;
      if (count > 0) bytesRead += static_cast<uint32_t>(count);
      if (count <= 0) {
        if (count < 0) ioError = true;
        length = 0;
        return -1;
      }
      start = pos;
      length = static_cast<uint16_t>(count);
    }
    return buffer[pos - start];
  }
};

// 8-bit lookahead for an AC table: entry[next 8 bits] = (code length << 8) | symbol, or 0 for a
// code longer than 8 bits (decoded by the canonical walk from length 9). With optimised tables
// nearly every AC symbol is 8 bits or shorter, so one lookup replaces a walk of up to 16 steps.
// Half a KB each, so only the AC tables luma scans use get one (kMaxLookaheads).
struct Lookahead {
  uint16_t entry[256];
};

struct HuffmanTable {
  uint8_t count[17] = {};
  uint16_t firstCode[17] = {};
  uint16_t firstSymbol[17] = {};
  const Lookahead* lookahead = nullptr;  // into State::lookaheads; State never moves
  uint16_t symbolCount = 0;
  uint8_t symbols[256] = {};

  bool build(const uint8_t counts[16]) {
    uint32_t code = 0;
    uint16_t offset = 0;
    for (uint8_t length = 1; length <= 16; ++length) {
      const uint8_t n = counts[length - 1];
      if (code + n > (1UL << length)) return false;
      count[length] = n;
      firstCode[length] = static_cast<uint16_t>(code);
      firstSymbol[length] = offset;
      code = (code + n) << 1;
      offset = static_cast<uint16_t>(offset + n);
    }
    symbolCount = offset;
    return offset <= 256;
  }

  void buildLookahead(Lookahead& la) {
    memset(la.entry, 0, sizeof(la.entry));
    for (uint8_t length = 1; length <= 8; ++length) {
      for (uint16_t i = 0; i < count[length]; ++i) {
        const uint32_t code = static_cast<uint32_t>(firstCode[length] + i);
        const uint16_t value = static_cast<uint16_t>((length << 8) | symbols[firstSymbol[length] + i]);
        const uint32_t first = code << (8 - length);
        for (uint32_t k = 0; k < (1u << (8 - length)); ++k) la.entry[first + k] = value;
      }
    }
    lookahead = &la;
  }

  bool sameAs(const HuffmanTable& other) const {
    return symbolCount == other.symbolCount && memcmp(count, other.count, sizeof(count)) == 0 &&
           memcmp(symbols, other.symbols, symbolCount) == 0;
  }
};

struct Component {
  uint8_t id = 0;
  uint8_t h = 1;
  uint8_t v = 1;
  uint8_t quantizer = 0;
};

// One scan that touches luma. Chroma-only scans are skipped entirely.
struct Scan {
  uint32_t dataStart = 0;
  uint16_t restartInterval = 0;
  uint8_t compCount = 0;
  uint8_t comp[MAX_COMPONENTS] = {};     // frame component indices, in scan order
  uint8_t dcTable[MAX_COMPONENTS] = {};  // pool indices (first DC scans)
  uint8_t acTable = 0;                   // pool index (AC scans)
  uint8_t ss = 0, se = 0, ah = 0, al = 0;
};

// Everything needed to resume a scan where the previous band left it.
struct Cursor {
  uint32_t pos = 0;   // next entropy-coded byte
  uint32_t bits = 0;  // left-aligned bit buffer
  uint8_t bitCount = 0;
  bool markerHit = false;  // reached a marker (or EOF): further bits read as zero, as libjpeg does
  uint16_t unitsToRestart = 0;
  uint32_t eobrun = 0;
  int32_t predictor[MAX_COMPONENTS] = {};
};

struct State {
  Source source;

  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t compCount = 0;
  uint8_t maxH = 1;
  uint8_t maxV = 1;
  Component comps[MAX_COMPONENTS];
  uint16_t quant[4][64] = {};  // natural order

  HuffmanTable pool[TABLE_POOL];
  uint8_t poolUsed = 0;
  static constexpr uint8_t kMaxLookaheads = 6;  // AC tables luma scans use: ~2 (spectral selection) to ~6 (SA)
  Lookahead lookaheads[kMaxLookaheads];
  uint8_t lookaheadsUsed = 0;
  int8_t current[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};  // [DC/AC][id] -> pool index

  Scan scans[MAX_LUMA_SCANS];
  Cursor cursors[MAX_LUMA_SCANS];
  uint8_t scanCount = 0;
  uint16_t restartInterval = 0;
  bool frameSeen = false;

  // Scaled inverse DCT basis for the chosen output size, x 1024 (see idctBlock).
  int16_t basis[8][8] = {};
  int8_t keptIndex[64] = {};  // natural index -> index into a block's kept coefficients, -1 = not kept
};

struct Geometry {
  uint16_t lumaCols = 0;  // luma blocks per row actually coded in non-interleaved scans
  uint16_t lumaRows = 0;
  uint16_t mcuCols = 0;       // interleaved scans
  uint16_t paddedCols = 0;    // band width in blocks (covers both)
  uint8_t bandBlockRows = 1;  // luma block rows per band (one MCU row)
  uint16_t bandCount = 0;
  uint8_t n = 8;      // output pixels per block side
  uint8_t kept = 64;  // coefficients kept per block (n * n)
};

Geometry geometryFor(const uint16_t width, const uint16_t height, const uint8_t compCount, const uint8_t maxH,
                     const uint8_t maxV, const uint8_t shift) {
  Geometry g;
  g.lumaCols = static_cast<uint16_t>((width + 7) / 8);
  g.lumaRows = static_cast<uint16_t>((height + 7) / 8);
  if (compCount == 1) {  // every scan of a one-component frame is non-interleaved, MCU = one block
    g.mcuCols = g.lumaCols;
    g.paddedCols = g.lumaCols;
    g.bandBlockRows = 1;
    g.bandCount = g.lumaRows;
  } else {
    g.mcuCols = static_cast<uint16_t>((width + 8 * maxH - 1) / (8 * maxH));
    g.paddedCols = static_cast<uint16_t>(g.mcuCols * maxH);
    g.bandBlockRows = maxV;
    g.bandCount = static_cast<uint16_t>((height + 8 * maxV - 1) / (8 * maxV));
  }
  g.n = static_cast<uint8_t>(8 >> shift);
  g.kept = static_cast<uint8_t>(g.n * g.n);
  return g;
}

constexpr size_t alignUp(const size_t v) { return (v + 7) & ~static_cast<size_t>(7); }

// Workspace layout: State | masks (uint64 per band block) | coefficients (int16) | one block row
// of output pixels.
struct Layout {
  size_t maskOffset, coefOffset, pixelOffset, total;
};

Layout layoutFor(const Geometry& g) {
  const size_t blocks = static_cast<size_t>(g.bandBlockRows) * g.paddedCols;
  Layout l{};
  l.maskOffset = alignUp(sizeof(State));
  l.coefOffset = alignUp(l.maskOffset + blocks * sizeof(uint64_t));
  l.pixelOffset = alignUp(l.coefOffset + blocks * g.kept * sizeof(int16_t));
  l.total = alignUp(l.pixelOffset + static_cast<size_t>(g.n) * g.paddedCols * g.n) + 8;  // + alignment slack
  return l;
}

Result inputFailure(const Source& source) { return source.ioError ? Result::IoError : Result::InvalidData; }

// ---- header pass ------------------------------------------------------------------------

bool tableReferenced(const State& st, const uint8_t index) {
  for (const auto& cls : st.current) {
    for (const int8_t t : cls) {
      if (t == index) return true;
    }
  }
  for (uint8_t s = 0; s < st.scanCount; ++s) {
    const Scan& scan = st.scans[s];
    if (scan.ss == 0) {
      if (scan.ah == 0) {
        for (uint8_t k = 0; k < scan.compCount; ++k) {
          if (scan.dcTable[k] == index) return true;
        }
      }
    } else if (scan.acTable == index) {
      return true;
    }
  }
  return false;
}

int freeTableSlot(const State& st) {
  if (st.poolUsed < TABLE_POOL) return st.poolUsed;
  for (uint8_t i = 0; i < TABLE_POOL; ++i) {
    if (!tableReferenced(st, i)) return i;
  }
  return -1;
}

Result parseQuantizers(State& st, uint32_t p, uint32_t end) {
  while (p < end) {
    const int descriptor = st.source.at(p++);
    if (descriptor < 0) return inputFailure(st.source);
    const uint8_t precision = static_cast<uint8_t>(descriptor) >> 4;
    const uint8_t id = static_cast<uint8_t>(descriptor) & 0x0F;
    if (precision > 1 || id >= 4 || p + (precision ? 128U : 64U) > end) return Result::InvalidData;
    for (uint8_t k = 0; k < 64; ++k) {
      int value = st.source.at(p++);
      if (precision) value = (value << 8) | st.source.at(p++);
      if (value < 0) return inputFailure(st.source);
      st.quant[id][ZIGZAG[k]] = static_cast<uint16_t>(value);
    }
  }
  return Result::Ok;
}

Result parseHuffmanTables(State& st, uint32_t p, const uint32_t end) {
  while (p < end) {
    if (p + 17 > end) return Result::InvalidData;
    const int descriptor = st.source.at(p++);
    if (descriptor < 0) return inputFailure(st.source);
    const uint8_t cls = static_cast<uint8_t>(descriptor) >> 4;
    const uint8_t id = static_cast<uint8_t>(descriptor) & 0x0F;
    if (cls > 1 || id >= 4) return Result::InvalidData;
    const int slot = freeTableSlot(st);
    if (slot < 0) return Result::Unsupported;
    HuffmanTable& table = st.pool[slot];
    table.lookahead = nullptr;  // a reclaimed slot: any lookahead described the old table
    uint8_t counts[16];
    for (uint8_t& c : counts) {
      const int value = st.source.at(p++);
      if (value < 0) return inputFailure(st.source);
      c = static_cast<uint8_t>(value);
    }
    if (!table.build(counts) || p + table.symbolCount > end) return Result::InvalidData;
    for (uint16_t i = 0; i < table.symbolCount; ++i) {
      const int value = st.source.at(p++);
      if (value < 0) return inputFailure(st.source);
      table.symbols[i] = static_cast<uint8_t>(value);
    }
    int index = slot;
    for (uint8_t i = 0; i < st.poolUsed; ++i) {
      if (i != slot && st.pool[i].sameAs(table)) {
        index = i;
        break;
      }
    }
    st.current[cls][id] = static_cast<int8_t>(index);
    if (index == slot && slot == st.poolUsed) ++st.poolUsed;
  }
  return Result::Ok;
}

Result parseFrame(State& st, uint32_t p, const uint32_t end) {
  if (end - p < 6) return Result::InvalidData;
  const int precision = st.source.at(p);
  st.height = static_cast<uint16_t>((st.source.at(p + 1) << 8) | st.source.at(p + 2));
  st.width = static_cast<uint16_t>((st.source.at(p + 3) << 8) | st.source.at(p + 4));
  st.compCount = static_cast<uint8_t>(st.source.at(p + 5));
  p += 6;
  if (precision != 8 || st.width == 0 || st.height == 0 || st.compCount == 0 || st.compCount > MAX_COMPONENTS ||
      end - p != 3U * st.compCount) {
    return Result::Unsupported;
  }
  for (uint8_t i = 0; i < st.compCount; ++i, p += 3) {
    Component& c = st.comps[i];
    const int sampling = st.source.at(p + 1);
    c.id = static_cast<uint8_t>(st.source.at(p));
    c.h = static_cast<uint8_t>(sampling >> 4);
    c.v = static_cast<uint8_t>(sampling & 0x0F);
    c.quantizer = static_cast<uint8_t>(st.source.at(p + 2));
    if (c.h == 0 || c.v == 0 || c.h > 4 || c.v > 4 || c.quantizer >= 4) return Result::Unsupported;
    st.maxH = std::max(st.maxH, c.h);
    st.maxV = std::max(st.maxV, c.v);
  }
  // Luma must carry the full sampling, or its blocks would not map 1:1 onto output pixels.
  if (st.compCount > 1 && (st.comps[0].h != st.maxH || st.comps[0].v != st.maxV)) return Result::Unsupported;
  st.frameSeen = true;
  return Result::Ok;
}

// Records the scan if it touches luma. `p` is the first entropy-coded byte on success.
Result parseScan(State& st, uint32_t p, const uint32_t end) {
  if (!st.frameSeen) return Result::InvalidData;
  const int n = st.source.at(p++);
  if (n <= 0 || n > MAX_COMPONENTS || end - p != 2U * n + 3U) return Result::InvalidData;
  Scan scan;
  scan.compCount = static_cast<uint8_t>(n);
  bool hasLuma = false;
  uint8_t acId = 0;
  uint8_t dcIds[MAX_COMPONENTS] = {};
  for (int k = 0; k < n; ++k, p += 2) {
    const int selector = st.source.at(p);
    const int tables = st.source.at(p + 1);
    int index = -1;
    for (uint8_t c = 0; c < st.compCount; ++c) {
      if (st.comps[c].id == selector) index = c;
    }
    if (index < 0 || tables < 0) return Result::InvalidData;
    scan.comp[k] = static_cast<uint8_t>(index);
    dcIds[k] = static_cast<uint8_t>(tables >> 4) & 3;
    acId = static_cast<uint8_t>(tables & 3);
    hasLuma |= index == 0;
  }
  scan.ss = static_cast<uint8_t>(st.source.at(p));
  scan.se = static_cast<uint8_t>(st.source.at(p + 1));
  const int approx = st.source.at(p + 2);
  scan.ah = static_cast<uint8_t>(approx >> 4);
  scan.al = static_cast<uint8_t>(approx & 0x0F);
  const bool badSpectrum = scan.ss == 0 ? scan.se != 0 : (scan.se < scan.ss || scan.se > 63 || n != 1);
  if (badSpectrum || scan.al > 13) {
    return Result::InvalidData;
  }
  if (!hasLuma) return Result::Ok;
  if (st.scanCount >= MAX_LUMA_SCANS) return Result::Unsupported;

  if (scan.ss == 0) {
    if (scan.ah == 0) {
      for (int k = 0; k < n; ++k) {
        const int8_t t = st.current[0][dcIds[k]];
        if (t < 0) return Result::InvalidData;
        scan.dcTable[k] = static_cast<uint8_t>(t);
      }
    }
  } else {
    const int8_t t = st.current[1][acId];
    if (t < 0) return Result::InvalidData;
    scan.acTable = static_cast<uint8_t>(t);
    if (!st.pool[t].lookahead && st.lookaheadsUsed < State::kMaxLookaheads) {
      st.pool[t].buildLookahead(st.lookaheads[st.lookaheadsUsed++]);
    }
  }
  scan.restartInterval = st.restartInterval;
  scan.dataStart = end;
  st.scans[st.scanCount++] = scan;
  return Result::Ok;
}

// Walks the whole file once: tables, frame, and every luma scan's entry point. Entropy-coded
// data has no length field, so each scan is skipped by looking for its terminating marker.
// `options.shouldAbort` is polled every INDEX_POLL_BYTES: a 2 MB cover is seconds of SD reads,
// and the caller's hook is where the watchdog gets fed.
constexpr uint32_t INDEX_POLL_BYTES = 32 * 1024;

Result indexFile(State& st, const DecodeOptions& options) {
  Source& src = st.source;
  uint32_t nextPoll = INDEX_POLL_BYTES;
  if (src.at(0) != 0xFF || src.at(1) != 0xD8) return inputFailure(src);
  uint32_t p = 2;
  for (;;) {
    int value = src.at(p);
    if (value < 0) break;  // truncated after the last complete segment: decode what we have
    if (value != 0xFF) return Result::InvalidData;
    int marker = src.at(p + 1);
    while (marker == 0xFF) marker = src.at(++p + 1);
    if (marker < 0) break;
    p += 2;
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (marker == 0xD9) break;

    const int hi = src.at(p);
    const int lo = src.at(p + 1);
    if (hi < 0 || lo < 0) return inputFailure(src);
    const uint32_t length = static_cast<uint32_t>((hi << 8) | lo);
    if (length < 2) return Result::InvalidData;
    const uint32_t payload = p + 2;
    const uint32_t end = p + length;
    Result result = Result::Ok;
    if (marker == 0xDB) {
      result = parseQuantizers(st, payload, end);
    } else if (marker == 0xC4) {
      result = parseHuffmanTables(st, payload, end);
    } else if (isStartOfFrame(static_cast<uint8_t>(marker))) {
      result = marker == 0xC2 ? parseFrame(st, payload, end) : Result::Unsupported;
    } else if (marker == 0xDD) {
      if (length != 4) return Result::InvalidData;
      st.restartInterval = static_cast<uint16_t>((src.at(payload) << 8) | src.at(payload + 1));
    } else if (marker == 0xDA) {
      result = parseScan(st, payload, end);
      if (result != Result::Ok) return result;
      // Skip the entropy-coded segment: stuffed 0xFF00 and RSTn belong to it, anything else ends it.
      p = end;
      for (;;) {
        const int b = src.at(p);
        if (b < 0) break;
        if (b != 0xFF) {
          if (++p >= nextPoll) {
            nextPoll = p + INDEX_POLL_BYTES;
            if (options.shouldAbort && options.shouldAbort(options.abortUser)) return Result::Aborted;
          }
          continue;
        }
        const int next = src.at(p + 1);
        if (next < 0) break;
        if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) {
          p += 2;
        } else if (next == 0xFF) {
          ++p;
        } else {
          break;
        }
      }
      if (src.at(p) < 0) break;
      continue;
    }
    if (result != Result::Ok) return result;
    p = end;
  }
  if (src.ioError) return Result::IoError;
  if (!st.frameSeen || st.scanCount == 0) return Result::InvalidData;
  return Result::Ok;
}

// ---- entropy decoding -------------------------------------------------------------------

struct BitReader {
  Source& src;
  Cursor& c;

  void fill() {
    while (c.bitCount <= 24) {
      uint32_t byte = 0;
      if (!c.markerHit) {
        const int b = src.at(c.pos);
        if (b < 0) {
          c.markerHit = true;
        } else if (b == 0xFF) {
          const int next = src.at(c.pos + 1);
          if (next == 0x00) {
            byte = 0xFF;
            c.pos += 2;
          } else {
            c.markerHit = true;  // stay on the 0xFF so a restart can find its marker
          }
        } else {
          byte = static_cast<uint32_t>(b);
          ++c.pos;
        }
      }
      c.bits |= byte << (24 - c.bitCount);
      c.bitCount = static_cast<uint8_t>(c.bitCount + 8);
    }
  }

  uint32_t bits(const uint8_t count) {
    if (count == 0) return 0;
    if (c.bitCount < count) fill();
    const uint32_t value = c.bits >> (32 - count);
    c.bits <<= count;
    c.bitCount = static_cast<uint8_t>(c.bitCount - count);
    return value;
  }

  bool readBit() { return bits(1) != 0; }

  int symbol(const HuffmanTable& table) {
    if (c.bitCount < 16) fill();
    uint8_t length = 1;
    if (table.lookahead) {
      const uint16_t e = table.lookahead->entry[c.bits >> 24];
      if (e != 0) {
        const uint8_t len = static_cast<uint8_t>(e >> 8);
        c.bits <<= len;
        c.bitCount = static_cast<uint8_t>(c.bitCount - len);
        return e & 0xFF;
      }
      length = 9;  // every code of 8 bits or fewer is in the table
    }
    for (; length <= 16; ++length) {
      const uint32_t code = c.bits >> (32 - length);
      const uint32_t offset = code - table.firstCode[length];
      if (code >= table.firstCode[length] && offset < table.count[length]) {
        c.bits <<= length;
        c.bitCount = static_cast<uint8_t>(c.bitCount - length);
        return table.symbols[table.firstSymbol[length] + offset];
      }
    }
    return -1;
  }

  static int32_t extend(const uint32_t value, const uint8_t size) {
    if (size == 0) return 0;
    return value < (1U << (size - 1)) ? static_cast<int32_t>(value) - (1 << size) + 1 : static_cast<int32_t>(value);
  }

  // Discards padding bits and consumes the RSTn marker the encoder placed here.
  bool restart() {
    c.bits = 0;
    c.bitCount = 0;
    c.markerHit = false;
    int b = src.at(c.pos);
    while (b >= 0 && b != 0xFF) b = src.at(++c.pos);  // tolerate junk before the marker
    int marker = src.at(c.pos + 1);
    while (marker == 0xFF) marker = src.at(++c.pos + 1);
    if (marker < 0xD0 || marker > 0xD7) return false;
    c.pos += 2;
    c.eobrun = 0;
    memset(c.predictor, 0, sizeof(c.predictor));
    return true;
  }
};

// A luma block in the band, or nowhere (a chroma block of an interleaved DC scan).
struct BlockRef {
  int16_t* coef = nullptr;
  uint64_t* mask = nullptr;
};

inline void setCoefficient(const State& st, const BlockRef& block, const uint8_t zigzag, const int32_t value) {
  const uint8_t natural = ZIGZAG[zigzag];
  *block.mask |= 1ULL << natural;
  const int8_t kept = st.keptIndex[natural];
  if (kept >= 0) block.coef[kept] = static_cast<int16_t>(std::clamp<int32_t>(value, -32768, 32767));
}

// Correction bit for a coefficient already non-zero (G.1.2.3): widen its magnitude by p1.
inline void refineCoefficient(const State& st, BitReader& in, const BlockRef& block, const uint8_t natural,
                              const int32_t p1) {
  if (!in.readBit()) return;
  const int8_t kept = st.keptIndex[natural];
  if (kept < 0) return;
  int16_t& coef = block.coef[kept];
  if ((coef & p1) == 0) coef = static_cast<int16_t>(coef >= 0 ? coef + p1 : coef - p1);
}

bool decodeDcFirst(const State& st, BitReader& in, const Scan& scan, const uint8_t scanComp, const BlockRef& block) {
  const int size = in.symbol(st.pool[scan.dcTable[scanComp]]);
  if (size < 0 || size > 11) return false;
  const uint8_t comp = scan.comp[scanComp];
  in.c.predictor[comp] += BitReader::extend(in.bits(static_cast<uint8_t>(size)), static_cast<uint8_t>(size));
  if (block.coef) {
    block.coef[0] = static_cast<int16_t>(std::clamp<int32_t>(in.c.predictor[comp] * (1 << scan.al), -32768, 32767));
  }
  return true;
}

void decodeDcRefine(BitReader& in, const Scan& scan, const BlockRef& block) {
  if (in.readBit() && block.coef) block.coef[0] = static_cast<int16_t>(block.coef[0] | (1 << scan.al));
}

// G.1.2.2 (libjpeg decode_mcu_AC_first)
bool decodeAcFirst(const State& st, BitReader& in, const Scan& scan, const BlockRef& block) {
  Cursor& c = in.c;
  if (c.eobrun > 0) {
    --c.eobrun;
    return true;
  }
  const HuffmanTable& table = st.pool[scan.acTable];
  for (int k = scan.ss; k <= scan.se; ++k) {
    const int rs = in.symbol(table);
    if (rs < 0) return false;
    const uint8_t r = static_cast<uint8_t>(rs >> 4);
    const uint8_t s = static_cast<uint8_t>(rs & 15);
    if (s) {
      k += r;
      if (k > 63) return false;
      const int32_t value = BitReader::extend(in.bits(s), s) * (1 << scan.al);
      setCoefficient(st, block, static_cast<uint8_t>(k), value);
    } else if (r == 15) {
      k += 15;
    } else {
      c.eobrun = 1U << r;
      if (r) c.eobrun += in.bits(r);
      --c.eobrun;
      break;
    }
  }
  return true;
}

// G.1.2.3 (libjpeg decode_mcu_AC_refine). Which coefficients are non-zero comes from the mask,
// because a refinement must emit/consume a correction bit for every non-zero coefficient in the
// band -- including those this output scale does not keep.
bool decodeAcRefine(const State& st, BitReader& in, const Scan& scan, const BlockRef& block) {
  Cursor& c = in.c;
  const int32_t p1 = 1 << scan.al;
  const HuffmanTable& table = st.pool[scan.acTable];
  int k = scan.ss;
  if (c.eobrun == 0) {
    for (; k <= scan.se; ++k) {
      const int rs = in.symbol(table);
      if (rs < 0) return false;
      int r = rs >> 4;
      const int s = rs & 15;
      int32_t value = 0;
      if (s) {
        if (s != 1) return false;
        value = in.readBit() ? p1 : -p1;
      } else if (r != 15) {
        c.eobrun = 1U << r;
        if (r) c.eobrun += in.bits(static_cast<uint8_t>(r));
        break;  // the rest of this block is handled by the EOB-run logic below
      }
      // Advance over already-non-zero coefficients (refining each) and r still-zero ones.
      do {
        const uint8_t natural = ZIGZAG[k];
        if (*block.mask & (1ULL << natural)) {
          refineCoefficient(st, in, block, natural, p1);
        } else {
          if (--r < 0) break;  // reached the zero coefficient that becomes non-zero
        }
        ++k;
      } while (k <= scan.se);
      if (value) {
        if (k > 63) return false;
        setCoefficient(st, block, static_cast<uint8_t>(k), value);
      }
    }
  }
  if (c.eobrun > 0) {
    for (; k <= scan.se; ++k) {
      const uint8_t natural = ZIGZAG[k];
      if (*block.mask & (1ULL << natural)) refineCoefficient(st, in, block, natural, p1);
    }
    --c.eobrun;
  }
  return true;
}

bool beginUnit(BitReader& in, const Scan& scan) {
  if (scan.restartInterval == 0) return true;
  if (in.c.unitsToRestart == 0) {
    if (!in.restart()) return false;
    in.c.unitsToRestart = scan.restartInterval;
  }
  --in.c.unitsToRestart;
  return true;
}

// ---- inverse DCT ------------------------------------------------------------------------

// 512 * cos(k pi / 16), k = 0..31: every entry of every basis below is one of these.
constexpr int16_t COS16[32] = {512,  502,  473,  426,  362,  284,  196,  100,  0,    -100, -196,
                               -284, -362, -426, -473, -502, -512, -502, -473, -426, -362, -284,
                               -196, -100, 0,    100,  196,  284,  362,  426,  473,  502};

// basis[X][u] = C(u)/2 * cos((2X+1) u pi / 2n): the n-point inverse DCT over the block's top-left
// n x n coefficients, i.e. the 8x8 block resampled at the centres of its n x n output pixels with
// everything above the new Nyquist limit dropped (libjpeg's reduced-size IDCT does the same). The
// C(u)/2 normalisation is the 8-point one, so the DC level is preserved. Scaled by 1024
// (|entries| <= 0.5); with s = 8/n the angle is (2X+1) u s pi / 16, so it comes from COS16 and
// the decoder needs no floating point (the ESP32-C3 has no FPU).
void buildBasis(State& st, const uint8_t n) {
  const int s = 8 / n;
  for (int X = 0; X < n; ++X) {
    for (int u = 0; u < n; ++u) {
      st.basis[X][u] = u == 0 ? 362 : COS16[((2 * X + 1) * u * s) % 32];  // 362 = 1024 / (2 sqrt 2)
    }
  }
  for (int natural = 0; natural < 64; ++natural) {
    const int row = natural / 8;
    const int col = natural % 8;
    st.keptIndex[natural] = static_cast<int8_t>(row < n && col < n ? row * n + col : -1);
  }
}

// A block with no AC coefficient is one flat level: the same integer pipeline as idctBlock,
// applied once. Line art and diagrams are mostly such blocks, and the reduced IDCT was half of
// the decode before this shortcut.
void fillDcBlock(const int16_t dc, const uint16_t quant0, const uint8_t n, uint8_t* out, const size_t stride) {
  const int32_t f = std::clamp<int32_t>(dc * static_cast<int32_t>(quant0), -8191, 8191);
  const int32_t t = (f * 362 + 64) >> 7;
  const uint8_t level = static_cast<uint8_t>(std::clamp<int32_t>(128 + ((362 * t + (1 << 12)) >> 13), 0, 255));
  for (int Y = 0; Y < n; ++Y) memset(out + static_cast<size_t>(Y) * stride, level, n);
}

void idctBlock(const State& st, const int16_t* coef, const uint16_t* quant, const uint8_t n, uint8_t* out,
               const size_t stride) {
  int32_t t[8][8];  // basis * F, x 8: the only full-block temporary (256 B of stack)
  for (int v = 0; v < n; ++v) {
    int32_t f[8];
    bool used = false;
    for (int u = 0; u < n; ++u) {
      // Clamped: no valid 8-bit coefficient comes near it, and it keeps both passes inside int32.
      f[u] = std::clamp<int32_t>(coef[v * n + u] * static_cast<int32_t>(quant[v * 8 + u]), -8191, 8191);
      used |= f[u] != 0;
    }
    for (int X = 0; X < n; ++X) {
      int32_t sum = 0;
      if (used) {
        for (int u = 0; u < n; ++u) sum += f[u] * st.basis[X][u];
      }
      t[v][X] = (sum + 64) >> 7;
    }
  }
  for (int Y = 0; Y < n; ++Y) {
    uint8_t* row = out + static_cast<size_t>(Y) * stride;
    for (int X = 0; X < n; ++X) {
      int32_t sum = 0;
      for (int v = 0; v < n; ++v) sum += st.basis[Y][v] * t[v][X];
      row[X] = static_cast<uint8_t>(std::clamp<int32_t>(128 + ((sum + (1 << 12)) >> 13), 0, 255));
    }
  }
}

// ---- band loop --------------------------------------------------------------------------

Result decodeBands(State& st, const Geometry& g, uint8_t* workspace, const Layout& layout, const DecodeOptions& options,
                   const BandCallback callback, void* user) {
  auto* masks = reinterpret_cast<uint64_t*>(workspace + layout.maskOffset);
  auto* coefs = reinterpret_cast<int16_t*>(workspace + layout.coefOffset);
  uint8_t* pixels = workspace + layout.pixelOffset;
  const size_t bandBlocks = static_cast<size_t>(g.bandBlockRows) * g.paddedCols;
  const size_t pixelStride = static_cast<size_t>(g.paddedCols) * g.n;
  const uint16_t outWidth = static_cast<uint16_t>(st.width >> (3 - __builtin_ctz(g.n)));
  const uint16_t outHeight = static_cast<uint16_t>(st.height >> (3 - __builtin_ctz(g.n)));
  const uint16_t* quant = st.quant[st.comps[0].quantizer];

  for (uint8_t s = 0; s < st.scanCount; ++s) {
    Cursor& c = st.cursors[s];
    c = Cursor{};
    c.pos = st.scans[s].dataStart;
    c.unitsToRestart = st.scans[s].restartInterval;
  }

  auto blockAt = [&](const int bandRow, const int col) {
    const size_t index = static_cast<size_t>(bandRow) * g.paddedCols + col;
    return BlockRef{coefs + index * g.kept, masks + index};
  };

  for (uint16_t band = 0; band < g.bandCount; ++band) {
    if (options.shouldAbort && options.shouldAbort(options.abortUser)) return Result::Aborted;
    memset(masks, 0, bandBlocks * sizeof(uint64_t));
    memset(coefs, 0, bandBlocks * g.kept * sizeof(int16_t));
    const int firstRow = band * g.bandBlockRows;

    for (uint8_t s = 0; s < st.scanCount; ++s) {
      const Scan& scan = st.scans[s];
      BitReader in{st.source, st.cursors[s]};
      if (scan.compCount > 1) {
        // Interleaved (always a DC scan): one MCU row, chroma decoded only to stay in step.
        for (uint16_t mx = 0; mx < g.mcuCols; ++mx) {
          if (!beginUnit(in, scan)) return Result::InvalidData;
          for (uint8_t k = 0; k < scan.compCount; ++k) {
            const Component& comp = st.comps[scan.comp[k]];
            for (uint8_t by = 0; by < comp.v; ++by) {
              for (uint8_t bx = 0; bx < comp.h; ++bx) {
                const BlockRef block = scan.comp[k] == 0 ? blockAt(by, mx * st.maxH + bx) : BlockRef{};
                if (scan.ah == 0) {
                  if (!decodeDcFirst(st, in, scan, k, block)) return Result::InvalidData;
                } else {
                  decodeDcRefine(in, scan, block);
                }
              }
            }
          }
        }
        continue;
      }
      // Non-interleaved luma: whole block rows of the component's own (unpadded) grid.
      const int lastRow = std::min<int>(firstRow + g.bandBlockRows, g.lumaRows);
      for (int row = firstRow; row < lastRow; ++row) {
        for (uint16_t col = 0; col < g.lumaCols; ++col) {
          if (!beginUnit(in, scan)) return Result::InvalidData;
          const BlockRef block = blockAt(row - firstRow, col);
          bool ok = true;
          if (scan.ss == 0) {
            if (scan.ah == 0) {
              ok = decodeDcFirst(st, in, scan, 0, block);
            } else {
              decodeDcRefine(in, scan, block);
            }
          } else {
            ok = scan.ah == 0 ? decodeAcFirst(st, in, scan, block) : decodeAcRefine(st, in, scan, block);
          }
          if (!ok) return Result::InvalidData;
        }
      }
    }
    if (st.source.ioError) return Result::IoError;

    // Emit one block row of output at a time; only the columns that reach the output are built.
    const uint16_t colsNeeded = static_cast<uint16_t>((outWidth + g.n - 1) / g.n);
    for (int r = 0; r < g.bandBlockRows; ++r) {
      const int y = (firstRow + r) * g.n;
      if (y >= outHeight) break;
      for (uint16_t col = 0; col < colsNeeded; ++col) {
        const BlockRef block = blockAt(r, col);
        uint8_t* dst = pixels + static_cast<size_t>(col) * g.n;
        if ((*block.mask & ~1ULL) == 0) {
          fillDcBlock(block.coef[0], quant[0], g.n, dst, pixelStride);
        } else {
          idctBlock(st, block.coef, quant, g.n, dst, pixelStride);
        }
      }
      const uint16_t rows = static_cast<uint16_t>(std::min<int>(g.n, outHeight - y));
      if (!callback(user, static_cast<uint16_t>(y), pixels, outWidth, rows, static_cast<uint16_t>(pixelStride))) {
        return Result::Stopped;
      }
    }
  }
  return Result::Ok;
}

}  // namespace

Result probe(FsFile& file, ImageInfo& info) {
  info = {};
  if (!file || !file.seek(0)) return Result::InvalidData;
  auto finish = [&](const Result result) {
    file.seek(0);
    return result;
  };
  auto readByte = [&]() -> int {
    uint8_t value = 0;
    return file.read(&value, 1) == 1 ? value : -1;
  };
  if (readByte() != 0xFF || readByte() != 0xD8) return finish(Result::InvalidData);
  for (;;) {
    int value = readByte();
    while (value >= 0 && value != 0xFF) value = readByte();
    if (value < 0) return finish(Result::InvalidData);
    do {
      value = readByte();
    } while (value == 0xFF);
    if (value < 0) return finish(Result::InvalidData);
    const uint8_t marker = static_cast<uint8_t>(value);
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (marker == 0xD9 || marker == 0xDA) return finish(Result::InvalidData);
    const int hi = readByte();
    const int lo = readByte();
    if (hi < 0 || lo < 0) return finish(Result::InvalidData);
    const uint16_t length = static_cast<uint16_t>((hi << 8) | lo);
    if (length < 2) return finish(Result::InvalidData);
    if (!isStartOfFrame(marker)) {
      if (!file.seek(file.position() + length - 2)) return finish(Result::InvalidData);
      continue;
    }
    if (marker != 0xC2) return finish(Result::Unsupported);
    uint8_t frame[6 + 3 * MAX_COMPONENTS] = {};
    if (length < 8 || length - 2U > sizeof(frame) || file.read(frame, length - 2U) != static_cast<int>(length - 2U)) {
      return finish(Result::Unsupported);
    }
    info.height = static_cast<uint16_t>((frame[1] << 8) | frame[2]);
    info.width = static_cast<uint16_t>((frame[3] << 8) | frame[4]);
    info.componentCount = frame[5];
    if (frame[0] != 8 || info.width == 0 || info.height == 0 || info.componentCount == 0 ||
        info.componentCount > MAX_COMPONENTS || length - 8U != 3U * info.componentCount) {
      return finish(Result::Unsupported);
    }
    uint8_t lumaH = 1;
    uint8_t lumaV = 1;
    for (uint8_t i = 0; i < info.componentCount; ++i) {
      const uint8_t h = frame[7 + 3 * i] >> 4;
      const uint8_t v = frame[7 + 3 * i] & 0x0F;
      if (h == 0 || v == 0 || h > 4 || v > 4) return finish(Result::Unsupported);
      if (i == 0) {
        lumaH = h;
        lumaV = v;
      }
      info.maxHorizontal = std::max(info.maxHorizontal, h);
      info.maxVertical = std::max(info.maxVertical, v);
    }
    if (info.componentCount > 1 && (lumaH != info.maxHorizontal || lumaV != info.maxVertical)) {
      return finish(Result::Unsupported);
    }
    return finish(Result::Ok);
  }
}

size_t workspaceBytes(const ImageInfo& info, const uint8_t scaleShift) {
  if (info.width == 0 || info.height == 0 || scaleShift > 3) return 0;
  return layoutFor(geometryFor(info.width, info.height, info.componentCount, info.maxHorizontal, info.maxVertical,
                               scaleShift))
      .total;
}

Result decode(FsFile& file, const DecodeOptions& options, const BandCallback callback, void* user) {
  if (!file || callback == nullptr || options.scaleShift > 3) return Result::InvalidData;
  ImageInfo info;
  const Result probed = probe(file, info);
  if (probed != Result::Ok) return probed;

  const Geometry g = geometryFor(info.width, info.height, info.componentCount, info.maxHorizontal, info.maxVertical,
                                 options.scaleShift);
  const Layout layout = layoutFor(g);

  std::unique_ptr<uint8_t[]> owned;
  uint8_t* base = options.workspace;
  size_t size = options.workspaceSize;
  if (!base) {
    owned = makeUniqueNoThrow<uint8_t[]>(layout.total);
    if (!owned) return Result::OutOfMemory;
    base = owned.get();
    size = layout.total;
  }
  // Align the caller's block; layoutFor() includes the slack this may consume.
  const size_t skew = (8 - (reinterpret_cast<uintptr_t>(base) & 7)) & 7;
  if (size < layout.total || size - skew < layout.total - 8) return Result::OutOfMemory;
  uint8_t* workspace = base + skew;

  auto* st = new (workspace) State();
  st->source.file = &file;
  buildBasis(*st, g.n);
  const uint32_t t0 = options.clock ? options.clock() : 0;
  Result result = indexFile(*st, options);
  const uint32_t t1 = options.clock ? options.clock() : 0;
  if (result == Result::Ok) {
    // The probe and the full index read the same SOF; disagreeing means the file changed shape.
    if (st->width != info.width || st->height != info.height || st->compCount != info.componentCount) {
      result = Result::InvalidData;
    } else {
      result = decodeBands(*st, g, workspace, layout, options, callback, user);
    }
  }
  if (options.stats) {
    const uint32_t t2 = options.clock ? options.clock() : 0;
    options.stats->indexMs = t1 - t0;
    options.stats->bandsMs = t2 - t1;
    options.stats->reads = st->source.reads;
    options.stats->bytesRead = st->source.bytesRead;
  }
  st->~State();
  file.seek(0);
  return result;
}

const char* resultName(const Result result) {
  switch (result) {
    case Result::Ok:
      return "ok";
    case Result::Unsupported:
      return "unsupported";
    case Result::InvalidData:
      return "invalid data";
    case Result::IoError:
      return "I/O error";
    case Result::OutOfMemory:
      return "out of memory";
    case Result::Aborted:
      return "aborted";
    case Result::Stopped:
      return "stopped";
  }
  return "unknown";
}

}  // namespace ProgressiveJpeg
