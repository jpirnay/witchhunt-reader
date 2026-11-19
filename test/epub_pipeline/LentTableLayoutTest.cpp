// A table laid out in a lent region: its cells' TextBlocks, objects and bytes, live in the row block above the page
// block (memory-and-parsing §9b). A row the page cannot hold is dropped and laid out again on the next page, and a cell
// whose line list the heap refuses sends its row out as paragraphs; on both paths every line must be gone before its
// block is rewound. This binary fills a rewound block (BUILD_ARENA_TRACE), so a line destroyed after it aborts the
// test, and the layout must be the heap build's, byte for byte.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "PipelineRunner.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

fs::path freshWorkDir(const std::string& tag) {
  const fs::path dir =
      fs::temp_directory_path() / ("lent_table_layout_" + std::to_string(static_cast<long>(getpid()))) / tag;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

// Rows of three cells, several lines each at this page width, between two paragraphs: many rows open a page.
std::string longTable() {
  std::string body = "<p>before</p><table>";
  for (int r = 0; r < 60; ++r) {
    body += "<tr>";
    for (int c = 0; c < 3; ++c) {
      body += "<td>row " + std::to_string(r) + " cell " + std::to_string(c) + " with words enough to wrap</td>";
    }
    body += "</tr>";
  }
  return body + "</table><p>after</p>";
}

// The book's dump, built on the heap or in a lent region of the X3's framebuffer size; `lists` cell line lists may be
// allocated before each further one is refused (SIZE_MAX: none is).
std::string dump(const std::string& book, const fs::path& cache, const size_t lentBytes, const size_t lists) {
  pipeline_harness::Profile profile;
  profile.viewportWidth = 480;
  profile.viewportHeight = 800;
  profile.lentArenaBytes = lentBytes;
  cellLinesLeft() = lists;
  std::ostringstream out;
  EXPECT_TRUE(pipeline_harness::runAndDump(book, cache.string(), profile, out)) << out.str();
  cellLinesLeft() = SIZE_MAX;
  return out.str();
}

void expectLentLaysOutAsHeap(const std::string& tag, const size_t lists) {
  SCOPED_TRACE(tag);
  const fs::path work = freshWorkDir(tag);
  std::string book;
  {
    const auto epub = syntheticBook(work, 0, longTable());
    ASSERT_TRUE(epub);
    book = epub->getPath();
  }
  const std::string heap = dump(book, work / "heap", 0, lists);
  ASSERT_NE(std::string::npos, heap.find(" pages=")) << heap;
  EXPECT_EQ(heap, dump(book, work / "lent", 52272, lists));
  std::error_code ec;  // Windows keeps a just-closed file busy for a moment; a leftover temp dir is harmless
  fs::remove_all(work, ec);
}

}  // namespace

TEST(LentTableLayout, ARowLaidOutAgainOnTheNextPageLaysOutAsOnTheHeap) { expectLentLaysOutAsHeap("relaid", SIZE_MAX); }

TEST(LentTableLayout, ARefusedLineListLaysOutAsOnTheHeap) {
  // A refusal a few rows in, and one later: the rows from there on go out as paragraphs.
  expectLentLaysOutAsHeap("refused_early", 7);
  expectLentLaysOutAsHeap("refused_late", 100);
}
