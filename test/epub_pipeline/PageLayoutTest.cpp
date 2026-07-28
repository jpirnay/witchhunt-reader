// G2a — PageLayout pull-core scaffold gate (docs/stage1-single-source-live-pagination-2026-07-27.md).
//
// PageLayout::layoutPage(cursor) lays out exactly ONE page from a page-boundary cursor, live over a
// content.bin spine, by seekToBlock()ing and driving the trusted LayoutSink for one page. The GOLDEN
// is the whole-spine LayoutSink run (compiled::replaySpine) — the same engine EpubPipelineTest pins
// byte-identically to the committed goldens. This test asserts that stopping after ONE page from the
// spine-start cursor reproduces the golden spine's FIRST page exactly, across the corpus × profile
// matrix. (Mid-spine cursors + precise end-cursors + backward layout are G2b; this scaffold pins the
// cursor type, seekToBlock, and the position-identical oracle rig with zero layout-behavior risk.)

#include <gtest/gtest.h>

#include <GfxRenderer.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <process.h>  // _getpid — per-process temp isolation under parallel ctest
#include <sstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/content/BlockStreamReader.h"
#include "Epub/content/LayoutSink.h"
#include "Epub/content/PageLayout.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

std::string freshDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / ("pagelayout_test_" + std::to_string(_getpid())) / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

// Build the LayoutParams a spine is laid out with, mirroring replayFromContentBin's mapping.
compiled::LayoutParams paramsFor(const pipeline_harness::Profile& p, const std::string& epubPath,
                                 const std::string& cacheDir, int spineIndex) {
  compiled::LayoutParams params;
  params.fontId = p.fontId;
  params.lineCompression = p.lineCompression;
  params.extraParagraphSpacing = p.extraParagraphSpacing;
  params.paragraphAlignment = p.paragraphAlignment;
  params.viewportWidth = p.viewportWidth;
  params.viewportHeight = p.viewportHeight;
  params.hyphenationEnabled = p.hyphenationEnabled;
  params.bionicReadingEnabled = p.bionicReadingEnabled;
  params.embeddedStyle = p.embeddedStyle;
  params.epubFilePath = epubPath;
  params.imageBasePath = cacheDir + "/img_" + std::to_string(spineIndex) + "_00000000_";
  return params;
}

// Golden: drive the whole spine through LayoutSink (compiled::replaySpine) and return its FIRST page,
// dumped via the shared canonical serializer. Empty string when the spine has no page.
std::string goldenFirstPageDump(compiled::BlockStreamReader& reader, GfxRenderer& renderer,
                                const compiled::LayoutParams& params, uint32_t spineIndex,
                                const std::string& cacheDir) {
  std::vector<std::unique_ptr<Page>> pages;
  compiled::LayoutSink sink(renderer, params,
                            [&pages](std::unique_ptr<Page> page) { pages.push_back(std::move(page)); });
  if (!compiled::replaySpine(reader, spineIndex, sink)) return "<replay-failed>";
  if (pages.empty()) return "";
  std::ostringstream out;
  pipeline_harness::dumpOnePage(out, *pages.front(), 0, cacheDir);
  return out.str();
}

struct BookProfile {
  std::string dir;
  std::string book;
  pipeline_harness::Profile profile;
};

std::vector<std::string> corpusBooks() {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

// All of a spine's golden pages, each dumped via the shared serializer, in order.
std::vector<std::string> goldenAllPages(compiled::BlockStreamReader& reader, GfxRenderer& renderer,
                                        const compiled::LayoutParams& params, uint32_t spineIndex,
                                        const std::string& cacheDir) {
  std::vector<std::unique_ptr<Page>> pages;
  compiled::LayoutSink sink(renderer, params,
                            [&pages](std::unique_ptr<Page> page) { pages.push_back(std::move(page)); });
  std::vector<std::string> dumps;
  if (!compiled::replaySpine(reader, spineIndex, sink)) return dumps;  // empty → caller skips
  for (const auto& p : pages) {
    std::ostringstream out;
    pipeline_harness::dumpOnePage(out, *p, 0, cacheDir);
    dumps.push_back(out.str());
  }
  return dumps;
}

// Books whose spines are pure text/image/HR — the pull core owns their full pagination, so the
// forward CHAIN (layoutPage -> end -> layoutPage(end) -> ...) must reproduce every golden page.
// Float + table spines are served by the scaffold (block-granular end cursor), excluded from the
// all-pages chain (their first page is still gated by the first-page test above).
bool pullOwnsAllPages(const std::string& book) {
  return book != "test_float_images.epub" && book != "test_tables.epub";
}

std::vector<pipeline_harness::Profile> profileMatrix() {
  using P = pipeline_harness::Profile;
  std::vector<P> ps;
  ps.push_back(P{});  // default
  P narrow;
  narrow.name = "narrow";
  narrow.viewportWidth = 300;
  narrow.viewportHeight = 500;
  ps.push_back(narrow);
  P bigFont;
  bigFont.name = "bigFont";
  bigFont.fontId = 3;
  ps.push_back(bigFont);
  return ps;
}

std::vector<BookProfile> bookProfileMatrix() {
  std::vector<BookProfile> out;
  for (const auto& book : corpusBooks())
    for (const auto& p : profileMatrix()) out.push_back({CORPUS_DIR, book, p});
  return out;
}

class PageLayoutMatrix : public testing::TestWithParam<BookProfile> {};

// The core G2a gate: PageLayout's one-page layout from the spine-start cursor == the golden spine's
// first page, for every spine, across the corpus × profile matrix.
TEST_P(PageLayoutMatrix, FirstPageMatchesWholeSpineGolden) {
  const BookProfile& bp = GetParam();
  const std::string epub = bp.dir + "/" + bp.book;
  const std::string cacheDir = freshDir(bp.book + "_" + bp.profile.name);

  std::ostringstream compileLog;
  ASSERT_TRUE(pipeline_harness::compileToContentBin(epub, cacheDir, bp.profile, compileLog)) << compileLog.str();

  FsFile binFile;
  ASSERT_TRUE(binFile.openForRead(cacheDir + "/content.bin"));
  compiled::BlockStreamReader reader;
  ASSERT_TRUE(reader.open(binFile));

  GfxRenderer renderer;
  bool checkedAnySpine = false;
  for (uint32_t si = 0; si < reader.spineCount(); ++si) {
    if (!reader.spineAvailable(si)) continue;
    const compiled::LayoutParams params = paramsFor(bp.profile, epub, cacheDir, static_cast<int>(si));

    const std::string golden = goldenFirstPageDump(reader, renderer, params, si, cacheDir);
    if (golden.empty()) continue;  // spine with no page (e.g. empty) — nothing to compare

    compiled::PagePosition start;
    start.spineIndex = static_cast<uint16_t>(si);
    const compiled::LaidOutPage lp = compiled::layoutPage(reader, renderer, params, start);
    ASSERT_TRUE(lp.ok) << "layoutPage failed for spine " << si;
    ASSERT_TRUE(lp.page) << "layoutPage produced no page for spine " << si;

    std::ostringstream pullOut;
    pipeline_harness::dumpOnePage(pullOut, *lp.page, 0, cacheDir);
    EXPECT_EQ(golden, pullOut.str()) << "pull-core first page diverges from golden for spine " << si;
    checkedAnySpine = true;
  }
  binFile.close();
  EXPECT_TRUE(checkedAnySpine) << "no spine produced a comparable first page for " << bp.book;
}

// P5a: the pull core's FORWARD CHAIN reproduces EVERY golden page. Start at the spine cursor, lay out
// a page, follow its end cursor to the next page, and so on — each page must be byte-identical to the
// corresponding golden page, and the chain must terminate (atSpineEnd) exactly when the golden does.
// This exercises the precise end cursor + mid-block resume that the first-page gate cannot. Restricted
// to books the pull core fully owns (text/image/HR); float + table spines are scaffold-served.
TEST_P(PageLayoutMatrix, ForwardChainMatchesAllGoldenPages) {
  const BookProfile& bp = GetParam();
  if (!pullOwnsAllPages(bp.book)) GTEST_SKIP() << bp.book << " has float/table spines (scaffold-served)";
  const std::string epub = bp.dir + "/" + bp.book;
  const std::string cacheDir = freshDir(bp.book + "_" + bp.profile.name + "_chain");

  std::ostringstream compileLog;
  ASSERT_TRUE(pipeline_harness::compileToContentBin(epub, cacheDir, bp.profile, compileLog)) << compileLog.str();

  FsFile binFile;
  ASSERT_TRUE(binFile.openForRead(cacheDir + "/content.bin"));
  compiled::BlockStreamReader reader;
  ASSERT_TRUE(reader.open(binFile));

  GfxRenderer renderer;
  for (uint32_t si = 0; si < reader.spineCount(); ++si) {
    if (!reader.spineAvailable(si)) continue;
    const compiled::LayoutParams params = paramsFor(bp.profile, epub, cacheDir, static_cast<int>(si));

    const std::vector<std::string> golden = goldenAllPages(reader, renderer, params, si, cacheDir);
    if (golden.empty()) continue;

    compiled::PagePosition cursor;
    cursor.spineIndex = static_cast<uint16_t>(si);
    for (size_t pageIdx = 0; pageIdx < golden.size(); ++pageIdx) {
      const compiled::LaidOutPage lp = compiled::layoutPage(reader, renderer, params, cursor);
      ASSERT_TRUE(lp.ok && lp.page) << "chain layoutPage failed spine " << si << " page " << pageIdx;

      std::ostringstream pullOut;
      pipeline_harness::dumpOnePage(pullOut, *lp.page, 0, cacheDir);
      if (golden[pageIdx] != pullOut.str() && std::getenv("PL_DIFF")) {
        fprintf(stderr, "=== spine %u page %zu/%zu (endBlock=%u endOff=%u) ===\n--GOLDEN--\n%s\n--PULL--\n%s\n", si,
                pageIdx, golden.size(), lp.end.blockIndex, lp.end.offset, golden[pageIdx].c_str(),
                pullOut.str().c_str());
      }
      ASSERT_EQ(golden[pageIdx], pullOut.str())
          << "forward chain diverges spine " << si << " page " << pageIdx << " of " << golden.size();

      const bool isLast = (pageIdx + 1 == golden.size());
      ASSERT_EQ(lp.atSpineEnd, isLast) << "spine " << si << " page " << pageIdx << " of " << golden.size()
                                       << ": atSpineEnd mismatch (pull says " << lp.atSpineEnd << ")";
      cursor = lp.end;
    }
  }
  binFile.close();
}

// P5b: BACKWARD layout (prev-page). Forward-chain a spine to collect each page's start cursor, then
// for every page K>=1, layoutPageBackward(startCursor[K]) must reproduce golden page K-1 exactly (the
// page that ENDS where page K begins) and report that page's own start cursor. Restricted to
// pull-owned books (text/image/HR); float + table spines are scaffold-served.
TEST_P(PageLayoutMatrix, BackwardMatchesGoldenPages) {
  const BookProfile& bp = GetParam();
  if (!pullOwnsAllPages(bp.book)) GTEST_SKIP() << bp.book << " has float/table spines (scaffold-served)";
  const std::string epub = bp.dir + "/" + bp.book;
  const std::string cacheDir = freshDir(bp.book + "_" + bp.profile.name + "_bwd");

  std::ostringstream compileLog;
  ASSERT_TRUE(pipeline_harness::compileToContentBin(epub, cacheDir, bp.profile, compileLog)) << compileLog.str();

  FsFile binFile;
  ASSERT_TRUE(binFile.openForRead(cacheDir + "/content.bin"));
  compiled::BlockStreamReader reader;
  ASSERT_TRUE(reader.open(binFile));

  GfxRenderer renderer;
  for (uint32_t si = 0; si < reader.spineCount(); ++si) {
    if (!reader.spineAvailable(si)) continue;
    const compiled::LayoutParams params = paramsFor(bp.profile, epub, cacheDir, static_cast<int>(si));

    const std::vector<std::string> golden = goldenAllPages(reader, renderer, params, si, cacheDir);
    if (golden.size() < 2) continue;  // need at least 2 pages to step backward

    // Collect each page's start cursor by forward-chaining.
    std::vector<compiled::PagePosition> starts;
    compiled::PagePosition cursor;
    cursor.spineIndex = static_cast<uint16_t>(si);
    for (size_t pageIdx = 0; pageIdx < golden.size(); ++pageIdx) {
      const compiled::LaidOutPage lp = compiled::layoutPage(reader, renderer, params, cursor);
      ASSERT_TRUE(lp.ok && lp.page);
      starts.push_back(lp.start);
      cursor = lp.end;
    }

    // For each page K>=1, backward from page K's start yields page K-1.
    for (size_t k = 1; k < golden.size(); ++k) {
      const compiled::LaidOutPage bwd = compiled::layoutPageBackward(reader, renderer, params, starts[k]);
      ASSERT_TRUE(bwd.ok && bwd.page) << "layoutPageBackward failed spine " << si << " from page " << k;
      std::ostringstream bwdOut;
      pipeline_harness::dumpOnePage(bwdOut, *bwd.page, 0, cacheDir);
      ASSERT_EQ(golden[k - 1], bwdOut.str())
          << "backward page diverges spine " << si << " (backward from page " << k << " should be page " << (k - 1)
          << ")";
      // The backward page's own start cursor must equal the forward page K-1's start.
      ASSERT_TRUE(bwd.start.samePosition(starts[k - 1]))
          << "backward start cursor mismatch spine " << si << " page " << (k - 1);
    }
  }
  binFile.close();
}

INSTANTIATE_TEST_SUITE_P(Matrix, PageLayoutMatrix, testing::ValuesIn(bookProfileMatrix()),
                         [](const testing::TestParamInfo<BookProfile>& info) {
                           std::string n = info.param.book + "_" + info.param.profile.name;
                           for (char& c : n)
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           return n;
                         });

}  // namespace
