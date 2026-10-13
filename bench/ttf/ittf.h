/* THROWAWAY PROTOTYPE (TTF spike): integer TrueType loader + ftgrays. */
#pragma once
#include <limits.h>
#include <stdint.h>

#include "ftstandalone.h"
#include "ftimage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ITTF_MAX_POINTS 1024
#define ITTF_MAX_CONTOURS 128

typedef struct {
  const uint8_t* d;
  uint32_t len;
  uint32_t loca, glyf, hmtx, cmapSub;
  uint16_t upem, numGlyphs, numHMetrics, cmapFmt;
  int16_t longLoca, ascender, descender;
} ITtf;

typedef struct {
  int32_t px[ITTF_MAX_POINTS], py[ITTF_MAX_POINTS];
  FT_Vector vec[ITTF_MAX_POINTS];
  unsigned char tags[ITTF_MAX_POINTS];
  short contours[ITTF_MAX_CONTOURS];
  uint16_t numPoints, numContours;
} ITtfOutline;

typedef struct {
  const uint8_t* pixels;
  int width, height, left, top;
  int32_t advance26_6;
} ITtfBitmap;

int ittf_init(ITtf* f, const uint8_t* d, uint32_t len);
uint32_t ittf_glyph(const ITtf* f, uint32_t cp);
int ittf_advance26_6(const ITtf* f, uint32_t g, int32_t ppem26_6);
int ittf_render(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, uint8_t* buf, uint32_t cap,
                ITtfBitmap* out);

/* Renders straight into the device's 2-bit stream through the span callback.
 * Returns 1 = rendered, 0 = empty glyph (bm->width == 0), <0 = error / over cap. */
int ittf_render_2bit(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, uint8_t* out2, uint32_t cap,
                     ITtfBitmap* bm);

/* Picks the ftgrays build with a 2, 4 or 8 KB cell pool (the pool lives on the stack). */
void ittf_select_pool(int kb);

#ifdef __cplusplus
}
#endif
