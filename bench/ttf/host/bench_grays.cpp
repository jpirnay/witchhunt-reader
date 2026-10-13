// THROWAWAY: integer loader + ftgrays vs FtFont (FreeType, unhinted) vs libschrift,
// same px-per-em, Latin set. Reports us/glyph and pixel agreement with FreeType.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "FtFont.h"
#include "ittf.h"
#include "schrift.h"

using namespace freeink::font;

int main(int argc, char** argv) {
  if (argc < 3) return 1;
  FILE* fp = fopen(argv[1], "rb");
  fseek(fp, 0, SEEK_END); long len = ftell(fp); fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> data(len); fread(data.data(), 1, len, fp); fclose(fp);
  const int px = atoi(argv[2]);
  std::vector<uint32_t> cps;
  for (uint32_t c = 0x21; c <= 0x7E; ++c) cps.push_back(c);
  for (uint32_t c = 0xA1; c <= 0xFF; ++c) cps.push_back(c);
  constexpr int kRounds = 100;

  static ITtf f;
  if (ittf_init(&f, data.data(), static_cast<uint32_t>(len))) { printf("init failed\n"); return 1; }
  static ITtfOutline outline;
  static uint8_t buf[256 * 256];
  ITtfBitmap bm;

  auto t0 = std::chrono::steady_clock::now();
  long cov = 0;
  for (int r = 0; r < kRounds; ++r)
    for (uint32_t cp : cps)
      if (ittf_render(&f, ittf_glyph(&f, cp), px << 6, &outline, buf, sizeof buf, &bm) == 0) cov += bm.width * bm.height;
  auto t1 = std::chrono::steady_clock::now();
  const double usInt = std::chrono::duration<double, std::micro>(t1 - t0).count() / (kRounds * cps.size());

  FtFont ft;
  ft.init(data.data(), static_cast<uint32_t>(len), static_cast<uint16_t>(px));
  FtFont::RenderOptions none; none.hinting = FtFont::HintingMode::None;
  ft.setRenderOptions(none);
  t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < kRounds; ++r)
    for (uint32_t cp : cps) ft.rasterize(cp, static_cast<uint16_t>(px));
  t1 = std::chrono::steady_clock::now();
  const double usFt = std::chrono::duration<double, std::micro>(t1 - t0).count() / (kRounds * cps.size());

  SFT sft{};
  sft.font = sft_loadmem(data.data(), static_cast<size_t>(len));
  sft.xScale = sft.yScale = px; sft.flags = SFT_DOWNWARD_Y;
  std::vector<uint8_t> img(256 * 256);
  t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < kRounds; ++r)
    for (uint32_t cp : cps) {
      SFT_Glyph g; SFT_GMetrics m;
      if (sft_lookup(&sft, cp, &g) < 0 || sft_gmetrics(&sft, g, &m) < 0) continue;
      SFT_Image im{img.data(), m.minWidth, m.minHeight};
      sft_render(&sft, g, im);
    }
  t1 = std::chrono::steady_clock::now();
  const double usSft = std::chrono::duration<double, std::micro>(t1 - t0).count() / (kRounds * cps.size());
  sft_freefont(sft.font);

  // Pixel agreement with FreeType unhinted (same rasterizer, different loader/rounding).
  long same = 0, total = 0, sumDiff = 0, boxMismatch = 0, advMismatch = 0, q2diff = 0;
  auto q2 = [](int v) { int n = v >> 4; return n >= 12 ? 3 : n >= 8 ? 2 : n >= 4 ? 1 : 0; };
  for (uint32_t cp : cps) {
    if (ittf_render(&f, ittf_glyph(&f, cp), px << 6, &outline, buf, sizeof buf, &bm)) continue;
    const GlyphBitmap* g = ft.rasterize(cp, static_cast<uint16_t>(px));
    if (!g) continue;
    FtFont::GlyphMetrics gm;
    ft.metrics26_6(cp, static_cast<uint32_t>(px) << 6, gm);
    if (gm.advance26_6 != bm.advance26_6) advMismatch++;
    if (g->width != bm.width || g->height != bm.height || g->xoff != bm.left || g->yoff != -bm.top) { boxMismatch++; continue; }
    for (int i = 0; i < bm.width * bm.height; ++i) {
      int d = abs(int(g->pixels[i]) - int(bm.pixels[i]));
      same += (d <= 1); sumDiff += d; total++; q2diff += q2(g->pixels[i]) != q2(bm.pixels[i]);
    }
  }
  printf("%s @%dpx-em: integer+ftgrays %.2f us | FtFont(no hint) %.2f us | libschrift %.2f us  per glyph\n",
         strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1], px, usInt, usFt, usSft);
  printf("   vs FreeType: %ld/%zu glyph boxes differ, %ld advances differ; within +-1 on %.2f%% of pixels, mean |diff| %.3f, 2-bit level differs on %.2f%%\n",
         boxMismatch, cps.size(), advMismatch, total ? 100.0 * same / total : 0.0, total ? double(sumDiff) / total : 0.0, total ? 100.0 * q2diff / total : 0.0);
  return 0;
}
