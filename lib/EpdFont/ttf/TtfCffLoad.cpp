// The rasterizer's CFF loader: Rasterizer::loadCff, in a translation unit of its own so that neither loader shapes the
// other's stack frames (TtfRasterizer.h).
#include <cstdint>

#include "TtfCff.h"
#include "TtfCffCharString.h"
#include "TtfFixed.h"
#include "TtfGrays.h"
#include "TtfRasterizer.h"

namespace ttf {

namespace {

bool fitsInt32(const int64_t value) { return value >= INT32_MIN && value <= INT32_MAX; }
bool inRange(const int64_t value) { return value >= -kMaxCoord && value <= kMaxCoord; }

// The outline runCharString left, in font units, to 26.6: the font matrix and offset, then `scale`. Out of line, after
// the interpreter returns: its registers widen no frame the interpreter runs under.
[[gnu::noinline]] Status scaleOutline(FT_Vector* points, const uint16_t count, const CffMatrix& matrix,
                                      const int32_t scale) {
  const bool transformed = !matrix.identity();
  if (!transformed && matrix.dx == 0 && matrix.dy == 0) {
    // The loop below without the matrix, the offset and their checks, which cannot fail here: each coordinate is the
    // 32-bit value runCharString left, scaled.
    for (uint16_t i = 0; i < count; ++i) {
      const int64_t x = mulFixWide(static_cast<int32_t>(points[i].x), scale);
      const int64_t y = mulFixWide(static_cast<int32_t>(points[i].y), scale);
      if (!inRange(x) || !inRange(y)) return Status::Unsupported;
      points[i].x = static_cast<FT_Pos>(x);
      points[i].y = static_cast<FT_Pos>(y);
    }
    return Status::Ok;
  }
  for (uint16_t i = 0; i < count; ++i) {
    // runCharString leaves integer font units, far inside 32 bits.
    const int32_t px = static_cast<int32_t>(points[i].x);
    const int32_t py = static_cast<int32_t>(points[i].y);
    int64_t x = px;
    int64_t y = py;
    if (transformed) {
      x = mulFixWide(px, matrix.xx) + mulFixWide(py, matrix.xy);
      y = mulFixWide(px, matrix.yx) + mulFixWide(py, matrix.yy);
    }
    x += matrix.dx;
    y += matrix.dy;
    if (!fitsInt32(x) || !fitsInt32(y)) return Status::Unsupported;
    x = mulFixWide(static_cast<int32_t>(x), scale);
    y = mulFixWide(static_cast<int32_t>(y), scale);
    if (!inRange(x) || !inRange(y)) return Status::Unsupported;
    points[i].x = static_cast<FT_Pos>(x);
    points[i].y = static_cast<FT_Pos>(y);
  }
  return Status::Ok;
}

}  // namespace

// A CFF glyph as cff_slot_load leaves it unhinted (FT/cff/cffgload.c:420-460, 682-731): the charstring run into the
// scratch arrays in font units; the font matrix applied when it is not identity (FT_Outline_Transform, each point by
// FT_Vector_Transform); the font offset added (FT_Outline_Translate); then each point scaled to 26.6 with FT_MulFix,
// by `scale` times the Top DICT's units per em over the font DICT's when they differ (FT_MulDiv). The charstring draws
// from the glyph's origin, so there is no left side bearing shift.
Status Rasterizer::loadCff(FontBytes& bytes, const Face& face, const uint16_t glyph, int32_t scale, uint16_t* numPoints,
                           uint16_t* numContours) {
  *numPoints = 0;
  *numContours = 0;
  // A block planned without CFF faces has no room for the interpreter: the plan's fault, not the glyph's.
  if (plan_.decoderBytes < kCffDecoderBytes) return Status::Transient;
  CffGlyphSource source;
  Status status = cffGlyphSource(bytes, face, glyph, &source, &cffSources_);
  if (status != Status::Ok) return status;
  auto* points = reinterpret_cast<FT_Vector*>(block_ + pointsOffset_);
  CffOutline outline{points, block_ + tagsOffset_, reinterpret_cast<unsigned short*>(block_ + contoursOffset_),
                     plan_.points, plan_.contours};
  status = runCharString(bytes, face, source, reinterpret_cast<CffDecoder*>(block_ + decoderOffset_), &outline);
  if (status != Status::Ok) return status;
  if (source.scaleNum != source.scaleDen) {
    const int64_t scaled =
        mulDivWide(scale, static_cast<int32_t>(source.scaleNum), static_cast<int32_t>(source.scaleDen));
    if (!fitsInt32(scaled)) return Status::Unsupported;
    scale = static_cast<int32_t>(scaled);
  }
  status = scaleOutline(points, outline.numPoints, source.matrix, scale);
  if (status != Status::Ok) return status;
  *numPoints = outline.numPoints;
  *numContours = outline.numContours;
  return Status::Ok;
}

}  // namespace ttf
