/* THROWAWAY PROTOTYPE (TTF spike, 2026-10-13): an integer-only TrueType glyph
 * loader feeding FreeType's standalone anti-aliasing rasterizer (ftgrays.c).
 * No floating point anywhere: font units are scaled to 26.6 with integer
 * multiply/divide, compound transforms use F2Dot14. Not production code. */
#include "ittf.h"

#include <string.h>

#include "ftgrays.h"

static uint16_t u16(const ITtf* f, uint32_t o) { return o + 2 <= f->len ? (uint16_t)((f->d[o] << 8) | f->d[o + 1]) : 0; }
static int16_t i16(const ITtf* f, uint32_t o) { return (int16_t)u16(f, o); }
static uint32_t u32(const ITtf* f, uint32_t o) {
  return o + 4 <= f->len ? ((uint32_t)f->d[o] << 24) | ((uint32_t)f->d[o + 1] << 16) | ((uint32_t)f->d[o + 2] << 8) | f->d[o + 3] : 0;
}

static uint32_t table(const ITtf* f, const char* tag) {
  uint16_t n = u16(f, 4);
  for (uint16_t i = 0; i < n; ++i) {
    uint32_t r = 12 + 16u * i;
    if (r + 16 <= f->len && memcmp(f->d + r, tag, 4) == 0) return u32(f, r + 8);
  }
  return 0;
}

int ittf_init(ITtf* f, const uint8_t* d, uint32_t len) {
  memset(f, 0, sizeof *f);
  f->d = d; f->len = len;
  uint32_t head = table(f, "head"), hhea = table(f, "hhea"), maxp = table(f, "maxp");
  f->loca = table(f, "loca"); f->glyf = table(f, "glyf"); f->hmtx = table(f, "hmtx");
  uint32_t cmap = table(f, "cmap");
  if (!head || !hhea || !maxp || !f->loca || !f->glyf || !f->hmtx || !cmap) return -1;
  f->upem = u16(f, head + 18);
  f->longLoca = i16(f, head + 50);
  f->numGlyphs = u16(f, maxp + 4);
  f->numHMetrics = u16(f, hhea + 34);
  f->ascender = i16(f, hhea + 4);
  f->descender = i16(f, hhea + 6);
  /* prefer format 12 (3,10), else format 4 (3,1) */
  uint16_t n = u16(f, cmap + 2);
  for (uint16_t i = 0; i < n; ++i) {
    uint32_t r = cmap + 4 + 8u * i;
    uint16_t pid = u16(f, r), eid = u16(f, r + 2);
    uint32_t sub = cmap + u32(f, r + 4);
    uint16_t fmt = u16(f, sub);
    if (pid == 3 && eid == 10 && fmt == 12) { f->cmapSub = sub; f->cmapFmt = 12; break; }
    if (pid == 3 && eid == 1 && fmt == 4 && !f->cmapSub) { f->cmapSub = sub; f->cmapFmt = 4; }
  }
  return f->cmapSub ? 0 : -1;
}

uint32_t ittf_glyph(const ITtf* f, uint32_t cp) {
  uint32_t s = f->cmapSub;
  if (f->cmapFmt == 12) {
    uint32_t n = u32(f, s + 12), lo = 0, hi = n;
    while (lo < hi) {
      uint32_t m = (lo + hi) / 2, g = s + 16 + 12 * m;
      if (cp < u32(f, g)) hi = m; else if (cp > u32(f, g + 4)) lo = m + 1;
      else return u32(f, g + 8) + (cp - u32(f, g));
    }
    return 0;
  }
  if (cp > 0xFFFF) return 0;
  uint16_t segX2 = u16(f, s + 6);
  uint32_t ends = s + 14, starts = ends + segX2 + 2, deltas = starts + segX2, ranges = deltas + segX2;
  uint32_t lo = 0, hi = segX2 / 2;
  while (lo < hi) {
    uint32_t m = (lo + hi) / 2;
    if (cp > u16(f, ends + 2 * m)) lo = m + 1; else hi = m;
  }
  if (lo >= segX2 / 2u || cp < u16(f, starts + 2 * lo)) return 0;
  uint16_t ro = u16(f, ranges + 2 * lo);
  if (!ro) return (uint16_t)(cp + u16(f, deltas + 2 * lo));
  uint16_t g = u16(f, ranges + 2 * lo + ro + 2 * (cp - u16(f, starts + 2 * lo)));
  return g ? (uint16_t)(g + u16(f, deltas + 2 * lo)) : 0;
}

static uint32_t glyphOffset(const ITtf* f, uint32_t g, uint32_t* end) {
  if (g >= f->numGlyphs) return 0;
  uint32_t a, b;
  if (f->longLoca) { a = u32(f, f->loca + 4 * g); b = u32(f, f->loca + 4 * g + 4); }
  else { a = 2u * u16(f, f->loca + 2 * g); b = 2u * u16(f, f->loca + 2 * g + 2); }
  *end = f->glyf + b;
  return a == b ? 0 : f->glyf + a;
}

/* font units -> 26.6, rounded, integer only */
static int32_t scale(int32_t v, int32_t ppem26_6, int32_t upem) {
  int64_t p = (int64_t)v * ppem26_6;
  return (int32_t)(p >= 0 ? (p + upem / 2) / upem : -((-p + upem / 2) / upem));
}

typedef struct { int32_t xx, xy, yx, yy, dx, dy; } Xf; /* 2.14 matrix, offsets in font units */

static int loadGlyph(const ITtf* f, uint32_t g, ITtfOutline* o, Xf xf, int depth) {
  uint32_t end, p = glyphOffset(f, g, &end);
  if (!p) return 0; /* empty glyph (space) */
  int16_t nc = i16(f, p);
  if (nc >= 0) {
    if (o->numContours + nc > ITTF_MAX_CONTOURS) return -1;
    uint32_t ec = p + 10;
    uint16_t nPts = (uint16_t)(u16(f, ec + 2 * (nc - 1)) + 1);
    if (nc == 0) return 0;
    if (o->numPoints + nPts > ITTF_MAX_POINTS) return -1;
    uint16_t base = o->numPoints;
    for (int16_t c = 0; c < nc; ++c) o->contours[o->numContours + c] = (int16_t)(base + u16(f, ec + 2 * c));
    uint32_t q = ec + 2u * nc;
    q += 2 + u16(f, q); /* skip instructions */
    /* flags */
    for (uint16_t i = 0; i < nPts;) {
      uint8_t fl = q < f->len ? f->d[q++] : 0;
      o->tags[base + i++] = fl;
      if (fl & 8) { uint8_t r = q < f->len ? f->d[q++] : 0; while (r-- && i < nPts) o->tags[base + i++] = fl; }
    }
    int32_t v = 0;
    for (uint16_t i = 0; i < nPts; ++i) { /* x */
      uint8_t fl = o->tags[base + i];
      if (fl & 2) { uint8_t dv = q < f->len ? f->d[q++] : 0; v += (fl & 16) ? dv : -dv; }
      else if (!(fl & 16)) { v += i16(f, q); q += 2; }
      o->px[base + i] = v;
    }
    v = 0;
    for (uint16_t i = 0; i < nPts; ++i) { /* y */
      uint8_t fl = o->tags[base + i];
      if (fl & 4) { uint8_t dv = q < f->len ? f->d[q++] : 0; v += (fl & 32) ? dv : -dv; }
      else if (!(fl & 32)) { v += i16(f, q); q += 2; }
      o->py[base + i] = v;
    }
    for (uint16_t i = 0; i < nPts; ++i) { /* transform in font units (2.14) then keep */
      int32_t x = o->px[base + i], y = o->py[base + i];
      o->px[base + i] = ((xf.xx * x + xf.yx * y + 8192) >> 14) + xf.dx;
      o->py[base + i] = ((xf.xy * x + xf.yy * y + 8192) >> 14) + xf.dy;
      o->tags[base + i] = (o->tags[base + i] & 1) ? FT_CURVE_TAG_ON : FT_CURVE_TAG_CONIC;
    }
    o->numPoints += nPts;
    o->numContours += nc;
    return 0;
  }
  if (depth > 4) return -1;
  uint32_t q = p + 10;
  uint16_t flags;
  do {
    flags = u16(f, q); uint16_t gi = u16(f, q + 2); q += 4;
    int32_t a1, a2;
    if (flags & 1) { a1 = i16(f, q); a2 = i16(f, q + 2); q += 4; }
    else { a1 = (int8_t)f->d[q]; a2 = (int8_t)f->d[q + 1]; q += 2; }
    Xf c = {16384, 0, 0, 16384, 0, 0};
    if (flags & 8) { c.xx = c.yy = i16(f, q); q += 2; }
    else if (flags & 0x40) { c.xx = i16(f, q); c.yy = i16(f, q + 2); q += 4; }
    else if (flags & 0x80) { c.xx = i16(f, q); c.xy = i16(f, q + 2); c.yx = i16(f, q + 4); c.yy = i16(f, q + 6); q += 8; }
    if (flags & 2) { c.dx = a1; c.dy = a2; } /* ARGS_ARE_XY_VALUES; point matching unsupported in spike */
    /* compose child into parent: parent(child(p)) */
    Xf m = {(xf.xx * c.xx + xf.yx * c.xy + 8192) >> 14, (xf.xy * c.xx + xf.yy * c.xy + 8192) >> 14,
            (xf.xx * c.yx + xf.yx * c.yy + 8192) >> 14, (xf.xy * c.yx + xf.yy * c.yy + 8192) >> 14,
            ((xf.xx * c.dx + xf.yx * c.dy + 8192) >> 14) + xf.dx, ((xf.xy * c.dx + xf.yy * c.dy + 8192) >> 14) + xf.dy};
    if (loadGlyph(f, gi, o, m, depth + 1) < 0) return -1;
  } while (flags & 0x20);
  return 0;
}

int ittf_advance26_6(const ITtf* f, uint32_t g, int32_t ppem26_6) {
  uint32_t i = g < f->numHMetrics ? g : (uint32_t)f->numHMetrics - 1;
  return scale(u16(f, f->hmtx + 4 * i), ppem26_6, f->upem);
}

static void* s_raster;

/* The 2 KB build is ftgrays_build.c (ft_grays_raster); 4 and 8 KB come from ftgrays_build_4k/8k.c. */
extern const FT_Raster_Funcs ittf_grays_raster_4k;
extern const FT_Raster_Funcs ittf_grays_raster_8k;
static const FT_Raster_Funcs* s_funcs = &ft_grays_raster;

void ittf_select_pool(int kb) {
  s_funcs = kb >= 8 ? &ittf_grays_raster_8k : kb >= 4 ? &ittf_grays_raster_4k : &ft_grays_raster;
  s_raster = NULL;
}

/* Span sink for FT_RASTER_FLAG_DIRECT: packs coverage straight into the device's 2-bit,
 * MSB-first, row-continuous stream with the PC converter's thresholds. y is bottom-up. */
typedef struct { uint8_t* out; int w, h; } SpanSink;
static void packSpans(int y, int count, const FT_Span* spans, void* user) {
  SpanSink* s = (SpanSink*)user;
  const int row = s->h - 1 - y;
  if (row < 0 || row >= s->h) return;
  for (int i = 0; i < count; ++i) {
    const int n = spans[i].coverage >> 4;
    const uint8_t q = (uint8_t)(n >= 12 ? 3 : n >= 8 ? 2 : n >= 4 ? 1 : 0);
    if (!q) continue;
    for (int x = spans[i].x; x < spans[i].x + spans[i].len; ++x) {
      if (x < 0 || x >= s->w) continue;
      const int p = row * s->w + x;
      s->out[p >> 2] |= (uint8_t)(q << (6 - 2 * (p & 3)));
    }
  }
}

static int loadScaled(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, ITtfBitmap* out,
                      int32_t* xMinOut, int32_t* yMinOut);

int ittf_render_2bit(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, uint8_t* out2, uint32_t cap,
                     ITtfBitmap* bm) {
  int32_t xMin, yMin;
  int r = loadScaled(f, g, ppem26_6, o, bm, &xMin, &yMin);
  if (r <= 0) return r; /* <0 error, 0 empty glyph (bm->width == 0) */
  const uint32_t bytes = (uint32_t)(bm->width * bm->height + 3) / 4;
  if (bytes > cap) return -1;
  memset(out2, 0, bytes);
  FT_Outline ol;
  ol.n_contours = (short)o->numContours; ol.n_points = (short)o->numPoints;
  ol.points = o->vec; ol.tags = (char*)o->tags; ol.contours = o->contours; ol.flags = FT_OUTLINE_NONE;
  SpanSink sink = {out2, bm->width, bm->height};
  if (!s_raster) s_funcs->raster_new(NULL, (FT_Raster*)&s_raster);
  FT_Raster_Params params;
  memset(&params, 0, sizeof params);
  params.source = &ol;
  params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
  params.gray_spans = packSpans;
  params.user = &sink;
  params.clip_box.xMin = 0; params.clip_box.yMin = 0;
  params.clip_box.xMax = bm->width; params.clip_box.yMax = bm->height;
  if (s_funcs->raster_render((FT_Raster)s_raster, &params)) return -1;
  bm->pixels = out2;
  return 1;
}

/* Loads, scales and translates the outline so the cbox starts at (0,0). Returns 1 with a
 * non-empty box, 0 for an empty glyph, <0 on error. */
static int loadScaled(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, ITtfBitmap* out,
                      int32_t* xMinOut, int32_t* yMinOut) {
  memset(out, 0, sizeof *out);
  o->numPoints = 0; o->numContours = 0;
  Xf id = {16384, 0, 0, 16384, 0, 0};
  if (loadGlyph(f, g, o, id, 0) < 0) return -1;
  out->advance26_6 = ittf_advance26_6(f, g, ppem26_6);
  if (!o->numPoints) return 0;
  int32_t xMin = INT32_MAX, yMin = INT32_MAX, xMax = INT32_MIN, yMax = INT32_MIN;
  for (uint16_t i = 0; i < o->numPoints; ++i) {
    int32_t x = scale(o->px[i], ppem26_6, f->upem), y = scale(o->py[i], ppem26_6, f->upem);
    o->vec[i].x = x; o->vec[i].y = y;
    if (x < xMin) xMin = x; if (x > xMax) xMax = x;
    if (y < yMin) yMin = y; if (y > yMax) yMax = y;
  }
  xMin &= -64; yMin &= -64; xMax = (xMax + 63) & -64; yMax = (yMax + 63) & -64;
  int w = (xMax - xMin) >> 6, h = (yMax - yMin) >> 6;
  if (w <= 0 || h <= 0) return 0;
  for (uint16_t i = 0; i < o->numPoints; ++i) { o->vec[i].x -= xMin; o->vec[i].y -= yMin; }
  out->width = w; out->height = h; out->left = xMin >> 6; out->top = yMax >> 6;
  *xMinOut = xMin; *yMinOut = yMin;
  return 1;
}

int ittf_render(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, uint8_t* buf, uint32_t cap,
                ITtfBitmap* out) {
  memset(out, 0, sizeof *out);
  o->numPoints = 0; o->numContours = 0;
  Xf id = {16384, 0, 0, 16384, 0, 0};
  if (loadGlyph(f, g, o, id, 0) < 0) return -1;
  out->advance26_6 = ittf_advance26_6(f, g, ppem26_6);
  if (!o->numPoints) return 0;
  int32_t xMin = INT32_MAX, yMin = INT32_MAX, xMax = INT32_MIN, yMax = INT32_MIN;
  for (uint16_t i = 0; i < o->numPoints; ++i) {
    int32_t x = scale(o->px[i], ppem26_6, f->upem), y = scale(o->py[i], ppem26_6, f->upem);
    o->vec[i].x = x; o->vec[i].y = y;
    if (x < xMin) xMin = x; if (x > xMax) xMax = x;
    if (y < yMin) yMin = y; if (y > yMax) yMax = y;
  }
  xMin &= -64; yMin &= -64; xMax = (xMax + 63) & -64; yMax = (yMax + 63) & -64;
  int w = (xMax - xMin) >> 6, h = (yMax - yMin) >> 6;
  if (w <= 0 || h <= 0 || (uint32_t)(w * h) > cap) return -1;
  for (uint16_t i = 0; i < o->numPoints; ++i) { o->vec[i].x -= xMin; o->vec[i].y -= yMin; }
  FT_Outline ol;
  ol.n_contours = (short)o->numContours; ol.n_points = (short)o->numPoints;
  ol.points = o->vec; ol.tags = (char*)o->tags; ol.contours = o->contours; ol.flags = FT_OUTLINE_NONE;
  FT_Bitmap bm;
  memset(&bm, 0, sizeof bm);
  bm.rows = (unsigned)h; bm.width = (unsigned)w; bm.pitch = w; bm.buffer = buf;
  bm.num_grays = 256; bm.pixel_mode = FT_PIXEL_MODE_GRAY;
  memset(buf, 0, (size_t)w * h);
  if (!s_raster) s_funcs->raster_new(NULL, (FT_Raster*)&s_raster);
  FT_Raster_Params params;
  memset(&params, 0, sizeof params);
  params.target = &bm; params.source = &ol; params.flags = FT_RASTER_FLAG_AA;
  if (s_funcs->raster_render((FT_Raster)s_raster, &params)) return -1;
  out->pixels = buf; out->width = w; out->height = h;
  out->left = xMin >> 6; out->top = yMax >> 6;
  return 0;
}
