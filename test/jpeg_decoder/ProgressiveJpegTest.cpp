// Full progressive JPEG decoding (lib/ProgressiveJpeg).
//
// The DC-only fallback shows a progressive JPEG at 1/8 resolution, upscaled: text in a diagram
// becomes mush. ProgressiveJpeg decodes every scan, band by band, and must reproduce libjpeg's
// own luma -- at 1/1 exactly (up to IDCT rounding), and at 1/2..1/8 as the reduced-size IDCT of
// the same blocks.
//
// Fixtures (fixtures/generate_fixtures.py) use libjpeg's successive-approximation scan script:
// interleaved DC first + refinement, spectral-selection AC first scans with Al > 0, and AC
// refinement scans -- every decode path. One of them adds restart markers every 3 units.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "HalStorage.h"  // FsFile (stdio-backed shim)
#include "ProgressiveJpeg.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "."
#endif

namespace {

std::string fixture(const std::string& name) { return std::string(FIXTURE_DIR) + "/" + name; }

struct Gray {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> px;
  int at(const int x, const int y) const { return px[static_cast<size_t>(y) * w + x]; }
};

Gray loadPgm(const std::string& path) {
  Gray out;
  FILE* fp = fopen(path.c_str(), "rb");
  EXPECT_NE(fp, nullptr) << path;
  if (!fp) return out;
  char magic[3] = {};
  int maxval = 0;
  if (fscanf(fp, "%2s %d %d %d", magic, &out.w, &out.h, &maxval) == 4 && std::string(magic) == "P5" && maxval == 255) {
    fgetc(fp);
    out.px.resize(static_cast<size_t>(out.w) * out.h);
    EXPECT_EQ(fread(out.px.data(), 1, out.px.size(), fp), out.px.size());
  }
  fclose(fp);
  return out;
}

// What a 1/2^s decode should equal, derived from libjpeg's full-resolution luma: each 8x8 block
// forward-transformed, its top-left n x n coefficients inverted with the n-point IDCT (n = 8 >> s),
// over the floor(w >> s) x floor(h >> s) output. Edge blocks are padded by replicating the last
// row/column, which is what the encoder did.
Gray reducedReference(const Gray& full, const int shift) {
  const int n = 8 >> shift;
  const double pi = 3.14159265358979323846;
  auto c = [](const int k) { return k == 0 ? 0.70710678118654752 : 1.0; };
  Gray out;
  out.w = full.w >> shift;
  out.h = full.h >> shift;
  out.px.resize(static_cast<size_t>(out.w) * out.h);
  for (int by = 0; by * 8 < full.h; ++by) {
    for (int bx = 0; bx * 8 < full.w; ++bx) {
      double f[8][8];
      for (int v = 0; v < n; ++v) {
        for (int u = 0; u < n; ++u) {
          double sum = 0;
          for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
              const int p = full.at(std::min(bx * 8 + x, full.w - 1), std::min(by * 8 + y, full.h - 1)) - 128;
              sum += p * std::cos((2 * x + 1) * u * pi / 16) * std::cos((2 * y + 1) * v * pi / 16);
            }
          }
          f[v][u] = c(u) * c(v) / 4 * sum;
        }
      }
      for (int Y = 0; Y < n; ++Y) {
        for (int X = 0; X < n; ++X) {
          const int ox = bx * n + X;
          const int oy = by * n + Y;
          if (ox >= out.w || oy >= out.h) continue;
          double value = 128;
          for (int v = 0; v < n; ++v) {
            for (int u = 0; u < n; ++u) {
              value += c(u) / 2 * c(v) / 2 * f[v][u] * std::cos((2 * X + 1) * u * pi / (2 * n)) *
                       std::cos((2 * Y + 1) * v * pi / (2 * n));
            }
          }
          out.px[static_cast<size_t>(oy) * out.w + ox] =
              static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255));
        }
      }
    }
  }
  return out;
}

struct Capture {
  Gray image;
  int bands = 0;
  bool orderOk = true;
  static bool accept(void* user, uint16_t y, const uint8_t* gray, uint16_t width, uint16_t rows, uint16_t stride) {
    auto& c = *static_cast<Capture*>(user);
    if (y != c.image.h || (c.image.w != 0 && c.image.w != width) || stride < width) c.orderOk = false;
    c.image.w = width;
    for (uint16_t r = 0; r < rows; ++r) {
      c.image.px.insert(c.image.px.end(), gray + static_cast<size_t>(r) * stride,
                        gray + static_cast<size_t>(r) * stride + width);
    }
    c.image.h += rows;
    ++c.bands;
    return true;
  }
};

ProgressiveJpeg::Result decode(const std::string& name, const int shift, Capture& capture, uint8_t* workspace = nullptr,
                               const size_t workspaceSize = 0) {
  FsFile file;
  EXPECT_TRUE(file.openForRead(fixture(name)));
  ProgressiveJpeg::DecodeOptions options;
  options.scaleShift = static_cast<uint8_t>(shift);
  options.workspace = workspace;
  options.workspaceSize = workspaceSize;
  const auto result = ProgressiveJpeg::decode(file, options, Capture::accept, &capture);
  file.close();
  return result;
}

struct Diff {
  double mean = 0;
  int max = 0;
};

Diff compare(const Gray& a, const Gray& b) {
  Diff d;
  long sum = 0;
  for (size_t i = 0; i < a.px.size(); ++i) {
    const int e = std::abs(static_cast<int>(a.px[i]) - static_cast<int>(b.px[i]));
    sum += e;
    d.max = std::max(d.max, e);
  }
  d.mean = a.px.empty() ? 0 : static_cast<double>(sum) / a.px.size();
  return d;
}

struct Case {
  const char* jpg;
  const char* reference;
};

class ProgressiveJpegMatchesLibjpeg : public testing::TestWithParam<std::tuple<Case, int>> {};

TEST_P(ProgressiveJpegMatchesLibjpeg, AtEveryScale) {
  const auto& [c, shift] = GetParam();
  const Gray full = loadPgm(fixture(c.reference));
  ASSERT_FALSE(full.px.empty());
  const Gray expected = reducedReference(full, shift);

  Capture capture;
  const auto result = decode(c.jpg, shift, capture);
  ASSERT_EQ(result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(result);
  EXPECT_TRUE(capture.orderOk) << "bands must arrive in order, full width";
  ASSERT_EQ(capture.image.w, expected.w);
  ASSERT_EQ(capture.image.h, expected.h);

  const Diff d = compare(capture.image, expected);
  // Our integer IDCT differs from libjpeg's islow by a level or two. The scaled references start
  // from libjpeg's CLAMPED pixels, so next to saturated edges they are a little off by
  // construction. A decoding error (a lost refinement bit, a scan out of step) shows up as a
  // whole block tens of levels off.
  // Measured: 1/1 mean 0.04 max 1; scaled mean <= 0.37 max <= 4.
  EXPECT_LT(d.mean, shift == 0 ? 0.2 : 0.6) << c.jpg << " at 1/" << (1 << shift);
  EXPECT_LE(d.max, shift == 0 ? 2 : 12) << c.jpg << " at 1/" << (1 << shift);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ProgressiveJpegMatchesLibjpeg,
                         testing::Combine(testing::Values(Case{"prog_full_420.jpg", "prog_full_420.y.pgm"},
                                                          Case{"prog_full_gray.jpg", "prog_full_gray.y.pgm"},
                                                          Case{"prog_full_444_rst.jpg", "prog_full_444_rst.y.pgm"}),
                                          testing::Values(0, 1, 2, 3)));

TEST(ProgressiveJpeg, ProbeReportsGeometry) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("prog_full_420.jpg")));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok);
  EXPECT_EQ(file.position(), 0u) << "probe must rewind";
  file.close();
  EXPECT_EQ(info.width, 203);
  EXPECT_EQ(info.height, 141);
  EXPECT_EQ(info.componentCount, 3);
  EXPECT_EQ(info.maxHorizontal, 2);
  EXPECT_EQ(info.maxVertical, 2);
}

TEST(ProgressiveJpeg, RejectsBaselineWithoutEmittingRows) {
  Capture capture;
  EXPECT_EQ(decode("contrast_420.jpg", 0, capture), ProgressiveJpeg::Result::Unsupported);
  EXPECT_EQ(capture.bands, 0);
}

// The workspace is the decoder's whole working set; a caller-supplied block (the reader's scratch
// arena) must be enough on its own, even misaligned, and one byte short must be refused up front.
TEST(ProgressiveJpeg, RunsInACallerWorkspaceAndRefusesAShortOne) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("prog_full_420.jpg")));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok);
  file.close();
  const size_t need = ProgressiveJpeg::workspaceBytes(info, 1);
  ASSERT_GT(need, 0u);

  std::vector<uint8_t> block(need + 1);
  Capture ok;
  EXPECT_EQ(decode("prog_full_420.jpg", 1, ok, block.data() + 1, need), ProgressiveJpeg::Result::Ok)
      << "an odd address must still work: the decoder aligns inside its own slack";
  EXPECT_EQ(ok.image.h, 141 >> 1);

  Capture refused;
  EXPECT_EQ(decode("prog_full_420.jpg", 1, refused, block.data(), need - 9), ProgressiveJpeg::Result::OutOfMemory);
  EXPECT_EQ(refused.bands, 0);
}

// Smaller output keeps fewer coefficients per block: the working set has to shrink with it.
TEST(ProgressiveJpeg, WorkspaceShrinksWithScale) {
  ProgressiveJpeg::ImageInfo info;
  info.width = 1210;
  info.height = 1327;
  info.componentCount = 3;
  const size_t full = ProgressiveJpeg::workspaceBytes(info, 0);
  const size_t half = ProgressiveJpeg::workspaceBytes(info, 1);
  const size_t eighth = ProgressiveJpeg::workspaceBytes(info, 3);
  EXPECT_GT(full, half);
  EXPECT_GT(half, eighth);
  EXPECT_LT(half, 20u * 1024) << "the Strange Pictures diagrams decode at 1/2; this is their budget";
}

}  // namespace
