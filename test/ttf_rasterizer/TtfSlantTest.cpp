// Synthetic italic's shear (spec 7.4): FT_GlyphSlot_Oblique in integers. The pixels against FreeType itself are in
// TtfVsFreeTypeTest.cpp; here, the boxes of a few simple glyphs and the engine's guards.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "TtfBuilder.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

class TtfSlant : public ::testing::Test {
 protected:
  void SetUp() override {
    Font font;
    Glyph notdef;
    notdef.contours = {rect(0, 0, 64, 64)};
    Glyph aboveBaseline;
    aboveBaseline.contours = {rect(100, 0, 500, 700)};
    Glyph belowBaseline;  // reaches below the baseline
    belowBaseline.contours = {rect(100, -200, 500, 500)};
    Glyph empty;  // no contours
    font.glyphs = {notdef, aboveBaseline, belowBaseline, empty};
    data = buildFont(font);
    bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
    ppem = ppemFromPoints(40);
    ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash, true, true)));
  }

  EpdGlyph draw(const uint16_t glyph, const bool bold, const bool slant) {
    EpdGlyph metrics{};
    Status status = rasterizer.load(bytes, face, glyph, ppem, &metrics);
    if (status == Status::Ok && bold) status = rasterizer.embolden(emboldenStrength(ppem), &metrics);
    if (status == Status::Ok && slant) status = rasterizer.slant(&metrics);
    if (status == Status::Ok) status = rasterizer.rasterize(rasterizer.output());
    EXPECT_EQ(Status::Ok, status) << "glyph " << glyph;
    return metrics;
  }

  std::vector<uint8_t> data;
  FontBytes bytes;
  Face face;
  Rasterizer rasterizer;
  uint32_t ppem = 0;
};

}  // namespace

TEST_F(TtfSlant, AGlyphLeansRightAboveTheBaselineAndLeftBelowIt) {
  const EpdGlyph upright1 = draw(1, false, false);
  const EpdGlyph slanted1 = draw(1, false, true);
  // Glyph 1 upright: 34 x 59 at left 8, top 59. Its top edge, 3733/64 px up, moves 793/64 px right.
  EXPECT_EQ(47, slanted1.width);
  EXPECT_EQ(59, slanted1.height);
  EXPECT_EQ(8, slanted1.left);
  EXPECT_EQ(59, slanted1.top);
  EXPECT_EQ(upright1.advanceX, slanted1.advanceX);
  const EpdGlyph upright2 = draw(2, false, false);
  const EpdGlyph slanted2 = draw(2, false, true);
  // Glyph 2 upright: 34 x 59 at left 8, top 42. Its bottom edge, 1067/64 px down, moves 227/64 px left.
  EXPECT_EQ(47, slanted2.width);
  EXPECT_EQ(59, slanted2.height);
  EXPECT_EQ(4, slanted2.left);
  EXPECT_EQ(42, slanted2.top);
  EXPECT_EQ(upright2.advanceX, slanted2.advanceX);
}

TEST_F(TtfSlant, AnEmboldenedGlyphKeepsItsAdvanceAndLeansAsWell) {
  const EpdGlyph bold = draw(1, true, false);
  const EpdGlyph boldSlanted = draw(1, true, true);
  // The same advance, left, top and height (nothing lies below the baseline); a wider box.
  EXPECT_EQ(bold.advanceX, boldSlanted.advanceX);
  EXPECT_EQ(bold.left, boldSlanted.left);
  EXPECT_EQ(bold.top, boldSlanted.top);
  EXPECT_EQ(bold.height, boldSlanted.height);
  EXPECT_GT(boldSlanted.width, bold.width);
}

TEST_F(TtfSlant, AGlyphWithoutAnOutlineIsUnchanged) {
  const EpdGlyph upright = draw(3, false, false);
  const EpdGlyph slanted = draw(3, false, true);
  EXPECT_EQ(0, slanted.width);
  EXPECT_EQ(0, slanted.height);
  EXPECT_EQ(upright.advanceX, slanted.advanceX);
}

TEST_F(TtfSlant, AfterAFailedLoadItReturnsThatStatus) {
  EpdGlyph metrics{};
  const Status loadStatus = rasterizer.load(bytes, face, static_cast<uint16_t>(face.numGlyphs), ppem, &metrics);
  EXPECT_EQ(Status::Malformed, loadStatus);
  EXPECT_EQ(Status::Malformed, rasterizer.slant(&metrics));
}

TEST_F(TtfSlant, ABitmapThatNoLongerFitsTheOutputIsUnsupported) {
  Rasterizer tight;
  ScratchPlan plan = planScratch(face, ppem, kPoolBytesFlash);
  plan.outputBytes = glyphDataBytes(34, 59, true);
  ASSERT_TRUE(tight.allocate(plan));
  EpdGlyph metrics{};
  ASSERT_EQ(Status::Ok, tight.load(bytes, face, 1, ppem, &metrics));  // 34 x 59 fits
  EXPECT_EQ(Status::Unsupported, tight.slant(&metrics));              // 47 x 59 does not
}
