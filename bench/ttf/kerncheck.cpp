// THROWAWAY (TTF step 0): host check that the compiled class tables and the cached reader give exactly
// what the reference fast reader gives, for EVERY pair of the font's kerning repertoire.
// usage: kerncheck FONT.ttf
#include <chrono>
#include <cstdio>
#include <vector>

#include "ittf.h"
#include "kernopts.h"

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  FILE* fp = fopen(argv[1], "rb");
  if (!fp) return 2;
  fseek(fp, 0, SEEK_END);
  const long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> d(len);
  fread(d.data(), 1, len, fp);
  fclose(fp);
  static ITtf f;
  if (ittf_init(&f, d.data(), static_cast<uint32_t>(len))) return 2;
  uint32_t gposOff = 0, gposLen = 0;
  for (unsigned i = 0, n = (d[4] << 8) | d[5]; i < n; ++i) {
    const uint8_t* r = &d[12 + 16 * i];
    if (memcmp(r, "GPOS", 4) == 0) {
      gposOff = (r[8] << 24) | (r[9] << 16) | (r[10] << 8) | r[11];
      gposLen = (r[12] << 24) | (r[13] << 16) | (r[14] << 8) | r[15];
    }
  }
  fastkern::FastKern fk;
  fk.init(d.data() + gposOff, gposLen);
  std::vector<uint16_t> cps, gids;
  for (uint32_t cp = 0x20; cp <= 0xFFFF; ++cp) {
    if (!kernopts::inRepertoire(cp)) continue;
    const uint32_t g = ittf_glyph(&f, cp);
    if (g) {
      cps.push_back(static_cast<uint16_t>(cp));
      gids.push_back(static_cast<uint16_t>(g));
    }
  }
  kernopts::CompileStats st;
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<uint8_t> blob = kernopts::compileKern(fk, cps, gids, st);
  const auto t1 = std::chrono::steady_clock::now();
  const kernopts::CompiledKern ck(blob.data());
  kernopts::CachedKern cached(fk);
  long compiledMismatch = 0, cachedMismatch = 0, pairs = 0;
  for (size_t a = 0; a < cps.size(); ++a)
    for (size_t b = 0; b < cps.size(); ++b) {
      const int32_t ref = fk.pair(gids[a], gids[b]);
      compiledMismatch += ck.pair(cps[a], cps[b]) != ref;
      cachedMismatch += cached.pair(gids[a], gids[b]) != ref;
      ++pairs;
    }
  printf("%s: %zu subtables; repertoire %u glyphs; classes %u x %u%s; %u rows built (host %.0f ms); "
         "blob %u B, %u non-zero cells; %ld pairs checked: compiled %ld mismatches, cached %ld mismatches\n",
         strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1], fk.subtables.size(), st.glyphs,
         st.leftClasses, st.rightClasses, st.tooManyClasses ? " (OVER 255!)" : "", st.rowsBuilt,
         std::chrono::duration<double, std::milli>(t1 - t0).count(), st.blobBytes, st.nonzeroCells, pairs, compiledMismatch,
         cachedMismatch);
  return (compiledMismatch || cachedMismatch || st.tooManyClasses) ? 1 : 0;
}
