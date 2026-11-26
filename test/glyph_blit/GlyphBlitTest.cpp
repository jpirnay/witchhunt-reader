// The glyph blitters (GlyphBlit.h) draw a glyph's bitmap into a 1-bit plane several pixels at a time. Whatever they do
// inside, they must leave exactly the frame a pixel-at-a-time draw leaves: in every orientation, plane mask and pen
// colour, wherever the glyph sits (partly or wholly off the panel), whatever its size, and however the write band
// clips it.
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "GlyphBlit.h"

namespace {

using glyph_blit::Frame;

// Multiples of 8, as every panel's physical width is: the blitters clip at the frame's last byte, the model at its
// last pixel, and the two agree only there.
constexpr int kPanelW = 64;
constexpr int kPanelH = 48;
constexpr int kWidthBytes = kPanelW / 8;

constexpr Frame kFrames[] = {Frame::Portrait, Frame::LandscapeClockwise, Frame::PortraitInverted,
                             Frame::LandscapeCounterClockwise};

struct Glyph {
  int width;
  int height;
  std::vector<uint8_t> bytes;  // rows packed tight, as fonts store them: pixel (x, y) is pixel y * width + x
};

Glyph randomGlyph(std::mt19937& rng, const int width, const int height, const int bitsPerPixel) {
  Glyph g{width, height, {}};
  g.bytes.resize((static_cast<size_t>(width) * height * bitsPerPixel + 7) / 8);
  for (auto& b : g.bytes) b = static_cast<uint8_t>(rng());
  return g;
}

// Physical pixel of logical (lx, ly): GfxRenderer's rotateCoordinates().
void toPhysical(const Frame frame, const int lx, const int ly, int& phyX, int& phyY) {
  switch (frame) {
    case Frame::Portrait:
      phyX = ly;
      phyY = kPanelH - 1 - lx;
      break;
    case Frame::LandscapeClockwise:
      phyX = kPanelW - 1 - lx;
      phyY = kPanelH - 1 - ly;
      break;
    case Frame::PortraitInverted:
      phyX = kPanelW - 1 - ly;
      phyY = lx;
      break;
    case Frame::LandscapeCounterClockwise:
      phyX = lx;
      phyY = ly;
      break;
  }
}

// The write window: physical rows [originY, originY + rows) of the panel, held in a buffer of that many rows.
struct Band {
  int originY;
  int rows;
};

// A frame of band.rows rows of random content with a guard row either side that nothing may touch.
std::vector<uint8_t> randomFrame(const uint32_t seed, const Band band) {
  std::vector<uint8_t> frame(static_cast<size_t>(kWidthBytes) * (band.rows + 2));
  std::mt19937 rng(seed);
  for (auto& b : frame) b = static_cast<uint8_t>(rng());
  return frame;
}

// One pixel the way drawPixel() writes it, inside the band: black clears the bit, white sets it.
void modelPixel(std::vector<uint8_t>& frame, const Band band, const Frame f, const int lx, const int ly,
                const bool black) {
  int phyX = 0;
  int phyY = 0;
  toPhysical(f, lx, ly, phyX, phyY);
  if (phyX < 0 || phyX >= kPanelW) return;
  const int row = phyY - band.originY;
  if (row < 0 || row >= band.rows) return;
  uint8_t& byte = frame[static_cast<size_t>(row + 1) * kWidthBytes + (phyX >> 3)];
  const uint8_t bit = static_cast<uint8_t>(0x80u >> (phyX & 7));
  if (black) {
    byte = static_cast<uint8_t>(byte & ~bit);
  } else {
    byte = static_cast<uint8_t>(byte | bit);
  }
}

uint8_t raw2Bit(const Glyph& g, const int x, const int y) {
  const int p = y * g.width + x;
  return (g.bytes[p >> 2] >> (6 - (p & 3) * 2)) & 0x03;
}

bool bit1(const Glyph& g, const int x, const int y) {
  const int p = y * g.width + x;
  return (g.bytes[p >> 3] >> (7 - (p & 7))) & 1;
}

// A 2-bit glyph pixel is drawn where bit `raw` of the plane's mask is set, black (bit cleared) or white (bit set).
std::vector<uint8_t> modelPlane(const uint8_t drawMask, const bool state, const Frame f, const Glyph& g, const int x,
                                const int y, const Band band, const uint32_t frameSeed) {
  std::vector<uint8_t> frame = randomFrame(frameSeed, band);
  for (int gy = 0; gy < g.height; gy++) {
    for (int gx = 0; gx < g.width; gx++) {
      if ((drawMask >> raw2Bit(g, gx, gy)) & 1) modelPixel(frame, band, f, x + gx, y + gy, state);
    }
  }
  return frame;
}

// draw2Bit(): the B/W plane (0x0E) takes the pen's colour, a gray plane always sets the bit.
std::vector<uint8_t> model2Bit(const uint8_t drawMask, const Frame f, const Glyph& g, const int x, const int y,
                               const bool black, const Band band, const uint32_t frameSeed) {
  return modelPlane(drawMask, drawMask == 0x0E && black, f, g, x, y, band, frameSeed);
}

std::vector<uint8_t> blit2Bit(const uint8_t drawMask, const Frame f, const Glyph& g, const int x, const int y,
                              const bool black, const Band band, const uint32_t frameSeed) {
  std::vector<uint8_t> frame = randomFrame(frameSeed, band);
  uint8_t* const fb = frame.data() + kWidthBytes;
  const uint8_t* const bitmap = g.bytes.data();
  switch (drawMask) {
    case 0x0E:
      glyph_blit::draw2Bit<0x0E>(fb, bitmap, g.width, g.height, x, y, black, f, kPanelW, kPanelH, kWidthBytes,
                                 band.originY, band.rows);
      break;
    case 0x06:
      glyph_blit::draw2Bit<0x06>(fb, bitmap, g.width, g.height, x, y, black, f, kPanelW, kPanelH, kWidthBytes,
                                 band.originY, band.rows);
      break;
    case 0x04:
      glyph_blit::draw2Bit<0x04>(fb, bitmap, g.width, g.height, x, y, black, f, kPanelW, kPanelH, kWidthBytes,
                                 band.originY, band.rows);
      break;
    case 0x02:
      glyph_blit::draw2Bit<0x02>(fb, bitmap, g.width, g.height, x, y, black, f, kPanelW, kPanelH, kWidthBytes,
                                 band.originY, band.rows);
      break;
    default:
      ADD_FAILURE() << "no plane draws mask " << static_cast<int>(drawMask);
  }
  return frame;
}

// A 1-bit glyph has no band: it always draws into the whole frame.
std::vector<uint8_t> model1Bit(const Frame f, const Glyph& g, const int x, const int y, const bool black,
                               const uint32_t frameSeed) {
  const Band whole{0, kPanelH};
  std::vector<uint8_t> frame = randomFrame(frameSeed, whole);
  for (int gy = 0; gy < g.height; gy++) {
    for (int gx = 0; gx < g.width; gx++) {
      if (bit1(g, gx, gy)) modelPixel(frame, whole, f, x + gx, y + gy, black);
    }
  }
  return frame;
}

std::vector<uint8_t> blit1Bit(const Frame f, const Glyph& g, const int x, const int y, const bool black,
                              const uint32_t frameSeed) {
  std::vector<uint8_t> frame = randomFrame(frameSeed, Band{0, kPanelH});
  glyph_blit::draw1Bit(frame.data() + kWidthBytes, g.bytes.data(), g.width, g.height, x, y, black, f, kPanelW, kPanelH,
                       kWidthBytes);
  return frame;
}

// Pens from wholly off the panel on one side to wholly off on the other, in both axes.
int randomPen(std::mt19937& rng, const int extent) {
  return static_cast<int>(rng() % static_cast<uint32_t>(extent + 50)) - 40;
}

}  // namespace

TEST(GlyphBlit, TwoBitGlyphsLeaveThePerPixelFrame) {
  std::mt19937 rng(0x2B17u);
  constexpr uint8_t kMasks[] = {0x0E, 0x06, 0x04, 0x02};
  const Band bands[] = {{0, kPanelH}, {13, 9}, {0, 1}, {kPanelH - 8, 8}, {5, 0}};
  int drawn = 0;
  for (const Frame f : kFrames) {
    for (const uint8_t mask : kMasks) {
      for (const Band band : bands) {
        for (int i = 0; i < 60; i++) {
          const int width = 1 + static_cast<int>(rng() % 37);
          const int height = 1 + static_cast<int>(rng() % 37);
          const Glyph g = randomGlyph(rng, width, height, 2);
          const int x = randomPen(rng, kPanelW);
          const int y = randomPen(rng, kPanelW);
          const bool black = (rng() & 1) != 0;
          const uint32_t seed = rng();
          ASSERT_EQ(model2Bit(mask, f, g, x, y, black, band, seed), blit2Bit(mask, f, g, x, y, black, band, seed))
              << "frame " << static_cast<int>(f) << " mask " << static_cast<int>(mask) << " band " << band.originY
              << "+" << band.rows << " glyph " << width << "x" << height << " at " << x << "," << y << " black "
              << black;
          drawn++;
        }
      }
    }
  }
  EXPECT_EQ(4 * 4 * 5 * 60, drawn);
}

// Glyphs inside the panel, every size up to 24 x 24: each row start in the bitmap falls on every pixel of a byte.
TEST(GlyphBlit, TwoBitGlyphsOfEverySizeLeaveThePerPixelFrame) {
  std::mt19937 rng(0x51E5u);
  const Band whole{0, kPanelH};
  for (const Frame f : kFrames) {
    for (int width = 1; width <= 24; width++) {
      for (int height = 1; height <= 24; height++) {
        const Glyph g = randomGlyph(rng, width, height, 2);
        const int x = static_cast<int>(rng() % 8) + 2;
        const int y = static_cast<int>(rng() % 8) + 2;
        const uint32_t seed = rng();
        ASSERT_EQ(model2Bit(0x0E, f, g, x, y, true, whole, seed), blit2Bit(0x0E, f, g, x, y, true, whole, seed))
            << "frame " << static_cast<int>(f) << " glyph " << width << "x" << height << " at " << x << "," << y;
      }
    }
  }
}

TEST(GlyphBlit, OneBitGlyphsLeaveThePerPixelFrame) {
  std::mt19937 rng(0x1B17u);
  for (const Frame f : kFrames) {
    for (int i = 0; i < 400; i++) {
      const int width = 1 + static_cast<int>(rng() % 37);
      const int height = 1 + static_cast<int>(rng() % 37);
      const Glyph g = randomGlyph(rng, width, height, 1);
      const int x = randomPen(rng, kPanelW);
      const int y = randomPen(rng, kPanelW);
      const bool black = (rng() & 1) != 0;
      const uint32_t seed = rng();
      ASSERT_EQ(model1Bit(f, g, x, y, black, seed), blit1Bit(f, g, x, y, black, seed))
          << "frame " << static_cast<int>(f) << " glyph " << width << "x" << height << " at " << x << "," << y
          << " black " << black;
    }
  }
}

TEST(GlyphBlit, OneBitGlyphsOfEverySizeLeaveThePerPixelFrame) {
  std::mt19937 rng(0x0B5Eu);
  for (const Frame f : kFrames) {
    for (int width = 1; width <= 24; width++) {
      for (int height = 1; height <= 24; height++) {
        const Glyph g = randomGlyph(rng, width, height, 1);
        const int x = static_cast<int>(rng() % 8) + 2;
        const int y = static_cast<int>(rng() % 8) + 2;
        const uint32_t seed = rng();
        ASSERT_EQ(model1Bit(f, g, x, y, true, seed), blit1Bit(f, g, x, y, true, seed))
            << "frame " << static_cast<int>(f) << " glyph " << width << "x" << height << " at " << x << "," << y;
      }
    }
  }
}

// draw2BitPlanes(): one glyph into up to three planes at once (the frame and the two gray planes a one-pass page
// captures) must leave each plane as drawing it alone does, whatever each plane's mask, pen colour and band.
TEST(GlyphBlit, TwoBitGlyphsIntoSeveralPlanesLeaveEachPlanesFrame) {
  std::mt19937 rng(0x3B1Au);
  // The masks drawMaskFor2BitMode() gives, for every text darkness, then any mask of the three inked levels.
  constexpr uint8_t kMsb[] = {0x02, 0x06, 0x06, 0x00, 0x06};
  constexpr uint8_t kLsb[] = {0x04, 0x04, 0x06, 0x00, 0x00};
  const Band bands[] = {{0, kPanelH}, {13, 9}, {5, 0}};
  int drawn = 0;
  for (const Frame f : kFrames) {
    for (int i = 0; i < 600; i++) {
      const int width = 1 + static_cast<int>(rng() % 37);
      const int height = 1 + static_cast<int>(rng() % 37);
      const Glyph g = randomGlyph(rng, width, height, 2);
      const int x = randomPen(rng, kPanelW);
      const int y = randomPen(rng, kPanelW);
      const int darkness = i % 7;
      const uint8_t masks[3] = {
          0x0E,
          darkness < 5 ? kMsb[darkness] : static_cast<uint8_t>(rng() & 0x0E),
          darkness < 5 ? kLsb[darkness] : static_cast<uint8_t>(rng() & 0x0E),
      };
      const bool states[3] = {(rng() & 1) != 0, (rng() & 1) != 0, false};
      const Band planeBands[3] = {bands[rng() % 3], {0, kPanelH}, bands[rng() % 3]};
      const uint32_t seeds[3] = {rng(), rng(), rng()};
      const int count = 1 + static_cast<int>(rng() % 3);

      std::vector<uint8_t> frames[3];
      glyph_blit::Plane planes[3];
      for (int p = 0; p < 3; p++) {
        frames[p] = randomFrame(seeds[p], planeBands[p]);
        planes[p] = {frames[p].data() + kWidthBytes, planeBands[p].originY, planeBands[p].rows, masks[p], states[p]};
      }
      glyph_blit::draw2BitPlanes(planes, count, g.bytes.data(), width, height, x, y, f, kPanelW, kPanelH, kWidthBytes);
      for (int p = 0; p < 3; p++) {
        // A plane past `count` is not drawn at all.
        const std::vector<uint8_t> expected = p < count
                                                  ? modelPlane(masks[p], states[p], f, g, x, y, planeBands[p], seeds[p])
                                                  : randomFrame(seeds[p], planeBands[p]);
        ASSERT_EQ(expected, frames[p]) << "plane " << p << " of " << count << " mask " << static_cast<int>(masks[p])
                                       << " frame " << static_cast<int>(f) << " glyph " << width << "x" << height
                                       << " at " << x << "," << y;
      }
      drawn++;
    }
  }
  EXPECT_EQ(4 * 600, drawn);
}
