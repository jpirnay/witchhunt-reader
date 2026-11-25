// frameregion (lib/GfxRenderer/FrameRegion.h): the bytes of the frame buffer a logical rectangle covers, under
// GfxRenderer's region helpers (getRegionByteSize, copyRegionToBuffer, copyBufferToRegion), and a frame's digest.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "FrameRegion.h"
#include "TouchTransform.h"

namespace {

using frameregion::Span;

constexpr int kOrientations[] = {frameregion::Portrait, frameregion::LandscapeClockwise, frameregion::PortraitInverted,
                                 frameregion::LandscapeCounterClockwise};

bool portrait(const int o) { return o == frameregion::Portrait || o == frameregion::PortraitInverted; }

// The span is the bounding box, in rows and bytes, of every pixel of the rectangle, clipped to the panel (none when no
// pixel is on it): the four corners the renderer used to map give the same box as the two opposite ones.
TEST(FrameRegion, TheSpanBoundsEveryPixelOfTheRectangle) {
  struct R {
    int x, y, w, h;
  };
  const R rects[] = {{0, 0, 528, 30}, {0, 750, 528, 42}, {13, 17, 101, 9}, {-5, -5, 20, 20}, {500, 780, 100, 100}};
  constexpr int kWidth = 792;
  constexpr int kHeight = 528;
  for (const int o : kOrientations) {
    const int screenW = portrait(o) ? kHeight : kWidth;
    const int screenH = portrait(o) ? kWidth : kHeight;
    for (const R& r : rects) {
      SCOPED_TRACE(::testing::Message() << "orientation " << o << " rect " << r.x << "," << r.y << " " << r.w << "x"
                                        << r.h);
      int row0 = 1 << 30, row1 = -1, byte0 = 1 << 30, byte1 = -1;
      for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
          if (x < 0 || y < 0 || x >= screenW || y >= screenH) continue;
          int px = 0;
          int py = 0;
          frameregion::toPanel(o, x, y, kWidth, kHeight, &px, &py);
          row0 = std::min(row0, py);
          row1 = std::max(row1, py);
          byte0 = std::min(byte0, px / 8);
          byte1 = std::max(byte1, px / 8);
        }
      }
      Span span;
      const bool any = row1 >= 0;  // a portrait band lies off a landscape screen
      ASSERT_EQ(frameregion::spanOf(o, r.x, r.y, r.w, r.h, kWidth, kHeight, &span), any);
      if (!any) continue;
      EXPECT_EQ(span.row0, row0);
      EXPECT_EQ(span.rows, row1 - row0 + 1);
      EXPECT_EQ(span.byte0, byte0);
      EXPECT_EQ(span.rowBytes, byte1 - byte0 + 1);
    }
  }
}

TEST(FrameRegion, AnEmptyRectangleHasNoSpan) {
  Span span;
  EXPECT_FALSE(frameregion::spanOf(frameregion::Portrait, 0, 0, 528, 0, 792, 528, &span));
  EXPECT_FALSE(frameregion::spanOf(frameregion::Portrait, 0, 0, 0, 10, 792, 528, &span));
  EXPECT_FALSE(frameregion::spanOf(frameregion::LandscapeCounterClockwise, 900, 0, 10, 10, 792, 528, &span));
}

// toPanel() is the renderer's rotateCoordinates(); touchtransform::tapToLogical(), host-tested on its own, inverts it.
TEST(FrameRegion, ToPanelIsTheInverseOfTheTouchMapping) {
  for (const int o : kOrientations) {
    const int screenW = portrait(o) ? 480 : 800;
    const int screenH = portrait(o) ? 800 : 480;
    for (const auto& p : {std::pair<int, int>{0, 0}, {17, 301}, {screenW - 1, screenH - 1}}) {
      int px = 0;
      int py = 0;
      frameregion::toPanel(o, p.first, p.second, 800, 480, &px, &py);
      int x = -1;
      int y = -1;
      touchtransform::tapToLogical(o, 800, 480, (px + 0.5f) / 800.0f, (py + 0.5f) / 480.0f, x, y);
      EXPECT_EQ(x, p.first) << "orientation " << o;
      EXPECT_EQ(y, p.second) << "orientation " << o;
    }
  }
}

TEST(FrameRegion, CopyOutAndInRestoreTheSpanAlone) {
  constexpr int kStride = 99;  // the X3's 792 px
  constexpr int kRows = 528;
  std::vector<uint8_t> frame(static_cast<size_t>(kStride) * kRows);
  uint32_t seed = 7;
  for (auto& byte : frame) {
    seed = seed * 1664525u + 1013904223u;
    byte = static_cast<uint8_t>(seed >> 24);
  }
  const std::vector<uint8_t> before = frame;
  Span span;
  ASSERT_TRUE(frameregion::spanOf(frameregion::Portrait, 0, 760, 528, 32, 792, 528, &span));
  std::vector<uint8_t> kept(span.size());
  frameregion::copyOut(frame.data(), kStride, span, kept.data());
  std::fill(frame.begin(), frame.end(), 0xFF);
  frameregion::copyIn(frame.data(), kStride, span, kept.data());
  for (int row = 0; row < kRows; row++) {
    for (int b = 0; b < kStride; b++) {
      const size_t i = static_cast<size_t>(row) * kStride + b;
      const bool inSpan =
          row >= span.row0 && row < span.row0 + span.rows && b >= span.byte0 && b < span.byte0 + span.rowBytes;
      ASSERT_EQ(frame[i], inSpan ? before[i] : 0xFF) << "row " << row << " byte " << b;
    }
  }
}

TEST(FrameRegion, TheDigestTellsOneBitApart) {
  std::vector<uint8_t> frame(52273, 0xFF);  // not a whole number of words
  const uint32_t white = frameregion::digest(frame.data(), frame.size());
  EXPECT_EQ(frameregion::digest(frame.data(), frame.size()), white);
  frame[26000] ^= 0x10;
  EXPECT_NE(frameregion::digest(frame.data(), frame.size()), white);
  frame[26000] ^= 0x10;
  frame.back() ^= 0x01;
  EXPECT_NE(frameregion::digest(frame.data(), frame.size()), white);
}

// A screenshot is written in the orientation the reader is held (crosspoint-reader PR #4457): a pixel drawn at logical
// (x, y) comes back at x in logical row y, in every orientation, and nothing else in the row is dark.
TEST(FrameRegion, ALogicalRowReadsBackWhatWasDrawnThere) {
  constexpr int kWidth = 792;  // X3: 99 bytes a panel row
  constexpr int kHeight = 528;
  constexpr int kStride = kWidth / 8;
  for (const int o : kOrientations) {
    const int screenW = portrait(o) ? kHeight : kWidth;
    const int screenH = portrait(o) ? kWidth : kHeight;
    const std::pair<int, int> dark[] = {{0, 0},   {screenW - 1, 0}, {0, screenH - 1}, {screenW - 1, screenH - 1},
                                        {13, 17}, {14, 17}};
    std::vector<uint8_t> frame(static_cast<size_t>(kStride) * kHeight, 0xFF);  // a set bit is white
    for (const auto& p : dark) {
      int px = 0;
      int py = 0;
      frameregion::toPanel(o, p.first, p.second, kWidth, kHeight, &px, &py);
      frame[static_cast<size_t>(py) * kStride + px / 8] &= static_cast<uint8_t>(~(0x80 >> (px % 8)));
    }
    std::vector<uint8_t> row(static_cast<size_t>(screenW + 7) / 8);
    for (const int y : {0, 17, screenH - 1}) {
      SCOPED_TRACE(::testing::Message() << "orientation " << o << " row " << y);
      frameregion::copyLogicalRow(frame.data(), kStride, o, kWidth, kHeight, y, screenW, row.data());
      for (int x = 0; x < screenW; x++) {
        const bool isDark = ((row[x / 8] >> (7 - x % 8)) & 1) == 0;
        const bool drawn = std::find(std::begin(dark), std::end(dark), std::make_pair(x, y)) != std::end(dark);
        EXPECT_EQ(isDark, drawn) << "x " << x;
      }
    }
  }
}

// Portrait is the form the sleep images written through the screenshot writer keep (WeatherActivity's forecast,
// BmpViewerActivity's "set as sleep image"): byte for byte the rotation that writer always used, whose BMP row r
// (bottom-up) read panel pixel (panelWidth - 1 - r, panelHeight - 1 - x).
TEST(FrameRegion, APortraitRowIsTheRotationSleepImagesAreWrittenIn) {
  constexpr int kWidth = 800;
  constexpr int kHeight = 480;
  constexpr int kStride = kWidth / 8;
  std::vector<uint8_t> frame(static_cast<size_t>(kStride) * kHeight);
  uint32_t seed = 2463534242u;
  for (uint8_t& b : frame) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    b = static_cast<uint8_t>(seed);
  }
  const int screenW = kHeight;
  const int screenH = kWidth;
  std::vector<uint8_t> row(static_cast<size_t>(screenW) / 8);
  for (const int bmpRow : {0, 1, 399, screenH - 1}) {
    frameregion::copyLogicalRow(frame.data(), kStride, frameregion::Portrait, kWidth, kHeight, screenH - 1 - bmpRow,
                                screenW, row.data());
    std::vector<uint8_t> old(row.size(), 0);
    for (int x = 0; x < screenW; x++) {
      const int srcX = kWidth - 1 - bmpRow;
      const int srcY = kHeight - 1 - x;
      const uint8_t pixel = (frame[static_cast<size_t>(srcY) * kStride + srcX / 8] >> (7 - srcX % 8)) & 0x01;
      old[x / 8] |= static_cast<uint8_t>(pixel << (7 - x % 8));
    }
    EXPECT_EQ(row, old) << "BMP row " << bmpRow;
  }
}

}  // namespace
