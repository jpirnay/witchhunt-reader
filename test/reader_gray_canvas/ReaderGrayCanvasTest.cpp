// A reader page with pictures on a panel that keeps a deeper gray buffer of its own (ReaderGrayCanvas.h): when such a
// page goes out there, the tone its pictures take, and the order the page is composed in.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "CrossPointSettings.h"
#include "ReaderGrayCanvas.h"

namespace {

using S = CrossPointSettings;
namespace RGC = ReaderGrayCanvas;

// --- When ---------------------------------------------------------------------------------------------------------

// A dual-plane panel reports four levels; the T5S3's driver reports what its waveform resolves (11 at room
// temperature), or four when it could not derive that.
TEST(ReaderGrayCanvas, OnlyAPanelWithMoreThanFourLevels) {
  EXPECT_FALSE(RGC::pageGoesOutOnCanvas(4, true, false, S::IMAGES_DISPLAY));
  EXPECT_TRUE(RGC::pageGoesOutOnCanvas(5, true, false, S::IMAGES_DISPLAY));
  EXPECT_TRUE(RGC::pageGoesOutOnCanvas(11, true, false, S::IMAGES_DISPLAY));
}

TEST(ReaderGrayCanvas, OnlyAPageWithAPictureToShow) {
  EXPECT_FALSE(RGC::pageGoesOutOnCanvas(11, false, false, S::IMAGES_DISPLAY)) << "a text page keeps its single push";
  EXPECT_FALSE(RGC::pageGoesOutOnCanvas(11, true, true, S::IMAGES_DISPLAY))
      << "pictures drawn as boxes (memory ladder, waiting for a build) have no gray to show";
}

// Display shows the gray. Placeholder keeps only in-cell graphics and Suppress none; neither is Display.
TEST(ReaderGrayCanvas, OnlyWithImagesOnDisplay) {
  EXPECT_FALSE(RGC::pageGoesOutOnCanvas(11, true, false, S::IMAGES_PLACEHOLDER));
  EXPECT_FALSE(RGC::pageGoesOutOnCanvas(11, true, false, S::IMAGES_SUPPRESS));
}

// --- The pictures' tone -------------------------------------------------------------------------------------------

// v' = 255 * (v / 255)^0.28, rounded, for every byte: the curve the user chose on the T5S3.
TEST(ReaderGrayCanvas, PictureToneIsTheChosenCurve) {
  for (int v = 0; v < 256; ++v) {
    const int expected = static_cast<int>(255.0 * std::pow(v / 255.0, 0.28) + 0.5);
    EXPECT_EQ(RGC::kPictureTone[v], expected) << "at " << v;
  }
}

TEST(ReaderGrayCanvas, PictureToneKeepsBlackAndWhiteAndOnlyBrightens) {
  EXPECT_EQ(RGC::kPictureTone[0], 0);
  EXPECT_EQ(RGC::kPictureTone[255], 255);
  for (int v = 1; v < 256; ++v) {
    EXPECT_GE(RGC::kPictureTone[v], RGC::kPictureTone[v - 1]) << "falls at " << v;
    EXPECT_GE(RGC::kPictureTone[v], v) << "darkens at " << v;
  }
  // The steps of the ramp measured on the device.
  EXPECT_EQ(RGC::kPictureTone[85], 187);
  EXPECT_EQ(RGC::kPictureTone[136], 214);
  EXPECT_EQ(RGC::kPictureTone[187], 234);
  EXPECT_EQ(RGC::kPictureTone[221], 245);
}

TEST(ReaderGrayCanvas, AntiAliasingDarkIsNoLighterThanLight) {
  EXPECT_LE(RGC::kAntiAliasingDark, RGC::kAntiAliasingLight);
  EXPECT_LT(RGC::kAntiAliasingLight, 255) << "a glyph edge never goes white";
}

// --- The order ----------------------------------------------------------------------------------------------------

// A 20 x 3 panel (3 bytes a row, the last half used), on a canvas padded to 24 bytes a row.
constexpr int kWidth = 20;
constexpr int kHeight = 3;
constexpr int kWidthBytes = 3;
constexpr uint16_t kStride = 24;
constexpr uint8_t kPadding = 0x5A;
constexpr uint8_t kPicture = 0x80;

struct Panel {
  std::vector<uint8_t> frameBuffer = std::vector<uint8_t>(kWidthBytes * kHeight, 0xFF);  // white
  std::vector<uint8_t> lsb = std::vector<uint8_t>(kWidthBytes * kHeight, 0);
  std::vector<uint8_t> msb = std::vector<uint8_t>(kWidthBytes * kHeight, 0);
  std::vector<uint8_t> canvas = std::vector<uint8_t>(kStride * kHeight, kPadding);

  static void set(std::vector<uint8_t>& plane, const int x, const int y) {
    plane[y * kWidthBytes + x / 8] |= static_cast<uint8_t>(0x80 >> (x % 8));
  }
  void black(const int x, const int y) {
    frameBuffer[y * kWidthBytes + x / 8] &= ~static_cast<uint8_t>(0x80 >> (x % 8));
  }
  // A glyph edge: black in the B/W pass, with its grey in the planes (LSB dark, MSB alone light).
  void dark(const int x, const int y) {
    black(x, y);
    set(lsb, x, y);
    set(msb, x, y);
  }
  void light(const int x, const int y) {
    black(x, y);
    set(msb, x, y);
  }
  uint8_t at(const int x, const int y) const { return canvas[y * kStride + x]; }
  RGC::PageBuffers buffers(const bool antiAliased) const {
    return {frameBuffer.data(),
            antiAliased ? lsb.data() : nullptr,
            antiAliased ? msb.data() : nullptr,
            kWidthBytes,
            kWidth,
            kHeight};
  }
};

// White, then the page's black pixels, then the greys of its glyph edges; the pictures last, over all of it.
TEST(ReaderGrayCanvas, ComposesStampThenAntiAliasingThenPictures) {
  Panel panel;
  panel.black(1, 0);  // text
  panel.dark(2, 0);
  panel.light(3, 0);
  panel.black(12, 1);  // a black dot of a picture's dither, at columns 10..14 of row 1
  panel.dark(11, 1);   // an edge under the picture: the picture wins

  int calls = 0;
  const int result = RGC::composePage(panel.canvas.data(), kStride, panel.buffers(true), [&] {
    ++calls;
    // What the pictures are painted over: the page as the B/W pass and the anti-aliasing left it.
    EXPECT_EQ(panel.at(0, 0), 0xFF);
    EXPECT_EQ(panel.at(1, 0), 0x00);
    EXPECT_EQ(panel.at(2, 0), RGC::kAntiAliasingDark);
    EXPECT_EQ(panel.at(3, 0), RGC::kAntiAliasingLight);
    EXPECT_EQ(panel.at(12, 1), 0x00);
    for (int x = 10; x < 15; ++x) panel.canvas[1 * kStride + x] = kPicture;
    return 7;
  });

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(result, 7) << "what the pictures came to is handed back";
  EXPECT_EQ(panel.at(1, 0), 0x00);
  EXPECT_EQ(panel.at(2, 0), RGC::kAntiAliasingDark);
  EXPECT_EQ(panel.at(3, 0), RGC::kAntiAliasingLight);
  for (int x = 10; x < 15; ++x) EXPECT_EQ(panel.at(x, 1), kPicture) << "the picture's box at " << x;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const bool marked = (y == 0 && x >= 1 && x <= 3) || (y == 1 && x >= 10 && x < 15);
      if (!marked) EXPECT_EQ(panel.at(x, y), 0xFF) << "white at " << x << "," << y;
    }
    for (int x = kWidth; x < kStride; ++x) EXPECT_EQ(panel.at(x, y), kPadding) << "past the panel's width";
  }
}

// A picture paintPictures() leaves alone (its gray decode failed, or it is a GIF) keeps what the B/W pass drew: its
// dither, black dots on white.
TEST(ReaderGrayCanvas, APictureLeftAloneKeepsItsDither) {
  Panel panel;
  panel.black(5, 2);
  panel.black(7, 2);
  RGC::composePage(panel.canvas.data(), kStride, panel.buffers(true), [] { return 0; });
  EXPECT_EQ(panel.at(4, 2), 0xFF);
  EXPECT_EQ(panel.at(5, 2), 0x00);
  EXPECT_EQ(panel.at(6, 2), 0xFF);
  EXPECT_EQ(panel.at(7, 2), 0x00);
}

// Without anti-aliasing there are no planes, and every glyph pixel is the black the B/W pass drew.
TEST(ReaderGrayCanvas, WithoutAntiAliasingTextIsBlack) {
  Panel panel;
  panel.dark(2, 0);
  panel.light(3, 0);
  RGC::composePage(panel.canvas.data(), kStride, panel.buffers(false), [] { return 0; });
  EXPECT_EQ(panel.at(2, 0), 0x00);
  EXPECT_EQ(panel.at(3, 0), 0x00);
}

// The darkness settings put a grey in one plane alone: LSB alone (raw 2 at darkness 0) is the dark one, as the panel's
// own single push reads it.
TEST(ReaderGrayCanvas, LsbAloneIsTheDarkGrey) {
  Panel panel;
  panel.black(4, 1);
  Panel::set(panel.lsb, 4, 1);
  RGC::composePage(panel.canvas.data(), kStride, panel.buffers(true), [] { return 0; });
  EXPECT_EQ(panel.at(4, 1), RGC::kAntiAliasingDark);
}

}  // namespace
