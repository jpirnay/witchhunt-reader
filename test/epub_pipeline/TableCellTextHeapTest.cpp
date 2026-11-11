// A table cell whose text the heap cannot hold stops the build the way any low-heap stop does: the pages laid out
// before it are kept as a truncated section, and nothing aborts. The cell's ParsedText used to be a bare `new`, which
// aborts the firmware when the heap is out (-fno-exceptions). TABLE_CELL_FAULT_INJECTION refuses the n-th cell its
// text (ChapterHtmlSlimParser::makeCellText).
#include <gtest/gtest.h>
#include <unistd.h>

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
