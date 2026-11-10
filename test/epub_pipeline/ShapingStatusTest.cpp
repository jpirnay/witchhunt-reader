// A shaped-script run laid out unshaped is latched into the section file (kStatusShapingDegraded) so the reader can
// rebuild once; a clean build lays its words out shaped (glyph forms) and a refused one does not; a clean rebuild on
// the same Section clears the bit and records the page count; a build's end frees the shaping pass's memory and
// faces; and while a build runs its plans keep the shaped free-heap floor. Each case builds a one-chapter Hindi book
// through the host pipeline, with the Devanagari subset as the face of every font.
#include <OtBuffer.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"
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

// Words the subset face holds: hindi, patrika, shri, prarthana, krishna, jnana.
std::string hindiWords() {
  return "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80 "
         "\xE0\xA4\xAA\xE0\xA4\xA4\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xBE "
         "\xE0\xA4\xB6\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x80 "
         "\xE0\xA4\xAA\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBE\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xA5\xE0\xA4\xA8\xE0\xA4\xBE "
         "\xE0\xA4\x95\xE0\xA5\x83\xE0\xA4\xB7\xE0\xA5\x8D\xE0\xA4\xA3 "
         "\xE0\xA4\x9C\xE0\xA5\x8D\xE0\xA4\x9E\xE0\xA4\xBE\xE0\xA4\xA8 ";
}

// Paragraphs of those words.
std::string hindiBody() {
  std::string body;
  for (int paragraph = 0; paragraph < 6; ++paragraph) {
    body += "<p>";
    for (int i = 0; i < 12; ++i) body += hindiWords();
    body += "</p>\n";
  }
  return body;
}

// The words alone, in a table's cell and nowhere else.
std::string hindiTable() { return "<table><tr><td>" + hindiWords() + hindiWords() + "</td></tr></table>\n"; }

// How many words of the section's pages are laid out as glyphs (a shaped word's form), read back from the file.
int glyphFormWords(Section& section) {
  int words = 0;
  for (uint16_t n = 0; n < section.pageCount; ++n) {
    section.currentPage = n;
    const auto page = section.loadPageFromSectionFile();
    if (!page) return -1;
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const TextBlock& block = *static_cast<const PageLine&>(*element).getBlock();
      for (uint16_t w = 0; w < block.wordCount(); ++w) words += block.hasGlyphForm(w) ? 1 : 0;
    }
  }
  return words;
}

// The page counts recorded for the book's one spine under `p`: 0 files when none is.
int recordedFiles(const Epub& epub, const Section::BuildParams& p) {
  return Section::indexedPageTotals(epub.getCachePath(), p, epub.getSpineItemsCount(), 0, 0, /*current=*/-1).filesAfter;
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
  // A degraded build records no page count (a load would record it, so this is read first).
  EXPECT_EQ(recordedFiles(*epub, params()), 0);

  Section loaded(epub, 0, renderer);
  ASSERT_TRUE(loaded.loadSectionFile(params()));
  EXPECT_TRUE(loaded.isShapingDegraded());
  expectOtherwiseComplete(loaded);
  // No word was shaped, so no word has a glyph form.
  EXPECT_EQ(glyphFormWords(loaded), 0);
}

TEST_F(ShapingStatus, ADegradedTableCellsRunIsLatched) {
  const std::shared_ptr<Epub> epub = syntheticBook(work, 0, hindiTable());
  const pipeline_harness::ShapingFaceScope face(kDevanagariFace);
  GfxRenderer renderer;
  Section built(epub, 0, renderer);
  {
    const AllocationRefusal refusal(kShapingBlockFrom, kShapingBlockTo);
    ASSERT_TRUE(built.createSectionFile(params(), {}, /*skipEviction=*/true));
  }
  EXPECT_TRUE(built.isShapingDegraded());
  expectOtherwiseComplete(built);
}

TEST_F(ShapingStatus, ACleanRebuildOnTheSameSectionClearsTheBit) {
  const std::shared_ptr<Epub> epub = syntheticBook(work, 0, hindiBody());
  const pipeline_harness::ShapingFaceScope face(kDevanagariFace);
  GfxRenderer renderer;
  Section section(epub, 0, renderer);
  {
    const AllocationRefusal refusal(kShapingBlockFrom, kShapingBlockTo);
    ASSERT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true));
  }
  ASSERT_TRUE(section.isShapingDegraded());
  EXPECT_EQ(recordedFiles(*epub, params()), 0);

  // The reader rebuilds on the Section it already holds.
  ASSERT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true));
  EXPECT_FALSE(section.isShapingDegraded());
  expectOtherwiseComplete(section);
  EXPECT_EQ(recordedFiles(*epub, params()), 1);

  Section loaded(epub, 0, renderer);
  ASSERT_TRUE(loaded.loadSectionFile(params()));
  EXPECT_FALSE(loaded.isShapingDegraded());
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
    // The face is installed and the words are shaped: they are drawn as glyphs.
    EXPECT_GT(glyphFormWords(loaded), 0);
  }

  // The harness's own run, from a profile.
  pipeline_harness::Profile profile;
  profile.shapingFontFile = kDevanagariFace;
  bool visited = false;
  int shapedWords = 0;
  ASSERT_TRUE(pipeline_harness::forEachPage((work / "book0.epub").string(), (work / "profile_cache").string(), profile,
                                            [&](int, const Section& section, const Page& page, GfxRenderer&) {
                                              visited = true;
                                              EXPECT_FALSE(section.isShapingDegraded());
                                              for (const auto& element : page.elements) {
                                                if (element->getTag() != TAG_PageLine) continue;
                                                const TextBlock& block =
                                                    *static_cast<const PageLine&>(*element).getBlock();
                                                for (uint16_t w = 0; w < block.wordCount(); ++w)
                                                  shapedWords += block.hasGlyphForm(w) ? 1 : 0;
                                              }
                                            }));
  EXPECT_TRUE(visited);
  EXPECT_GT(shapedWords, 0);
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

namespace {

// The face source of ABuildsPlansKeepTheShapedFloor: it gives no face (the words are reordered) and records whether
// the shaping engine's heap check refused a block larger than any heap, as it does only while a check is installed.
int probes = 0;
bool checkInstalled = false;
ShapingFace* probeHeapCheck(int, EpdFontFamily::Style, uint32_t*, bool*) {
  ++probes;
  checkInstalled = !ot::heapAvailable(size_t{1} << 30);
  return nullptr;
}

}  // namespace

TEST_F(ShapingStatus, ABuildsPlansKeepTheShapedFloor) {
  // The engine's check is installed for the build's span alone (ShapingHeapCheck in BuildState): a plan asked for
  // during the build must leave the shaped store's free-heap floor; before and after it, nothing is checked.
  const std::shared_ptr<Epub> epub = syntheticBook(work, 0, hindiBody());
  EXPECT_TRUE(ot::heapAvailable(size_t{1} << 30));
  GfxRenderer::shapingFaceSource = &probeHeapCheck;
  probes = 0;
  {
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    ASSERT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true));
  }
  EXPECT_GT(probes, 0);
  EXPECT_TRUE(checkInstalled);
  EXPECT_TRUE(ot::heapAvailable(size_t{1} << 30));
}
