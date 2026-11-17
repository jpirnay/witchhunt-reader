// The Redraw pass's black-and-white page (BuildPageGray, Pass::Redraw): the frame it hands to the cleanup must be the
// frame that was pushed, byte for byte, or the controller's baseline differs from the glass in those pixels and the
// next fast refresh drives them wrong. The page is drawn again; the status bar is not, because its clock, battery and
// page count may have moved since the push: its pixels are kept (BuildPageGray::chromeBands, frameregion) and put back.
//
// The page and the status bar here are stand-ins drawn through the same logical-to-panel mapping as the renderer's
// region helpers; on the device the pass also compares the digest of the frame it drew with that of the pushed one
// (EpubReaderActivity::runBuildPageGrayRedraw) and logs "frame=same" or "frame=DIFFERS".
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "FrameRegion.h"
#include "activities/reader/BuildPageGray.h"

namespace {

using frameregion::Span;

constexpr int kOrientations[] = {frameregion::Portrait, frameregion::LandscapeClockwise, frameregion::PortraitInverted,
                                 frameregion::LandscapeCounterClockwise};

// A 1-bit panel buffer, rows of `stride` bytes, white = set bits as GfxRenderer clears it.
struct Panel {
  int width;
  int height;
  int orientation;
  std::vector<uint8_t> bits;

  Panel(const int w, const int h, const int o)
      : width(w), height(h), orientation(o), bits(static_cast<size_t>((w + 7) / 8) * h, 0xFF) {}
  int stride() const { return (width + 7) / 8; }
  bool portrait() const { return orientation == frameregion::Portrait || orientation == frameregion::PortraitInverted; }
  int screenWidth() const { return portrait() ? height : width; }
  int screenHeight() const { return portrait() ? width : height; }
  void clear(const uint8_t value) { std::fill(bits.begin(), bits.end(), value); }
  void black(const int x, const int y) {
    if (x < 0 || y < 0 || x >= screenWidth() || y >= screenHeight()) return;
    int px = 0;
    int py = 0;
    frameregion::toPanel(orientation, x, y, width, height, &px, &py);
    bits[static_cast<size_t>(py) * stride() + px / 8] &= static_cast<uint8_t>(~(0x80 >> (px % 8)));
  }
  void fill(const int x, const int y, const int w, const int h) {
    for (int j = y; j < y + h; j++)
      for (int i = x; i < x + w; i++) black(i, j);
  }
};

// A glyph stand-in: 5 x 9 pixels whose pattern follows the character, so a different value draws different pixels.
void drawText(Panel& panel, int x, const int y, const std::string& text) {
  for (const char c : text) {
    const uint32_t bits = (static_cast<uint32_t>(static_cast<unsigned char>(c)) * 2654435761u) | 1u;
    for (int i = 0; i < 45; i++) {
      if (bits & (1u << (i % 32))) panel.black(x + i % 5, y + i / 5);
    }
    x += 7;
  }
}

// The body: lines of words in the content area, the same every time it is drawn.
void drawPage(Panel& panel, const int contentTop, const int contentBottom) {
  uint32_t seed = 12345;
  for (int y = contentTop; y + 9 <= contentBottom; y += 24) {
    for (int x = 20; x + 5 <= panel.screenWidth() - 20; x += 7) {
      seed = seed * 1103515245u + 12345u;
      if ((seed >> 16) % 6 != 0) drawText(panel, x, y, std::string(1, static_cast<char>('a' + (seed >> 20) % 26)));
    }
  }
}

// What the status bar shows: the values that move between the push and the pass.
struct Bar {
  std::string clock;
  int battery;
  std::string count;
};

// The theme's layout in miniature: a progress bar against the bezel and filling it (fillMargin), the items above it.
constexpr int kBezel = 3;
constexpr int kProgress = 4;
constexpr int kItems = 22;
constexpr int kBarBottom = kProgress + kItems;  // UITheme::getStatusBarBottomHeight()
constexpr int kBarTop = kProgress;              // a progress bar alone at the top
void drawStatusBar(Panel& panel, const Bar& bar, const bool topBar) {
  const int w = panel.screenWidth();
  const int h = panel.screenHeight();
  panel.fill(0, h - kBezel - kProgress, w * bar.battery / 100, kProgress + kBezel - 1);
  const int textY = h - kBezel - kProgress - kItems + 4;
  drawText(panel, 4, textY, std::to_string(bar.battery) + "%");
  drawText(panel, 60, textY, bar.clock);
  drawText(panel, w - 4 - 7 * static_cast<int>(bar.count.size()), textY, bar.count);
  if (topBar) panel.fill(0, kBezel, w * bar.battery / 100, kProgress);
}

struct Geometry {
  int width;
  int height;
};
constexpr Geometry kX3{792, 528};
constexpr Geometry kX4{800, 480};

// The pass, as runBuildPageGrayRedraw() does it: keep the bands, render the planes over the frame, draw the page again,
// put the bands back.
struct Kept {
  Span top;
  Span bottom;
  bool hasTop = false;
  bool hasBottom = false;
  std::vector<uint8_t> bytes;
};
Kept keepBands(const Panel& panel, const BuildPageGray::ChromeBands& bands) {
  Kept kept;
  const auto& t = bands.top;
  const auto& b = bands.bottom;
  kept.hasTop =
      frameregion::spanOf(panel.orientation, t.x, t.y, t.width, t.height, panel.width, panel.height, &kept.top);
  kept.hasBottom =
      frameregion::spanOf(panel.orientation, b.x, b.y, b.width, b.height, panel.width, panel.height, &kept.bottom);
  const size_t topBytes = kept.hasTop ? kept.top.size() : 0;
  kept.bytes.resize(topBytes + (kept.hasBottom ? kept.bottom.size() : 0));
  if (kept.hasTop) frameregion::copyOut(panel.bits.data(), panel.stride(), kept.top, kept.bytes.data());
  if (kept.hasBottom)
    frameregion::copyOut(panel.bits.data(), panel.stride(), kept.bottom, kept.bytes.data() + topBytes);
  return kept;
}
void putBandsBack(Panel& panel, const Kept& kept) {
  const size_t topBytes = kept.hasTop ? kept.top.size() : 0;
  if (kept.hasTop) frameregion::copyIn(panel.bits.data(), panel.stride(), kept.top, kept.bytes.data());
  if (kept.hasBottom) frameregion::copyIn(panel.bits.data(), panel.stride(), kept.bottom, kept.bytes.data() + topBytes);
}
void renderPlanes(Panel& panel) {
  panel.clear(0x00);
  uint32_t seed = 99;
  for (auto& byte : panel.bits) {
    seed = seed * 1664525u + 1013904223u;
    byte = static_cast<uint8_t>(seed >> 24);
  }
}

BuildPageGray::ChromeBands bandsFor(const Panel& panel, const bool topBar) {
  return BuildPageGray::chromeBands(panel.screenWidth(), panel.screenHeight(), kBezel, topBar ? kBarTop : 0, kBezel,
                                    kBarBottom);
}
int contentTop(const bool topBar) { return kBezel + (topBar ? kBarTop : 0) + 6; }
int contentBottom(const Panel& panel) { return panel.screenHeight() - kBezel - kBarBottom; }

const Bar kAtPush{"12:59", 80, "1/~7"};
const Bar kAtPass{"13:00", 79, "1/~9"};  // the minute ticked, the battery stepped, the build firmed the count up

TEST(RedrawFrame, TheFrameHandedToTheCleanupIsThePushedOneThoughTheStatusBarsValuesMoved) {
  for (const Geometry g : {kX3, kX4}) {
    for (const int o : kOrientations) {
      for (const bool topBar : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << g.width << "x" << g.height << " orientation " << o << " topBar " << topBar);
        Panel panel(g.width, g.height, o);
        drawPage(panel, contentTop(topBar), contentBottom(panel));
        drawStatusBar(panel, kAtPush, topBar);
        const std::vector<uint8_t> pushed = panel.bits;
        const uint32_t pushedDigest = frameregion::digest(pushed.data(), pushed.size());

        const Kept kept = keepBands(panel, bandsFor(panel, topBar));
        renderPlanes(panel);
        ASSERT_NE(panel.bits, pushed);

        panel.clear(0xFF);
        drawPage(panel, contentTop(topBar), contentBottom(panel));
        putBandsBack(panel, kept);
        EXPECT_EQ(panel.bits, pushed);
        EXPECT_EQ(frameregion::digest(panel.bits.data(), panel.bits.size()), pushedDigest);
      }
    }
  }
}

// The control: a status bar drawn again with the values of the pass is not the frame on the glass, and the digest the
// device compares tells the two apart.
TEST(RedrawFrame, DrawingTheStatusBarAgainWouldNotGiveThePushedFrame) {
  for (const int o : kOrientations) {
    Panel panel(kX3.width, kX3.height, o);
    drawPage(panel, contentTop(false), contentBottom(panel));
    drawStatusBar(panel, kAtPush, false);
    const std::vector<uint8_t> pushed = panel.bits;

    panel.clear(0xFF);
    drawPage(panel, contentTop(false), contentBottom(panel));
    drawStatusBar(panel, kAtPass, false);
    EXPECT_NE(panel.bits, pushed);
    EXPECT_NE(frameregion::digest(panel.bits.data(), panel.bits.size()),
              frameregion::digest(pushed.data(), pushed.size()));
  }
}

TEST(RedrawFrame, TheBandsRunFromTheScreensEdgePastTheBarsInnerEdge) {
  const auto bands = BuildPageGray::chromeBands(528, 792, /*bezelTop=*/3, /*barTop=*/4, /*bezelBottom=*/2,
                                                /*barBottom=*/30);
  EXPECT_EQ(bands.top.x, 0);
  EXPECT_EQ(bands.top.y, 0);
  EXPECT_EQ(bands.top.width, 528);
  EXPECT_EQ(bands.top.height, 3 + 4 + BuildPageGray::kChromePad);
  EXPECT_EQ(bands.bottom.x, 0);
  EXPECT_EQ(bands.bottom.width, 528);
  EXPECT_EQ(bands.bottom.height, 2 + 30 + BuildPageGray::kChromePad);
  EXPECT_EQ(bands.bottom.y + bands.bottom.height, 792);
}

TEST(RedrawFrame, ASideWithoutABarKeepsNothing) {
  const auto bands = BuildPageGray::chromeBands(528, 792, 3, /*barTop=*/0, 2, /*barBottom=*/0);
  EXPECT_EQ(bands.top.height, 0);
  EXPECT_EQ(bands.bottom.height, 0);
  Span span;
  EXPECT_FALSE(frameregion::spanOf(frameregion::Portrait, 0, 0, 528, 0, 792, 528, &span));
}

TEST(RedrawFrame, ABandNeverReachesPastTheScreen) {
  const auto bands = BuildPageGray::chromeBands(100, 40, 10, 30, 10, 30);
  EXPECT_EQ(bands.top.height, 40);
  EXPECT_EQ(bands.bottom.y, 0);
  EXPECT_EQ(bands.bottom.height, 40);
}

}  // namespace
