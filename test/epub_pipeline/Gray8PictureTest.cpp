// Reader pictures painted into a panel's own 8-bit canvas: the converters' RenderConfig::gray8 sink (JPEG baseline and
// progressive, PNG), ImageBlock::renderGray8 and Page::renderImagesGray8.
//
// On such a panel (the T5S3) a page with pictures goes out on that canvas: the page as the B/W pass drew it, then each
// picture's samples over its box. These tests pin that a picture's samples land where the B/W pass draws it, whole or
// as one slice of a picture split over pages, upright or turned, in a table cell too; that the sink writes nothing but
// the canvas; and that a picture that gets no gray keeps what the B/W pass drew there.
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/TurnedFrame.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/JpegToFramebufferConverter.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const char* kPngBook = CORPUS_DIR "/test_png_images.epub";
const char* kJpegBook = CORPUS_DIR "/test_jpeg_images.epub";

// The stub's panel: 480 x 800, one byte a pixel on the canvas.
constexpr int kPanelW = 480;
constexpr int kPanelH = 800;
constexpr uint16_t kStride = kPanelW;
constexpr int kFbBytes = kPanelW / 8 * kPanelH;
// Odd, and the tests that count written bytes paint through evenTone(), so no sample can pass for an unwritten byte.
constexpr uint8_t kUnset = 0x5B;

struct Box {
  int x;
  int y;
  int w;
  int h;
  bool contains(const int px, const int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// Every sample to an even byte: the canvas's unwritten bytes are odd.
const uint8_t* evenTone() {
  static const std::vector<uint8_t> tone = [] {
    std::vector<uint8_t> t(256);
    for (int v = 0; v < 256; ++v) t[v] = static_cast<uint8_t>(v & 0xFE);
    return t;
  }();
  return tone.data();
}

// An inverting curve: a sample that went through it cannot pass for one that did not.
const std::vector<uint8_t>& invertingTone() {
  static const std::vector<uint8_t> tone = [] {
    std::vector<uint8_t> t(256);
    for (int v = 0; v < 256; ++v) t[v] = static_cast<uint8_t>(255 - v);
    return t;
  }();
  return tone;
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct Gray8PictureFixture : testing::Test {
  fs::path work;
  GfxRenderer renderer;
  std::vector<uint8_t> canvas;

  void SetUp() override {
    work = fs::temp_directory_path() / ("gray8_picture_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    // Landscape counter-clockwise is the identity transform on the stub's panel: a screen pixel is the canvas byte and
    // the framebuffer bit of the same coordinates.
    renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
    clearCanvas();
    fillFrameBuffer(0xFF);
  }
  void TearDown() override { fs::remove_all(work); }

  void clearCanvas() { canvas.assign(static_cast<size_t>(kStride) * kPanelH, kUnset); }
  void fillFrameBuffer(const uint8_t byte) const { std::memset(renderer.getWriteTarget(), byte, kFbBytes); }
  bool frameBufferWhite() const {
    const uint8_t* fb = renderer.getWriteTarget();
    return std::all_of(fb, fb + kFbBytes, [](const uint8_t b) { return b == 0xFF; });
  }
  bool black(const int x, const int y) const {
    return (renderer.getWriteTarget()[y * (kPanelW / 8) + (x >> 3)] & (0x80 >> (x & 7))) == 0;
  }
  uint8_t at(const int x, const int y) const { return canvas[static_cast<size_t>(y) * kStride + x]; }
  int writtenIn(const Box& box) const {
    int n = 0;
    for (int y = box.y; y < box.y + box.h; ++y) {
      for (int x = box.x; x < box.x + box.w; ++x) n += at(x, y) != kUnset;
    }
    return n;
  }
  int writtenOutside(const Box& box) const {
    int n = 0;
    for (int y = 0; y < kPanelH; ++y) {
      for (int x = 0; x < kPanelW; ++x) n += !box.contains(x, y) && at(x, y) != kUnset;
    }
    return n;
  }
  bool untouched() const {
    return std::all_of(canvas.begin(), canvas.end(), [](const uint8_t b) { return b == kUnset; });
  }

  // Straight through a converter, as ImageBlock::renderGray8 calls it.
  bool decodeJpeg(const std::string& path, const Box& box, const uint8_t* tone = nullptr,
                  const std::string& cachePath = "") {
    DirectGray8Writer writer;
    writer.init(renderer, canvas.data(), kStride);
    writer.tone = tone;
    RenderConfig config;
    config.x = box.x;
    config.y = box.y;
    config.maxWidth = box.w;
    config.maxHeight = box.h;
    config.useExactDimensions = true;
    config.useDithering = false;
    config.monochromeOutput = false;
    config.cachePath = cachePath;
    config.gray8 = &writer;
    JpegToFramebufferConverter converter;
    return converter.decodeToFramebuffer(path, renderer, config);
  }

  // The warm pass's decode, which leaves the 1-bit cache the B/W pass draws from; then the framebuffer it scribbled on
  // is cleared, as the reader clears it before the page render.
  void warm(ImageBlock& image, const int x, const int y) {
    image.render(renderer, x, y, /*forceLoad=*/true, /*monochromeOutput=*/true);
    ASSERT_TRUE(image.hasPixelCache());
    fillFrameBuffer(0xFF);
  }

  // A file the picture's 1-bit cache is said to be (renderGray8 asks only whether there is one).
  void fakePixelCache(const fs::path& imagePath) const {
    fs::path cache = imagePath;
    cache.replace_extension(".1bit.pxc");
    writeFile(cache, {0x00});
  }
};

// --- The JPEG sink ------------------------------------------------------------------------------------------------

// Every pixel of the box gets a sample; nothing outside it, nothing in the framebuffer, and no pixel cache although one
// was asked for (a 2-bit cache could not replay these samples).
TEST_F(Gray8PictureFixture, JpegBaselineStoresSamplesAndNothingElse) {
  const Box box{10, 20, 96, 64};
  const std::string cachePath = (work / "contrast.pxc").string();
  ASSERT_TRUE(decodeJpeg(JPEG_FIXTURE_DIR "/contrast_420.jpg", box, evenTone(), cachePath));
  EXPECT_EQ(writtenIn(box), box.w * box.h);
  EXPECT_EQ(writtenOutside(box), 0);
  EXPECT_TRUE(frameBufferWhite());
  EXPECT_FALSE(fs::exists(cachePath));
  EXPECT_FALSE(fs::exists(cachePath + ".part"));

  // Black shapes on white, decoded 1:1: the samples follow the reference decode's dark and light.
  const std::vector<uint8_t> pgm = readFile(JPEG_FIXTURE_DIR "/contrast_420.gray.pgm");
  const size_t header = pgm.size() - static_cast<size_t>(box.w) * box.h;
  int dark = 0;
  int light = 0;
  for (int y = 0; y < box.h; ++y) {
    for (int x = 0; x < box.w; ++x) {
      const uint8_t ref = pgm[header + static_cast<size_t>(y) * box.w + x];
      const uint8_t got = at(box.x + x, box.y + y);
      if (ref <= 32) {
        EXPECT_LE(got, 96) << "a dark reference pixel came out light at " << x << "," << y;
        ++dark;
      } else if (ref >= 224) {
        EXPECT_GE(got, 160) << "a light reference pixel came out dark at " << x << "," << y;
        ++light;
      }
    }
  }
  EXPECT_GT(dark, 100);
  EXPECT_GT(light, 100);
}

// Progressive goes through its own decoder and emits whole bands; the sink is the same.
TEST_F(Gray8PictureFixture, JpegProgressiveStoresSamples) {
  const Box box{0, 0, 157, 99};
  ASSERT_TRUE(decodeJpeg(JPEG_FIXTURE_DIR "/prog_full_gray.jpg", box, evenTone()));
  EXPECT_EQ(writtenIn(box), box.w * box.h);
  EXPECT_EQ(writtenOutside(box), 0);
  EXPECT_TRUE(frameBufferWhite());

  const std::vector<uint8_t> pgm = readFile(JPEG_FIXTURE_DIR "/prog_full_gray.y.pgm");
  const size_t header = pgm.size() - static_cast<size_t>(box.w) * box.h;
  long totalDiff = 0;
  for (int y = 0; y < box.h; ++y) {
    for (int x = 0; x < box.w; ++x) {
      totalDiff += std::abs(static_cast<int>(at(x, y)) - pgm[header + static_cast<size_t>(y) * box.w + x]);
    }
  }
  EXPECT_LT(totalDiff, 2L * box.w * box.h) << "mean difference from the reference luma, per pixel";
}

// The writer's tone applies to every sample, and nothing else changes.
TEST_F(Gray8PictureFixture, JpegSamplesTakeTheWritersTone) {
  const Box box{10, 20, 96, 64};
  ASSERT_TRUE(decodeJpeg(JPEG_FIXTURE_DIR "/contrast_420.jpg", box));
  const std::vector<uint8_t> plain = canvas;
  clearCanvas();
  ASSERT_TRUE(decodeJpeg(JPEG_FIXTURE_DIR "/contrast_420.jpg", box, invertingTone().data()));
  for (int y = box.y; y < box.y + box.h; ++y) {
    for (int x = box.x; x < box.x + box.w; ++x) {
      ASSERT_EQ(at(x, y), invertingTone()[plain[static_cast<size_t>(y) * kStride + x]]) << x << "," << y;
    }
  }
}

// A JPEG no decoder here takes gets its framed gap in the B/W pass. Asked for its gray it fails instead, and writes
// nothing: the caller keeps what the B/W pass drew.
TEST_F(Gray8PictureFixture, JpegOfAnUnsupportedModeFailsAndWritesNothing) {
  std::vector<uint8_t> bytes = readFile(JPEG_FIXTURE_DIR "/contrast_420.jpg");
  bool patched = false;
  for (size_t i = 0; i + 1 < bytes.size(); ++i) {
    if (bytes[i] == 0xFF && bytes[i + 1] == 0xC0) {  // SOF0 -> SOF3, lossless
      bytes[i + 1] = 0xC3;
      patched = true;
      break;
    }
  }
  ASSERT_TRUE(patched);
  const fs::path lossless = work / "lossless.jpg";
  writeFile(lossless, bytes);
  EXPECT_FALSE(decodeJpeg(lossless.string(), {10, 20, 96, 64}));
  EXPECT_TRUE(untouched());
}

// --- A picture -----------------------------------------------------------------------------------------------------

// Where the B/W pass had no pixel cache to draw from it drew a placeholder: that stays, and nothing is decoded.
TEST_F(Gray8PictureFixture, APictureWithoutItsPixelCacheIsLeftAlone) {
  ImageBlock image((work / "img.png").string(), 120, 160, "", kPngBook, "OEBPS/images/scaling_test.png");
  EXPECT_FALSE(image.renderGray8(renderer, 10, 20, canvas.data(), kStride, nullptr));
  EXPECT_TRUE(untouched());
  EXPECT_EQ(renderer.bwRectsComposited, 0);
  EXPECT_FALSE(fs::exists(work / "img.png")) << "not even extracted";
}

// A PNG over exactly its box, through the tone, the framebuffer and the caches as they were.
TEST_F(Gray8PictureFixture, APngPictureFillsItsBox) {
  ImageBlock image((work / "img.png").string(), 120, 160, "", kPngBook, "OEBPS/images/scaling_test.png");
  warm(image, 10, 20);
  const Box box{10, 20, 120, 160};
  ASSERT_TRUE(image.renderGray8(renderer, box.x, box.y, canvas.data(), kStride, evenTone()));
  EXPECT_EQ(writtenIn(box), box.w * box.h);
  EXPECT_EQ(writtenOutside(box), 0);
  EXPECT_TRUE(frameBufferWhite());
  EXPECT_FALSE(fs::exists(work / "img.bayer.pxc")) << "the gray decode writes no cache";
}

TEST_F(Gray8PictureFixture, JpegPicturesFillTheirBoxes) {
  ImageBlock baseline((work / "base.jpg").string(), 175, 125, "", kJpegBook, "OEBPS/images/jpeg_format.jpg");
  warm(baseline, 10, 20);
  ASSERT_TRUE(baseline.renderGray8(renderer, 10, 20, canvas.data(), kStride, evenTone()));
  EXPECT_EQ(writtenIn({10, 20, 175, 125}), 175 * 125);

  clearCanvas();
  ImageBlock progressive((work / "prog.jpg").string(), 160, 200, "", kJpegBook, "OEBPS/images/gradient_test.jpg");
  warm(progressive, 200, 30);
  ASSERT_TRUE(progressive.renderGray8(renderer, 200, 30, canvas.data(), kStride, evenTone()));
  EXPECT_EQ(writtenIn({200, 30, 160, 200}), 160 * 200);
  EXPECT_EQ(writtenOutside({200, 30, 160, 200}), 0);
  EXPECT_TRUE(frameBufferWhite());
}

// A slice of a picture split over pages holds that slice's rows of the whole picture, and nothing past it.
TEST_F(Gray8PictureFixture, ASplitPictureKeepsItsOwnSlice) {
  ImageBlock image((work / "img.png").string(), 120, 160, "", kPngBook, "OEBPS/images/scaling_test.png");
  warm(image, 10, 20);
  ASSERT_TRUE(image.renderGray8(renderer, 10, 20, canvas.data(), kStride, evenTone()));
  const std::vector<uint8_t> whole = canvas;

  constexpr int kFrom = 60;
  constexpr int kRows = 50;
  const auto piece = image.makeCrop(kFrom, kRows);
  clearCanvas();
  const Box box{200, 300, 120, kRows};
  ASSERT_TRUE(piece->renderGray8(renderer, box.x, box.y, canvas.data(), kStride, evenTone()));
  EXPECT_EQ(writtenOutside(box), 0) << "rows of the picture outside its slice";
  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < box.w; ++col) {
      ASSERT_EQ(at(box.x + col, box.y + row), whole[static_cast<size_t>(20 + kFrom + row) * kStride + 10 + col])
          << "slice row " << row << ", column " << col;
    }
  }
}

// A GIF has no gray decode: it keeps its dither, and nothing is put back because nothing was written.
TEST_F(Gray8PictureFixture, AGifKeepsItsDither) {
  const fs::path gif = work / "pic.gif";
  std::vector<uint8_t> bytes = {'G', 'I', 'F', '8', '9', 'a'};
  bytes.resize(64, 0);
  writeFile(gif, bytes);
  fakePixelCache(gif);
  ImageBlock image(gif.string(), 40, 30);
  EXPECT_FALSE(image.renderGray8(renderer, 10, 20, canvas.data(), kStride, nullptr));
  EXPECT_TRUE(untouched());
  EXPECT_EQ(renderer.bwRectsComposited, 0);
}

// A decode that fails part way has written some rows; the box gets back what the B/W pass drew there.
TEST_F(Gray8PictureFixture, AFailedDecodePutsTheDitherBack) {
  std::vector<uint8_t> bytes = readFile(JPEG_FIXTURE_DIR "/thin_lines_gray.jpg");
  bytes.resize(bytes.size() * 2 / 3);  // cut inside the scan
  const fs::path cut = work / "cut.jpg";
  writeFile(cut, bytes);
  fakePixelCache(cut);
  const Box box{10, 20, 300, 16};
  // What the B/W pass drew in the box: a few black dots.
  fillFrameBuffer(0xFF);
  for (int x = box.x; x < box.x + box.w; x += 7)
    renderer.getWriteTarget()[(box.y + 3) * (kPanelW / 8) + (x >> 3)] &= ~(0x80 >> (x & 7));

  ImageBlock image(cut.string(), box.w, box.h);
  EXPECT_FALSE(image.renderGray8(renderer, box.x, box.y, canvas.data(), kStride, nullptr));
  EXPECT_EQ(renderer.bwRectsComposited, 1);
  for (int y = box.y; y < box.y + box.h; ++y) {
    for (int x = box.x; x < box.x + box.w; ++x) {
      ASSERT_EQ(at(x, y), black(x, y) ? 0x00 : 0xFF) << x << "," << y;
    }
  }
  EXPECT_EQ(writtenOutside(box), 0);
}

// A picture of a vertical page is turned: it is drawn upright in the reader's frame, and its gray lands on the same
// panel pixels as its B/W dither.
TEST_F(Gray8PictureFixture, ATurnedPictureLandsWhereItsDitherDoes) {
  renderer.setOrientation(GfxRenderer::Portrait);  // the reader's frame
  renderer.setDrawOrientation(
      static_cast<GfxRenderer::Orientation>(turnedframe::contentOf(GfxRenderer::Portrait)));  // the page's
  ImageBlock image((work / "img.png").string(), 160, 120, "", kPngBook, "OEBPS/images/scaling_test.png");
  image.setTurned();
  constexpr int kX = 40;
  constexpr int kY = 30;
  image.render(renderer, kX, kY, /*forceLoad=*/true, /*monochromeOutput=*/true);
  ASSERT_TRUE(image.hasPixelCache());

  // The physical box: the content rect in the reader's frame, then Portrait's transform (phyX = y, phyY = 799 - x).
  const turnedframe::Rect reader = turnedframe::toReader(renderer.getScreenWidth(GfxRenderer::Portrait),
                                                         {kX, kY, image.getWidth(), image.getHeight()});
  const Box physical{reader.y, kPanelH - reader.x - reader.width, reader.height, reader.width};
  int blackOutside = 0;
  int blackInside = 0;
  for (int y = 0; y < kPanelH; ++y) {
    for (int x = 0; x < kPanelW; ++x) {
      if (!black(x, y)) continue;
      (physical.contains(x, y) ? blackInside : blackOutside)++;
    }
  }
  ASSERT_GT(blackInside, 0) << "the B/W pass drew the picture";
  ASSERT_EQ(blackOutside, 0) << "the expected box is not where the B/W pass drew";

  ASSERT_TRUE(image.renderGray8(renderer, kX, kY, canvas.data(), kStride, evenTone()));
  EXPECT_EQ(writtenIn(physical), physical.w * physical.h);
  EXPECT_EQ(writtenOutside(physical), 0);
  EXPECT_EQ(renderer.getOrientation(), turnedframe::contentOf(GfxRenderer::Portrait)) << "the live frame comes back";
}

// --- A page --------------------------------------------------------------------------------------------------------

// Every picture of the page is offered, a table cell's too, each at the place render() draws it.
TEST_F(Gray8PictureFixture, APageOffersEveryPictureATableCellsToo) {
  const std::string path = (work / "img.png").string();
  ImageBlock warmed(path, 60, 80, "", kPngBook, "OEBPS/images/scaling_test.png");
  warm(warmed, 0, 0);

  Page page;
  page.elements.push_back(std::make_unique<PageImage>(
      std::make_unique<ImageBlock>(path, 60, 80, "", kPngBook, "OEBPS/images/scaling_test.png"), 10, 20));
  constexpr uint16_t kRowHeight = 80 + 2 * TABLE_CELL_PADDING;
  std::vector<TableRow> rows(1);
  rows[0].height = kRowHeight;
  rows[0].cells.resize(2);
  rows[0].cells[1].image = std::make_unique<ImageBlock>(path, 60, 80, "", kPngBook, "OEBPS/images/scaling_test.png");
  page.elements.push_back(std::make_unique<PageTableFragment>(2, 200, kRowHeight, std::move(rows), 100, 150));
  // A picture the B/W pass drew as a placeholder: offered, and left alone.
  page.elements.push_back(std::make_unique<PageImage>(
      std::make_unique<ImageBlock>((work / "other.png").string(), 60, 80, "", kPngBook, "OEBPS/images/png_format.png"),
      300, 20));

  const Gray8Pictures count = page.renderImagesGray8(renderer, 0, 5, 7, canvas.data(), kStride, evenTone());
  EXPECT_EQ(count.offered, 3);
  EXPECT_EQ(count.inGray, 2);
  const Box picture{10 + 5, 20 + 7, 60, 80};
  // The cell's picture: the table's second column starts halfway across its 200 px, below the cell padding.
  const Box cell{100 + 5 + 100 + TABLE_CELL_PADDING, 150 + 7 + TABLE_CELL_PADDING, 60, 80};
  EXPECT_EQ(writtenIn(picture), picture.w * picture.h);
  EXPECT_EQ(writtenIn(cell), cell.w * cell.h);
  int stray = 0;
  for (int y = 0; y < kPanelH; ++y) {
    for (int x = 0; x < kPanelW; ++x) stray += !picture.contains(x, y) && !cell.contains(x, y) && at(x, y) != kUnset;
  }
  EXPECT_EQ(stray, 0);
}

}  // namespace
