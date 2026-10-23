#include "ProgressiveJpeg.h"

#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <type_traits>

namespace ProgressiveJpeg {
namespace {

constexpr uint8_t MAX_COMPONENTS = 4;
constexpr size_t INPUT_BUFFER = 512;

// What a session has room for. Huffman tables live in a pool the scans point into: a DHT can
// redefine a slot between scans (optimised files emit one per scan), so a table a recorded scan
// still uses must survive, and the pool only reclaims entries nobody references.
struct FullCaps {
  static constexpr bool kDcOnly = false;
  static constexpr uint8_t kTables = 16;
  static constexpr uint16_t kSymbols = 256;
  static constexpr uint8_t kLookaheads = 6;  // AC tables luma scans use: ~2 (spectral selection) to ~6 (SA)
  static constexpr uint8_t kScans = 32;      // luma scans
  static constexpr uint8_t kQuantized = 64;  // coefficients a quantization table keeps
  static constexpr uint8_t kBasis = 8;       // IDCT basis rows (8 >> scale)
};
// At 1/8 a block is its DC level: the AC scans are neither recorded nor decoded, and the session
// holds only what the DC scans need -- 2.3 KB on the C3 instead of 11.7.
struct DcCaps {
  static constexpr bool kDcOnly = true;
  // DC tables in use at once: one per table id, plus the ones a recorded DC scan still uses after
  // a redefinition.
  static constexpr uint8_t kTables = 8;
  static constexpr uint16_t kSymbols = 16;  // a DC symbol is a size category, 0..15
  static constexpr uint8_t kLookaheads = 0;
  static constexpr uint8_t kScans = 14;     // the first DC scan and a refinement per bit (Al <= 13)
  static constexpr uint8_t kQuantized = 1;  // the DC quantizer
  static constexpr uint8_t kBasis = 1;
};
// The read windows (DecodeOptions::readWindows): one per luma scan, the first SCAN_WINDOWS of
// them, while the bands decode, and all of them as one while the file is indexed.
constexpr size_t SCAN_WINDOW_BYTES = 1024;
constexpr uint8_t SCAN_WINDOWS = 8;

// Zigzag index -> natural (row * 8 + column) index.
constexpr uint8_t ZIGZAG[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

bool isStartOfFrame(const uint8_t marker) {
  return (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
         (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
}

// Bits ss..se: a spectral band of zigzag indices.
uint64_t spectralBand(const uint8_t ss, const uint8_t se) {
  const uint64_t upTo = se >= 63 ? ~0ULL : (1ULL << (se + 1)) - 1;
  return upTo & ~((1ULL << ss) - 1);
}

// The coefficients (zigzag bits) a decode keeps at `n` output pixels a block side: the block's
// top-left n x n square.
uint64_t keptCoefficients(const uint8_t n) {
  uint64_t kept = 0;
  for (uint8_t k = 0; k < 64; ++k) {
    if (ZIGZAG[k] / 8 < n && ZIGZAG[k] % 8 < n) kept |= 1ULL << k;
  }
  return kept;
}

// Random-access byte reader over the file. Every cursor reads through it. With the workspace's
// minimum, all of them share one 512-byte buffer, and a cursor switch usually lands outside it:
// a seek + read for every scan in every band, the file read ~6 times over (a 106 KB cover: 1275
// reads, 650 KB). Given read windows (DecodeOptions::readWindows), the first SCAN_WINDOWS luma
// scans read through a window each, and the index pass through all of them as one.
struct Source {
  FsFile* file = nullptr;
  uint32_t base = 0;  // file offset of the JPEG's first byte; `pos` below is relative to it
  uint8_t buffer[INPUT_BUFFER] = {};
  // The window at() reads through (select()): its bytes, their capacity, and what they hold.
  uint8_t* data = buffer;
  uint16_t capacity = INPUT_BUFFER;
  uint32_t start = 0;
  uint16_t length = 0;
  bool ioError = false;
  uint32_t reads = 0;
  uint32_t bytesRead = 0;

  // Windows: SHARED (`buffer`), 1..SCAN_WINDOWS (a scan's), WHOLE (the scan windows as one). A
  // scan's or WHOLE's start and length are kept in a slot at the front of the extra area while
  // another window is read, so a band's data picks up where the last band left it; SHARED's are
  // not (it is only a cache). The bookkeeping lives there, not here: the session is part of the
  // workspace's minimum, which a converter's 1/2-scale budget is held to.
  struct Slot {
    uint32_t start;
    uint16_t length;
  };
  static constexpr uint8_t SHARED = 0;
  static constexpr uint8_t WHOLE = SCAN_WINDOWS + 1;
  static constexpr size_t SLOTS_BYTES = (WHOLE + 1) * sizeof(Slot);
  uint8_t* extra = nullptr;  // [slots][SCAN_WINDOWS windows] in the workspace; null: none
  uint8_t current = SHARED;

  Slot* slots() const { return reinterpret_cast<Slot*>(extra); }
  uint8_t* windows() const { return extra + SLOTS_BYTES; }

  // Points the windows at `area` (scanWindowBytes(), or null for none), all empty. A resumed
  // checkpoint is re-bound this way: the windows' bytes are not part of it.
  void bind(uint8_t* const area) {
    extra = area;
    if (extra != nullptr) {
      for (uint8_t i = 0; i <= WHOLE; ++i) slots()[i] = Slot{0, 0};
    }
    current = SHARED;
    data = buffer;
    capacity = INPUT_BUFFER;
    start = 0;
    length = 0;
  }
  // The window for the index pass.
  void selectWhole() { select(extra != nullptr ? WHOLE : SHARED); }
  // The window for luma scan `s` while the bands decode.
  void selectScan(const uint8_t s) { select(extra != nullptr && s < SCAN_WINDOWS ? 1 + s : SHARED); }
  void select(const uint8_t slot) {
    if (slot == current) return;
    if (current != SHARED) slots()[current] = Slot{start, length};
    current = slot;
    if (slot == SHARED) {
      data = buffer;
      capacity = INPUT_BUFFER;
      start = 0;
      length = 0;
      return;
    }
    start = slots()[slot].start;
    length = slots()[slot].length;
    if (slot == WHOLE) {
      data = windows();
      capacity = static_cast<uint16_t>(SCAN_WINDOWS * SCAN_WINDOW_BYTES);
    } else {
      data = windows() + (slot - 1) * SCAN_WINDOW_BYTES;
      capacity = SCAN_WINDOW_BYTES;
    }
  }

  // -1 past the end of the file (or on a read error, which sets ioError). A seek past the end
  // fails on SdFat: that is end of data too, which the bit reader pads with zeros like libjpeg.
  int at(const uint32_t pos) {
    if (pos - start >= length) {
      if (!file->seek(base + pos)) return -1;
      const int count = file->read(data, capacity);
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
    return data[pos - start];
  }
};

// 8-bit lookahead for an AC table: entry[next 8 bits] = (code length << 8) | symbol, or 0 for a
// code longer than 8 bits (decoded by the canonical walk from length 9). With optimised tables
// nearly every AC symbol is 8 bits or shorter, so one lookup replaces a walk of up to 16 steps.
// Half a KB each, so only the AC tables luma scans use get one (kMaxLookaheads).
struct Lookahead {
  uint16_t entry[256];
};

template <uint16_t kSymbols>
struct HuffmanTable {
  uint8_t count[17] = {};
  uint16_t firstCode[17] = {};
  uint16_t firstSymbol[17] = {};
  const Lookahead* lookahead = nullptr;  // into State::lookaheads; State never moves
  uint16_t symbolCount = 0;
  uint8_t symbols[kSymbols] = {};

  // False for a code space that is over-full, or more symbols than the table holds.
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
    return offset <= kSymbols;
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
  uint8_t padBits = 0;      // how many of the buffer's last bits are padding past a marker or EOF
  bool markerHit = false;   // reached a marker (or EOF): further bits read as zero, as libjpeg does
  uint8_t nextRestart = 0;  // n of the RSTn marker the next restart must find
  uint16_t unitsToRestart = 0;
  uint32_t eobrun = 0;
  // An 8-bit frame's DC values fit in 12 bits; 16 keep the cursor, and every session's 32 of them,
  // at their size.
  int16_t predictor[MAX_COMPONENTS] = {};
  // The scan contributes to its units (MCUs of an interleaved scan, luma blocks otherwise, in scan
  // order) before this one only: its data is corrupt or ran out there, or that of a scan whose
  // coefficients it refines is (see finishUnit). The rest keep what earlier scans gave.
  uint32_t stopUnit = UINT32_MAX;
};

template <class C>
struct State {
  using Table = HuffmanTable<C::kSymbols>;
  Source source;

  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t compCount = 0;
  uint8_t maxH = 1;
  uint8_t maxV = 1;
  Component comps[MAX_COMPONENTS];
  uint16_t quant[4][C::kQuantized] = {};  // natural order, the first kQuantized coefficients

  Table pool[C::kTables];
  uint8_t poolUsed = 0;
  std::array<Lookahead, C::kLookaheads> lookaheads;
  uint8_t lookaheadsUsed = 0;
  int8_t current[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};  // [DC/AC][id] -> pool index

  Scan scans[C::kScans];
  Cursor cursors[C::kScans];
  uint8_t scanCount = 0;
  uint16_t restartInterval = 0;
  bool frameSeen = false;
  bool endedEarly = false;  // the file ends before its EOI marker: scans after the end may be missing
  // The luma coefficients (zigzag bits) a recorded scan finishes, down to their last bit (Al == 0):
  // a file that ends early is still whole when they hold every coefficient the decode keeps.
  uint64_t completed = 0;

  // Scaled inverse DCT basis for the chosen output size, x 1024 (see idctBlock).
  int16_t basis[C::kBasis][C::kBasis] = {};
  // Natural index -> index into a block's kept coefficients, -1 = not kept.
  int8_t keptIndex[C::kBasis * C::kBasis] = {};
};

struct Geometry {
  uint16_t lumaCols = 0;  // luma blocks per row actually coded in non-interleaved scans
  uint16_t lumaRows = 0;
  uint16_t mcuCols = 0;       // interleaved scans
  uint16_t paddedCols = 0;    // band width in blocks (covers both)
  uint8_t bandBlockRows = 1;  // luma block rows per band (one MCU row)
  uint16_t bandCount = 0;
  uint8_t n = 8;          // output pixels per block side
  uint8_t kept = 64;      // coefficients kept per block (n * n)
  uint16_t outWidth = 0;  // the rows emitted: floor(width >> scale), or at 1/8 with wholeBlocks every block
  uint16_t outHeight = 0;
};

Geometry geometryFor(const uint16_t width, const uint16_t height, const uint8_t compCount, const uint8_t maxH,
                     const uint8_t maxV, const uint8_t shift, const bool wholeBlocks = false) {
  Geometry g;
  const bool everyBlock = wholeBlocks && shift == 3;
  g.outWidth = static_cast<uint16_t>(everyBlock ? (width + 7) / 8 : width >> shift);
  g.outHeight = static_cast<uint16_t>(everyBlock ? (height + 7) / 8 : height >> shift);
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

// `options.shouldAbort` is polled every INDEX_POLL_BYTES: a 2 MB cover is seconds of SD reads,
// and the caller's hook is where the watchdog gets fed.
constexpr uint32_t INDEX_POLL_BYTES = 32 * 1024;
// A Decoder::step() unit while indexing: about eight SD reads.
constexpr uint32_t INDEX_STEP_BYTES = 4 * 1024;

// Where the index pass stands, so it can stop after a byte budget and resume (Decoder::step):
// either between segments, or inside an entropy-coded segment it is skipping.
struct IndexCursor {
  uint32_t p = 0;  // next byte to look at
  uint32_t nextPoll = INDEX_POLL_BYTES;
  bool started = false;    // SOI checked
  bool inEntropy = false;  // skipping a scan's entropy-coded data
};

// Everything a decode keeps between Decoder::step() calls. It sits at the front of the workspace,
// so a paused decode holds no memory of its own beyond the caller's block.
enum class Phase : uint8_t { Index, Bands, Done };
template <class C>
struct Session {
  State<C> st;
  ImageInfo info;
  Geometry g;
  Layout layout;
  IndexCursor index;
  uint8_t* workspace = nullptr;  // aligned; the Session itself is at offset 0
  BandCallback callback = nullptr;
  void* user = nullptr;
  uint16_t nextBand = 0;
  Phase phase = Phase::Index;
  Result outcome = Result::Ok;  // once Done: what every further step() reports
};
// A checkpoint is the session's bytes (Decoder::writeCheckpoint), so nothing in it may need a
// constructor or destructor to be valid.
static_assert(std::is_trivially_copyable<Session<FullCaps>>::value, "a checkpoint is the session's raw bytes");
static_assert(std::is_trivially_copyable<Session<DcCaps>>::value, "a checkpoint is the session's raw bytes");

// A 1/8 decode keeps only DC (see DcCaps); every other scale the whole file.
bool dcOnly(const uint8_t scaleShift) { return scaleShift == 3; }

// The work for one kind of session: `f(session)` with the session at `p` as what it is.
template <typename F>
decltype(auto) withSession(void* const p, const bool dc, F&& f) {
  if (dc) return f(*static_cast<Session<DcCaps>*>(p));
  return f(*static_cast<Session<FullCaps>*>(p));
}

template <class C>
Layout layoutFor(const Geometry& g) {
  const size_t blocks = static_cast<size_t>(g.bandBlockRows) * g.paddedCols;
  Layout l{};
  l.maskOffset = alignUp(sizeof(Session<C>));
  // Which coefficients are non-zero matters to AC refinements only: a DC session keeps no masks.
  l.coefOffset = alignUp(l.maskOffset + (C::kDcOnly ? 0 : blocks * sizeof(uint64_t)));
  l.pixelOffset = alignUp(l.coefOffset + blocks * g.kept * sizeof(int16_t));
  l.total = alignUp(l.pixelOffset + static_cast<size_t>(g.n) * g.paddedCols * g.n) + 8;  // + alignment slack
  return l;
}

Result inputFailure(const Source& source) { return source.ioError ? Result::IoError : Result::InvalidData; }

// ---- header pass ------------------------------------------------------------------------

template <class C>
bool tableReferenced(const State<C>& st, const uint8_t index) {
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

template <class C>
int freeTableSlot(const State<C>& st) {
  if (st.poolUsed < C::kTables) return st.poolUsed;
  for (uint8_t i = 0; i < C::kTables; ++i) {
    if (!tableReferenced(st, i)) return i;
  }
  return -1;
}

template <class C>
Result parseQuantizers(State<C>& st, uint32_t p, uint32_t end) {
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
      if (ZIGZAG[k] < C::kQuantized) st.quant[id][ZIGZAG[k]] = static_cast<uint16_t>(value);
    }
  }
  return Result::Ok;
}

template <class C>
Result parseHuffmanTables(State<C>& st, uint32_t p, const uint32_t end) {
  while (p < end) {
    if (p + 17 > end) return Result::InvalidData;
    const int descriptor = st.source.at(p++);
    if (descriptor < 0) return inputFailure(st.source);
    const uint8_t cls = static_cast<uint8_t>(descriptor) >> 4;
    const uint8_t id = static_cast<uint8_t>(descriptor) & 0x0F;
    if (cls > 1 || id >= 4) return Result::InvalidData;
    uint8_t counts[16];
    uint16_t symbolCount = 0;
    for (uint8_t& c : counts) {
      const int value = st.source.at(p++);
      if (value < 0) return inputFailure(st.source);
      c = static_cast<uint8_t>(value);
      symbolCount = static_cast<uint16_t>(symbolCount + c);
    }
    if constexpr (C::kDcOnly) {
      if (cls == 1) {  // an AC table: no scan this session decodes uses it
        if (p + symbolCount > end) return Result::InvalidData;
        p += symbolCount;
        continue;
      }
    }
    const int slot = freeTableSlot(st);
    if (slot < 0) return Result::Unsupported;
    auto& table = st.pool[slot];
    table.lookahead = nullptr;  // a reclaimed slot: any lookahead described the old table
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

template <class C>
Result parseFrame(State<C>& st, uint32_t p, const uint32_t end) {
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

// Records the scan if it touches luma (and, in a DC session, is a DC scan: an AC one is not even
// validated). `p` is the first entropy-coded byte on success.
template <class C>
Result parseScan(State<C>& st, uint32_t p, const uint32_t end) {
  if (!st.frameSeen) return Result::InvalidData;
  if constexpr (C::kDcOnly) {
    // A DC session skips every AC scan unread, so nothing in one's header may refuse the picture:
    // only Ss is read, the third byte from the end of any SOS whatever its component count says.
    constexpr uint32_t SHORTEST_SCAN_HEADER = 6;  // Ns, one component (Cs, Td/Ta), Ss, Se, Ah/Al
    if (end - p >= SHORTEST_SCAN_HEADER && st.source.at(end - 3) > 0) return Result::Ok;
  }
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
  if (st.scanCount >= C::kScans) return Result::Unsupported;

  if (scan.ss == 0) {
    if (scan.ah == 0) {
      for (int k = 0; k < n; ++k) {
        const int8_t t = st.current[0][dcIds[k]];
        if (t < 0) return Result::InvalidData;
        scan.dcTable[k] = static_cast<uint8_t>(t);
      }
    }
  } else if constexpr (!C::kDcOnly) {
    const int8_t t = st.current[1][acId];
    if (t < 0) return Result::InvalidData;
    scan.acTable = static_cast<uint8_t>(t);
    if (!st.pool[t].lookahead && st.lookaheadsUsed < C::kLookaheads) {
      st.pool[t].buildLookahead(st.lookaheads[st.lookaheadsUsed++]);
    }
  }
  scan.restartInterval = st.restartInterval;
  scan.dataStart = end;
  st.scans[st.scanCount++] = scan;
  if (scan.al == 0) st.completed |= spectralBand(scan.ss, scan.se);
  return Result::Ok;
}

// Walks the whole file once: tables, frame, and every luma scan's entry point. Entropy-coded
// data has no length field, so each scan is skipped by looking for its terminating marker.
// One pass over the file: every table, the frame and each scan's data offset. Pending when
// `budget` bytes of entropy data were skipped without reaching the end of the file; call again to
// resume. UINT32_MAX never pauses (the one-shot decode).
template <class C>
Result indexStep(State<C>& st, IndexCursor& ic, const DecodeOptions& options, const uint32_t budget) {
  Source& src = st.source;
  src.selectWhole();
  if (!ic.started) {
    if (src.at(0) != 0xFF || src.at(1) != 0xD8) return inputFailure(src);
    ic.p = 2;
    ic.started = true;
  }
  uint32_t& p = ic.p;
  const uint32_t stopAt = budget >= UINT32_MAX - p ? UINT32_MAX : p + budget;
  for (;;) {
    if (ic.inEntropy) {
      // Skip the entropy-coded segment: stuffed 0xFF00 and RSTn belong to it, anything else ends it.
      for (;;) {
        const int b = src.at(p);
        if (b < 0) break;
        if (b != 0xFF) {
          if (++p >= ic.nextPoll) {
            ic.nextPoll = p + INDEX_POLL_BYTES;
            if (options.shouldAbort && options.shouldAbort(options.abortUser)) return Result::Aborted;
          }
          if (p >= stopAt) return Result::Pending;  // still inside the segment: resume here
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
      ic.inEntropy = false;
      if (src.at(p) < 0) {
        st.endedEarly = true;
        break;
      }
      continue;
    }

    // The file may end anywhere before its EOI: decode what was indexed up to there.
    int value = src.at(p);
    if (value < 0) {
      st.endedEarly = true;
      break;
    }
    if (value != 0xFF) return Result::InvalidData;
    int marker = src.at(p + 1);
    while (marker == 0xFF) marker = src.at(++p + 1);
    if (marker < 0) {
      st.endedEarly = true;
      break;
    }
    p += 2;
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (marker == 0xD9) break;

    const int hi = src.at(p);
    const int lo = src.at(p + 1);
    const uint32_t length = static_cast<uint32_t>((hi << 8) | lo);
    if (hi >= 0 && lo >= 0 && length < 2) return Result::InvalidData;
    if (hi < 0 || lo < 0 || src.at(p + length - 1) < 0) {  // the segment is cut short
      st.endedEarly = true;
      break;
    }
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
      p = end;
      ic.inEntropy = true;
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
  // Set once a unit took padding bits for data: the scan's data ended inside it.
  bool overrun = false;

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
      if (c.markerHit) c.padBits = static_cast<uint8_t>(c.padBits + 8);
      c.bits |= byte << (24 - c.bitCount);
      c.bitCount = static_cast<uint8_t>(c.bitCount + 8);
    }
  }

  // Drops `count` bits from the front of the buffer; padding sits at its end.
  void consume(const uint8_t count) {
    if (count > c.bitCount - c.padBits) overrun = true;
    c.bits <<= count;
    c.bitCount = static_cast<uint8_t>(c.bitCount - count);
    if (c.padBits > c.bitCount) c.padBits = c.bitCount;
  }

  uint32_t bits(const uint8_t count) {
    if (count == 0) return 0;
    if (c.bitCount < count) fill();
    const uint32_t value = c.bits >> (32 - count);
    consume(count);
    return value;
  }

  bool readBit() { return bits(1) != 0; }

  template <class Table>
  int symbol(const Table& table) {
    if (c.bitCount < 16) fill();
    uint8_t length = 1;
    if (table.lookahead) {
      const uint16_t e = table.lookahead->entry[c.bits >> 24];
      if (e != 0) {
        consume(static_cast<uint8_t>(e >> 8));
        return e & 0xFF;
      }
      length = 9;  // every code of 8 bits or fewer is in the table
    }
    for (; length <= 16; ++length) {
      const uint32_t code = c.bits >> (32 - length);
      const uint32_t offset = code - table.firstCode[length];
      if (code >= table.firstCode[length] && offset < table.count[length]) {
        consume(length);
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
    c.padBits = 0;
    c.markerHit = false;
    int b = src.at(c.pos);
    while (b >= 0 && b != 0xFF) b = src.at(++c.pos);  // tolerate junk before the marker
    int marker = b < 0 ? -1 : src.at(c.pos + 1);
    while (marker == 0xFF) marker = src.at(++c.pos + 1);
    if (marker < 0) overrun = true;  // the file ends where the next interval should start
    // Another RSTn than the next one means this interval's marker is missing: the data that follows
    // belongs to a later interval, and decoding it here would misplace every block after it.
    if (marker != 0xD0 + c.nextRestart) return false;
    c.nextRestart = static_cast<uint8_t>((c.nextRestart + 1) & 7);
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

inline void setCoefficient(const State<FullCaps>& st, const BlockRef& block, const uint8_t zigzag,
                           const int32_t value) {
  const uint8_t natural = ZIGZAG[zigzag];
  *block.mask |= 1ULL << natural;
  const int8_t kept = st.keptIndex[natural];
  if (kept >= 0) block.coef[kept] = static_cast<int16_t>(std::clamp<int32_t>(value, -32768, 32767));
}

// Correction bit for a coefficient already non-zero (G.1.2.3): widen its magnitude by p1.
inline void refineCoefficient(const State<FullCaps>& st, BitReader& in, const BlockRef& block, const uint8_t natural,
                              const int32_t p1) {
  if (!in.readBit()) return;
  const int8_t kept = st.keptIndex[natural];
  if (kept < 0) return;
  int16_t& coef = block.coef[kept];
  if ((coef & p1) == 0) coef = static_cast<int16_t>(coef >= 0 ? coef + p1 : coef - p1);
}

template <class C>
bool decodeDcFirst(const State<C>& st, BitReader& in, const Scan& scan, const uint8_t scanComp, const BlockRef& block) {
  const int size = in.symbol(st.pool[scan.dcTable[scanComp]]);
  if (size < 0 || size > 11) return false;
  const uint8_t comp = scan.comp[scanComp];
  in.c.predictor[comp] = static_cast<int16_t>(
      in.c.predictor[comp] + BitReader::extend(in.bits(static_cast<uint8_t>(size)), static_cast<uint8_t>(size)));
  if (block.coef) {
    block.coef[0] = static_cast<int16_t>(std::clamp<int32_t>(in.c.predictor[comp] * (1 << scan.al), -32768, 32767));
  }
  return true;
}

void decodeDcRefine(BitReader& in, const Scan& scan, const BlockRef& block) {
  if (in.readBit() && block.coef) block.coef[0] = static_cast<int16_t>(block.coef[0] | (1 << scan.al));
}

// G.1.2.2 (libjpeg decode_mcu_AC_first)
bool decodeAcFirst(const State<FullCaps>& st, BitReader& in, const Scan& scan, const BlockRef& block) {
  Cursor& c = in.c;
  if (c.eobrun > 0) {
    --c.eobrun;
    return true;
  }
  const auto& table = st.pool[scan.acTable];
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
bool decodeAcRefine(const State<FullCaps>& st, BitReader& in, const Scan& scan, const BlockRef& block) {
  Cursor& c = in.c;
  const int32_t p1 = 1 << scan.al;
  const auto& table = st.pool[scan.acTable];
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

// After one unit of scan `s`: where its data is unusable, the scan stops (Cursor::stopUnit) and
// the decode goes on without it -- the picture degrades instead of failing. A unit that could not
// be decoded stops it there (it keeps what it read before); one that only ran past the end of the
// data is kept, as libjpeg keeps it, and the scan stops after it.
//
// An AC refinement reads a correction bit for every coefficient its predecessors made non-zero, so
// it can only follow them: once an AC scan stops at a unit, every later refinement of any of its
// coefficients -- and in turn of theirs -- stops at that unit too, or it would read its bits against
// the wrong coefficients and paint noise. A DC refinement reads one bit a block whatever came
// before, and goes on.
template <class C>
void finishUnit(State<C>& st, const uint8_t s, BitReader& in, const bool ok, const uint32_t unit) {
  if (ok && !in.overrun) return;
  in.c.stopUnit = ok ? unit + 1 : unit;
  const Scan& stopped = st.scans[s];
  if (stopped.ss == 0) return;
  uint64_t lost = spectralBand(stopped.ss, stopped.se);
  for (uint8_t later = static_cast<uint8_t>(s + 1); later < st.scanCount; ++later) {
    const Scan& scan = st.scans[later];
    const uint64_t band = spectralBand(scan.ss, scan.se);
    if (scan.ss == 0 || scan.ah == 0 || (band & lost) == 0) continue;
    lost |= band;
    st.cursors[later].stopUnit = std::min(st.cursors[later].stopUnit, unit);
  }
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
void buildBasis(State<FullCaps>& st, const uint8_t n) {
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

void idctBlock(const State<FullCaps>& st, const int16_t* coef, const uint16_t* quant, const uint8_t n, uint8_t* out,
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

// Whether the picture lacks part of the file: a scan stopped short, or the file ended before the
// scans that finish every coefficient the decode keeps (`kept`, zigzag bits). A file that lacks only
// its EOI marker is whole.
template <class C>
bool degraded(const State<C>& st, const uint64_t kept) {
  for (uint8_t s = 0; s < st.scanCount; ++s) {
    if (st.cursors[s].stopUnit != UINT32_MAX) return true;
  }
  return st.endedEarly && (st.completed & kept) != kept;
}

// Every luma scan's cursor at the start of its entropy data, before the first band.
template <class C>
void initCursors(State<C>& st) {
  for (uint8_t s = 0; s < st.scanCount; ++s) {
    Cursor& c = st.cursors[s];
    c = Cursor{};
    c.pos = st.scans[s].dataStart;
    c.unitsToRestart = st.scans[s].restartInterval;
  }
}

// One band: advance every scan across its block rows, then emit its output rows.
template <class C>
Result decodeBand(State<C>& st, const Geometry& g, uint8_t* workspace, const Layout& layout, const uint16_t band,
                  const BandCallback callback, void* user) {
  auto* masks = C::kDcOnly ? nullptr : reinterpret_cast<uint64_t*>(workspace + layout.maskOffset);
  auto* coefs = reinterpret_cast<int16_t*>(workspace + layout.coefOffset);
  uint8_t* pixels = workspace + layout.pixelOffset;
  const size_t bandBlocks = static_cast<size_t>(g.bandBlockRows) * g.paddedCols;
  const size_t pixelStride = static_cast<size_t>(g.paddedCols) * g.n;
  const uint16_t outWidth = g.outWidth;
  const uint16_t outHeight = g.outHeight;
  const uint16_t* quant = st.quant[st.comps[0].quantizer];

  auto blockAt = [&](const int bandRow, const int col) {
    const size_t index = static_cast<size_t>(bandRow) * g.paddedCols + col;
    return BlockRef{coefs + index * g.kept, masks ? masks + index : nullptr};
  };

  if (masks) memset(masks, 0, bandBlocks * sizeof(uint64_t));
  memset(coefs, 0, bandBlocks * g.kept * sizeof(int16_t));
  const int firstRow = band * g.bandBlockRows;
  for (uint8_t s = 0; s < st.scanCount; ++s) {
    const Scan& scan = st.scans[s];
    st.source.selectScan(s);
    Cursor& cursor = st.cursors[s];
    BitReader in{st.source, cursor};
    if (scan.compCount > 1) {
      // Interleaved (always a DC scan): one MCU row, chroma decoded only to stay in step.
      const uint32_t firstUnit = static_cast<uint32_t>(band) * g.mcuCols;
      for (uint16_t mx = 0; mx < g.mcuCols && firstUnit + mx < cursor.stopUnit; ++mx) {
        bool ok = beginUnit(in, scan);
        for (uint8_t k = 0; ok && k < scan.compCount; ++k) {
          const Component& comp = st.comps[scan.comp[k]];
          for (uint8_t by = 0; ok && by < comp.v; ++by) {
            for (uint8_t bx = 0; ok && bx < comp.h; ++bx) {
              const BlockRef block = scan.comp[k] == 0 ? blockAt(by, mx * st.maxH + bx) : BlockRef{};
              if (scan.ah == 0) {
                ok = decodeDcFirst(st, in, scan, k, block);
              } else {
                decodeDcRefine(in, scan, block);
              }
            }
          }
        }
        finishUnit(st, s, in, ok, firstUnit + mx);
      }
      continue;
    }
    // Non-interleaved luma: whole block rows of the component's own (unpadded) grid.
    const int lastRow = std::min<int>(firstRow + g.bandBlockRows, g.lumaRows);
    for (int row = firstRow; row < lastRow; ++row) {
      const uint32_t rowUnit = static_cast<uint32_t>(row) * g.lumaCols;
      for (uint16_t col = 0; col < g.lumaCols && rowUnit + col < cursor.stopUnit; ++col) {
        bool ok = beginUnit(in, scan);
        if (ok) {
          const BlockRef block = blockAt(row - firstRow, col);
          if (scan.ss == 0) {
            if (scan.ah == 0) {
              ok = decodeDcFirst(st, in, scan, 0, block);
            } else {
              decodeDcRefine(in, scan, block);
            }
          } else if constexpr (!C::kDcOnly) {
            ok = scan.ah == 0 ? decodeAcFirst(st, in, scan, block) : decodeAcRefine(st, in, scan, block);
          }
        }
        finishUnit(st, s, in, ok, rowUnit + col);
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
      if constexpr (C::kDcOnly) {
        fillDcBlock(block.coef[0], quant[0], g.n, dst, pixelStride);
      } else if ((*block.mask & ~1ULL) == 0) {
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
  return Result::Ok;
}

}  // namespace

Result probe(FsFile& file, ImageInfo& info, const uint32_t base) {
  info = {};
  if (!file || !file.seek(base)) return Result::InvalidData;
  auto finish = [&](const Result result) {
    file.seek(base);
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

size_t scanWindowBytes() { return Source::SLOTS_BYTES + SCAN_WINDOWS * SCAN_WINDOW_BYTES; }

bool ReadWindows::take(BuildArena* const region) {
  release();
  const size_t bytes = scanWindowBytes();
  if (region == nullptr || !region->valid() ||
      region->capacity() - region->used() < bytes + 2 * alignof(std::max_align_t)) {
    return false;
  }
  block_ = region->reserveBlock();
  data_ = static_cast<uint8_t*>(region->alloc(bytes));
  if (data_ == nullptr) {
    region->release(block_);
    return false;
  }
  region_ = region;
  return true;
}

void ReadWindows::release() {
  if (region_ != nullptr && block_.valid()) region_->release(block_);
  region_ = nullptr;
  data_ = nullptr;
}

void ReadWindows::applyTo(DecodeOptions& options) const {
  options.readWindows = data_;
  options.readWindowsSize = data_ != nullptr ? scanWindowBytes() : 0;
}

size_t workspaceBytes(const ImageInfo& info, const uint8_t scaleShift) {
  if (info.width == 0 || info.height == 0 || scaleShift > 3) return 0;
  const Geometry g =
      geometryFor(info.width, info.height, info.componentCount, info.maxHorizontal, info.maxVertical, scaleShift);
  return (dcOnly(scaleShift) ? layoutFor<DcCaps>(g) : layoutFor<FullCaps>(g)).total;
}

Result Decoder::begin(FsFile& file, const DecodeOptions& options, const BandCallback callback, void* user) {
  const Result claimed = claim(file, options, callback, user);
  if (claimed != Result::Ok) return claimed;
  if (!dcOnly_) {
    auto& ss = *static_cast<Session<FullCaps>*>(session_);
    buildBasis(ss.st, ss.g.n);
  }
  return Result::Ok;
}

Result Decoder::claim(FsFile& file, const DecodeOptions& options, const BandCallback callback, void* user) {
  end();
  if (!file || callback == nullptr || options.scaleShift > 3) return Result::InvalidData;
  ImageInfo info;
  const Result probed = probe(file, info, options.base);
  if (probed != Result::Ok) return probed;

  const Geometry g = geometryFor(info.width, info.height, info.componentCount, info.maxHorizontal, info.maxVertical,
                                 options.scaleShift, options.wholeBlocks);
  dcOnly_ = dcOnly(options.scaleShift);
  const Layout layout = dcOnly_ ? layoutFor<DcCaps>(g) : layoutFor<FullCaps>(g);

  uint8_t* base = options.workspace;
  size_t size = options.workspaceSize;
  if (!base) {
    owned_ = makeUniqueNoThrow<uint8_t[]>(layout.total);
    if (!owned_) return Result::OutOfMemory;
    base = owned_.get();
    size = layout.total;
  }
  // Align the caller's block; layoutFor() includes the slack this may consume.
  const size_t skew = (8 - (reinterpret_cast<uintptr_t>(base) & 7)) & 7;
  if (size < layout.total || size - skew < layout.total - 8) {
    owned_.reset();
    return Result::OutOfMemory;
  }
  uint8_t* workspace = base + skew;

  if (dcOnly_) {
    session_ = new (workspace) Session<DcCaps>();
  } else {
    session_ = new (workspace) Session<FullCaps>();
  }
  // The windows' bookkeeping (Source::Slot) wants 4-byte alignment.
  uint8_t* windows = nullptr;
  if (options.readWindows != nullptr) {
    const size_t pad = (4 - (reinterpret_cast<uintptr_t>(options.readWindows) & 3)) & 3;
    if (options.readWindowsSize >= pad + scanWindowBytes()) windows = options.readWindows + pad;
  }
  withSession(session_, dcOnly_, [&](auto& ss) {
    ss.info = info;
    ss.g = g;
    ss.layout = layout;
    ss.workspace = workspace;
    ss.callback = callback;
    ss.user = user;
    ss.st.source.file = &file;
    ss.st.source.base = options.base;
    ss.st.source.bind(windows);
  });
  file_ = &file;
  options_ = options;
  if (options_.stats) *options_.stats = DecodeStats{};
  return Result::Ok;
}

namespace {

template <class C>
Result stepSession(Session<C>& ss, const DecodeOptions& options, const uint16_t units) {
  if (ss.phase == Phase::Done) return ss.outcome;
  const uint32_t t0 = options.clock ? options.clock() : 0;
  Result result = Result::Pending;
  if (ss.phase == Phase::Index) {
    const uint32_t budget =
        units == UINT16_MAX ? UINT32_MAX : static_cast<uint32_t>(std::max<uint16_t>(units, 1)) * INDEX_STEP_BYTES;
    result = indexStep(ss.st, ss.index, options, budget);
    if (result == Result::Ok) {
      // The probe and the full index read the same SOF; disagreeing means the file changed shape.
      if (ss.st.width != ss.info.width || ss.st.height != ss.info.height || ss.st.compCount != ss.info.componentCount) {
        result = Result::InvalidData;
      } else {
        initCursors(ss.st);
        ss.phase = Phase::Bands;
        result = Result::Pending;
      }
    }
    if (options.stats) options.stats->indexMs += (options.clock ? options.clock() : 0) - t0;
  } else {
    const uint16_t count = std::max<uint16_t>(units, 1);
    for (uint16_t i = 0; i < count && ss.nextBand < ss.g.bandCount; ++i) {
      if (options.shouldAbort && options.shouldAbort(options.abortUser)) {
        result = Result::Aborted;
        break;
      }
      const Result band = decodeBand(ss.st, ss.g, ss.workspace, ss.layout, ss.nextBand, ss.callback, ss.user);
      if (band != Result::Ok) {
        result = band;
        break;
      }
      ++ss.nextBand;
    }
    if (result == Result::Pending && ss.nextBand >= ss.g.bandCount) {
      ss.phase = Phase::Done;
      result = degraded(ss.st, keptCoefficients(ss.g.n)) ? Result::Degraded : Result::Ok;
    }
    if (options.stats) options.stats->bandsMs += (options.clock ? options.clock() : 0) - t0;
  }
  if (options.stats) {
    options.stats->reads = ss.st.source.reads;
    options.stats->bytesRead = ss.st.source.bytesRead;
  }
  // An abort stops between index chunks or before a band, where the session is exactly the state
  // the next step would start from: it stays resumable (and checkpointable). Anything else is final.
  if (result != Result::Pending && result != Result::Aborted) {
    ss.phase = Phase::Done;
    ss.outcome = result;
  }
  return result;
}

// "PJC2": the session layout of the damage handling (Cursor::padBits, stopUnit, State::completed).
// A layout change bumps it; sessionBytes alone may not notice one.
constexpr uint32_t CHECKPOINT_MAGIC = 0x32434A50;
struct CheckpointHeader {
  uint32_t magic;
  uint32_t sessionBytes;   // sizeof(Session): another firmware's layout, or another kind, is refused, not misread
  uint64_t lookaheadBase;  // where State::lookaheads sat: each table's lookahead pointer rebases from it
  uint32_t base;
  uint8_t scaleShift;
  uint8_t reserved[3];
};

template <class C>
bool writeSession(const Session<C>& ss, const DecodeOptions& options, FsFile& out) {
  if (ss.phase == Phase::Done) return false;
  CheckpointHeader header{};
  header.magic = CHECKPOINT_MAGIC;
  header.sessionBytes = sizeof(Session<C>);
  header.lookaheadBase = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ss.st.lookaheads.data()));
  header.base = options.base;
  header.scaleShift = options.scaleShift;
  return out.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
         out.write(reinterpret_cast<const uint8_t*>(&ss), sizeof(Session<C>)) == sizeof(Session<C>);
}

// A checkpoint's session read over a freshly claimed one. False when it does not fit.
template <class C>
bool readSession(Session<C>& ss, const DecodeOptions& options, const BandCallback callback, void* user, FsFile& file,
                 FsFile& checkpoint) {
  CheckpointHeader header{};
  if (checkpoint.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != static_cast<int>(sizeof(header)) ||
      header.magic != CHECKPOINT_MAGIC || header.sessionBytes != sizeof(Session<C>) || header.base != options.base ||
      header.scaleShift != options.scaleShift) {
    return false;
  }
  // The fresh session knows the image as it is now; the checkpoint must describe the same one.
  const ImageInfo info = ss.info;
  const Geometry g = ss.g;
  const Layout layout = ss.layout;
  uint8_t* const workspace = ss.workspace;
  uint8_t* const extra = ss.st.source.extra;
  if (checkpoint.read(reinterpret_cast<uint8_t*>(&ss), sizeof(Session<C>)) != static_cast<int>(sizeof(Session<C>))) {
    return false;
  }
  if (ss.info.width != info.width || ss.info.height != info.height || ss.info.componentCount != info.componentCount ||
      ss.g.bandCount != g.bandCount || ss.g.n != g.n || ss.g.paddedCols != g.paddedCols ||
      ss.g.outWidth != g.outWidth || ss.g.outHeight != g.outHeight || ss.layout.total != layout.total ||
      ss.phase == Phase::Done || ss.nextBand > g.bandCount) {
    return false;
  }
  // Re-bind what the checkpoint cannot carry: this workspace, this file, this caller.
  ss.workspace = workspace;
  ss.callback = callback;
  ss.user = user;
  ss.st.source.file = &file;
  ss.st.source.base = options.base;
  ss.st.source.bind(extra);  // this workspace's windows (or none), empty: they refill on the first read
  ss.st.source.ioError = false;
  for (auto& table : ss.st.pool) {
    if (table.lookahead == nullptr) continue;
    const uint64_t offset = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(table.lookahead)) - header.lookaheadBase;
    if (offset % sizeof(Lookahead) != 0 || offset / sizeof(Lookahead) >= C::kLookaheads) return false;
    table.lookahead = &ss.st.lookaheads[offset / sizeof(Lookahead)];
  }
  return true;
}

}  // namespace

Result Decoder::step(const uint16_t units) {
  if (session_ == nullptr) return Result::InvalidData;
  return withSession(session_, dcOnly_, [&](auto& ss) { return stepSession(ss, options_, units); });
}

size_t Decoder::checkpointSize() const {
  return sizeof(CheckpointHeader) + (dcOnly_ ? sizeof(Session<DcCaps>) : sizeof(Session<FullCaps>));
}

bool Decoder::writeCheckpoint(FsFile& out) const {
  if (session_ == nullptr) return false;
  return withSession(session_, dcOnly_, [&](const auto& ss) { return writeSession(ss, options_, out); });
}

Result Decoder::resume(FsFile& file, const DecodeOptions& options, const BandCallback callback, void* user,
                       FsFile& checkpoint) {
  const Result claimed = claim(file, options, callback, user);
  if (claimed != Result::Ok) return claimed;
  if (!withSession(session_, dcOnly_,
                   [&](auto& ss) { return readSession(ss, options, callback, user, file, checkpoint); })) {
    end();
    return Result::InvalidData;
  }
  return Result::Ok;
}

uint16_t Decoder::bandsDone() const {
  return session_ ? withSession(session_, dcOnly_, [](const auto& ss) { return ss.nextBand; }) : 0;
}
uint16_t Decoder::bandCount() const {
  return session_ ? withSession(session_, dcOnly_, [](const auto& ss) { return ss.g.bandCount; }) : 0;
}

void Decoder::end() {
  if (session_ == nullptr) return;
  withSession(session_, dcOnly_, [](auto& ss) {
    using S = std::remove_reference_t<decltype(ss)>;
    ss.~S();
  });
  session_ = nullptr;
  owned_.reset();
  if (file_) file_->seek(options_.base);
  file_ = nullptr;
}

Result decode(FsFile& file, const DecodeOptions& options, const BandCallback callback, void* user) {
  Decoder decoder;
  Result result = decoder.begin(file, options, callback, user);
  if (result != Result::Ok) return result;
  do {
    result = decoder.step(UINT16_MAX);  // one step indexes the whole file, the next decodes every band
  } while (result == Result::Pending);
  return result;
}

namespace {

// Bilinear resampling of whole rows to the output size, a source row at a time: decodeResized()'s.
// Corner-aligned -- the first and last output pixels sit on the first and last source ones -- and
// it keeps the previous source row, so output rows between two source rows blend them.
class RowScaler {
 public:
  RowScaler(uint16_t sourceWidth, uint16_t sourceHeight, uint16_t outputWidth, uint16_t outputHeight, uint8_t* rows,
            RowCallback callback, void* callbackUser)
      : sourceWidth_(sourceWidth),
        sourceHeight_(sourceHeight),
        outputWidth_(outputWidth),
        outputHeight_(outputHeight),
        previous_(rows),
        current_(rows + outputWidth),
        blended_(rows + outputWidth * 2U),
        callback_(callback),
        callbackUser_(callbackUser) {}

  bool submit(const uint8_t* sourceRow, uint16_t sourceY) {
    resizeHorizontal(sourceRow, current_);
    while (nextOutputY_ < outputHeight_) {
      const uint32_t sourcePosition = mapCoordinate(nextOutputY_, outputHeight_, sourceHeight_);
      const uint16_t lowerY = static_cast<uint16_t>(sourcePosition >> 16);
      const uint16_t upperY = std::min<uint16_t>(lowerY + 1, sourceHeight_ - 1);
      if (upperY > sourceY) break;

      const uint8_t* lower = lowerY == sourceY ? current_ : previous_;
      const uint8_t* upper = upperY == sourceY ? current_ : previous_;
      const uint32_t fraction = sourcePosition & 0xFFFF;
      for (uint16_t x = 0; x < outputWidth_; ++x) {
        blended_[x] = interpolate(lower[x], upper[x], fraction);
      }
      if (!callback_(callbackUser_, nextOutputY_, blended_, outputWidth_)) return false;
      ++nextOutputY_;
    }
    std::swap(previous_, current_);
    submittedY_ = sourceY;
    return true;
  }

  bool finish() {
    while (nextOutputY_ < outputHeight_ && submittedY_ >= 0) {
      if (!callback_(callbackUser_, nextOutputY_, previous_, outputWidth_)) return false;
      ++nextOutputY_;
    }
    return nextOutputY_ == outputHeight_;
  }

 private:
  // 16.16 source position for one output row/column.
  //
  // The intermediate MUST be 64-bit. `*` binds tighter than `<<`, so the product is formed first
  // and then shifted; in 32 bits that wraps as soon as outputIndex * (sourceSize - 1) reaches
  // 65536 — for a 1920x2708 progressive cover scaled to 382x540 that is output row 194 of 540.
  // Past the wrap, lowerY collapses to ~0, submit()'s "have we reached this source row yet" test
  // passes for every remaining row at once, and they all get emitted from the last decoded row:
  // the top third of the image decodes correctly and the rest is that one row repeated.
  // The RESULT always fits in uint32 ((sourceSize - 1) << 16 <= 0xFFFF0000); only the
  // intermediate needs the wider type.
  static uint32_t mapCoordinate(uint16_t outputIndex, uint16_t outputSize, uint16_t sourceSize) {
    if (outputSize <= 1 || sourceSize <= 1) return 0;
    if (outputIndex + 1 == outputSize) return static_cast<uint32_t>(sourceSize - 1) << 16;
    return static_cast<uint32_t>((static_cast<uint64_t>(outputIndex) * (sourceSize - 1) << 16) / (outputSize - 1));
  }

  static uint8_t interpolate(uint8_t first, uint8_t second, uint32_t fraction) {
    return static_cast<uint8_t>((first * (0x10000U - fraction) + second * fraction) >> 16);
  }

  void resizeHorizontal(const uint8_t* source, uint8_t* output) const {
    for (uint16_t x = 0; x < outputWidth_; ++x) {
      const uint32_t sourcePosition = mapCoordinate(x, outputWidth_, sourceWidth_);
      const uint16_t left = static_cast<uint16_t>(sourcePosition >> 16);
      const uint16_t right = std::min<uint16_t>(left + 1, sourceWidth_ - 1);
      output[x] = interpolate(source[left], source[right], sourcePosition & 0xFFFF);
    }
  }

  uint16_t sourceWidth_;
  uint16_t sourceHeight_;
  uint16_t outputWidth_;
  uint16_t outputHeight_;
  uint8_t* previous_;
  uint8_t* current_;
  uint8_t* blended_;
  RowCallback callback_;
  void* callbackUser_;
  uint16_t nextOutputY_ = 0;
  int32_t submittedY_ = -1;
};

// A band of the 1/8 decode, row by row into the scaler.
bool resizeBand(void* user, const uint16_t y, const uint8_t* gray, uint16_t, const uint16_t rows,
                const uint16_t stride) {
  auto& scaler = *static_cast<RowScaler*>(user);
  for (uint16_t r = 0; r < rows; ++r) {
    if (!scaler.submit(gray + static_cast<size_t>(r) * stride, static_cast<uint16_t>(y + r))) return false;
  }
  return true;
}

}  // namespace

Result decodeResized(FsFile& file, const DecodeOptions& options, const uint16_t outputWidth,
                     const uint16_t outputHeight, const RowCallback callback, void* user) {
  if (outputWidth == 0 || outputHeight == 0 || callback == nullptr) return Result::InvalidData;
  ImageInfo info;
  const Result probed = probe(file, info, options.base);
  if (probed != Result::Ok) return probed;
  // Every block, the partial ones at the edges too (DecodeOptions::wholeBlocks): the picture maps
  // onto the box as a whole.
  const uint16_t sourceWidth = static_cast<uint16_t>((info.width + 7) / 8);
  const uint16_t sourceHeight = static_cast<uint16_t>((info.height + 7) / 8);
  // The previous and current source row at the output width, and the blended output row: the
  // resampler's whole state, outputWidth bytes each, freed on every return.
  auto rows = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(outputWidth) * 3);
  if (!rows) return Result::OutOfMemory;
  RowScaler scaler(sourceWidth, sourceHeight, outputWidth, outputHeight, rows.get(), callback, user);
  DecodeOptions eighth = options;
  eighth.scaleShift = 3;
  eighth.wholeBlocks = true;
  const Result result = decode(file, eighth, resizeBand, &scaler);
  if (result != Result::Ok && result != Result::Degraded) return result;
  return scaler.finish() ? result : Result::Stopped;
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
    case Result::Pending:
      return "pending";
    case Result::Degraded:
      return "degraded";
  }
  return "unknown";
}

}  // namespace ProgressiveJpeg
