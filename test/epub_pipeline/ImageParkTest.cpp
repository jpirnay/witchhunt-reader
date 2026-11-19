// Parked image decodes (RenderConfig::checkpointPath).
//
// The reader's image lane hands its region back on every page turn, so a decode it cannot finish
// is stopped by CooperativeAbort. It used to be thrown away and started over -- in runs 18-21 (X3)
// the lane discarded 73 s of preempted decodes against 54 s it completed. Now the decode parks: the
// rows already done stay in the caches' .part files, a checkpoint holds the decoder's and the
// pipeline's state, and the next decode of the same image resumes.
//
// The guarantee tested here: however often a decode is stopped, and wherever, the caches it
// finally publishes are byte for byte those of an uninterrupted decode -- both variants, for the
// full progressive decoder and for TJpgDec.
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "CooperativeAbort.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/JpegToFramebufferConverter.h"
#include "Epub/converters/PixelCache.h"
#include "GfxRenderer.h"
#include "HalStorage.h"
#include "ProgressiveJpeg.h"

namespace fs = std::filesystem;

namespace {

// Fires on the Nth poll of the abort hook (1-based); 0 never fires.
int gPollsLeft = 0;
int gPolls = 0;
// When set, every poll appends its time: the gap between two polls of an uninterrupted decode is one
// band (progressive) or one MCU row (baseline) plus the cache write, on this host.
std::vector<std::chrono::steady_clock::time_point>* gPollStamps = nullptr;
bool countedAbort() {
  ++gPolls;
  if (gPollStamps) gPollStamps->push_back(std::chrono::steady_clock::now());
  if (gPollsLeft <= 0) return false;
  return --gPollsLeft == 0;
}

struct ImageParkFixture : testing::TestWithParam<const char*> {
  fs::path work;
  GfxRenderer renderer;

  void SetUp() override {
    const std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
    work = fs::temp_directory_path() / ("img_park_" + std::to_string(std::hash<std::string>{}(name)));
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

  std::string image() const { return std::string(JPEG_FIXTURE_DIR "/") + GetParam(); }
  std::string at(const std::string& name) const { return (work / name).string(); }

  // The reader's configuration: the 1-bit variant drawn, the 4-level one as its companion, a
  // downscale that area-averages (so the carry across rows of blocks is in play).
  RenderConfig config(const std::string& tag, const int width = 150, const int height = 104,
                      const bool checkpoint = true) const {
    RenderConfig c;
    c.x = 12;
    c.y = 30;
    c.maxWidth = width;
    c.maxHeight = height;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = at(tag + "_bw.pxc");
    c.companionCachePath = at(tag + "_grey.pxc");
    if (checkpoint) c.checkpointPath = at(tag + ".ckpt");
    return c;
  }

  // One decode call; true when it stopped for input (the lane's "preempted").
  bool decodeOnce(const RenderConfig& c, const int abortOnPoll) {
    gPollsLeft = abortOnPoll;
    CooperativeAbort::clearAborted();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(image(), renderer, c));
    gPollsLeft = 0;
    return CooperativeAbort::consumeAborted();
  }

  static std::vector<uint8_t> read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

TEST_P(ImageParkFixture, EveryParkPointResumesToTheUninterruptedCaches) {
  const RenderConfig ref = config("ref", 150, 104, /*checkpoint=*/false);
  ASSERT_FALSE(decodeOnce(ref, 0));
  const auto refBw = read(ref.cachePath);
  const auto refGrey = read(ref.companionCachePath);
  ASSERT_GT(refBw.size(), PixelCache::PXC_HEADER_BYTES);
  ASSERT_EQ(refBw.size(), refGrey.size());
  ASSERT_NE(refBw, refGrey);

  // How many times an uninterrupted checkpointable decode polls the hook: every one is a park point.
  const RenderConfig probe = config("probe");
  gPolls = 0;
  ASSERT_FALSE(decodeOnce(probe, 0));
  const int polls = gPolls;
  ASSERT_GT(polls, 3) << "the fixture must be stoppable at several points";
  EXPECT_EQ(read(probe.cachePath), refBw) << "checkpointing must not change an uninterrupted decode";

  for (int stop = 1; stop <= polls; ++stop) {
    SCOPED_TRACE("stopped at poll " + std::to_string(stop));
    const RenderConfig c = config("p" + std::to_string(stop));
    ASSERT_TRUE(decodeOnce(c, stop));
    EXPECT_FALSE(fs::exists(c.cachePath)) << "a parked decode publishes nothing";
    EXPECT_TRUE(fs::exists(c.checkpointPath));
    EXPECT_TRUE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    ASSERT_FALSE(decodeOnce(c, 0)) << "the resumed decode runs to the end";
    EXPECT_FALSE(fs::exists(c.checkpointPath)) << "a finished decode leaves no checkpoint";
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    EXPECT_EQ(read(c.cachePath), refBw);
    EXPECT_EQ(read(c.companionCachePath), refGrey);
  }
}

// A decode stopped again and again -- a reader flipping pages -- still ends with the same caches.
TEST_P(ImageParkFixture, RepeatedParksStillEndInTheSameCaches) {
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));
  const RenderConfig c = config("many");
  int parks = 0;
  while (decodeOnce(c, 2)) {  // every call makes one poll's worth of progress, then parks
    ++parks;
    ASSERT_LT(parks, 1000);
  }
  EXPECT_GT(parks, 1);
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(ref.companionCachePath));
}

// A key press between the lane's input check and a resume's first poll must not park the resume
// where it started: after 118-265 ms of setup (X3) that is no progress at all, again and again. A
// resume decodes one band (progressive) or kResumeMinRows MCU rows (baseline) before it polls.
// Here the hook fires on the FIRST poll of every call, i.e. input is already waiting. The clock
// ticks, as the lane's does.
TEST_P(ImageParkFixture, AResumeWithInputWaitingStillAdvancesBeforeItParks) {
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));
  const RenderConfig c = config("eager");
  host_clock::Ticking ticking;
  ASSERT_TRUE(decodeOnce(c, 3)) << "first call parks somewhere past its set-up";
  size_t lastPart = fs::file_size(PixelCache::partPathFor(c.cachePath));
  int parks = 0;
  for (;;) {
    if (!decodeOnce(c, 1)) break;
    ++parks;
    ASSERT_LT(parks, 400) << "a resume that polls first parks at once, forever";
    ASSERT_TRUE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    const size_t part = fs::file_size(PixelCache::partPathFor(c.cachePath));
    EXPECT_GE(part, lastPart);
    lastPart = part;
  }
  EXPECT_GT(parks, 0);
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(ref.companionCachePath));
  if (std::string(GetParam()).find("_base") != std::string::npos) {
    // 203x141 is 9 MCU rows of 16. The first call parked after 3 of them; a resume then decodes
    // kResumeMinRows (4) before it polls, so the 6 left take 2 resumes and one park in between. A
    // resume that decoded a single row before polling would park 5 times.
    EXPECT_LE(parks, 2) << "a baseline resume must decode kResumeMinRows MCU rows before it polls";
  }
}

// What a page turn can wait behind, on this host (not the device): the gap between two polls of an
// uninterrupted, checkpointable decode is one band (progressive) or one MCU row (baseline).
TEST_P(ImageParkFixture, TheWorstBandOrRowOnThisHost) {
  std::vector<std::chrono::steady_clock::time_point> stamps;
  gPollStamps = &stamps;
  const RenderConfig c = config("timed");
  EXPECT_FALSE(decodeOnce(c, 0));
  gPollStamps = nullptr;
  ASSERT_GT(stamps.size(), 4u);
  long worstUs = 0;
  long worstFourUs = 0;
  for (size_t i = 1; i < stamps.size(); ++i) {
    const auto us = [&](const size_t a, const size_t b) {
      return std::chrono::duration_cast<std::chrono::microseconds>(stamps[b] - stamps[a]).count();
    };
    worstUs = std::max<long>(worstUs, us(i - 1, i));
    if (i >= 4) worstFourUs = std::max<long>(worstFourUs, us(i - 4, i));
  }
  std::cout << "[ host band ] " << GetParam() << ": " << stamps.size() << " polls, worst gap between two polls "
            << worstUs << " us, worst four gaps " << worstFourUs << " us (host figures)\n";
}

// A decode parked in the index scan, which polls every 32 KB of the file, resumes there: the minimum
// progress after a resume is a band's, and the index has no band. With input waiting at every call,
// the resumes must park at the index's next poll, not run the index to its end (seconds of card
// reads for a 2 MB cover) and decode a band.
struct ImageParkIndexFixture : ImageParkFixture {};

TEST_P(ImageParkIndexFixture, AResumeInTheIndexScanStillStopsAtItsNextPoll) {
  ASSERT_GT(fs::file_size(image()), 3u * 32 * 1024) << "the fixture must have three index polls";
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));
  const RenderConfig c = config("index");
  host_clock::Ticking ticking;
  // The fresh call parks at the first index poll; the two resumes at the second and the third.
  ASSERT_TRUE(decodeOnce(c, 1));
  ASSERT_TRUE(fs::exists(c.checkpointPath));
  const size_t afterFirst = fs::file_size(PixelCache::partPathFor(c.cachePath));
  ASSERT_TRUE(decodeOnce(c, 1));
  ASSERT_TRUE(decodeOnce(c, 1));
  EXPECT_EQ(fs::file_size(PixelCache::partPathFor(c.cachePath)), afterFirst)
      << "no band was decoded while the index scan was still being resumed";
  int parks = 3;
  while (decodeOnce(c, 1)) ASSERT_LT(++parks, 400);
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(ref.companionCachePath));
}

INSTANTIATE_TEST_SUITE_P(IndexPolls, ImageParkIndexFixture, testing::Values("prog_noisy_420_index.jpg"));

// A checkpoint only resumes the decode it was taken of: another box starts over, cleanly.
TEST_P(ImageParkFixture, AChangedConfigurationStartsOver) {
  const RenderConfig small = config("ref_small", 120, 83, false);
  ASSERT_FALSE(decodeOnce(small, 0));

  RenderConfig c = config("changed");
  ASSERT_TRUE(decodeOnce(c, 3));
  ASSERT_TRUE(fs::exists(c.checkpointPath));
  c.maxWidth = 120;
  c.maxHeight = 83;
  ASSERT_FALSE(decodeOnce(c, 0));
  EXPECT_FALSE(fs::exists(c.checkpointPath));
  EXPECT_EQ(read(c.cachePath), read(small.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(small.companionCachePath));
}

// A partial cache that no longer matches its checkpoint (a crash between the two writes, a
// card error) fails that one decode and clears the way: the next one starts over.
TEST_P(ImageParkFixture, ADamagedPartialFileStartsOverNextTime) {
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));

  const RenderConfig c = config("damaged");
  ASSERT_TRUE(decodeOnce(c, 3));
  const std::string part = PixelCache::partPathFor(c.cachePath);
  fs::resize_file(part, fs::file_size(part) + 1);

  JpegToFramebufferConverter converter;
  EXPECT_FALSE(converter.decodeToFramebuffer(image(), renderer, c)) << "this decode fails";
  EXPECT_FALSE(fs::exists(c.checkpointPath));
  EXPECT_FALSE(fs::exists(part));
  EXPECT_FALSE(fs::exists(c.cachePath));

  ASSERT_FALSE(decodeOnce(c, 0));
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
}

// A pass that cannot cache (the heap gate refused one) must not throw a parked decode away: it
// draws without touching the parked partial files, and the next pass with room resumes. It used to
// discard the checkpoint, so a page render on a tight heap undid the image lane's work.
TEST_P(ImageParkFixture, APassWithoutRoomToCacheLeavesTheParkedDecode) {
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));

  const RenderConfig c = config("tight");
  ASSERT_TRUE(decodeOnce(c, 3));
  const std::string part = PixelCache::partPathFor(c.cachePath);
  ASSERT_TRUE(fs::exists(c.checkpointPath));
  const auto partBefore = read(part);
  const auto checkpointBefore = read(c.checkpointPath);

  // Room to decode (a progressive decode needs 16 KB), none to cache (band + 20 KB).
  const uint32_t savedHeap = ESP.getFreeHeap();
  ESP.setFreeHeap(18 * 1024);
  {
    JpegToFramebufferConverter converter;
    converter.decodeToFramebuffer(image(), renderer, c);  // baseline refuses outright below 28 KB
  }
  ESP.setFreeHeap(savedHeap);

  EXPECT_FALSE(fs::exists(c.cachePath)) << "nothing was cached";
  EXPECT_EQ(read(c.checkpointPath), checkpointBefore) << "the parked decode is left as it was";
  EXPECT_EQ(read(part), partBefore);

  ASSERT_FALSE(decodeOnce(c, 0)) << "the next pass with room resumes and finishes";
  EXPECT_FALSE(fs::exists(c.checkpointPath));
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(ref.companionCachePath));
}

// Without a checkpoint path a stopped progressive decode is thrown away, as before, and leaves
// nothing. TJpgDec polls only when it can park, so a baseline decode ignores the stop and runs to
// the end, as it always did.
TEST_P(ImageParkFixture, WithoutACheckpointPathADecodeDoesNotPark) {
  const RenderConfig c = config("nockpt", 150, 104, false);
  const bool stopped = decodeOnce(c, 2);
  if (std::string(GetParam()).find("_base") != std::string::npos) {
    EXPECT_FALSE(stopped) << "a baseline decode with nowhere to park must not stop";
    EXPECT_TRUE(fs::exists(c.cachePath));
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    return;
  }
  ASSERT_TRUE(stopped);
  EXPECT_FALSE(fs::exists(c.cachePath));
  EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
}

// prog_full_420_base_rst.jpg: baseline with a restart marker every 3 MCUs, so a parked TJpgDec
// decode resumes with the restart counters mid-interval and, every third row, on a boundary.
INSTANTIATE_TEST_SUITE_P(ProgressiveAndBaseline, ImageParkFixture,
                         testing::Values("prog_full_420.jpg", "prog_full_420_base.jpg", "prog_full_gray.jpg",
                                         "prog_full_420_base_rst.jpg"));

// Guards the restart fixture itself: restart markers only change the entropy coding, so the same
// picture with and without them must decode to the same caches -- else the park tests above would
// compare a resumed decode against a wrong reference and prove nothing.
TEST(ImageParkRestartFixture, RestartMarkersDecodeLikeTheirTwin) {
  const fs::path work = fs::temp_directory_path() / "img_park_rst_twin";
  fs::remove_all(work);
  fs::create_directories(work);
  GfxRenderer renderer;
  const auto decodeTo = [&](const char* jpg, const std::string& tag) {
    RenderConfig c;
    c.x = 12;
    c.y = 30;
    c.maxWidth = 150;
    c.maxHeight = 104;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = (work / (tag + "_bw.pxc")).string();
    c.companionCachePath = (work / (tag + "_grey.pxc")).string();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(std::string(JPEG_FIXTURE_DIR "/") + jpg, renderer, c));
    std::ifstream bw(c.cachePath, std::ios::binary);
    std::ifstream grey(c.companionCachePath, std::ios::binary);
    std::vector<uint8_t> out{std::istreambuf_iterator<char>(bw), std::istreambuf_iterator<char>()};
    out.insert(out.end(), std::istreambuf_iterator<char>(grey), std::istreambuf_iterator<char>());
    return out;
  };
  const auto plain = decodeTo("prog_full_420_base.jpg", "plain");
  ASSERT_GT(plain.size(), 2 * PixelCache::PXC_HEADER_BYTES);
  EXPECT_EQ(decodeTo("prog_full_420_base_rst.jpg", "rst"), plain);
  fs::remove_all(work);
}

// A progressive JPEG with room for no finer workspace than the 1/8 one is drawn from its 1/8 decode, resized: the cache
// is stamped coarse, for a pass with room to decode again, and the decode counts as short of memory and says what a
// finer one needs, so the reader's memory ladder can tell whether making room is worth it. A full decode is neither.
TEST(CoarseProgressiveJpeg, AnEighthForLackOfRoomIsStampedCoarseAndShortOfMemory) {
  const fs::path work = fs::temp_directory_path() / "img_coarse_preview";
  fs::remove_all(work);
  fs::create_directories(work);
  GfxRenderer renderer;
  const std::string jpg = JPEG_FIXTURE_DIR "/prog_full_420.jpg";
  ProgressiveJpeg::ImageInfo info;
  {
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("TEST", jpg, file));
    ASSERT_EQ(ProgressiveJpeg::Result::Ok, ProgressiveJpeg::probe(file, info));
    file.close();
  }
  const auto decodeMagic = [&](const std::string& tag) {
    RenderConfig c;
    c.maxWidth = 150;
    c.maxHeight = 104;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = (work / (tag + "_bw.pxc")).string();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(jpg, renderer, c));
    std::ifstream in(c.cachePath, std::ios::binary);
    uint16_t magic = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    return magic;
  };
  const uint32_t savedHeap = ESP.getFreeHeap();
  // A byte short of the 1/8 workspace and the 20 KB a finer one must leave (JPEG_CACHE_POST_GATE_BYTES +
  // JPEG_CACHE_HEAP_FLOOR): the 1/8 one, a few KB, asks for no floor, and its cache band fits still.
  const auto coarsest = static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 3));
  const auto finer = static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 2));
  ESP.setFreeHeap(coarsest + 20 * 1024 - 1);
  image_scratch::takeCoarseNeed();
  uint32_t shortBefore = image_scratch::heapShortCount();
  EXPECT_EQ(PixelCache::PXC_MAGIC_COARSE, decodeMagic("eighth"));
  EXPECT_NE(shortBefore, image_scratch::heapShortCount());
  const image_scratch::CoarseNeed need = image_scratch::takeCoarseNeed();
  EXPECT_EQ(finer, need.bytes) << "the next finer decode is 1/4's";
  EXPECT_EQ(20u * 1024, need.heapFloor);
  EXPECT_FALSE(image_scratch::coarseNeedFits(need)) << "no more room than the decode had";
  EXPECT_TRUE(image_scratch::coarseNeedFits(need, finer - coarsest + 1)) << "what is freed first counts toward it";
  ESP.setFreeHeap(savedHeap);

  shortBefore = image_scratch::heapShortCount();
  EXPECT_EQ(PixelCache::PXC_MAGIC, decodeMagic("full"));
  EXPECT_EQ(shortBefore, image_scratch::heapShortCount());
  EXPECT_EQ(0u, image_scratch::takeCoarseNeed().bytes);
  fs::remove_all(work);
}

// A decode that can wait for more room (beside a section build) settles for nothing: the same shortage that publishes a
// coarse picture above leaves no cache, since a coarse one would count as cached and stay.
TEST(CoarseProgressiveJpeg, ADecodeThatCanWaitLeavesTheCoarsePictureUnmade) {
  const fs::path work = fs::temp_directory_path() / "img_coarse_wait";
  fs::remove_all(work);
  fs::create_directories(work);
  GfxRenderer renderer;
  const std::string jpg = JPEG_FIXTURE_DIR "/prog_full_420.jpg";
  ProgressiveJpeg::ImageInfo info;
  {
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("TEST", jpg, file));
    ASSERT_EQ(ProgressiveJpeg::Result::Ok, ProgressiveJpeg::probe(file, info));
    file.close();
  }
  RenderConfig c;
  c.maxWidth = 150;
  c.maxHeight = 104;
  c.useExactDimensions = true;
  c.monochromeOutput = true;
  c.cachePath = (work / "bw.pxc").string();
  const uint32_t savedHeap = ESP.getFreeHeap();
  ESP.setFreeHeap(static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 3)) + 20 * 1024 - 1);
  image_scratch::takeCoarseNeed();
  const uint32_t shortBefore = image_scratch::heapShortCount();
  {
    image_scratch::ScopedCanWait canWait(true);
    JpegToFramebufferConverter converter;
    EXPECT_FALSE(converter.decodeToFramebuffer(jpg, renderer, c));
  }
  EXPECT_FALSE(image_scratch::canWait()) << "the scope gives the flag back";
  EXPECT_NE(shortBefore, image_scratch::heapShortCount()) << "short of memory, for the ladder";
  EXPECT_EQ(0u, image_scratch::takeCoarseNeed().bytes);
  EXPECT_FALSE(fs::exists(c.cachePath));
  ESP.setFreeHeap(savedHeap);
  fs::remove_all(work);
}

// The check the memory ladder makes before it frees memory for a coarse picture, and again before it decodes, is the
// decoder's own: where it says the finer workspace fits, the decode gets it; a byte short, the decode does not.
TEST(CoarseProgressiveJpeg, WhereTheFinerWorkspaceFitsTheDecodeIsFiner) {
  const fs::path work = fs::temp_directory_path() / "img_coarse_finer";
  fs::remove_all(work);
  fs::create_directories(work);
  GfxRenderer renderer;
  const std::string jpg = JPEG_FIXTURE_DIR "/prog_full_420.jpg";
  ProgressiveJpeg::ImageInfo info;
  {
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("TEST", jpg, file));
    ASSERT_EQ(ProgressiveJpeg::Result::Ok, ProgressiveJpeg::probe(file, info));
    file.close();
  }
  RenderConfig c;
  c.maxWidth = 150;
  c.maxHeight = 104;
  c.useExactDimensions = true;
  c.monochromeOutput = true;
  c.cachePath = (work / "bw.pxc").string();
  // The same image decoded again over its cache, as the light step does: what does the decode need next?
  const auto decodeNeed = [&](const uint32_t freeHeap) {
    ESP.setFreeHeap(freeHeap);
    image_scratch::takeCoarseNeed();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(jpg, renderer, c));
    return image_scratch::takeCoarseNeed();
  };
  const uint32_t savedHeap = ESP.getFreeHeap();
  const auto coarsest = static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 3));
  const auto finer = static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 2));
  const image_scratch::CoarseNeed need = decodeNeed(coarsest + 20 * 1024 - 1);  // the 1/8 decode, resized
  ESP.setFreeHeap(savedHeap);                                                   // restored before an ASSERT can return
  ASSERT_EQ(finer, need.bytes);

  ESP.setFreeHeap(need.bytes + need.heapFloor - 1);
  EXPECT_FALSE(image_scratch::coarseNeedFits(need));
  EXPECT_EQ(finer, decodeNeed(need.bytes + need.heapFloor - 1).bytes) << "a byte short: 1/8 again";

  ESP.setFreeHeap(need.bytes + need.heapFloor);
  EXPECT_TRUE(image_scratch::coarseNeedFits(need));
  {
    const HostLargestFreeBlock fragmented(need.bytes);
    EXPECT_FALSE(image_scratch::coarseNeedFits(need)) << "free in all, but in no one block";
  }
  // The finer decode is 1/4's, so the next finer is one up again.
  EXPECT_EQ(static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 1)),
            decodeNeed(need.bytes + need.heapFloor).bytes);
  ESP.setFreeHeap(savedHeap);
  fs::remove_all(work);
}

// --- A file that will not decode: its frame, and one attempt a session (DecodeFailures) -------------

// A baseline fixture cut at half its length, in its own folder.
struct FailedDecodeFixture : testing::Test {
  fs::path work;
  GfxRenderer renderer;
  DecodeFailures failures;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("img_failed_decode_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    CooperativeAbort::setLongTaskAbortPredicate(countedAbort);
    gPollsLeft = 0;
    CooperativeAbort::clearAborted();
    ImageBlock::setDecodeFailures(&failures);
  }
  void TearDown() override {
    ImageBlock::setDecodeFailures(nullptr);
    CooperativeAbort::setLongTaskAbortPredicate(nullptr);
    CooperativeAbort::clearAborted();
    ESP.setFreeHeap(200 * 1024);
    fs::remove_all(work);
  }

  std::string cutCopy(const char* fixture, const double keep = 0.5) const {
    std::ifstream in(std::string(JPEG_FIXTURE_DIR "/") + fixture, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bytes.resize(static_cast<size_t>(bytes.size() * keep));
    const fs::path out = work / fixture;
    std::ofstream(out, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.string();
  }
  std::string wholeCopy(const char* fixture) const { return cutCopy(fixture, 1.0); }
  static RenderConfig boxAt(const std::string& cachePath) {
    RenderConfig c;
    c.x = 12;
    c.y = 30;
    c.maxWidth = 150;
    c.maxHeight = 104;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = cachePath;
    return c;
  }
};

TEST_F(FailedDecodeFixture, ACutBaselineFileDrawsTheFrameAndCountsAsFailed) {
  const std::string jpg = cutCopy("prog_full_420_base.jpg");
  const RenderConfig c = boxAt((work / "bw.pxc").string());
  const uint32_t before = image_scratch::decodeFailedCount();
  const int rects = renderer.rectsDrawn;
  JpegToFramebufferConverter converter;
  EXPECT_TRUE(converter.decodeToFramebuffer(jpg, renderer, c)) << "the place is taken care of, as a progressive file's";
  EXPECT_EQ(rects + 1, renderer.rectsDrawn) << "a frame, not an empty space";
  EXPECT_EQ(before + 1, image_scratch::decodeFailedCount());
  EXPECT_FALSE(fs::exists(c.cachePath)) << "a cut picture is not cached";
}

TEST_F(FailedDecodeFixture, ASessionDecodesACutFileOnceAndDrawsItsFrameEveryPass) {
  const std::string jpg = cutCopy("prog_full_420_base.jpg");
  ImageBlock block(jpg, 150, 104, "");
  const uint32_t before = image_scratch::decodeFailedCount();
  const int rects = renderer.rectsDrawn;
  for (int pass = 0; pass < 5; ++pass) block.render(renderer, 12, 30, /*forceLoad=*/true, /*monochromeOutput=*/true);
  EXPECT_EQ(before + 1, image_scratch::decodeFailedCount()) << "one decode attempt, not one per pass";
  EXPECT_EQ(rects + 5, renderer.rectsDrawn) << "the frame on every pass";
  EXPECT_TRUE(failures.has(DecodeFailures::hashOf(jpg)));

  // A new session (the reader clears the list on exit) decodes it again.
  failures.clear();
  block.render(renderer, 12, 30, true, true);
  EXPECT_EQ(before + 2, image_scratch::decodeFailedCount()) << "cleared: decoded again";
}

TEST_F(FailedDecodeFixture, WithoutAReaderEveryPassDecodesAsBefore) {
  ImageBlock::setDecodeFailures(nullptr);
  const std::string jpg = cutCopy("prog_full_420_base.jpg");
  ImageBlock block(jpg, 150, 104, "");
  const uint32_t before = image_scratch::decodeFailedCount();
  for (int pass = 0; pass < 3; ++pass) block.render(renderer, 12, 30, true, true);
  EXPECT_EQ(before + 3, image_scratch::decodeFailedCount());
}

TEST_F(FailedDecodeFixture, ACutProgressiveFileIsRememberedToo) {
  const std::string jpg = cutCopy("prog_full_420.jpg");
  ImageBlock block(jpg, 150, 104, "");
  const uint32_t before = image_scratch::decodeFailedCount();
  for (int pass = 0; pass < 3; ++pass) block.render(renderer, 12, 30, true, true);
  EXPECT_EQ(before + 1, image_scratch::decodeFailedCount());
}

TEST_F(FailedDecodeFixture, APreemptedDecodeIsNotAFailure) {
  const std::string jpg = wholeCopy("prog_full_420_base.jpg");
  ImageBlock block(jpg, 150, 104, "");
  const uint32_t before = image_scratch::decodeFailedCount();
  gPollsLeft = 2;
  block.render(renderer, 12, 30, true, true);
  gPollsLeft = 0;
  ASSERT_TRUE(CooperativeAbort::consumeAborted()) << "the fixture must be stoppable";
  EXPECT_EQ(before, image_scratch::decodeFailedCount());
  EXPECT_FALSE(failures.has(DecodeFailures::hashOf(jpg)));
  block.render(renderer, 12, 30, true, true);  // input is idle again: the picture decodes
  EXPECT_TRUE(block.hasPixelCache());
}

TEST_F(FailedDecodeFixture, AHeapThatRefusesTheDecodeIsNotAFailure) {
  const std::string jpg = wholeCopy("prog_full_420_base.jpg");
  ImageBlock block(jpg, 150, 104, "");
  const uint32_t before = image_scratch::decodeFailedCount();
  ESP.setFreeHeap(4 * 1024);
  block.render(renderer, 12, 30, true, true);
  EXPECT_EQ(before, image_scratch::decodeFailedCount());
  EXPECT_FALSE(failures.has(DecodeFailures::hashOf(jpg)));
  ESP.setFreeHeap(200 * 1024);
  block.render(renderer, 12, 30, true, true);
  EXPECT_TRUE(block.hasPixelCache()) << "decoded once the memory is back";
}

TEST_F(FailedDecodeFixture, AFileCutInsideItsHeadersDrawsTheFrameAndIsAttemptedOnce) {
  const std::string jpg = cutCopy("prog_full_420_base.jpg", 0.01);
  ImageBlock block(jpg, 150, 104, "");
  const uint32_t before = image_scratch::decodeFailedCount();
  const int rects = renderer.rectsDrawn;
  for (int pass = 0; pass < 4; ++pass) block.render(renderer, 12, 30, true, true);
  EXPECT_EQ(before + 1, image_scratch::decodeFailedCount());
  EXPECT_EQ(rects + 4, renderer.rectsDrawn);
}

TEST_F(FailedDecodeFixture, TheFrameOfACropThatReachesPastTheScreenIsNotDrawn) {
  const std::string jpg = cutCopy("prog_full_420_base.jpg");
  ImageBlock whole(jpg, 150, 104, "");
  whole.render(renderer, 12, 30, true, true);  // fails, remembered
  const auto crop = whole.makeCrop(0, 40);
  ASSERT_TRUE(crop);
  const int rects = renderer.rectsDrawn;
  const int y = renderer.getScreenHeight() - 50;  // the 40 rows fit, the whole box's 104 do not
  crop->render(renderer, 12, y, true, true);
  EXPECT_EQ(rects, renderer.rectsDrawn);
}

TEST(DecodeFailuresList, HoldsEightAndEvictsTheOldest) {
  DecodeFailures list;
  for (uint32_t h = 100; h < 108; ++h) list.add(h);
  for (uint32_t h = 100; h < 108; ++h) EXPECT_TRUE(list.has(h));
  list.add(100);  // already there: no slot used
  list.add(200);
  EXPECT_FALSE(list.has(100)) << "the oldest went";
  EXPECT_TRUE(list.has(200));
  EXPECT_TRUE(list.has(107));
  list.clear();
  EXPECT_FALSE(list.has(107));
  EXPECT_FALSE(list.has(0)) << "an empty slot is not a hash of 0";
}

}  // namespace
