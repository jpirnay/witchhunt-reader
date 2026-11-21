// ImageScale (lib/GfxRenderer/ImageScale.h): image scaling as exact ratios, without floating point. The float
// version it replaced could land a pixel short of a whole result; these pin the exact floor, the negative offsets
// drawBitmap's row mapping passes, and cutToBox's promise to cover a box to the last pixel and no further.

#include <gtest/gtest.h>

#include <cmath>

#include "ImageScale.h"

namespace {

TEST(ImageScale, AppliesTheExactFloor) {
  // 340 / 1020 is one third: the float product 339.99997 floored to 339; the ratio gives 340.
  const ImageScale third(340, 1020);
  EXPECT_EQ(third.apply(1020), 340);
  EXPECT_EQ(third.apply(1019), 339);
  EXPECT_EQ(third.apply(0), 0);
  for (int v = 0; v < 5000; ++v) {
    EXPECT_EQ(third.apply(v), v / 3) << v;
  }
}

TEST(ImageScale, FloorsNegativeOffsets) {
  const ImageScale half(1, 2);
  EXPECT_EQ(half.apply(-1), -1);
  EXPECT_EQ(half.apply(-2), -1);
  EXPECT_EQ(half.apply(-3), -2);
  for (int v = -3000; v < 3000; ++v) {
    EXPECT_EQ(ImageScale(792, 1001).apply(v), static_cast<int>(std::floor(v * 792.0 / 1001.0))) << v;
  }
}

TEST(ImageScale, LargeValuesTakeTheWidePath) {
  // Past 16 bits a side the product leaves 32 bits; the result stays exact.
  const ImageScale s(70000, 100000);
  EXPECT_EQ(s.apply(100000), 70000);
  EXPECT_EQ(ImageScale(65535, 65536).apply(65536), 65535);
  EXPECT_EQ(ImageScale(3, 7).apply(1 << 20), (1 << 20) * 3 / 7);
}

TEST(ImageScale, BadRatiosAreOneToOneAndZeroIsNothing) {
  EXPECT_TRUE(ImageScale(5, 0).isIdentity());
  EXPECT_TRUE(ImageScale(-5, 10).isIdentity());
  EXPECT_EQ(ImageScale(0, 10).apply(1000), 0);
  EXPECT_TRUE(ImageScale(0, 10).isDownscale());
}

TEST(ImageScale, ComparesExactly) {
  EXPECT_TRUE(ImageScale(1, 3) < ImageScale(1, 2));
  EXPECT_FALSE(ImageScale(2, 4) < ImageScale(1, 2));
  EXPECT_TRUE(ImageScale(2, 4) == ImageScale(1, 2));
  EXPECT_EQ(ImageScale::fit(1000, 500, 400, 400), ImageScale(400, 1000));
  EXPECT_EQ(ImageScale::fill(1000, 500, 400, 400), ImageScale(400, 500));
}

TEST(ImageScale, PowerOfTwoThresholdsIncludeTheBoundary) {
  EXPECT_TRUE(ImageScale(100, 800).atMost(1, 8));
  EXPECT_FALSE(ImageScale(101, 800).atMost(1, 8));
  EXPECT_TRUE(ImageScale(101, 800).atMost(1, 4));
  EXPECT_TRUE(ImageScale(1, 2).atMost(1, 2));
  EXPECT_FALSE(ImageScale(1, 1).atMost(1, 2));
}

TEST(ImageScale, ExtentIsWhereTheLastPixelLands) {
  EXPECT_EQ(ImageScale().extent(480), 480);
  EXPECT_EQ(ImageScale(1, 2).extent(1584), 792);
  EXPECT_EQ(ImageScale(1, 2).extent(0), 0);
}

// The cut rounds up and only the uncut side binds the scale: the scaled cut ends on the box's last column and
// row, for every shape, wider or taller.
TEST(ImageScale, CutToBoxCoversTheBoxExactly) {
  for (const int boxW : {340, 480, 528}) {
    for (const int boxH : {540, 792, 800}) {
      for (int srcW = boxW + 1; srcW < 3 * boxW; srcW += 37) {
        for (int srcH = boxH + 1; srcH < 3 * boxH; srcH += 53) {
          const BoxCut cut = cutToBox(srcW, srcH, boxW, boxH);
          const int shownW = cut.visibleWidth > 0 ? cut.visibleWidth : srcW;
          const int shownH = cut.visibleHeight > 0 ? cut.visibleHeight : srcH;
          ASSERT_LE(shownW, srcW);
          ASSERT_LE(shownH, srcH);
          ImageScale scale;
          if (cut.maxWidth > 0) scale = ImageScale(cut.maxWidth, shownW);
          if (cut.maxHeight > 0)
            scale = cut.maxWidth > 0 ? ImageScale::smaller(scale, ImageScale(cut.maxHeight, shownH))
                                     : ImageScale(cut.maxHeight, shownH);
          ASSERT_EQ(scale.extent(shownW), boxW) << srcW << "x" << srcH << " into " << boxW << "x" << boxH;
          ASSERT_EQ(scale.extent(shownH), boxH) << srcW << "x" << srcH << " into " << boxW << "x" << boxH;
        }
      }
    }
  }
}

TEST(ImageScale, CutToBoxLeavesTheBoxsShapeUncut) {
  const BoxCut cut = cutToBox(1056, 1584, 528, 792);
  EXPECT_EQ(cut.visibleWidth, 0);
  EXPECT_EQ(cut.visibleHeight, 0);
  EXPECT_EQ(cut.maxWidth, 528);
  EXPECT_EQ(cut.maxHeight, 792);
}

TEST(ImageScale, RoundedQuotientRoundsHalvesAwayFromZero) {
  EXPECT_EQ(roundedQuotient(5, 2), 3);
  EXPECT_EQ(roundedQuotient(4, 2), 2);
  EXPECT_EQ(roundedQuotient(-5, 2), -3);
  EXPECT_EQ(roundedQuotient(-1, 4), 0);
  for (int a = -500; a <= 500; ++a) {
    EXPECT_EQ(roundedQuotient(a, 7), static_cast<int>(std::round(a / 7.0))) << a;
  }
}

}  // namespace
