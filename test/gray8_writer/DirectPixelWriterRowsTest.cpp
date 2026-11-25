// DirectPixelWriter::drawRows(): rows of 2-bit pixels drawn a framebuffer byte at a time (PackedPlaneTiles.h) must
// leave exactly the frame the per-pixel writer leaves, in every orientation, plane and polarity, wherever the picture
// sits, however the write band clips it and however the rows are batched.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "DirectPixelWriter.h"

namespace {

// Not a multiple of 8 on either side: the frame's last byte column is partly past the panel, as on no real panel
// but as writePixel()'s guard (displayWidthBytes * 8) allows.
constexpr int kPanelW = 61;
constexpr int kPanelH = 43;

struct Picture {
  int width;
  int rows;
  int rowBytes;
  std::vector<uint8_t> bytes;
};

Picture randomPicture(std::mt19937& rng, const int width, const int rows) {
  Picture p{width, rows, (width + 3) / 4, {}};
  p.bytes.resize(static_cast<size_t>(p.rowBytes) * rows);
  for (auto& b : p.bytes) b = static_cast<uint8_t>(rng());
  return p;
}

uint8_t pixelAt(const uint8_t* row, const int col) { return (row[col >> 2] >> (6 - (col & 3) * 2)) & 0x03; }

// The write window GfxRenderer gives the writer: physical rows [originY, originY + rows) of the panel, held in a
// buffer of that many rows. The whole panel, a strip of it, or none at all.
struct Band {
  int originY;
  int rows;
};

// The band after drawing `p` at (x, y), from the same random band, with a guard row either side that nothing may
// touch: writePixel() for every pixel, clipped by nothing but its own guards, or drawRows() in batches of `batch`
// (the read loop's shape: alignedRowCount() decides where a batch ends).
std::vector<uint8_t> drawn(const GfxRenderer::Orientation orientation, const GfxRenderer::RenderMode mode,
                           const bool dark, const Picture& p, const int x, const int y, const Band band,
                           const uint32_t frameSeed, const bool reference, const int batch) {
  GfxRenderer renderer(kPanelW, kPanelH, orientation);
  renderer.setRenderMode(mode);
  renderer.setOutputInverted(dark);
  const int widthBytes = renderer.getDisplayWidthBytes();
  std::vector<uint8_t> frame(static_cast<size_t>(widthBytes) * (band.rows + 2));
  std::mt19937 frameRng(frameSeed);
  for (auto& b : frame) b = static_cast<uint8_t>(frameRng());

  DirectPixelWriter pw;
  pw.init(renderer);
  pw.fb = frame.data() + widthBytes;
  pw.originY = band.originY;
  pw.clipRows = band.rows;
  if (reference) {
    for (int r = 0; r < p.rows; r++) {
      pw.beginRow(y + r);
      for (int col = 0; col < p.width; col++) {
        pw.writePixel(x + col, pixelAt(p.bytes.data() + static_cast<size_t>(r) * p.rowBytes, col));
      }
    }
  } else {
    for (int row = 0; row < p.rows;) {
      int n = std::min(batch, p.rows - row);
      if (n < p.rows - row) n = pw.alignedRowCount(y + row, n);
      pw.drawRows(p.bytes.data() + static_cast<size_t>(row) * p.rowBytes, p.rowBytes, n, x, y + row, p.width);
      row += n;
    }
  }
  return frame;
}

struct Case {
  GfxRenderer::Orientation orientation;
  GfxRenderer::RenderMode mode;
  bool dark;  // Dark Mode changes the B/W pass only (keepsImagePolarity), so only there is it a case of its own
};

std::vector<Case> allCases() {
  std::vector<Case> cases;
  for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                                 GfxRenderer::LandscapeCounterClockwise}) {
    cases.push_back({orientation, GfxRenderer::BW, false});
    cases.push_back({orientation, GfxRenderer::BW, true});
    cases.push_back({orientation, GfxRenderer::GRAYSCALE_LSB, false});
    cases.push_back({orientation, GfxRenderer::GRAYSCALE_MSB, false});
  }
  return cases;
}

class DrawRows : public ::testing::TestWithParam<Case> {};

TEST_P(DrawRows, LeavesTheFrameThePixelWriterLeaves) {
  const Case c = GetParam();
  std::mt19937 rng(1234u + static_cast<uint32_t>(c.orientation) * 7u + static_cast<uint32_t>(c.mode) * 3u +
                   (c.dark ? 1u : 0u));
  const bool portrait = c.orientation == GfxRenderer::Portrait || c.orientation == GfxRenderer::PortraitInverted;
  const int screenW = portrait ? kPanelH : kPanelW;
  const int screenH = portrait ? kPanelW : kPanelH;
  for (int trial = 0; trial < 600; trial++) {
    // Up to wider and taller than the screen, so a picture can overhang both edges at once.
    const int width = 1 + static_cast<int>(rng() % (screenW + 20));
    const int rows = 1 + static_cast<int>(rng() % (screenH + 10));
    const Picture p = randomPicture(rng, width, rows);
    // Mostly inside the screen; one trial in four placed anywhere around it, to exercise the clipping.
    const bool anywhere = rng() % 4 == 0;
    const int x = anywhere ? static_cast<int>(rng() % (screenW + 40)) - 20
                           : static_cast<int>(rng() % std::max(1, screenW - width + 1));
    const int y = anywhere ? static_cast<int>(rng() % (screenH + 40)) - 20
                           : static_cast<int>(rng() % std::max(1, screenH - rows + 1));
    // One trial in three draws into a strip of the panel, empty ones included.
    Band band{0, kPanelH};
    if (rng() % 3 == 0) {
      band.originY = static_cast<int>(rng() % kPanelH);
      band.rows = static_cast<int>(rng() % (kPanelH - band.originY + 1));
    }
    const uint32_t frameSeed = rng();
    const int batch = 1 + static_cast<int>(rng() % 24);
    const auto expected = drawn(c.orientation, c.mode, c.dark, p, x, y, band, frameSeed, /*reference=*/true, 0);
    const auto actual = drawn(c.orientation, c.mode, c.dark, p, x, y, band, frameSeed, /*reference=*/false, batch);
    ASSERT_EQ(expected, actual) << "trial " << trial << ": " << width << "x" << rows << " at " << x << "," << y
                                << " band " << band.originY << "+" << band.rows << " batch " << batch;
  }
}

INSTANTIATE_TEST_SUITE_P(DirectPixelWriter, DrawRows, ::testing::ValuesIn(allCases()));

// A batch the read loop may end anywhere ends on a byte column's edge when it can, so a column is never split.
TEST(DirectPixelWriterRows, AlignedRowCountEndsOnAByteColumnEdge) {
  for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted}) {
    GfxRenderer renderer(kPanelW, kPanelH, orientation);
    DirectPixelWriter pw;
    pw.init(renderer);
    for (int y = 0; y < 20; y++) {
      for (int maxRows = 8; maxRows < 30; maxRows++) {
        const int n = pw.alignedRowCount(y, maxRows);
        ASSERT_LE(n, maxRows);
        ASSERT_GT(n, maxRows - 8);
        pw.beginRow(y + n - 1);
        const int lastPhyX = pw.rowPhyXBase;
        pw.beginRow(y + n);
        EXPECT_NE(lastPhyX >> 3, pw.rowPhyXBase >> 3) << "y " << y << " maxRows " << maxRows;
      }
    }
  }
}

// The kernel on its own, every op, against what each op does to one bit. The drawBitmap() path takes the absolute
// gray-plane ops, which no DirectPixelWriter mode reaches.
using packed_plane::Op;

bool opBit(const Op op, const uint8_t v, const bool old) {
  switch (op) {
    case Op::BlackUnlessWhite:
      return v < 3 ? false : old;
    case Op::BlackWhereWhite:
      return v != 3;
    case Op::SetDarkGray:
      return v == 1 ? true : old;
    case Op::SetAnyGray:
      return (v == 1 || v == 2) ? true : old;
    case Op::WriteLowBit:
      return (v & 1) != 0;
    case Op::WriteHighBit:
      return (v & 2) != 0;
    case Op::None:
      return old;
  }
  return old;
}

constexpr Op kOps[] = {Op::BlackUnlessWhite, Op::BlackWhereWhite, Op::SetDarkGray,
                       Op::SetAnyGray,       Op::WriteLowBit,     Op::WriteHighBit};

void setBit(std::vector<uint8_t>& bytes, const size_t index, const uint8_t mask, const bool bit) {
  bytes[index] = bit ? static_cast<uint8_t>(bytes[index] | mask) : static_cast<uint8_t>(bytes[index] & ~mask);
}

TEST(PackedPlaneTiles, ByteColumnMatchesEachOpPixelByPixel) {
  std::mt19937 rng(77);
  for (const Op op : kOps) {
    for (int trial = 0; trial < 300; trial++) {
      const int width = 1 + static_cast<int>(rng() % 70);
      const int rowBytes = (width + 3) / 4;
      std::vector<std::vector<uint8_t>> laneRows(8);
      const uint8_t* lanes[8] = {};
      for (int lane = 0; lane < 8; lane++) {
        if (rng() % 4 == 0) continue;  // a lane the call leaves alone
        laneRows[lane].resize(static_cast<size_t>(rowBytes));
        for (auto& b : laneRows[lane]) b = static_cast<uint8_t>(rng());
        lanes[lane] = laneRows[lane].data();
      }
      const int first = static_cast<int>(rng() % width);
      const int end = first + 1 + static_cast<int>(rng() % (width - first));
      const ptrdiff_t stride = 3 + static_cast<ptrdiff_t>(rng() % 5);
      const ptrdiff_t step = rng() % 2 ? stride : -stride;
      const int columns = end - first;
      std::vector<uint8_t> frame(static_cast<size_t>(stride * (columns + 2)));
      for (auto& b : frame) b = static_cast<uint8_t>(rng());
      const ptrdiff_t firstIndex = step > 0 ? stride : stride * columns;
      std::vector<uint8_t> expected = frame;
      for (int col = first; col < end; col++) {
        const size_t index = static_cast<size_t>(firstIndex + (col - first) * step);
        for (int lane = 0; lane < 8; lane++) {
          if (!lanes[lane]) continue;
          const uint8_t mask = static_cast<uint8_t>(0x80u >> lane);
          setBit(expected, index, mask, opBit(op, pixelAt(lanes[lane], col), (expected[index] & mask) != 0));
        }
      }
      packed_plane::drawByteColumn(lanes, rowBytes, first, end, frame.data() + firstIndex, step, op);
      ASSERT_EQ(expected, frame) << "op " << static_cast<int>(op) << " trial " << trial;
    }
  }
}

TEST(PackedPlaneTiles, RowSpanMatchesEachOpPixelByPixel) {
  std::mt19937 rng(91);
  for (const Op op : kOps) {
    for (int trial = 0; trial < 300; trial++) {
      const int width = 1 + static_cast<int>(rng() % 70);
      const int rowBytes = (width + 3) / 4;
      std::vector<uint8_t> row(static_cast<size_t>(rowBytes));
      for (auto& b : row) b = static_cast<uint8_t>(rng());
      const int frameBits = 1 + static_cast<int>(rng() % 90);
      const int lineBytes = (frameBits + 7) / 8 + 1;  // one byte past the last bit the guard allows
      std::vector<uint8_t> line(static_cast<size_t>(lineBytes));
      for (auto& b : line) b = static_cast<uint8_t>(rng());
      const int first = static_cast<int>(rng() % width);
      const int end = first + 1 + static_cast<int>(rng() % (width - first));
      const int step = rng() % 2 ? 1 : -1;
      const int origin = static_cast<int>(rng() % (frameBits + 40)) - 20;
      std::vector<uint8_t> expected = line;
      for (int col = first; col < end; col++) {
        const int bitIndex = origin + col * step;
        if (bitIndex < 0 || bitIndex >= frameBits) continue;
        const auto index = static_cast<size_t>(bitIndex >> 3);
        const uint8_t mask = static_cast<uint8_t>(0x80u >> (bitIndex & 7));
        setBit(expected, index, mask, opBit(op, pixelAt(row.data(), col), (expected[index] & mask) != 0));
      }
      packed_plane::drawRowSpan(row.data(), rowBytes, first, end, line.data(), origin, step, frameBits, op);
      ASSERT_EQ(expected, line) << "op " << static_cast<int>(op) << " trial " << trial;
    }
  }
}

}  // namespace
