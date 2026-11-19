// A table cell whose text the heap cannot hold stops the build the way any low-heap stop does: the pages laid out
// before it are kept as a truncated section, and nothing aborts. The cell's ParsedText used to be a bare `new`, which
// aborts the firmware when the heap is out (-fno-exceptions). TABLE_CELL_FAULT_INJECTION refuses the n-th cell its
// text (ChapterHtmlSlimParser::makeCellText).
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr int kLeadParagraphs = 40;
constexpr int kRows = 3;
constexpr int kCols = 3;

Section::BuildParams pages() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 96;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  return p;
}

std::string word(const int i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "w%04d", i);
  return buf;
}

// Paragraphs that fill several pages, then a grid table, then a closing paragraph.
std::string body() {
  std::string out;
  for (int i = 0; i < kLeadParagraphs; ++i) out += "<p>" + word(i) + "</p>\n";
  out += "<table>\n";
  for (int r = 0; r < kRows; ++r) {
    out += "<tr>";
    for (int c = 0; c < kCols; ++c) out += "<td>c" + std::to_string(r) + std::to_string(c) + "</td>";
    out += "</tr>\n";
  }
  out += "</table>\n<p>after</p>\n";
  return out;
}

fs::path freshWorkDir(const std::string& tag) {
  const auto dir =
      fs::temp_directory_path() / ("table_cell_text_heap_" + std::to_string(static_cast<long>(getpid()))) / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::vector<std::string> wordsOfAllPages(Section& section) {
  std::vector<std::string> words;
  for (int pg = 0; pg < section.pageCount; ++pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    EXPECT_TRUE(page) << "page " << pg;
    if (!page) break;
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const auto& block = static_cast<const PageLine&>(*el).getBlock();
      if (!block || !block->valid()) continue;
      for (uint16_t w = 0; w < block->wordCount(); ++w) words.emplace_back(block->wordText(w));
    }
  }
  return words;
}

// Restores "never refuse" however the test ends.
struct CellTextsLeft {
  explicit CellTextsLeft(const size_t n) { tableCellTextsLeft() = n; }
  ~CellTextsLeft() { tableCellTextsLeft() = SIZE_MAX; }
  CellTextsLeft(const CellTextsLeft&) = delete;
  CellTextsLeft& operator=(const CellTextsLeft&) = delete;
};

}  // namespace

TEST(TableCellTextHeap, EveryCellWithItsTextBuildsTheWholeChapter) {
  const auto work = freshWorkDir("whole");
  auto epub = syntheticBook(work, 0, body());
  ASSERT_TRUE(epub);
  GfxRenderer renderer;
  Section built(epub, 0, renderer);
  ASSERT_TRUE(built.createSectionFile(pages(), {}, /*skipEviction=*/true));
  EXPECT_FALSE(built.isTruncatedCache());
  const auto words = wordsOfAllPages(built);
  ASSERT_FALSE(words.empty());
  EXPECT_EQ(words.back(), "after");
  epub.reset();
  fs::remove_all(work);
}

// Refuse the text of the n-th cell, for every cell of the table: the build stops there instead of aborting, keeps
// every paragraph before the table, and says it is truncated.
TEST(TableCellTextHeap, ARefusedCellStopsTheBuildAndKeepsThePagesBeforeIt) {
  GfxRenderer renderer;
  for (size_t allowed = 0; allowed < static_cast<size_t>(kRows * kCols); ++allowed) {
    SCOPED_TRACE("cells allowed their text: " + std::to_string(allowed));
    const auto work = freshWorkDir("refused_" + std::to_string(allowed));
    auto epub = syntheticBook(work, static_cast<int>(allowed), body());
    ASSERT_TRUE(epub);
    Section built(epub, 0, renderer);
    bool ok = false;
    {
      const CellTextsLeft guard(allowed);
      ok = built.createSectionFile(pages(), {}, /*skipEviction=*/true);
    }
    ASSERT_TRUE(ok) << "the paragraphs before the table fill pages, which are kept";
    EXPECT_TRUE(built.isTruncatedCache()) << "the chapter stopped early and must say so";
    const auto words = wordsOfAllPages(built);
    ASSERT_GE(words.size(), static_cast<size_t>(kLeadParagraphs) / 2);
    for (size_t i = 0; i < words.size() && i < static_cast<size_t>(kLeadParagraphs); ++i) {
      ASSERT_EQ(words[i], word(static_cast<int>(i))) << "word " << i;
    }
    EXPECT_NE(words.back(), "after");
    epub.reset();
    fs::remove_all(work);
  }
}

namespace {

// Restores "never refuse" and the count however the test ends.
struct CellLineHooks {
  CellLineHooks(const size_t scratch, const size_t lists) {
    cellLineScratchLeft() = scratch;
    cellLinesLeft() = lists;
    cellLinesAllocated() = 0;
  }
  ~CellLineHooks() {
    cellLineScratchLeft() = SIZE_MAX;
    cellLinesLeft() = SIZE_MAX;
  }
  CellLineHooks(const CellLineHooks&) = delete;
  CellLineHooks& operator=(const CellLineHooks&) = delete;
};

bool hasWord(const std::vector<std::string>& words, const std::string& w) {
  for (const auto& x : words)
    if (x == w) return true;
  return false;
}

// Builds `body()` with the hooks set; the build must finish whole, the table's words all present.
void expectTableKeptAsParagraphs(const std::string& tag, const size_t scratch, const size_t lists) {
  SCOPED_TRACE(tag);
  GfxRenderer renderer;
  const auto work = freshWorkDir(tag);
  auto epub = syntheticBook(work, 0, body());
  ASSERT_TRUE(epub);
  Section built(epub, 0, renderer);
  bool ok = false;
  {
    const CellLineHooks guard(scratch, lists);
    ok = built.createSectionFile(pages(), {}, /*skipEviction=*/true);
  }
  ASSERT_TRUE(ok);
  EXPECT_FALSE(built.isTruncatedCache()) << "a refused line list is a softer exit: the row goes out as paragraphs";
  const auto words = wordsOfAllPages(built);
  for (int r = 0; r < kRows; ++r) {
    for (int c = 0; c < kCols; ++c) {
      const std::string cell = "c" + std::to_string(r) + std::to_string(c);
      EXPECT_TRUE(hasWord(words, cell)) << "lost " << cell;
    }
  }
  EXPECT_EQ(words.back(), "after");
  epub.reset();
  fs::remove_all(work);
}

}  // namespace

// Each cell's line list is one allocation at its exact size, and the table's first grid row reserves the scratch once.
TEST(TableCellLines, OneAllocationPerCell) {
  GfxRenderer renderer;
  const auto work = freshWorkDir("lines_count");
  auto epub = syntheticBook(work, 0, body());
  ASSERT_TRUE(epub);
  Section built(epub, 0, renderer);
  {
    const CellLineHooks guard(SIZE_MAX, SIZE_MAX);
    ASSERT_TRUE(built.createSectionFile(pages(), {}, /*skipEviction=*/true));
    // Whole rows, one allocation per cell: a row the page cannot hold is laid out again on the next page.
    EXPECT_GE(cellLinesAllocated(), static_cast<size_t>(kRows * kCols));
    EXPECT_EQ(cellLinesAllocated() % kCols, 0u);
  }
  EXPECT_FALSE(built.isTruncatedCache());
  epub.reset();
  fs::remove_all(work);
}

TEST(TableCellLines, ARefusedScratchSendsTheRowOutAsParagraphs) {
  expectTableKeptAsParagraphs("scratch_refused", 0, SIZE_MAX);
}

TEST(TableCellLines, ARefusedExactListSendsTheRowOutAsParagraphs) {
  for (size_t allowed = 0; allowed < static_cast<size_t>(kCols); ++allowed) {
    expectTableKeptAsParagraphs("list_refused_" + std::to_string(allowed), SIZE_MAX, allowed);
  }
}

namespace {

enum class TableGrowth { RowCells, LayoutCells, PackerRows };

// Restores "never refuse" however the test ends. `asks` successes of `kind`, then `refusals` refused asks (SIZE_MAX:
// every one from then on).
struct GrowthHooks {
  GrowthHooks(const TableGrowth kind, const size_t asks, const size_t refusals) {
    tableRowCellsLeft() = layoutCellsLeft() = packerRowsLeft() = SIZE_MAX;
    size_t& left = kind == TableGrowth::RowCells      ? tableRowCellsLeft()
                   : kind == TableGrowth::LayoutCells ? layoutCellsLeft()
                                                      : packerRowsLeft();
    left = asks;
    tableGrowthRefusals() = refusals;
    tableGrowthRefused() = 0;
  }
  ~GrowthHooks() {
    tableRowCellsLeft() = layoutCellsLeft() = packerRowsLeft() = SIZE_MAX;
    tableGrowthRefusals() = SIZE_MAX;
  }
  GrowthHooks(const GrowthHooks&) = delete;
  GrowthHooks& operator=(const GrowthHooks&) = delete;
};

// The words of every page, a grid row's cells included (wordsOfAllPages sees paragraphs only), in reading order.
std::vector<std::string> wordsOfAllPagesAndGrids(Section& section) {
  std::vector<std::string> words;
  const auto addBlock = [&words](const TextBlock* block) {
    if (!block || !block->valid()) return;
    for (uint16_t w = 0; w < block->wordCount(); ++w) words.emplace_back(block->wordText(w));
  };
  for (int pg = 0; pg < section.pageCount; ++pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    EXPECT_TRUE(page) << "page " << pg;
    if (!page) break;
    for (const auto& el : page->elements) {
      if (el->getTag() == TAG_PageLine) {
        addBlock(static_cast<const PageLine&>(*el).getBlock());
      } else if (el->getTag() == TAG_PageTable) {
        for (const auto& row : static_cast<const PageTableFragment&>(*el).getRows())
          for (const auto& cell : row.cells)
            for (const auto& line : cell.lines) addBlock(line.get());
      }
    }
  }
  return words;
}

// Every word the whole chapter holds, in order.
std::vector<std::string> expectedWords() {
  std::vector<std::string> out;
  for (int i = 0; i < kLeadParagraphs; ++i) out.push_back(word(i));
  for (int r = 0; r < kRows; ++r)
    for (int c = 0; c < kCols; ++c) out.push_back("c" + std::to_string(r) + std::to_string(c));
  out.push_back("after");
  return out;
}

// Builds `body()` refusing the (`asks`+1)-th ask of `kind`. A refusal the table absorbs leaves the whole chapter, every
// word in order; one the build cannot absorb stops it with the pages before it kept and nothing lost out of order.
// Returns whether any ask was refused (false: `asks` is past the table's last ask).
bool buildRefusingGrowth(const TableGrowth kind, const size_t asks, const size_t refusals, const bool mustStop) {
  SCOPED_TRACE("kind " + std::to_string(static_cast<int>(kind)) + ", asks allowed " + std::to_string(asks) +
               ", refusals " + std::to_string(refusals));
  GfxRenderer renderer;
  const auto work = freshWorkDir("growth_" + std::to_string(static_cast<int>(kind)) + "_" + std::to_string(asks) + "_" +
                                 std::to_string(refusals == SIZE_MAX ? 99 : refusals));
  auto epub = syntheticBook(work, 0, body());
  EXPECT_TRUE(epub);
  if (!epub) return false;
  size_t refused = 0;
  {
    Section built(epub, 0, renderer);
    bool ok = false;
    {
      const GrowthHooks guard(kind, asks, refusals);
      ok = built.createSectionFile(pages(), {}, /*skipEviction=*/true);
      refused = tableGrowthRefused();
    }
    EXPECT_TRUE(ok);
    const auto words = wordsOfAllPagesAndGrids(built);
    const auto expected = expectedWords();
    if (refused == 0) {
      EXPECT_FALSE(built.isTruncatedCache());
      EXPECT_EQ(words, expected);
    } else if (mustStop) {
      EXPECT_TRUE(built.isTruncatedCache()) << "the heap stayed out: the build stops and says so";
      EXPECT_GE(words.size(), static_cast<size_t>(kLeadParagraphs) / 2);
      EXPECT_LE(words.size(), expected.size());
      EXPECT_TRUE(std::equal(words.begin(), words.end(), expected.begin())) << "what was kept is a prefix, in order";
    } else {
      EXPECT_FALSE(built.isTruncatedCache()) << "a softer exit: the row goes out as paragraphs";
      EXPECT_EQ(words, expected) << "every word of every degraded row, in order";
    }
  }
  epub.reset();
  fs::remove_all(work);
  return refused != 0;
}

// Walks the refusal over every ask of `kind` the fixture makes, until a count that refuses nothing.
void walkGrowth(const TableGrowth kind, const size_t refusals, const bool mustStop) {
  size_t asks = 0;
  while (buildRefusingGrowth(kind, asks, refusals, mustStop)) {
    ++asks;
    ASSERT_LT(asks, 64u) << "the walk never ran out of asks";
  }
  EXPECT_GT(asks, 0u) << "the fixture made no ask of this kind";
}

}  // namespace

// A refused cell slot degrades the row it would have joined, keeping its text, and asks once more.
TEST(TableRowGrowth, ARefusedCellSlotDegradesTheRowAndKeepsItsWords) {
  walkGrowth(TableGrowth::RowCells, 1, /*mustStop=*/false);
}

// A second refusal in a row has no row left to degrade: the build stops with its pages kept.
TEST(TableRowGrowth, ARefusedCellSlotThatStaysRefusedStopsTheBuild) {
  walkGrowth(TableGrowth::RowCells, SIZE_MAX, /*mustStop=*/true);
}

// A refused laid-out cell list is the paragraph exit, whether it is one row or every row.
TEST(TableRowGrowth, ARefusedLayoutCellListSendsTheRowOutAsParagraphs) {
  walkGrowth(TableGrowth::LayoutCells, 1, /*mustStop=*/false);
  walkGrowth(TableGrowth::LayoutCells, SIZE_MAX, /*mustStop=*/false);
}

// A refused fragment row list flushes the fragment and asks once more.
TEST(TableRowGrowth, ARefusedFragmentRowListFlushesAndAsksAgain) {
  walkGrowth(TableGrowth::PackerRows, 1, /*mustStop=*/false);
}

// Refused twice in a row, the build stops with its pages kept.
TEST(TableRowGrowth, AFragmentRowListRefusedTwiceStopsTheBuild) {
  walkGrowth(TableGrowth::PackerRows, SIZE_MAX, /*mustStop=*/true);
}
