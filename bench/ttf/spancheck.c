/* THROWAWAY: span-mode 2-bit output must equal bitmap-mode output packed afterwards. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ittf.h"

static uint8_t q2(uint8_t v) {
  int n = v >> 4;
  return (uint8_t)(n >= 12 ? 3 : n >= 8 ? 2 : n >= 4 ? 1 : 0);
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  FILE* fp = fopen(argv[1], "rb");
  if (!fp) return 2;
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  uint8_t* d = (uint8_t*)malloc((size_t)len);
  fread(d, 1, (size_t)len, fp);
  fclose(fp);
  const int px = atoi(argv[2]);
  static ITtf f;
  static ITtfOutline o;
  static uint8_t bm8[512 * 512], packed[512 * 512 / 4], span[512 * 512 / 4];
  if (ittf_init(&f, d, (uint32_t)len)) return 2;
  long glyphs = 0, mismatched = 0;
  for (uint32_t cp = 0x20; cp <= 0xFFFF; ++cp) {
    uint32_t g = ittf_glyph(&f, cp);
    if (!g) continue;
    ITtfBitmap a, b;
    if (ittf_render(&f, g, px << 6, &o, bm8, sizeof bm8, &a)) continue;
    int r = ittf_render_2bit(&f, g, px << 6, &o, span, sizeof span, &b);
    if (r < 0) { mismatched++; continue; }
    glyphs++;
    if (a.width != b.width || a.height != b.height || a.left != b.left || a.top != b.top) { mismatched++; continue; }
    const int n = a.width * a.height;
    memset(packed, 0, (size_t)(n + 3) / 4);
    for (int p = 0; p < n; ++p) packed[p >> 2] |= (uint8_t)(q2(a.pixels[p]) << (6 - 2 * (p & 3)));
    if (memcmp(packed, span, (size_t)(n + 3) / 4) != 0) mismatched++;
    /* The pool size only changes how many bands ftgrays splits a glyph into: same bytes. */
    static uint8_t other[512 * 512 / 4];
    for (int kb = 4; kb <= 8; kb *= 2) {
      ITtfBitmap c;
      ittf_select_pool(kb);
      if (ittf_render_2bit(&f, g, px << 6, &o, other, sizeof other, &c) < 0 || c.width != b.width ||
          c.height != b.height || memcmp(other, span, (size_t)(n + 3) / 4) != 0) {
        mismatched++;
      }
      ittf_select_pool(2);
    }
  }
  printf("%s @%d: %ld glyphs compared, %ld mismatched\n", argv[1], px, glyphs, mismatched);
  return mismatched ? 1 : 0;
}
