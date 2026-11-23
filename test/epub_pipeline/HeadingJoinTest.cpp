// A spine file that holds only its chapter's heading, laid out with the chapter after it (HeadingCarry, Section): the
// whole build, on test_heading_split.epub (make_test_heading_split.py lists what each spine item pins).
#include <Arduino.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Section.h"
#include "Epub/SpinePageIndex.h"
#include "KOReaderSync/ChapterXPathIndexer.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

const std::string kBook = std::string(CORPUS_DIR) + "/test_heading_split.epub";

class HeadingJoinTest : public testing::Test {
 protected:
  void SetUp() override {
    work = fs::temp_directory_path() / ("heading_join_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::string dump(const pipeline_harness::Profile& profile, const std::string& tag) {
    std::ostringstream out;
    EXPECT_TRUE(pipeline_harness::runAndDump(kBook, (work / tag).string(), profile, out)) << out.str();
    return out.str();
  }

  std::shared_ptr<Epub> load(const std::string& tag) {
    auto epub = std::make_shared<Epub>(kBook, (work / tag).string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }

  static Section::BuildParams params() {
    Section::BuildParams p;
    p.fontId = 1;
    p.viewportWidth = 214;
    p.viewportHeight = 358;
    p.embeddedStyle = true;
    p.fontSizeNormalization = false;
    return p;
  }

  // One spine item's part of a dump: its SPINE line and its pages.
  static std::string spineOf(const std::string& dump, const int spine) {
    const std::string start = "SPINE " + std::to_string(spine) + " ";
    const size_t from = dump.find(start);
    if (from == std::string::npos) return {};
    const size_t to = dump.find("\nSPINE ", from + 1);
    return dump.substr(from, to == std::string::npos ? std::string::npos : to + 1 - from);
  }

  fs::path work;
  GfxRenderer renderer;
};

const int kCarried[] = {1, 3, 6, 8, 10, 12};

}  // namespace

TEST_F(HeadingJoinTest, CarriesTheHeadingFilesAndKeepsTheRest) {
  const std::string out = dump({}, "default");
  for (int spine = 0; spine < 15; ++spine) {
    const bool carried = std::find(std::begin(kCarried), std::end(kCarried), spine) != std::end(kCarried);
    const std::string part = spineOf(out, spine);
    EXPECT_EQ(carried, part.find(" pages=0 ") != std::string::npos) << part.substr(0, part.find('\n'));
  }
}

TEST_F(HeadingJoinTest, AChapterCutAfterItsHeadingLaysOutAsTheWholeChapterDoes) {
  // Spine 0 holds number, rule, date line and text in one file; spines 1-2 hold the same, cut after the number.
  const std::string out = dump({}, "default");
  std::string whole = spineOf(out, 0);
  std::string cut = spineOf(out, 2);
  whole = whole.substr(whole.find('\n'));
  cut = cut.substr(cut.find('\n'));
  ASSERT_NE(std::string::npos, whole.find("t=1\n"));
  whole.replace(whole.find("t=1\n"), 4, "t=2\n");
  EXPECT_EQ(whole, cut);
}

TEST_F(HeadingJoinTest, MurphyLayoutMatchesGolden) {
  // The Murphy M3's text area at the owner's settings (CacheFileIdentityTest says where the numbers come from).
  pipeline_harness::Profile murphy;
  murphy.viewportWidth = 214;
  murphy.viewportHeight = 358;
  murphy.lineCompression = TextScale::percent(95);
  murphy.extraParagraphSpacing = true;
  const std::string out = dump(murphy, "murphy");
  ASSERT_EQ(out, dump(murphy, "murphy_again"));
  const fs::path golden = fs::path(GOLDEN_DIR) / "test_heading_split_murphy.golden.txt";
  if (std::getenv("UPDATE_GOLDENS")) {
    std::ofstream(golden, std::ios::binary) << out;
    GTEST_SKIP() << "golden regenerated: " << golden;
  }
  std::ifstream in(golden, std::ios::binary);
  ASSERT_TRUE(in) << "missing golden " << golden << " -- run with UPDATE_GOLDENS=1";
  std::stringstream bytes;
  bytes << in.rdbuf();
  EXPECT_EQ(bytes.str(), out);
}

TEST_F(HeadingJoinTest, ALentRegionAndTheHeapLayOutAlike) {
  pipeline_harness::Profile lent;
  lent.lentArenaBytes = 52272;  // the X3's secondary framebuffer, lent to Background-B and -C builds
  EXPECT_EQ(dump({}, "heap"), dump(lent, "lent"));
}

TEST_F(HeadingJoinTest, TheCarriedFileHasNoSectionFileAndTheJoinedOneANameOfItsOwn) {
  const auto epub = load("files");
  const Section::BuildParams p = params();
  Section heading(epub, 1, renderer);
  ASSERT_TRUE(heading.createSectionFile(p, {}, /*skipEviction=*/true));
  EXPECT_EQ(0, heading.pageCount);
  Section text(epub, 2, renderer);
  ASSERT_TRUE(text.createSectionFile(p, {}, /*skipEviction=*/true));
  ASSERT_TRUE(text.loadSectionFile(p));
  EXPECT_GT(text.pageCount, 0);

  const fs::path bucket = Epub::spineCacheDir(epub->getCachePath(), 1);
  int headingFiles = 0;
  int textFiles = 0;
  for (const auto& entry : fs::directory_iterator(bucket)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("1_", 0) == 0) ++headingFiles;
    if (name.rfind("2_", 0) == 0) ++textFiles;
  }
  EXPECT_EQ(0, headingFiles) << "a carried file gets no section file";
  EXPECT_EQ(1, textFiles);
  EXPECT_TRUE(fs::exists(Section::sectionHtmlCachePath(epub->getCachePath(), 1))) << "its XHTML is banked";
  // The joined layout is not the one without the heading: bookmarks counted in that one move by their text.
  EXPECT_EQ(Section::layoutFingerprintFor(*epub, 2, p), text.layoutFingerprint());
  EXPECT_NE(Section::layoutFingerprintFor(p), text.layoutFingerprint());
  EXPECT_EQ(Section::layoutFingerprintFor(p), Section::layoutFingerprintFor(*epub, 0, p));
}

TEST_F(HeadingJoinTest, TheChapterIsCountedWithoutBuildingTheHeadingFile) {
  const auto epub = load("count");
  const Section::BuildParams p = params();
  Section text(epub, 2, renderer);
  ASSERT_TRUE(text.createSectionFile(p, {}, /*skipEviction=*/true));
  const auto totals =
      Section::indexedPageTotals(epub->getCachePath(), p, epub->getSpineItemsCount(), 1, 2, /*current=*/2);
  EXPECT_EQ(1, totals.filesBefore) << "the heading file is known";
  EXPECT_EQ(0u, totals.pagesBefore) << "and has no page";
}

// The counter is drawn while the chapter is still being laid out (Background-C): the heading file must count as known
// from the build's start, not from its end, which nothing would draw again.
TEST_F(HeadingJoinTest, TheHeadingFileIsCountedFromTheBuildsStart) {
  const auto epub = load("count_live");
  const Section::BuildParams p = params();
  const host_clock::Ticking tick(1);
  Section text(epub, 2, renderer);
  ASSERT_EQ(Section::BuildStep::More, text.stepSectionBuild(p, /*budgetMs=*/1));
  ASSERT_TRUE(text.hasActiveBuild());
  const auto totals =
      Section::indexedPageTotals(epub->getCachePath(), p, epub->getSpineItemsCount(), 1, 2, /*current=*/2);
  EXPECT_EQ(1, totals.filesBefore);
  EXPECT_EQ(0u, totals.pagesBefore);
}

TEST_F(HeadingJoinTest, PositionsInTheJoinedFileAreItsOwn) {
  const auto epub = load("positions");
  const Section::BuildParams p = params();
  Section text(epub, 2, renderer);
  ASSERT_TRUE(text.createSectionFile(p, {}, /*skipEviction=*/true));
  ASSERT_TRUE(text.loadSectionFile(p));
  // The first page starts at the text file's first visible byte, and that names its first text, in its own document:
  // what a KOReader position and the saved progress are counted in.
  ASSERT_EQ(0u, text.getVisibleTextOffsetForPage(0).value_or(UINT32_MAX));
  const std::string xpath = ChapterXPathIndexer::findXPathForVisibleOffset(epub, 2, 0);
  EXPECT_NE(std::string::npos, xpath.find("DocFragment[3]")) << xpath;
}

TEST_F(HeadingJoinTest, ThePrintedPagesOfAHeadingFileLandOnTheFirstPage) {
  const auto epub = load("labels");
  const Section::BuildParams p = params();
  for (const auto& [spine, label] : {std::pair<int, const char*>{2, "(12)"}, {4, "(30)"}}) {
    Section text(epub, spine, renderer);
    ASSERT_TRUE(text.createSectionFile(p, {}, /*skipEviction=*/true));
    ASSERT_TRUE(text.loadSectionFile(p));
    EXPECT_EQ(label, text.getPrintedPageLabelForPage(0).value_or("none")) << "spine " << spine;
  }
}

TEST_F(HeadingJoinTest, ASlicedBuildEqualsTheBlockingOne) {
  const Section::BuildParams p = params();
  std::string blockingBytes;
  {
    const auto epub = load("blocking");
    Section text(epub, 2, renderer);
    ASSERT_TRUE(text.createSectionFile(p, {}, /*skipEviction=*/true));
    for (const auto& entry : fs::directory_iterator(Epub::spineCacheDir(epub->getCachePath(), 2))) {
      if (entry.path().filename().string().rfind("2_", 0) != 0) continue;
      std::ifstream in(entry.path(), std::ios::binary);
      blockingBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
  }
  ASSERT_FALSE(blockingBytes.empty());

  const auto epub = load("sliced");
  // Without a ticking clock the host's millis() is frozen and a sliced build finishes in one call.
  const host_clock::Ticking tick(1);
  Section text(epub, 2, renderer);
  int slices = 0;
  Section::BuildStep step = Section::BuildStep::More;
  for (int i = 0; i < 20000; ++i) {
    step = text.stepSectionBuild(p, /*budgetMs=*/1);
    if (step == Section::BuildStep::Done || step == Section::BuildStep::Failed) break;
    ++slices;
  }
  ASSERT_EQ(Section::BuildStep::Done, step);
  ASSERT_GT(slices, 1) << "the build did not slice";
  std::string slicedBytes;
  for (const auto& entry : fs::directory_iterator(Epub::spineCacheDir(epub->getCachePath(), 2))) {
    if (entry.path().filename().string().rfind("2_", 0) != 0) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    slicedBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  EXPECT_EQ(blockingBytes, slicedBytes);
}

TEST_F(HeadingJoinTest, AVerticalRequestJoinsNothing) {
  const auto epub = load("vertical");
  Section::BuildParams p = params();
  p.verticalText = VerticalText::Automatic;
  Section heading(epub, 1, renderer);
  ASSERT_TRUE(heading.createSectionFile(p, {}, /*skipEviction=*/true));
  ASSERT_TRUE(heading.loadSectionFile(p));
  EXPECT_EQ(1, heading.pageCount);
  EXPECT_EQ(Section::layoutFingerprintFor(p), Section::layoutFingerprintFor(*epub, 2, p));
}
