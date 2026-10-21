// Synthetic bold's outline direction (spec 7.2): book fonts come in either one, and the growth is outwards in both.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "TtfBuilder.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

struct Drawn {
  EpdGlyph metrics{};
  uint32_t ink = 0;  // the sum of the 2-bit values
};

class TtfEmbolden : public ::testing::Test {
 protected:
  void SetUp() override {
    Font font;
    Glyph notdef;
    notdef.contours = {rect(0, 0, 64, 64)};
    std::vector<Point> clockwise = rect(100, 0, 500, 700);
    std::reverse(clockwise.begin(), clockwise.end());
    Glyph trueTypeRing;  // the outer contour clockwise, the inner counter-clockwise
    trueTypeRing.contours = {clockwise, rect(200, 100, 400, 600)};
    std::vector<Point> inner = rect(200, 100, 400, 600);
    std::reverse(inner.begin(), inner.end());
    Glyph cffRing;  // the outer contour counter-clockwise, the inner clockwise
    cffRing.contours = {rect(100, 0, 500, 700), inner};
    Glyph flat;  // two points: an area of 0, which FreeType cannot orient
    flat.contours = {{{100, 0, true}, {500, 700, true}}};
    font.glyphs = {notdef, trueTypeRing, cffRing, flat};
    data = buildFont(font);
    bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
    ppem = ppemFromPoints(40);
    strength = emboldenStrength(ppem);
    ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash)));
  }

  Drawn draw(const uint16_t glyph, const bool bold) {
    Drawn out;
    Status status = rasterizer.load(bytes, face, glyph, ppem, &out.metrics);
    if (status == Status::Ok && bold) status = rasterizer.embolden(strength, &out.metrics);
    if (status == Status::Ok) status = rasterizer.rasterize(rasterizer.output());
    EXPECT_EQ(Status::Ok, status) << "glyph " << glyph;
    const uint32_t pixels = static_cast<uint32_t>(out.metrics.width) * out.metrics.height;
    for (uint32_t p = 0; p < pixels; ++p) out.ink += (rasterizer.output()[p >> 2] >> (6 - 2 * (p & 3))) & 3;
    return out;
  }

  std::vector<uint8_t> data;
  FontBytes bytes;
  Face face;
  Rasterizer rasterizer;
  uint32_t ppem = 0;
  Strength strength;
};

}  // namespace

TEST_F(TtfEmbolden, AnOutlineOfEitherDirectionGrowsOutwards) {
  Drawn bolds[2];
  for (const uint16_t glyph : {1, 2}) {
    SCOPED_TRACE(glyph);
    const Drawn regular = draw(glyph, false);
    EXPECT_EQ(34, regular.metrics.width);
    EXPECT_EQ(59, regular.metrics.height);
    EXPECT_EQ(8, regular.metrics.left);
    EXPECT_EQ(59, regular.metrics.top);
    const Drawn bold = draw(glyph, true);
    EXPECT_EQ(34 + 4, bold.metrics.width);               // grown across by the strength (221/64 px)
    EXPECT_EQ(regular.metrics.left, bold.metrics.left);  // the left side bearing stays
    EXPECT_GT(bold.ink, regular.ink);
    bolds[glyph - 1] = bold;
  }
  // The two rings: the same box once emboldened.
  EXPECT_EQ(bolds[0].metrics.width, bolds[1].metrics.width);
  EXPECT_EQ(bolds[0].metrics.height, bolds[1].metrics.height);
  EXPECT_EQ(bolds[0].metrics.left, bolds[1].metrics.left);
  EXPECT_EQ(bolds[0].metrics.top, bolds[1].metrics.top);
}

TEST_F(TtfEmbolden, AnOutlineWithNoOrientationStaysAsItIsButItsAdvanceGrows) {
  const Drawn flat = draw(3, false);
  const Drawn flatBold = draw(3, true);
  EXPECT_EQ(flat.metrics.width, flatBold.metrics.width);
  EXPECT_EQ(flat.metrics.height, flatBold.metrics.height);
  EXPECT_EQ(flat.metrics.left, flatBold.metrics.left);
  EXPECT_EQ(flat.metrics.top, flatBold.metrics.top);
  EXPECT_EQ(flat.metrics.advanceX + (strength.x + 2) / 4, flatBold.metrics.advanceX);
}
