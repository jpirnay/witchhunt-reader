// THROWAWAY: GPOS pair kerning with the kern subtables resolved once per font and binary search in
// format-2 coverage/class tables, checked against FreeInkFont's Gpos.cpp over every printable pair.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Gpos.h"
#include "ittf.h"

namespace {
struct View {
  const uint8_t* d = nullptr;
  size_t n = 0;
  bool has(size_t o, size_t c) const { return o <= n && c <= n - o; }
  unsigned u16(size_t o) const { return has(o, 2) ? unsigned(d[o]) * 256 + d[o + 1] : 0; }
  int32_t s16(size_t o) const { return int32_t(int16_t(u16(o))); }
  uint32_t u32(size_t o) const { return has(o, 4) ? (uint32_t(u16(o)) << 16) | u16(o + 2) : 0; }
  View sub(size_t o) const { return (o != 0 && has(o, 1)) ? View{d + o, n - o} : View{}; }
};

int coverageIndex(View v, unsigned g) {
  const unsigned fmt = v.u16(0), count = v.u16(2);
  if (fmt == 1 && v.has(4, size_t(count) * 2)) {
    unsigned lo = 0, hi = count;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (v.u16(4 + m * 2) < g) lo = m + 1; else hi = m; }
    return lo < count && v.u16(4 + lo * 2) == g ? int(lo) : -1;
  }
  if (fmt == 2 && v.has(4, size_t(count) * 6)) {  // ranges sorted by start: binary search
    unsigned lo = 0, hi = count;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (v.u16(4 + m * 6 + 2) < g) lo = m + 1; else hi = m; }
    if (lo < count && v.u16(4 + lo * 6) <= g) return int(v.u16(4 + lo * 6 + 4) + g - v.u16(4 + lo * 6));
  }
  return -1;
}

unsigned glyphClass(View v, unsigned g) {
  const unsigned fmt = v.u16(0);
  if (fmt == 1) {
    const unsigned start = v.u16(2), count = v.u16(4);
    if (!v.has(6, size_t(count) * 2)) return 0;
    return (g >= start && g - start < count) ? v.u16(6 + (g - start) * 2) : 0;
  }
  if (fmt == 2) {
    const unsigned count = v.u16(2);
    if (!v.has(4, size_t(count) * 6)) return 0;
    unsigned lo = 0, hi = count;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (v.u16(4 + m * 6 + 2) < g) lo = m + 1; else hi = m; }
    if (lo < count && v.u16(4 + lo * 6) <= g) return v.u16(4 + lo * 6 + 4);
  }
  return 0;
}

unsigned recSize(unsigned f) { unsigned c = 0; for (f &= 0xFFFF; f; f >>= 1) c += f & 1; return c * 2; }

int32_t pairAdj(View st, unsigned l, unsigned r, bool& found) {
  found = false;
  const unsigned fmt = st.u16(0), vf1 = st.u16(4), vf2 = st.u16(6);
  if ((vf1 & 4) == 0) return 0;
  const unsigned len1 = recSize(vf1), len2 = recSize(vf2);
  const unsigned adv = ((vf1 & 1) ? 2 : 0) + ((vf1 & 2) ? 2 : 0);
  if (fmt == 1) {
    const int cov = coverageIndex(st.sub(st.u16(2)), l);
    const unsigned sets = st.u16(8);
    if (cov < 0 || unsigned(cov) >= sets || !st.has(10, size_t(sets) * 2)) return 0;
    View set = st.sub(st.u16(10 + unsigned(cov) * 2));
    const unsigned pc = set.u16(0);
    const size_t rs = 2 + size_t(len1) + len2;
    if (!set.has(2, size_t(pc) * rs)) return 0;
    unsigned lo = 0, hi = pc;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (set.u16(2 + m * rs) < r) lo = m + 1; else hi = m; }
    if (lo >= pc || set.u16(2 + lo * rs) != r) return 0;
    found = true;
    return set.s16(2 + lo * rs + 2 + adv);
  }
  if (fmt == 2) {
    if (coverageIndex(st.sub(st.u16(2)), l) < 0) return 0;
    const unsigned c1n = st.u16(12), c2n = st.u16(14);
    const unsigned c1 = glyphClass(st.sub(st.u16(8)), l), c2 = glyphClass(st.sub(st.u16(10)), r);
    if (c1 >= c1n || c2 >= c2n) return 0;
    const size_t rs = size_t(len1) + len2;
    if (!st.has(16, size_t(c1n) * c2n * rs)) return 0;
    found = true;
    return st.s16(16 + (size_t(c1) * c2n + c2) * rs + adv);
  }
  return 0;
}

struct FastKern {
  std::vector<View> subtables;  // resolved once, in Gpos.cpp's search order

  void init(const uint8_t* gposBytes, size_t size) {
    View gpos{gposBytes, size};
    View scripts = gpos.sub(gpos.u16(4)), features = gpos.sub(gpos.u16(6)), lookups = gpos.sub(gpos.u16(8));
    auto langSysFor = [&](const char* tag) -> View {
      const unsigned n = scripts.u16(0);
      for (unsigned i = 0; i < n; ++i)
        if (memcmp(scripts.d + 2 + i * 6, tag, 4) == 0) {
          View s = scripts.sub(scripts.u16(2 + i * 6 + 4));
          return s.has(0, 4) ? s.sub(s.u16(0)) : View{};
        }
      return View{};
    };
    View ls = langSysFor("latn");
    if (!ls.has(0, 6)) ls = langSysFor("DFLT");
    if (!ls.has(0, 6)) return;
    auto addFeature = [&](unsigned fi) {
      if (fi >= features.u16(0) || memcmp(features.d + 2 + fi * 6, "kern", 4) != 0) return;
      View f = features.sub(features.u16(2 + fi * 6 + 4));
      for (unsigned i = 0, n = f.u16(2); i < n; ++i) {
        const unsigned li = f.u16(4 + i * 2);
        if (li >= lookups.u16(0)) continue;
        View lk = lookups.sub(lookups.u16(2 + li * 2));
        const unsigned type = lk.u16(0);
        if (type != 2 && type != 9) continue;
        for (unsigned j = 0, m = lk.u16(4); j < m; ++j) {
          View st = lk.sub(lk.u16(6 + j * 2));
          if (type == 9) {
            if (st.u16(0) != 1 || st.u16(2) != 2) continue;
            st = st.sub(st.u32(4));
          }
          subtables.push_back(st);
        }
      }
    };
    if (ls.u16(2) != 0xFFFF) addFeature(ls.u16(2));
    for (unsigned i = 0, n = ls.u16(4); i < n; ++i) addFeature(ls.u16(6 + i * 2));
  }

  int32_t pair(unsigned l, unsigned r) const {
    if (!l || !r) return 0;
    for (const View& st : subtables) {
      bool found = false;
      const int32_t v = pairAdj(st, l, r, found);
      if (found) return v;
    }
    return 0;
  }
};
}  // namespace

int main(int argc, char** argv) {
  FILE* fp = fopen(argv[1], "rb");
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> d(len);
  fread(d.data(), 1, len, fp);
  fclose(fp);
  static ITtf f;
  ittf_init(&f, d.data(), (uint32_t)len);
  uint32_t gposOff = 0, gposLen = 0;
  for (unsigned i = 0, n = (d[4] << 8) | d[5]; i < n; ++i) {
    const uint8_t* r = &d[12 + 16 * i];
    if (memcmp(r, "GPOS", 4) == 0) {
      gposOff = (r[8] << 24) | (r[9] << 16) | (r[10] << 8) | r[11];
      gposLen = (r[12] << 24) | (r[13] << 16) | (r[14] << 8) | r[15];
    }
  }
  FastKern fk;
  fk.init(d.data() + gposOff, gposLen);
  std::vector<uint32_t> gids;
  for (uint32_t c = 0x21; c <= 0x7E; ++c) gids.push_back(ittf_glyph(&f, c));
  long mismatches = 0, nonzero = 0;
  for (uint32_t a : gids)
    for (uint32_t b : gids) {
      const int32_t slow = freeink::font::gpos::PairKernAdjustment(d.data() + gposOff, gposLen, a, b);
      const int32_t fast = fk.pair(a, b);
      mismatches += slow != fast;
      nonzero += slow != 0;
    }
  const int reps = 20;
  volatile int32_t sink = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int k = 0; k < reps; ++k)
    for (uint32_t a : gids)
      for (uint32_t b : gids) sink += freeink::font::gpos::PairKernAdjustment(d.data() + gposOff, gposLen, a, b);
  auto t1 = std::chrono::steady_clock::now();
  for (int k = 0; k < reps; ++k)
    for (uint32_t a : gids)
      for (uint32_t b : gids) sink += fk.pair(a, b);
  auto t2 = std::chrono::steady_clock::now();
  const double pairs = double(reps) * gids.size() * gids.size();
  printf("%s: %zu subtables, %ld/%zu pairs non-zero, %ld mismatches; Gpos.cpp %.0f ns/pair, fast %.0f ns/pair\n",
         strrchr(argv[1], '/') + 1, fk.subtables.size(), nonzero, gids.size() * gids.size(), mismatches,
         std::chrono::duration<double, std::nano>(t1 - t0).count() / pairs,
         std::chrono::duration<double, std::nano>(t2 - t1).count() / pairs);
  return mismatches ? 1 : 0;
}
