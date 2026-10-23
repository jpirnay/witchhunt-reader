// JPEG page images that cannot be decoded whole.
//
// A decode that fails after it has drawn rows -- a baseline file cut short -- or finishes short of
// what the file should hold (ProgressiveJpeg's Degraded: a corrupt scan, a stream that ends early)
// keeps the rows it drew. The rest of the box is page background, no placeholder frame goes over
// the picture, and nothing is cached: the next visit decodes again, so a file that is whole by then
// is drawn whole. A decode that fails before its first row behaves as before.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CooperativeAbort.h"
#include "Epub/converters/JpegToFramebufferConverter.h"
#include "Epub/converters/PixelCache.h"
#include "GfxRenderer.h"
#include "HalStorage.h"

namespace fs = std::filesystem;

namespace {

// Fires on the Nth poll of the abort hook (1-based); 0 never fires.
int gPollsLeft = 0;
int gPolls = 0;
bool countedAbort() {
  ++gPolls;
  if (gPollsLeft <= 0) return false;
  return --gPollsLeft == 0;
}

// What a decode drew into its box: one byte per pixel, 1 where it drew black (the BW plane).
struct Drawn {
  bool ok = false;
  int rects = 0;  // frames drawn: the placeholder's
  int w = 0;
  int h = 0;
  std::vector<uint8_t> black;

  bool rowHasInk(const int y) const {
    for (int x = 0; x < w; ++x) {
      if (black[static_cast<size_t>(y) * w + x]) return true;
    }
    return false;
  }
  // Rows from the top two drawings share.
  int sameLeadingRows(const Drawn& other) const {
    int rows = 0;
    while (rows < h &&
           std::equal(black.begin() + rows * w, black.begin() + (rows + 1) * w, other.black.begin() + rows * other.w)) {
      ++rows;
    }
    return rows;
  }
};

struct JpegPartialDecode : testing::Test {
  fs::path work;

  void SetUp() override {
    const std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
    work = fs::temp_directory_path() / ("jpg_partial_" + std::to_string(std::hash<std::string>{}(name)));
    fs::remove_all(work);
    fs::create_directories(work);
    CooperativeAbort::setLongTaskAbortPredicate(countedAbort);
    gPollsLeft = 0;
    gPolls = 0;
    CooperativeAbort::clearAborted();
  }
  void TearDown() override {
    CooperativeAbort::setLongTaskAbortPredicate(nullptr);
    CooperativeAbort::clearAborted();
    fs::remove_all(work);
  }

  std::string at(const std::string& name) const { return (work / name).string(); }
  static std::string fixture(const char* name) { return std::string(JPEG_FIXTURE_DIR "/") + name; }

  // The reader's configuration: the 1-bit variant drawn, the 4-level one as its companion.
  RenderConfig config(const std::string& tag, const bool checkpoint = false) const {
    RenderConfig c;
    c.x = 12;
    c.y = 30;
    c.maxWidth = 150;
    c.maxHeight = 104;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = at(tag + "_bw.pxc");
    c.companionCachePath = at(tag + "_grey.pxc");
    if (checkpoint) c.checkpointPath = at(tag + ".ckpt");
    return c;
  }

  // One decode onto a white page; `abortOnPoll` stops it for input at that poll (0: never).
  static Drawn draw(const std::string& jpg, const RenderConfig& c, const int abortOnPoll = 0) {
    auto renderer = std::make_unique<GfxRenderer>();
    uint8_t* const frame = renderer->getWriteTarget();
    memset(frame, 0xFF, static_cast<size_t>(renderer->getDisplayWidthBytes()) * renderer->getDisplayHeight());
    gPollsLeft = abortOnPoll;
    CooperativeAbort::clearAborted();
    Drawn out;
    JpegToFramebufferConverter converter;
    out.ok = converter.decodeToFramebuffer(jpg, *renderer, c);
    gPollsLeft = 0;
    out.rects = renderer->rectsDrawn;
    out.w = c.maxWidth;
    out.h = c.maxHeight;
    out.black.resize(static_cast<size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
      for (int x = 0; x < out.w; ++x) {
        // Portrait: logical (x, y) is physical (y, 799 - x); a cleared bit is black.
        const int phyX = c.y + y;
        const int phyY = 799 - (c.x + x);
        const uint8_t byte = frame[phyY * renderer->getDisplayWidthBytes() + phyX / 8];
        out.black[static_cast<size_t>(y) * out.w + x] = (byte & (1 << (7 - (phyX & 7)))) == 0;
      }
    }
    return out;
  }

  // The first `length` bytes of a fixture, as a file in the work directory.
  std::string cutCopy(const char* name, const size_t length) const {
    std::ifstream in(fixture(name), std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    bytes.resize(std::min(length, bytes.size()));
    const std::string path = at(std::string("cut_") + name);
    std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
  }
  static size_t sizeOf(const char* name) { return static_cast<size_t>(fs::file_size(fixture(name))); }

  void expectNothingCached(const RenderConfig& c) const {
    EXPECT_FALSE(fs::exists(c.cachePath)) << "a picture short of its file is not cached";
    EXPECT_FALSE(fs::exists(c.companionCachePath));
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.companionCachePath)));
    if (!c.checkpointPath.empty()) EXPECT_FALSE(fs::exists(c.checkpointPath));
  }
};

TEST_F(JpegPartialDecode, AProgressiveFileCutInItsLastScanIsDrawnWholeAndNotCached) {
  const Drawn clean = draw(fixture("prog_full_420.jpg"), config("clean"));
  ASSERT_TRUE(clean.ok);
  ASSERT_TRUE(fs::exists(config("clean").cachePath)) << "control: a whole decode is cached";

  const RenderConfig c = config("cut");
  const Drawn cut = draw(fixture("prog_420_cut.jpg"), c);
  EXPECT_TRUE(cut.ok);
  EXPECT_EQ(cut.rects, 0) << "no placeholder frame over the picture";
  EXPECT_TRUE(cut.rowHasInk(cut.h - 1)) << "every row is drawn: the cut only costs the last refinement below it";
  const int same = cut.sameLeadingRows(clean);
  EXPECT_GE(same, 10) << "the rows above the cut are the whole file's";
  EXPECT_LT(same, cut.h) << "the cut must cost something, or this test proves nothing";
  expectNothingCached(c);
}

TEST_F(JpegPartialDecode, ABaselineFileCutShortKeepsItsTopRows) {
  const Drawn clean = draw(fixture("prog_full_420_base.jpg"), config("clean"));
  ASSERT_TRUE(clean.ok);
  ASSERT_TRUE(clean.rowHasInk(clean.h - 1));

  const RenderConfig c = config("cut");
  const Drawn cut = draw(cutCopy("prog_full_420_base.jpg", sizeOf("prog_full_420_base.jpg") / 2), c);
  EXPECT_TRUE(cut.ok) << "rows were drawn: the image is there, if not all of it";
  EXPECT_EQ(cut.rects, 0) << "no placeholder frame over the picture";
  const int same = cut.sameLeadingRows(clean);
  EXPECT_GE(same, 10) << "the rows decoded before the failure stay";
  EXPECT_LT(same, cut.h);
  EXPECT_FALSE(cut.rowHasInk(cut.h - 1)) << "the rest is page background";
  expectNothingCached(c);
}

TEST_F(JpegPartialDecode, ABaselineFileThatFailsBeforeItsFirstRowStillFails) {
  const RenderConfig c = config("early");
  const Drawn cut = draw(cutCopy("prog_full_420_base.jpg", 700), c);  // headers whole, entropy data barely begun
  EXPECT_FALSE(cut.ok);
  EXPECT_FALSE(cut.rowHasInk(0));
  expectNothingCached(c);
}

// The reader's image lane parks a decode it cannot finish and the next decode resumes it (see
// ImageParkTest). One that resumes and then ends degraded publishes no cache -- so the rows from
// before the park, which only its partial cache holds, are drawn from there first: the page shows
// what an uninterrupted decode would have drawn.
TEST_F(JpegPartialDecode, AResumedDecodeThatEndsDegradedDrawsTheRowsFromBeforeItsPark) {
  const std::string jpg = fixture("prog_420_cut.jpg");
  const RenderConfig ref = config("ref", /*checkpoint=*/true);
  gPolls = 0;
  const Drawn whole = draw(jpg, ref);
  ASSERT_TRUE(whole.ok);
  ASSERT_FALSE(CooperativeAbort::consumeAborted());
  expectNothingCached(ref);
  const int polls = gPolls;
  ASSERT_GT(polls, 3) << "the fixture must be stoppable at several points";

  for (int stop = 2; stop <= polls; ++stop) {
    SCOPED_TRACE("parked at poll " + std::to_string(stop));
    const RenderConfig c = config("p" + std::to_string(stop), /*checkpoint=*/true);
    (void)draw(jpg, c, stop);
    ASSERT_TRUE(CooperativeAbort::consumeAborted());
    ASSERT_TRUE(fs::exists(c.checkpointPath)) << "parked";
    const Drawn resumed = draw(jpg, c);
    EXPECT_TRUE(resumed.ok);
    EXPECT_EQ(resumed.rects, 0);
    EXPECT_TRUE(resumed.black == whole.black) << "the page shows the whole decode's rows";
    expectNothingCached(c);
  }
}

}  // namespace
