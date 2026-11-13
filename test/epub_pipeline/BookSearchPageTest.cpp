// A search hit lands on the page that shows it: the hit's offset, counted as the search reads the chapter out of the
// EPUB, is turned into a page by the section's offset LUT, which the layout parser filled while laying the same text
// out (Section::getPageForVisibleTextOffset), and that page holds the word. Markers spread over many pages, inside
// inline markup and after entities, which both counts must take alike.
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/BookSearch.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

struct Collected final : BookSearch::HitSink {
  std::vector<BookSearch::Hit> hits;
  void add(const BookSearch::Hit& hit) override { hits.push_back(hit); }
};

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  return p;
}

std::string wordsOn(const Page& page) {
  std::string out;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const TextBlock* block = static_cast<const PageLine&>(*el).getBlock();
    for (uint16_t w = 0; block && w < block->wordCount(); ++w) {
      out += block->wordText(w);
      out += ' ';
    }
  }
  return out;
}

}  // namespace

TEST(BookSearchPage, EveryHitIsOnThePageItsOffsetNames) {
  const fs::path work = fs::temp_directory_path() / "book_search_page";
  fs::remove_all(work);
  fs::create_directories(work);
  std::string body;
  const int kMarkers = 12;
  for (int paragraph = 0; paragraph < 120; ++paragraph) {
    body += "<p>";
    body += "Plain words fill the paragraph so that the chapter runs over many pages of text. ";
    if (paragraph % 10 == 4) {
      const int marker = paragraph / 10;
      // Some inside markup, some after an entity the layout expands, some plain.
      if (marker % 3 == 0) body += "<i>emph</i> &amp; <b>zq" + std::to_string(marker) + "marker</b>";
      if (marker % 3 == 1) body += "caf&#233;&nbsp;zq" + std::to_string(marker) + "marker";
      if (marker % 3 == 2) body += "zq" + std::to_string(marker) + "marker";
      body += " closes it.";
    }
    body += "</p>";
  }
  const std::shared_ptr<Epub> epub = syntheticBook(work, 0, body);
  GfxRenderer renderer;
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true));
  ASSERT_TRUE(section.loadSectionFile(params()));
  ASSERT_GT(section.pageCount, 5) << "the markers must be spread over pages";

  std::vector<int> pages;
  for (int marker = 0; marker < kMarkers; ++marker) {
    const std::string word = "zq" + std::to_string(marker) + "marker";
    const BookSearch::Pattern pattern(word);
    Collected out;
    size_t count = 0;
    BookSearch::ChapterSearch chapter(pattern, /*wholeWords=*/true, out, 4, count, 0,
                                      static_cast<uint32_t>(epub->getCumulativeSpineItemSize(0)));
    ASSERT_TRUE(chapter.begin());
    ASSERT_TRUE(epub->readItemContentsToStream(epub->getSpineItem(0).href, chapter.sink(), 1024, chapter.stopFlag()));
    ASSERT_TRUE(chapter.finish());
    ASSERT_EQ(1u, count) << word;
    ASSERT_EQ(1u, out.hits.size()) << word;
    const std::optional<uint16_t> page = section.getPageForVisibleTextOffset(out.hits[0].offset);
    ASSERT_TRUE(page.has_value()) << word;
    section.currentPage = *page;
    const std::unique_ptr<Page> loaded = section.loadPageFromSectionFile();
    ASSERT_TRUE(loaded) << word;
    EXPECT_NE(std::string::npos, wordsOn(*loaded).find(word)) << word << " is not on page " << *page;
    pages.push_back(*page);
  }
  EXPECT_LT(pages.front(), pages.back()) << "the markers fell on different pages";
  fs::remove_all(work);
}
