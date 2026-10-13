// THROWAWAY: integer loader + ftgrays vs FreeType over every BMP codepoint the font maps.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include "FtFont.h"
#include "ittf.h"
using namespace freeink::font;
int main(int argc, char** argv) {
  FILE* fp = fopen(argv[1], "rb"); fseek(fp, 0, SEEK_END); long len = ftell(fp); fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> d(len); fread(d.data(), 1, len, fp); fclose(fp);
  const int px = atoi(argv[2]);
  static ITtf f; ittf_init(&f, d.data(), (uint32_t)len);
  static ITtfOutline o; static uint8_t buf[512 * 512]; ITtfBitmap bm;
  FtFont ft; ft.init(d.data(), (uint32_t)len, (uint16_t)px);
  FtFont::RenderOptions none; none.hinting = FtFont::HintingMode::None; ft.setRenderOptions(none);
  long n = 0, fail = 0, box = 0, adv = 0, px2 = 0, q2d = 0, maxPts = 0;
  auto q2 = [](int v) { int k = v >> 4; return k >= 12 ? 3 : k >= 8 ? 2 : k >= 4 ? 1 : 0; };
  for (uint32_t cp = 0x20; cp <= 0xFFFF; ++cp) {
    uint32_t g = ittf_glyph(&f, cp);
    if (!g || g != ft.glyphId(cp)) { if (g != ft.glyphId(cp)) printf("cmap mismatch U+%04X\n", cp); continue; }
    n++;
    if (ittf_render(&f, g, px << 6, &o, buf, sizeof buf, &bm)) { fail++; continue; }
    if (o.numPoints > maxPts) maxPts = o.numPoints;
    const GlyphBitmap* gb = ft.rasterize(cp, (uint16_t)px);
    FtFont::GlyphMetrics gm; ft.metrics26_6(cp, (uint32_t)px << 6, gm);
    if (gm.advance26_6 != bm.advance26_6) adv++;
    if (!gb) { if (bm.width) box++; continue; }
    if (gb->width != bm.width || gb->height != bm.height || gb->xoff != bm.left || gb->yoff != -bm.top) { box++; continue; }
    for (int i = 0; i < bm.width * bm.height; ++i) { px2++; q2d += q2(gb->pixels[i]) != q2(bm.pixels[i]); }
  }
  printf("%s @%d: %ld glyphs, %ld load failures, %ld box diffs, %ld advance diffs, 2-bit differs on %.2f%% of %ld px, max points %ld\n",
         argv[1] + std::string(argv[1]).rfind('/') + 1, px, n, fail, box, adv, px2 ? 100.0 * q2d / px2 : 0.0, px2, maxPts);
}
