// A build that ends early tears its parser down while lines still live in the lent region: the open page's, and a
// table's packed rows, laid out in the page's arena block. They must be destroyed before that block is rewound. This
// binary fills a rewound block (BUILD_ARENA_TRACE), and a line whose bytes read the fill when it is destroyed aborts
// the test (TextBlock.h). The parser's destructor and finalize() used to rewind first and let the members go after.
#include <Arduino.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/Section.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kLentBytes = 52272;  // the X3's secondary framebuffer, lent to background builds

fs::path freshWorkDir(const std::string& tag) {
  const fs::path dir =
      fs::temp_directory_path() / ("lent_build_teardown_" + std::to_string(static_cast<long>(getpid()))) / tag;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

void removeWorkDir(const fs::path& dir) {
  std::error_code ec;  // Windows keeps a just-closed file busy for a moment; a leftover temp dir is harmless
  fs::remove_all(dir, ec);
}

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 1;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.lineCompression = 1.0f;
  p.embeddedStyle = true;
  return p;
}

// A table long enough to span pages, between two paragraphs.
std::string longTable() {
  std::string body = "<p>before</p><table>";
  for (int r = 0; r < 400; ++r) {
    body += "<tr>";
    for (int c = 0; c < 3; ++c) body += "<td>cell " + std::to_string(r) + " " + std::to_string(c) + " some words</td>";
    body += "</tr>";
  }
  return body + "</table><p>after</p>";
}

// Builds `spine` on a lent region in 1 ms slices and drops the Section after `steps` of them, aborting the build where
// it stands. True when it was still running, i.e. stopped part-way.
bool abortAfter(const std::shared_ptr<Epub>& epub, const int spine, const int steps) {
  GfxRenderer renderer;
  BuildArena lent(kLentBytes);
  EXPECT_TRUE(lent.valid());
  Section section(epub, spine, renderer);
  section.clearCache();
  section.setExternalBuildScratch(&lent);
  const host_clock::Ticking tick(1);
  Section::BuildStep step = Section::BuildStep::More;
  for (int i = 0; i < steps && step == Section::BuildStep::More; ++i) {
    step = section.stepSectionBuild(params(), /*budgetMs=*/1, {}, /*skipEviction=*/true);
  }
  return section.hasActiveBuild();
}  // the Section goes first, aborting the build, then the region

// Aborts after more and more slices until the build finishes on its own; returns how many aborts were part-way.
int abortAtEveryDepth(const std::shared_ptr<Epub>& epub, const int spine) {
  int partWay = 0;
  for (int steps = 10; steps <= 640; steps *= 2) {
    SCOPED_TRACE("aborted after " + std::to_string(steps) + " slices");
    if (!abortAfter(epub, spine, steps)) break;
    ++partWay;
  }
  return partWay;
}

}  // namespace

// Prose: the open page's lines (seen: 9, 11 and 32 of them at the abort).
TEST(LentBuildTeardown, AnAbortedBuildDropsItsOpenPageBeforeItsBlock) {
  const fs::path work = freshWorkDir("prose");
  {
    auto epub = std::make_shared<Epub>(CORPUS_DIR "/../fixtures/moby-dick.epub", (work / "cache").string());
    ASSERT_TRUE(epub->load(true));
    epub->setupCacheDir();
    int spine = -1;
    for (int i = 0; i < epub->getSpineItemsCount(); ++i) {
      if (epub->getSpineItem(i).href.find("2701-h-3.htm") != std::string::npos) spine = i;
    }
    ASSERT_GE(spine, 0);
    EXPECT_GE(abortAtEveryDepth(epub, spine), 2) << "the build must be stopped mid-parse for this to test anything";
  }
  removeWorkDir(work);
}

// A table: the rows packed for the fragment not yet on the page (seen: 1 to 13 of them at the abort).
TEST(LentBuildTeardown, AnAbortedBuildDropsATablesPackedRowsBeforeTheirBlock) {
  const fs::path work = freshWorkDir("table");
  {
    auto epub = syntheticBook(work, 0, longTable());
    ASSERT_TRUE(epub);
    EXPECT_GE(abortAtEveryDepth(epub, 0), 2) << "the build must be stopped mid-table for this to test anything";
  }
  removeWorkDir(work);
}

// A build the heap stops inside a table is finalized with rows still packed: finalize() rewinds the page block, and
// the rows used to go only with the parser, after it.
TEST(LentBuildTeardown, AStoppedBuildDropsATablesPackedRowsBeforeFinalizeRewinds) {
  const fs::path work = freshWorkDir("stopped");
  {
    auto epub = syntheticBook(work, 0, longTable());
    ASSERT_TRUE(epub);
    GfxRenderer renderer;
    BuildArena lent(kLentBytes);
    ASSERT_TRUE(lent.valid());
    Section section(epub, 0, renderer);
    section.setExternalBuildScratch(&lent);
    bool ok = false;
    {
      // Forty rows' cells get their text, the 41st row's first cell does not: the build stops with a page of the table
      // written and the next page's rows packed.
      tableCellTextsLeft() = 120;
      ok = section.createSectionFile(params(), {}, /*skipEviction=*/true);
      tableCellTextsLeft() = SIZE_MAX;
    }
    EXPECT_TRUE(ok) << "the pages before the refusal are kept";
    EXPECT_TRUE(section.isTruncatedCache());
  }
  removeWorkDir(work);
}
