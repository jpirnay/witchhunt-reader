// JPEG page images that cannot be decoded whole.
//
// A decode that ends short of what the file should hold -- ProgressiveJpeg's Degraded (a corrupt
// scan, a stream that ends early), or a baseline file whose data is corrupt or cut short -- keeps
// the rows it drew, the rest of the box is page background, and no placeholder frame goes over the
// picture. That picture is as final as a whole one: an image's bytes (an extract of the book) do not
// change, so decoding again gives the same rows. It is cached like a whole picture, and every later
// render of the page draws it from the cache. Before, it was decoded again in every render pass of
// every visit (8 decodes, ~14 s, for one page on the X4) and the image lane never counted the page
// as warmed.
//
// A read the card failed is no damage of the file: the rows so far are drawn, nothing is cached,
// and the next visit decodes again. A decode that fails before its first row behaves as before.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CooperativeAbort.h"
#include "Epub/Page.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/JpegToFramebufferConverter.h"
#include "Epub/converters/PixelCache.h"
#include "GfxRenderer.h"
#include "HalStorage.h"

namespace fs = std::filesystem;

namespace {

// Fires on the Nth poll of the abort hook (1-based); 0 never fires. A decode polls it before every
// band (progressive) or MCU row (a checkpointable baseline decode), so the polls count decodes.
int gPollsLeft = 0;
int gPolls = 0;
// From the Nth poll on (1-based; 0: never) every read fails, as on a card that stopped answering.
int gFailReadsAtPoll = 0;
// On the Nth poll (1-based; 0: never) the next read fails, and only that one.
int gFailOneReadAtPoll = 0;
bool countedAbort() {
  ++gPolls;
  if (gFailReadsAtPoll > 0 && gPolls == gFailReadsAtPoll) FsFile::failReadsFrom = 0;
  if (gFailOneReadAtPoll > 0 && gPolls == gFailOneReadAtPoll) FsFile::failOneReadFrom = 0;
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
    gFailReadsAtPoll = 0;
    gFailOneReadAtPoll = 0;
    CooperativeAbort::clearAborted();
  }
  void TearDown() override {
    CooperativeAbort::setLongTaskAbortPredicate(nullptr);
    CooperativeAbort::clearAborted();
    FsFile::failReadsFrom = -1;
    FsFile::failOneReadFrom = -1;
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

  // What a renderer drew into the box of `c`.
  static Drawn inBox(GfxRenderer& renderer, const RenderConfig& c) {
    Drawn out;
    out.rects = renderer.rectsDrawn;
    out.w = c.maxWidth;
    out.h = c.maxHeight;
    out.black.resize(static_cast<size_t>(out.w) * out.h);
    const uint8_t* const frame = renderer.getWriteTarget();
    for (int y = 0; y < out.h; ++y) {
      for (int x = 0; x < out.w; ++x) {
        // Portrait: logical (x, y) is physical (y, 799 - x); a cleared bit is black.
        const int phyX = c.y + y;
        const int phyY = 799 - (c.x + x);
        const uint8_t byte = frame[phyY * renderer.getDisplayWidthBytes() + phyX / 8];
        out.black[static_cast<size_t>(y) * out.w + x] = (byte & (1 << (7 - (phyX & 7)))) == 0;
      }
    }
    return out;
  }

  static std::unique_ptr<GfxRenderer> whitePage() {
    auto renderer = std::make_unique<GfxRenderer>();
    memset(renderer->getWriteTarget(), 0xFF,
           static_cast<size_t>(renderer->getDisplayWidthBytes()) * renderer->getDisplayHeight());
    return renderer;
  }

  // One decode onto a white page; `abortOnPoll` stops it for input at that poll (0: never).
  static Drawn draw(const std::string& jpg, const RenderConfig& c, const int abortOnPoll = 0) {
    auto renderer = whitePage();
    gPollsLeft = abortOnPoll;
    CooperativeAbort::clearAborted();
    JpegToFramebufferConverter converter;
    const bool ok = converter.decodeToFramebuffer(jpg, *renderer, c);
    gPollsLeft = 0;
    Drawn out = inBox(*renderer, c);
    out.ok = ok;
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

  static std::vector<uint8_t> read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  void expectNothingCached(const RenderConfig& c) const {
    EXPECT_FALSE(fs::exists(c.cachePath)) << "a picture a failed read cut short is not cached";
    EXPECT_FALSE(fs::exists(c.companionCachePath));
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.companionCachePath)));
    if (!c.checkpointPath.empty()) {
      EXPECT_FALSE(fs::exists(c.checkpointPath));
    }
  }

  // Both variants published whole, nothing left over.
  void expectCached(const RenderConfig& c) const {
    const size_t whole = PixelCache::PXC_HEADER_BYTES + static_cast<size_t>((c.maxWidth + 3) / 4) * c.maxHeight;
    ASSERT_TRUE(fs::exists(c.cachePath)) << "a damaged picture is cached like a whole one";
    EXPECT_EQ(fs::file_size(c.cachePath), whole);
    ASSERT_TRUE(fs::exists(c.companionCachePath));
    EXPECT_EQ(fs::file_size(c.companionCachePath), whole);
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.companionCachePath)));
    if (!c.checkpointPath.empty()) {
      EXPECT_FALSE(fs::exists(c.checkpointPath));
    }
  }
};

TEST_F(JpegPartialDecode, AProgressiveFileCutInItsLastScanIsDrawnWholeAndCached) {
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
  expectCached(c);
}

TEST_F(JpegPartialDecode, ABaselineFileCutShortKeepsItsTopRowsAndIsCached) {
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
  expectCached(c);
  // ...in the cache too: its last row is the value every renderer leaves alone (PixelCache::FILL_BYTE).
  const std::vector<uint8_t> bw = read(c.cachePath);
  const size_t rowBytes = static_cast<size_t>((c.maxWidth + 3) / 4);
  ASSERT_GE(bw.size(), rowBytes);
  EXPECT_TRUE(std::all_of(bw.end() - static_cast<std::ptrdiff_t>(rowBytes), bw.end(),
                          [](const uint8_t b) { return b == PixelCache::FILL_BYTE; }));
}

// A read the card fails partway is no damage of the file: what was drawn stays, nothing is cached.
TEST_F(JpegPartialDecode, AReadFailurePartwayKeepsTheRowsButCachesNothing) {
  struct Case {
    const char* jpg;
    int failAtPoll;  // a band (progressive) or MCU row (baseline) about a third of the way down
  };
  for (const Case& k : {Case{"prog_420_cut.jpg", 4}, Case{"prog_full_420.jpg", 4}, Case{"prog_full_420_base.jpg", 4}}) {
    SCOPED_TRACE(k.jpg);
    const Drawn whole = draw(fixture(k.jpg), config(std::string("whole_") + k.jpg));
    ASSERT_TRUE(whole.ok);
    // With a checkpoint path a baseline decode polls between its MCU rows too.
    const RenderConfig c = config(std::string("failed_") + k.jpg, /*checkpoint=*/true);
    gPolls = 0;
    gFailReadsAtPoll = k.failAtPoll;
    const Drawn failed = draw(fixture(k.jpg), c);
    gFailReadsAtPoll = 0;
    FsFile::failReadsFrom = -1;
    ASSERT_GE(gPolls, k.failAtPoll) << "the fault must strike, or this test proves nothing";
    EXPECT_TRUE(failed.ok);
    EXPECT_EQ(failed.rects, 0);
    EXPECT_GE(failed.sameLeadingRows(whole), 10) << "the rows drawn before the failure stay";
    EXPECT_FALSE(failed.rowHasInk(failed.h - 1)) << "the rest is page background";
    expectNothingCached(c);
  }
}

TEST_F(JpegPartialDecode, ABaselineFileThatFailsBeforeItsFirstRowStillFails) {
  const RenderConfig c = config("early");
  const Drawn cut = draw(cutCopy("prog_full_420_base.jpg", 700), c);  // headers whole, entropy data barely begun
  EXPECT_FALSE(cut.ok);
  EXPECT_FALSE(cut.rowHasInk(0));
  expectNothingCached(c);
}

// The page around such a picture, as the reader handles it. The image lane warms the page -- both
// variants -- and finds it complete, so it neither retries it nor gives up on it ("incomplete, will
// retry once"). Every render after that draws the picture from its cache: no decode, and the JPEG is
// not read.
TEST_F(JpegPartialDecode, APageWithADamagedPictureIsWarmedOnceAndThenDrawnFromItsCache) {
  struct Case {
    const char* name;
    std::string source;
  };
  const Case cases[] = {{"progressive", fixture("prog_420_cut.jpg")},
                        {"baseline", cutCopy("prog_full_420_base.jpg", sizeOf("prog_full_420_base.jpg") / 2)}};
  for (const Case& k : cases) {
    SCOPED_TRACE(k.name);
    const std::string jpg = at(std::string(k.name) + ".jpg");
    fs::copy_file(k.source, jpg, fs::copy_options::overwrite_existing);
    Page page;
    page.elements.push_back(std::make_unique<PageImage>(std::make_unique<ImageBlock>(jpg, 150, 104), 12, 30));
    ASSERT_TRUE(page.hasUncachedImages(/*forceLoad=*/true, /*monochrome=*/true, /*alsoWarmGrayscale=*/true));

    gPolls = 0;
    {
      auto renderer = whitePage();
      page.warmImageCaches(*renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true,
                           /*alsoWarmGrayscale=*/true);
    }
    EXPECT_GT(gPolls, 0) << "the warm decoded the picture";
    EXPECT_FALSE(page.hasUncachedImages(true, true, true)) << "the lane's warm is complete";

    auto& image = static_cast<PageImage&>(*page.elements[0]);
    EXPECT_TRUE(image.getImageBlock().hasPixelCache());
    EXPECT_TRUE(image.getImageBlock().hasGrayscaleCache());

    // The page render's passes: the 1-bit one, and the grey one anti-aliasing draws.
    for (const bool monochrome : {true, false}) {
      SCOPED_TRACE(monochrome ? "1-bit pass" : "grey pass");
      gPolls = 0;
      const size_t bytesBefore = FsFile::bytesRead;
      auto renderer = whitePage();
      image.renderWithForceLoad(*renderer, 0, 0, /*forceLoad=*/true, monochrome);
      EXPECT_EQ(gPolls, 0) << "drawn from the cache, not decoded";
      EXPECT_LT(FsFile::bytesRead - bytesBefore, fs::file_size(jpg)) << "the JPEG is not read again";
      if (monochrome) {
        const Drawn shown = inBox(*renderer, config("shown"));
        EXPECT_TRUE(std::find(shown.black.begin(), shown.black.end(), 1) != shown.black.end())
            << "the picture is drawn";
      }
    }
  }
}

// The reader's image lane parks a decode it cannot finish and the next decode resumes it (see
// ImageParkTest). One that resumes and then ends degraded publishes the caches the uninterrupted
// decode would have: the rows from before the park are in them, and the page is drawn from them.
TEST_F(JpegPartialDecode, AResumedDecodeThatEndsDegradedPublishesTheUninterruptedCaches) {
  const std::string jpg = fixture("prog_420_cut.jpg");
  const RenderConfig ref = config("ref", /*checkpoint=*/true);
  gPolls = 0;
  const Drawn whole = draw(jpg, ref);
  ASSERT_TRUE(whole.ok);
  ASSERT_FALSE(CooperativeAbort::consumeAborted());
  expectCached(ref);
  const std::vector<uint8_t> refBw = read(ref.cachePath);
  const std::vector<uint8_t> refGrey = read(ref.companionCachePath);
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
    expectCached(c);
    EXPECT_EQ(read(c.cachePath), refBw);
    EXPECT_EQ(read(c.companionCachePath), refGrey);
  }
}

// ...and one whose reads fail after it resumed publishes nothing, so the rows from before its park,
// which only its partial cache holds, are drawn from there: the page shows what was decoded.
TEST_F(JpegPartialDecode, AResumedDecodeWhoseReadFailsDrawsTheRowsFromBeforeItsPark) {
  const std::string jpg = fixture("prog_420_cut.jpg");
  const RenderConfig ref = config("ref", /*checkpoint=*/true);
  gPolls = 0;
  const Drawn whole = draw(jpg, ref);
  ASSERT_TRUE(whole.ok);
  const int polls = gPolls;
  ASSERT_GT(polls, 3);

  for (int stop = 2; stop <= polls; ++stop) {
    SCOPED_TRACE("parked at poll " + std::to_string(stop));
    const RenderConfig c = config("p" + std::to_string(stop), /*checkpoint=*/true);
    (void)draw(jpg, c, stop);
    ASSERT_TRUE(CooperativeAbort::consumeAborted());
    ASSERT_TRUE(fs::exists(c.checkpointPath)) << "parked";
    // The resumed decode's first band read fails; nothing else does.
    gPolls = 0;
    gFailOneReadAtPoll = 1;
    const Drawn resumed = draw(jpg, c);
    gFailOneReadAtPoll = 0;
    FsFile::failOneReadFrom = -1;
    EXPECT_TRUE(resumed.ok);
    EXPECT_EQ(resumed.rects, 0);
    EXPECT_GT(resumed.sameLeadingRows(whole), 0) << "the rows from before the park are drawn";
    EXPECT_FALSE(resumed.rowHasInk(resumed.h - 1)) << "the decode drew nothing after it resumed";
    expectNothingCached(c);
  }
}

}  // namespace
