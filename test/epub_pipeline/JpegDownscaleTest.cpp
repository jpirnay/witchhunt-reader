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
    return decodePath(std::string(JPEG_FIXTURE_DIR "/") + name, width, height);
  }
  Levels decodePath(const std::string& jpg, const int width, const int height) {
    RenderConfig config;
    config.x = 0;
    config.y = 0;
    config.maxWidth = width;
    config.maxHeight = height;
    config.useExactDimensions = true;
    config.useDithering = false;  // plain 4-level quantization: a darkened pixel is visible as < 3
    config.cachePath = (work / "lines.pxc").string();
    fs::remove(config.cachePath);  // what is read below is this decode's, or nothing
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(jpg, renderer, config));

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

// With no arena and too little heap for any finer workspace beside its floor, the decode falls back
// to the 1/8 one (which asks for no floor) rather than fail: a blurred picture beats a placeholder.
// A 1/8 level averages each 8x8 block, so a 1-px line never gets darker than ~3/4 white there -- no
// pixel reaches the two darkest levels, which the full decode draws every line with. That is how
// the test knows which decode ran.
TEST_F(JpegDownscaleFixture, ProgressiveFallsBackToTheEighthWhenNoFinerWorkspaceFits) {
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

  ASSERT_EQ(preview.px.size(), static_cast<size_t>(kDstLength) * kDstBreadth) << "the 1/8 decode must still render";
  EXPECT_EQ(darkPixels(preview), 0) << "expected the 1/8 decode, got a finer one";
}

// A 1/8 decode enlarged to the box goes through the bilinear resize of the whole picture
// (ProgressiveJpeg::decodeResized): the per-band resampler sees one row of blocks at a time and
// leaves a step at every band edge. It is the same picture whether the heap held the 1/8 workspace
// beside the usual 20 KB, or only the bare 1/8 workspace.
TEST_F(JpegDownscaleFixture, AnEnlargedEighthIsTheResizeHoweverItCameToBe) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(JPEG_FIXTURE_DIR "/prog_full_420.jpg"));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok);
  file.close();
  const uint32_t savedHeap = ESP.getFreeHeap();
  // 203x141 into 150x104 wants 1/1; a byte short of 1/4's workspace and its floor leaves 1/8.
  ESP.setFreeHeap(static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 2) + 20 * 1024 - 1));
  const Levels besideTheFloor = decode("prog_full_420.jpg", 150, 104);
  ESP.setFreeHeap(static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 3) + 20 * 1024 - 1));
  const Levels bare = decode("prog_full_420.jpg", 150, 104);
  ESP.setFreeHeap(savedHeap);
  ASSERT_FALSE(besideTheFloor.px.empty());
  EXPECT_EQ(besideTheFloor.px, bare.px);
}

// prog_full_420.jpg with the Huffman table of its first AC scan over-subscribed (two more codes of
// length 1, two fewer of a longer length: the segment keeps its size). Only AC scans use it.
std::vector<uint8_t> withABrokenAcTable() {
  std::ifstream in(JPEG_FIXTURE_DIR "/prog_full_420.jpg", std::ios::binary);
  std::vector<uint8_t> d{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  size_t lastDht = 0;
  for (size_t i = 2; i + 4 < d.size();) {
    if (d[i] != 0xFF) return {};
    const uint8_t marker = d[i + 1];
    const size_t length = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
    if (marker == 0xC4) lastDht = i;
    if (marker == 0xDA && d[i + 5 + 2 * d[i + 4]] > 0) break;  // Ss > 0: the first AC scan
    i += 2 + length;
    if (marker == 0xDA) {  // skip its entropy data to the next marker
      while (i + 1 < d.size() && !(d[i] == 0xFF && d[i + 1] != 0x00 && (d[i + 1] < 0xD0 || d[i + 1] > 0xD7))) ++i;
    }
  }
  if (lastDht == 0 || (d[lastDht + 4] >> 4) != 1) return {};  // an AC table first in the segment
  uint8_t* counts = &d[lastDht + 5];
  for (int k = 1; k < 16; ++k) {
    if (counts[k] >= 2) {
      counts[0] = static_cast<uint8_t>(counts[0] + 2);
      counts[k] = static_cast<uint8_t>(counts[k] - 2);
      return d;
    }
  }
  return {};
}

// The full decoder refuses that file before its first row (the table cannot be built); a 1/8
// session never builds an AC table, so the picture comes from the DC scans, resized. It is the
// clean file's 1/8 picture: only an AC table differs.
TEST_F(JpegDownscaleFixture, AFileTheFullDecoderRefusesIsDrawnFromItsDcScans) {
  const std::vector<uint8_t> broken = withABrokenAcTable();
  ASSERT_FALSE(broken.empty());
  const std::string path = (work / "broken_ac_table.jpg").string();
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(broken.data()), broken.size());
  {
    FsFile file;
    ASSERT_TRUE(file.openForRead(path));
    ProgressiveJpeg::DecodeOptions options;
    EXPECT_EQ(
        ProgressiveJpeg::decode(
            file, options, [](void*, uint16_t, const uint8_t*, uint16_t, uint16_t, uint16_t) { return true; }, nullptr),
        ProgressiveJpeg::Result::InvalidData)
        << "the full decoder refuses it";
    file.close();
  }
  FsFile file;
  ASSERT_TRUE(file.openForRead(JPEG_FIXTURE_DIR "/prog_full_420.jpg"));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok);
  file.close();
  const uint32_t savedHeap = ESP.getFreeHeap();
  ESP.setFreeHeap(static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 2) + 20 * 1024 - 1));
  const Levels eighth = decode("prog_full_420.jpg", 150, 104);  // the clean file, room for 1/8 only
  ESP.setFreeHeap(savedHeap);
  ASSERT_FALSE(eighth.px.empty());
  const Levels refused = decodePath(path, 150, 104);
  EXPECT_EQ(refused.px, eighth.px);
}

// ...but not a decode the card failed: the error is not the file's, and the 1/8 picture would be
// cached as the box's for good. The placeholder is drawn, nothing is cached, and the next pass
// decodes again. Reads fail from the middle of prog_420_dc_first.jpg on: the full decode's index
// reads the whole file and meets them; a 1/8 decode needs only the first scan and would not.
TEST_F(JpegDownscaleFixture, ACardErrorIsNotRetriedAtAnEighth) {
  const std::string jpg = JPEG_FIXTURE_DIR "/prog_420_dc_first.jpg";
  ASSERT_FALSE(decode("prog_420_dc_first.jpg", 150, 104).px.empty()) << "control: it decodes and caches";
  struct ReadsFailFrom {
    explicit ReadsFailFrom(const long offset) { FsFile::failReadsFrom = offset; }
    ~ReadsFailFrom() { FsFile::failReadsFrom = -1; }
    ReadsFailFrom(const ReadsFailFrom&) = delete;
    ReadsFailFrom& operator=(const ReadsFailFrom&) = delete;
  } cut(static_cast<long>(fs::file_size(jpg) / 2));
  {
    FsFile file;
    ASSERT_TRUE(file.openForRead(jpg));
    ProgressiveJpeg::DecodeOptions options;
    const auto eighth = ProgressiveJpeg::decodeResized(
        file, options, 150, 104, [](void*, uint16_t, const uint8_t*, uint16_t) { return true; }, nullptr);
    file.close();
    ASSERT_EQ(eighth, ProgressiveJpeg::Result::Ok) << "the 1/8 decode alone gets past the failing reads";
  }
  RenderConfig config;
  config.x = 0;
  config.y = 0;
  config.maxWidth = 150;
  config.maxHeight = 104;
  config.useExactDimensions = true;
  config.useDithering = false;
  config.cachePath = (work / "card_error.pxc").string();
  JpegToFramebufferConverter converter;
  EXPECT_TRUE(converter.decodeToFramebuffer(jpg, renderer, config)) << "the placeholder is drawn";
  EXPECT_FALSE(fs::exists(config.cachePath)) << "a decode the card failed is not cached";
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
