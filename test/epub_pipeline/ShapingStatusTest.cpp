// The shaping status of a section build (writing modes step 2, PR 3): a shaped-script run laid out unshaped is latched
// into the section file (kStatusShapingDegraded) so the reader can rebuild once, and a build's end frees the shaping
// pass's memory and faces. Each case builds a one-chapter Hindi book through the host pipeline, with the Devanagari
// subset as the face of every font.
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"
#include "SyntheticBook.h"
#include "TextShaping.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kDevanagariFace = "NotoSansDevanagari-Shaping-Regular.ttf";

// The shaping call's heap block (TextShaping::buildHeapBlock) for these words asks for 6.5 KB and is made in 512 B
// steps, so 6.5 KB or 7 KB. The band holds those and neither the build's own blocks below it nor the ZIP ring (9446 B)
// and the parse arena (10 KB) above it.
constexpr size_t kShapingBlockFrom = 6 * 1024;
constexpr size_t kShapingBlockTo = 8 * 1024 - 1;

// Words the subset face holds, in paragraphs: hindi, patrika, shri, prarthana, krishna, jnana.
std::string hindiBody() {
  const std::string words =
      "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80 "
      "\xE0\xA4\xAA\xE0\xA4\xA4\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xBE "
      "\xE0\xA4\xB6\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x80 "
      "\xE0\xA4\xAA\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBE\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xA5\xE0\xA4\xA8\xE0\xA4\xBE "
      "\xE0\xA4\x95\xE0\xA5\x83\xE0\xA4\xB7\xE0\xA5\x8D\xE0\xA4\xA3 "
      "\xE0\xA4\x9C\xE0\xA5\x8D\xE0\xA4\x9E\xE0\xA4\xBE\xE0\xA4\xA8 ";
  std::string body;
  for (int paragraph = 0; paragraph < 6; ++paragraph) {
    body += "<p>";
    for (int i = 0; i < 12; ++i) body += words;
    body += "</p>\n";
  }
  return body;
}

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 1;
  p.lineCompression = 1.0f;
  p.viewportWidth = 460;
  p.viewportHeight = 760;
  p.inlineFootnotePreviews = false;
  return p;
}

// What a build that completed leaves in the status byte besides the shaping bit.
void expectOtherwiseComplete(const Section& section) {
  EXPECT_GT(section.pageCount, 0);
  EXPECT_FALSE(section.isTruncatedCache());
  EXPECT_FALSE(section.isImageHeaderDegraded());
  EXPECT_FALSE(section.isTableRowDegraded());
  EXPECT_FALSE(section.isCssLowHeapDegraded());
  EXPECT_FALSE(section.isSimplified());
  EXPECT_FALSE(section.isFootnotePreviewsUnresolved());
}

struct ShapingStatus : testing::Test {
  fs::path work;

  void SetUp() override {
    // One directory per case: ctest runs the cases as parallel processes.
    work = fs::temp_directory_path() /
           (std::string("shaping_status_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override {
    GfxRenderer::shapingFaceSource = nullptr;
    fs::remove_all(work);
  }
};

}  // namespace

TEST_F(ShapingStatus, ADegradedBuildIsLatchedAndKeptInTheFile) {
  const std::shared_ptr<Epub> epub = syntheticBook(work, 0, hindiBody());
  const pipeline_harness::ShapingFaceScope face(kDevanagariFace);
  GfxRenderer renderer;
  Section built(epub, 0, renderer);
  {
    const AllocationRefusal refusal(kShapingBlockFrom, kShapingBlockTo);
    ASSERT_TRUE(built.createSectionFile(params(), {}, /*skipEviction=*/true));
  }
  EXPECT_TRUE(built.isShapingDegraded());
  // The band hit the shaping calls and nothing else: the build ran to its end.
  expectOtherwiseComplete(built);

  Section loaded(epub, 0, renderer);
  ASSERT_TRUE(loaded.loadSectionFile(params()));
  EXPECT_TRUE(loaded.isShapingDegraded());
  expectOtherwiseComplete(loaded);
}

TEST_F(ShapingStatus, ACleanBuildIsNotDegraded) {
  const std::shared_ptr<Epub> epub = syntheticBook(work, 0, hindiBody());
  {
    const pipeline_harness::ShapingFaceScope face(kDevanagariFace);
    GfxRenderer renderer;
    Section built(epub, 0, renderer);
    ASSERT_TRUE(built.createSectionFile(params(), {}, /*skipEviction=*/true));
    EXPECT_FALSE(built.isShapingDegraded());
    expectOtherwiseComplete(built);

    Section loaded(epub, 0, renderer);
    ASSERT_TRUE(loaded.loadSectionFile(params()));
    EXPECT_FALSE(loaded.isShapingDegraded());
  }

  // The harness's own run, from a profile.
  pipeline_harness::Profile profile;
  profile.shapingFontFile = kDevanagariFace;
  bool visited = false;
  ASSERT_TRUE(pipeline_harness::forEachPage((work / "book0.epub").string(), (work / "profile_cache").string(), profile,
                                            [&](int, const Section& section, const Page&, GfxRenderer&) {
                                              visited = true;
                                              EXPECT_FALSE(section.isShapingDegraded());
                                            }));
  EXPECT_TRUE(visited);
}

TEST_F(ShapingStatus, TheBuildsEndFreesTheShapingMemory) {
  const std::shared_ptr<Epub> epub = syntheticBook(work, 0, hindiBody());
  const pipeline_harness::ShapingFaceScope face(kDevanagariFace);
  GfxRenderer renderer;
  for (int build = 0; build < 2; ++build) {
    const int releasesBefore = GfxRenderer::shapingReleases;
    Section section(epub, 0, renderer);
    ASSERT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true)) << build;
    EXPECT_FALSE(TextShaping::buildMemoryHeld()) << build;
    EXPECT_EQ(GfxRenderer::shapingReleases, releasesBefore + 1) << build;
    section.clearCache();
  }
}
