// THROWAWAY (TTF step 0): the kerning options the user asked to have measured rather than guessed.
//
//   CachedKern   fast GPOS reader + a per-left-glyph subtable mask + a direct-mapped pair cache (RAM)
//   KernCompiler exact class tables derived from the GPOS structure at install time; CompiledKern
//                looks pairs up in them (two binary searches + a matrix read + a scale)
//
// (The third option, pairs stored lazily in the flash glyph store, lives in ttf_main.cpp because it
// needs the store.) Shared by the host check (kerncheck.cpp) and the device sketch.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fastkern.h"

namespace kernopts {

using fastkern::View;

// ---- option 2: runtime reader + caches -------------------------------------------------------

class CachedKern {
 public:
  static constexpr uint32_t kMaskSlots = 256;  // 1 KB
  static constexpr uint32_t kPairSlots = 512;  // 4 KB

  explicit CachedKern(const fastkern::FastKern& fk) : fk_(fk) { reset(); }
  void reset() {
    for (auto& m : masks_) m.gid = 0xFFFF;
    for (auto& p : pairs_) p.key = 0xFFFFFFFFu;
    hits = misses = 0;
  }
  static constexpr uint32_t ramBytes() { return kMaskSlots * 4 + kPairSlots * 8; }

  int32_t pair(uint32_t l, uint32_t r) {
    if (!l || !r) return 0;
    const uint32_t key = (l << 16) | (r & 0xFFFF);
    PairSlot& ps = pairs_[(key * 2654435761u) >> 23];
    if (ps.key == key) {
      ++hits;
      return ps.value;
    }
    ++misses;
    const uint16_t mask = maskFor(l);
    int32_t v = 0;
    for (uint32_t i = 0; mask >> i; ++i) {
      if (!((mask >> i) & 1)) continue;
      bool found = false;
      v = fastkern::pairAdj(fk_.subtables[i], l, r, found);
      if (found) break;
      v = 0;
    }
    ps.key = key;
    ps.value = static_cast<int16_t>(v);
    return v;
  }

  uint32_t hits = 0, misses = 0;

 private:
  struct MaskSlot {
    uint16_t gid;
    uint16_t mask;  // bit i: subtable i's coverage holds this left glyph (<= 16 subtables)
  };
  struct PairSlot {
    uint32_t key;
    int16_t value;
  };
  uint16_t maskFor(uint32_t l) {
    MaskSlot& m = masks_[l & (kMaskSlots - 1)];
    if (m.gid == l) return m.mask;
    uint16_t mask = 0;
    const size_t n = fk_.subtables.size() < 16 ? fk_.subtables.size() : 16;
    for (size_t i = 0; i < n; ++i) {
      const View& st = fk_.subtables[i];
      if (fastkern::coverageIndex(st.sub(st.u16(2)), l) >= 0) mask |= static_cast<uint16_t>(1u << i);
    }
    m.gid = static_cast<uint16_t>(l);
    m.mask = mask;
    return mask;
  }
  const fastkern::FastKern& fk_;
  MaskSlot masks_[kMaskSlots];
  PairSlot pairs_[kPairSlots];
};

// ---- option 1: compile at install -------------------------------------------------------------
//
// Exact, MINIMAL classes: two left glyphs share a class when their whole kerning rows (value against
// every repertoire glyph) are equal, two right glyphs when their columns are. A row is built straight
// from the subtables in search order, never pair by pair: a format-1 subtable that covers the left
// glyph settles the pairs its pair set lists, a format-2 one settles every still-open pair through
// ClassDef2. That is first-found-wins, exactly as the reference reader decides each pair. Rows are
// deduplicated by a 64-bit hash; each new left class folds its row into a per-column hash, which
// then deduplicates the right classes. A second pass over the left representatives fills the matrix.
// All-zero rows/columns are class 0 ("no kerning"). Scratch: n int16 row + n ClassDef2 classes per
// format-2 subtable + n column hashes.
//
// Output blob (written to flash and read in place):
//   [u16 L][u16 R][u16 nLeft][u16 nRight][nLeft x {u16 cp, u8 class}][nRight x {u16 cp, u8 class}]
//   [pad to 2][L x R x int16, font units]

struct CompileStats {
  uint32_t glyphs = 0, leftClasses = 0, rightClasses = 0, rowsBuilt = 0, blobBytes = 0, nonzeroCells = 0;
  bool tooManyClasses = false;
};

inline uint64_t fnv(uint64_t h, int32_t v) {
  for (int i = 0; i < 4; ++i) {
    h ^= static_cast<uint8_t>(v >> (8 * i));
    h *= 1099511628211ull;
  }
  return h;
}

namespace detail {
struct Sub {
  View st;
  unsigned fmt = 0;
  bool usable = false;  // the first value record carries an X advance (else the reference never "finds")
  unsigned len1 = 0, len2 = 0, adv = 0;
  std::vector<uint16_t> class2;  // format 2: ClassDef2 class of each repertoire glyph
};

constexpr int16_t kOpen = INT16_MIN;

// Builds glyph g's row over the repertoire into `row` (n entries).
inline void buildRow(const std::vector<Sub>& subs, const std::vector<std::pair<uint16_t, uint16_t>>& byGid,
                     unsigned g, int16_t* row, size_t n) {
  for (size_t i = 0; i < n; ++i) row[i] = kOpen;
  for (const Sub& s : subs) {
    if (!s.usable) continue;
    const View& st = s.st;
    const int cov = fastkern::coverageIndex(st.sub(st.u16(2)), g);
    if (cov < 0) continue;
    if (s.fmt == 1) {
      const unsigned sets = st.u16(8);
      if (unsigned(cov) >= sets || !st.has(10, size_t(sets) * 2)) continue;
      View set = st.sub(st.u16(10 + unsigned(cov) * 2));
      const unsigned pc = set.u16(0);
      const size_t rs = 2 + size_t(s.len1) + s.len2;
      if (!set.has(2, size_t(pc) * rs)) continue;
      for (unsigned p = 0; p < pc; ++p) {
        const uint16_t h = static_cast<uint16_t>(set.u16(2 + p * rs));
        auto it = std::lower_bound(byGid.begin(), byGid.end(), std::make_pair(h, uint16_t(0)));
        if (it == byGid.end() || it->first != h) continue;
        if (row[it->second] == kOpen) row[it->second] = static_cast<int16_t>(set.s16(2 + p * rs + 2 + s.adv));
      }
    } else if (s.fmt == 2) {
      const unsigned c1n = st.u16(12), c2n = st.u16(14);
      const unsigned c1 = fastkern::glyphClass(st.sub(st.u16(8)), g);
      const size_t rs = size_t(s.len1) + s.len2;
      if (c1 >= c1n || !st.has(16, size_t(c1n) * c2n * rs)) continue;
      for (size_t i = 0; i < n; ++i) {
        if (row[i] != kOpen) continue;
        const unsigned c2 = s.class2[i];
        if (c2 >= c2n) continue;
        row[i] = static_cast<int16_t>(st.s16(16 + (size_t(c1) * c2n + c2) * rs + s.adv));
      }
    }
  }
  for (size_t i = 0; i < n; ++i)
    if (row[i] == kOpen) row[i] = 0;
}
}  // namespace detail

// `cps`/`gids`: the repertoire, sorted by codepoint, BMP only.
inline std::vector<uint8_t> compileKern(const fastkern::FastKern& fk, const std::vector<uint16_t>& cps,
                                        const std::vector<uint16_t>& gids, CompileStats& st) {
  const size_t n = cps.size();
  st.glyphs = static_cast<uint32_t>(n);
  std::vector<detail::Sub> subs;
  for (const View& t : fk.subtables) {
    detail::Sub s;
    s.st = t;
    s.fmt = t.u16(0);
    const unsigned vf1 = t.u16(4), vf2 = t.u16(6);
    s.usable = (vf1 & 4) != 0;
    s.len1 = fastkern::recSize(vf1);
    s.len2 = fastkern::recSize(vf2);
    s.adv = ((vf1 & 1) ? 2 : 0) + ((vf1 & 2) ? 2 : 0);
    if (s.fmt == 2) {
      s.class2.resize(n);
      for (size_t i = 0; i < n; ++i) s.class2[i] = static_cast<uint16_t>(fastkern::glyphClass(t.sub(t.u16(10)), gids[i]));
    }
    subs.push_back(std::move(s));
  }
  std::vector<std::pair<uint16_t, uint16_t>> byGid(n);
  for (size_t i = 0; i < n; ++i) byGid[i] = {gids[i], static_cast<uint16_t>(i)};
  std::sort(byGid.begin(), byGid.end());

  std::vector<int16_t> row(n);
  std::vector<uint64_t> colHash(n, 1469598103934665603ull);
  std::vector<uint8_t> colAny(n, 0);
  std::unordered_map<uint64_t, uint16_t> leftIds;
  std::vector<uint16_t> leftClassOf(n, 0), leftRep;  // leftRep[k] = repertoire index of class k+1
  for (size_t a = 0; a < n; ++a) {
    detail::buildRow(subs, byGid, gids[a], row.data(), n);
    ++st.rowsBuilt;
    uint64_t h = 1469598103934665603ull;
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
      h = fnv(h, row[i]);
      any |= row[i] != 0;
    }
    if (!any) continue;
    auto it = leftIds.find(h);
    if (it == leftIds.end()) {
      it = leftIds.emplace(h, static_cast<uint16_t>(leftRep.size() + 1)).first;
      leftRep.push_back(static_cast<uint16_t>(a));
      for (size_t i = 0; i < n; ++i) {
        colHash[i] = fnv(colHash[i], row[i]);
        colAny[i] |= row[i] != 0;
      }
    }
    leftClassOf[a] = it->second;
  }
  std::unordered_map<uint64_t, uint16_t> rightIds;
  std::vector<uint16_t> rightClassOf(n, 0), rightRep;
  for (size_t i = 0; i < n; ++i) {
    if (!colAny[i]) continue;
    auto it = rightIds.find(colHash[i]);
    if (it == rightIds.end()) {
      it = rightIds.emplace(colHash[i], static_cast<uint16_t>(rightRep.size() + 1)).first;
      rightRep.push_back(static_cast<uint16_t>(i));
    }
    rightClassOf[i] = it->second;
  }
  const size_t L = leftRep.size(), R = rightRep.size();
  st.leftClasses = static_cast<uint32_t>(L);
  st.rightClasses = static_cast<uint32_t>(R);
  if (L > 255 || R > 255) st.tooManyClasses = true;

  std::vector<uint8_t> blob;
  auto put16 = [&](uint16_t v) {
    blob.push_back(static_cast<uint8_t>(v >> 8));
    blob.push_back(static_cast<uint8_t>(v));
  };
  uint16_t nLeft = 0, nRight = 0;
  for (size_t a = 0; a < n; ++a) nLeft += leftClassOf[a] != 0;
  for (size_t a = 0; a < n; ++a) nRight += rightClassOf[a] != 0;
  // One allocation of the exact size: growing by push_back would need up to twice that, contiguous.
  blob.reserve(8 + 3u * (nLeft + nRight) + 1 + 2u * L * R);
  put16(static_cast<uint16_t>(L));
  put16(static_cast<uint16_t>(R));
  put16(nLeft);
  put16(nRight);
  for (size_t a = 0; a < n; ++a)
    if (leftClassOf[a]) {
      put16(cps[a]);
      blob.push_back(static_cast<uint8_t>(leftClassOf[a]));
    }
  for (size_t a = 0; a < n; ++a)
    if (rightClassOf[a]) {
      put16(cps[a]);
      blob.push_back(static_cast<uint8_t>(rightClassOf[a]));
    }
  if (blob.size() & 1) blob.push_back(0);
  for (size_t k = 0; k < L; ++k) {
    detail::buildRow(subs, byGid, gids[leftRep[k]], row.data(), n);
    ++st.rowsBuilt;
    for (size_t j = 0; j < R; ++j) {
      const int16_t v = row[rightRep[j]];
      st.nonzeroCells += v != 0;
      put16(static_cast<uint16_t>(v));
    }
  }
  st.blobBytes = static_cast<uint32_t>(blob.size());
  return blob;
}

// Lookup over a compiled blob read in place (RAM or mapped flash). Returns font units.
class CompiledKern {
 public:
  explicit CompiledKern(const uint8_t* blob) : b_(blob) {
    L_ = u16(0);
    R_ = u16(2);
    nLeft_ = u16(4);
    nRight_ = u16(6);
    left_ = b_ + 8;
    right_ = left_ + 3u * nLeft_;
    matrix_ = right_ + 3u * nRight_;
    if ((matrix_ - b_) & 1) ++matrix_;
  }
  int32_t pair(uint32_t lcp, uint32_t rcp) const {
    const unsigned lc = classOf(left_, nLeft_, lcp);
    if (!lc) return 0;
    const unsigned rc = classOf(right_, nRight_, rcp);
    if (!rc) return 0;
    const uint8_t* p = matrix_ + 2u * ((lc - 1) * R_ + (rc - 1));
    return static_cast<int16_t>((p[0] << 8) | p[1]);
  }

 private:
  unsigned u16(size_t o) const { return (b_[o] << 8) | b_[o + 1]; }
  static unsigned classOf(const uint8_t* e, unsigned n, uint32_t cp) {
    unsigned lo = 0, hi = n;
    while (lo < hi) {
      const unsigned m = (lo + hi) / 2;
      const unsigned c = (e[m * 3] << 8) | e[m * 3 + 1];
      if (c < cp) lo = m + 1; else hi = m;
    }
    return lo < n && ((e[lo * 3] << 8) | e[lo * 3 + 1]) == cp ? e[lo * 3 + 2] : 0;
  }
  const uint8_t* b_;
  unsigned L_ = 0, R_ = 0, nLeft_ = 0, nRight_ = 0;
  const uint8_t* left_ = nullptr;
  const uint8_t* right_ = nullptr;
  const uint8_t* matrix_ = nullptr;
};

// The repertoire the compiled tables cover: Latin, Greek, Cyrillic, Vietnamese, general punctuation,
// Latin ligatures. A book's text outside it falls back to "no kerning" (or the runtime reader).
inline bool inRepertoire(uint32_t cp) {
  return (cp >= 0x20 && cp <= 0x24F) || (cp >= 0x370 && cp <= 0x3FF) || (cp >= 0x400 && cp <= 0x4FF) ||
         (cp >= 0x1E00 && cp <= 0x1EFF) || (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0xFB00 && cp <= 0xFB06);
}

}  // namespace kernopts
