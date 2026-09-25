// JPEG downscaling keeps thin strokes.
//
// After TJpgDec's 1/2^n DCT step the converter still has a residual scale of 0.5-1.0 to cover.
// It used to take one source pixel per output pixel, so every source column the sample grid
// skipped simply vanished: the 1-px lines in diagrams (the "Strange Pictures" riddles) broke
// up. Output pixels now average the source pixels their footprint covers, including the parts
// that arrived in an earlier decode block.
//
// Fixtures: 600x32 white with a 1-px black line every 7 px, and the same transposed, decoded to
// 420 (0.7 at 1/1 DCT). A line split evenly across two output pixels still darkens each by ~35%,
// which the undithered 4-level quantizer maps below white. Point sampling misses ~30% of the
// lines. The pitch of 7 puts lines in every column of the 8-px MCU, so the lines that end an MCU
// -- the ones only the block-edge carry keeps -- are covered in both directions.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Epub/converters/JpegToFramebufferConverter.h"
#include "GfxRenderer.h"
#include "HalStorage.h"
#include "ProgressiveJpeg.h"

namespace fs = std::filesystem;

namespace {

constexpr int kSrcLength = 600;  // along the axis the lines are spaced on
constexpr int kSrcBreadth = 32;
constexpr int kLinePitch = 7;
constexpr int kFirstLine = 3;
constexpr int kDstLength = 420;
constexpr int kDstBreadth = 22;  // 32 * 0.7, rounded down

struct Levels {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> px;  // 2-bit levels, 0 black .. 3 white
  uint8_t at(const int x, const int y) const { return px[static_cast<size_t>(y) * width + x]; }
};

struct JpegDownscaleFixture : testing::Test {
  fs::path work;
  GfxRenderer renderer;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("jpg_downscale_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // Decodes a fixture into a .pxc and unpacks it.
  Levels decode(const char* name, const int width, const int height) {
    RenderConfig config;
    config.x = 0;
    config.y = 0;
    config.maxWidth = width;
    config.maxHeight = height;
    config.useExactDimensions = true;
    config.useDithering = false;  // plain 4-level quantization: a darkened pixel is visible as < 3
    config.cachePath = (work / "lines.pxc").string();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(std::string(JPEG_FIXTURE_DIR "/") + name, renderer, config));

    std::ifstream in(config.cachePath, std::ios::binary);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const int bytesPerRow = (width + 3) / 4;
    constexpr size_t kHeader = 6;  // magic, width, height
    Levels out;
    if (file.size() != kHeader + static_cast<size_t>(bytesPerRow) * height) {
      ADD_FAILURE() << "unexpected .pxc size " << file.size();
      return out;
    }
    out.width = width;
    out.height = height;
    out.px.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
      const uint8_t* row = file.data() + kHeader + static_cast<size_t>(bytesPerRow) * y;
      for (int x = 0; x < width; ++x)
        out.px[static_cast<size_t>(y) * width + x] = (row[x / 4] >> (6 - 2 * (x % 4))) & 3;
    }
    return out;
  }

  // Lines (source positions kFirstLine + k * kLinePitch) with no darkened output pixel on the
  // sampled line across them. `at(i)` reads output position i along the spaced axis.
  template <typename At>
  static std::vector<int> lostLines(const At& at) {
    std::vector<int> lost;
    for (int src = kFirstLine; src < kSrcLength; src += kLinePitch) {
      const int first = src * kDstLength / kSrcLength;
      const int last = std::min(kDstLength - 1, (src + 1) * kDstLength / kSrcLength);
      bool seen = false;
      for (int i = first; i <= last; ++i) seen |= at(i) < 3;
      if (!seen) lost.push_back(src);
    }
    return lost;
  }
};

std::string join(const std::vector<int>& v) {
  std::string s;
  for (const int x : v) s += std::to_string(x) + " ";
  return s;
}

TEST_F(JpegDownscaleFixture, VerticalLinesSurviveTheResidualDownscale) {
  const Levels img = decode("thin_lines_gray.jpg", kDstLength, kDstBreadth);
  ASSERT_EQ(img.px.size(), static_cast<size_t>(kDstLength) * kDstBreadth);
  for (const int y : {0, kDstBreadth / 2, kDstBreadth - 1}) {
    const auto lost = lostLines([&](const int i) { return img.at(i, y); });
    EXPECT_TRUE(lost.empty()) << "row " << y << ": source columns " << join(lost) << "vanished";
  }
}

// The same across MCU rows: a line in the last source row of an MCU row is only kept if the
// output row straddling that boundary collects the upper MCU row's share.
TEST_F(JpegDownscaleFixture, HorizontalLinesSurviveTheResidualDownscale) {
  const Levels img = decode("thin_hlines_gray.jpg", kDstBreadth, kDstLength);
  ASSERT_EQ(img.px.size(), static_cast<size_t>(kDstLength) * kDstBreadth);
  for (const int x : {0, kDstBreadth / 2, kDstBreadth - 1}) {
    const auto lost = lostLines([&](const int i) { return img.at(x, i); });
    EXPECT_TRUE(lost.empty()) << "column " << x << ": source rows " << join(lost) << "vanished";
  }
}

// The other half of the contract: averaging must not put ink where the source has none. Every
// output pixel whose footprint (plus a pixel of margin for JPEG ringing) holds no line must stay
// exactly white -- in both orientations, since the carried partials at block and MCU-row edges
// are where a rounding slip shows up first (it once wrapped near-white to ~100: a dotted grid).
TEST_F(JpegDownscaleFixture, WhiteBetweenLinesStaysWhite) {
  const Levels v = decode("thin_lines_gray.jpg", kDstLength, kDstBreadth);
  const Levels h = decode("thin_hlines_gray.jpg", kDstBreadth, kDstLength);
  ASSERT_FALSE(v.px.empty());
  ASSERT_FALSE(h.px.empty());
  int checked = 0;
  for (int i = 0; i < kDstLength; ++i) {
    const int from = i * kSrcLength / kDstLength - 1;
    const int to = ((i + 1) * kSrcLength + kDstLength - 1) / kDstLength + 1;  // exclusive
    bool clear = true;
    for (int src = std::max(from, 0); src < to; ++src) clear &= (src - kFirstLine) % kLinePitch != 0;
    if (!clear) continue;
    ++checked;
    for (int j = 0; j < kDstBreadth; ++j) {
      ASSERT_EQ(v.at(i, j), 3) << "vertical-line image, column " << i << " row " << j;
      ASSERT_EQ(h.at(j, i), 3) << "horizontal-line image, row " << i << " column " << j;
    }
  }
  EXPECT_GT(checked, kDstLength / 4);
}

// Progressive JPEGs used to render from their first (DC) scan only: 1/8 resolution, upscaled,
// which wipes out every one of these lines. Through the full decoder they come out like the
// baseline file above.
TEST_F(JpegDownscaleFixture, ProgressiveLinesSurviveTheFullDecoder) {
  const Levels img = decode("thin_lines_prog.jpg", kDstLength, kDstBreadth);
  ASSERT_EQ(img.px.size(), static_cast<size_t>(kDstLength) * kDstBreadth);
  const auto lost = lostLines([&](const int i) { return img.at(i, kDstBreadth / 2); });
  EXPECT_TRUE(lost.empty()) << "source columns " << join(lost) << "vanished";
}

// With no arena and too little heap for even the 1/8 workspace, the decode must fall back to the
// DC-only preview rather than fail: a blurred picture beats a placeholder. The preview averages
// each 8x8 block, so a 1-px line never gets darker than ~3/4 white there -- no pixel reaches the
// two darkest levels, which the full decode draws every line with. That is how the test knows
// which decoder ran.
TEST_F(JpegDownscaleFixture, ProgressiveFallsBackToThePreviewWhenTheWorkspaceDoesNotFit) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(JPEG_FIXTURE_DIR "/thin_lines_prog.jpg"));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok);
  file.close();
  auto darkPixels = [](const Levels& img) {
    return std::count_if(img.px.begin(), img.px.end(), [](uint8_t v) { return v <= 1; });
  };

  const Levels full = decode("thin_lines_prog.jpg", kDstLength, kDstBreadth);
  ASSERT_GT(darkPixels(full), 0) << "the full decode draws the lines dark";

  const uint32_t savedHeap = ESP.getFreeHeap();
  // One byte short of the smallest workspace plus its 20 KB floor; still enough for the cache.
  ESP.setFreeHeap(static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 3) + 20 * 1024 - 1));
  const Levels preview = decode("thin_lines_prog.jpg", kDstLength, kDstBreadth);
  ESP.setFreeHeap(savedHeap);

  ASSERT_EQ(preview.px.size(), static_cast<size_t>(kDstLength) * kDstBreadth) << "the preview must still render";
  EXPECT_EQ(darkPixels(preview), 0) << "expected the 1/8 preview, got a full decode";
}

// One decode, both caches (RenderConfig::companionCachePath). The reader's BW-primary decode used
// to be followed by a second full decode for the grey cache -- 2.35 s each for a progressive
// diagram on the X3. The merge is only sound as a pure optimisation: the two files must be byte
// for byte what two separate decodes write.
class JpegCompanionCache : public JpegDownscaleFixture, public testing::WithParamInterface<const char*> {
 protected:
  std::vector<uint8_t> decodeTo(const char* primary, const bool monochrome, const char* companion = nullptr) {
    RenderConfig config;
    config.x = 0;
    config.y = 0;
    // 600 -> 570 is a residual scale of 0.95, as on the X3 diagram that exposed it: charging a
    // progressive band TJpgDec's 16 source rows (17 output rows) overran the 8 KB dither-band
    // budget (14 rows at this width), dropping the band -- and with it the companion.
    config.maxWidth = 570;
    config.maxHeight = 30;
    config.useExactDimensions = true;
    config.monochromeOutput = monochrome;
    config.cachePath = (work / primary).string();
    if (companion) config.companionCachePath = (work / companion).string();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(std::string(JPEG_FIXTURE_DIR "/") + GetParam(), renderer, config));
    return read(primary);
  }
  std::vector<uint8_t> read(const char* name) const {
    std::ifstream in((work / name).string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

TEST_P(JpegCompanionCache, MergedDecodeMatchesTwoSeparateOnes) {
  const auto bw = decodeTo("sep_bw.pxc", true);
  const auto grey = decodeTo("sep_grey.pxc", false);
  ASSERT_FALSE(bw.empty());
  ASSERT_FALSE(grey.empty());
  ASSERT_NE(bw, grey) << "the two variants must differ, or this test proves nothing";

  EXPECT_EQ(decodeTo("merged_bw.pxc", true, "merged_grey.pxc"), bw) << "the drawn variant must not change";
  EXPECT_EQ(read("merged_grey.pxc"), grey) << "the companion must equal its own separate decode";
}

INSTANTIATE_TEST_SUITE_P(ProgressiveAndBaseline, JpegCompanionCache,
                         testing::Values("thin_lines_prog.jpg", "thin_lines_gray.jpg"));

}  // namespace
