// A page the heap cannot hold stops the build the way any build the heap refuses stops: the pages laid out before it
// are kept as a truncated section, and nothing aborts. Eight places made their page with a bare `new Page()`, which
// under -fno-exceptions aborts the firmware mid-build; the next open of the chapter rebuilt into the same abort. The
// rest made it with nothrow new and carried on with no page, to dereference it a few lines on. PAGE_FAULT_INJECTION
// refuses the n-th page (ChapterHtmlSlimParser::ensurePage).
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
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

std::string word(const int i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "w%04d", i);
  return buf;
}

// Every kind of element that makes a page of its own: paragraphs, a rule, a block picture, a grid table, a picture
// taller than the page (sliced over several), a floated picture.
std::string body() {
  std::string out;
  int n = 0;
  for (int i = 0; i < 12; ++i) out += "<p>" + word(n++) + "</p>\n";
  out += "<hr/>\n<p>" + word(n++) + "</p>\n<p><img src=\"pic.png\" alt=\"pic\"/></p>\n<p>" + word(n++) + "</p>\n";
  out += "<table>";
  for (int r = 0; r < 3; ++r) out += "<tr><td>" + word(n++) + "</td><td>" + word(n++) + "</td></tr>";
  out += "</table>\n<p><img src=\"tall.png\" alt=\"tall\"/></p>\n";
  out += "<p><img src=\"pic.png\" alt=\"float\" style=\"float: left; width: 100px\"/>" + word(n++) + "</p>\n";
  for (int i = 0; i < 12; ++i) out += "<p>" + word(n++) + "</p>\n";
  return out;
}

Section::BuildParams params() {
  Section::BuildParams p;
  p.viewportWidth = 480;
  p.viewportHeight = 160;
  p.lineCompression = 1.0f;
  p.embeddedStyle = true;
  return p;
}

// Restores "never refuse" however the test ends.
struct PagesLeft {
  explicit PagesLeft(const size_t n) { pagesLeft() = n; }
  ~PagesLeft() { pagesLeft() = SIZE_MAX; }
  PagesLeft(const PagesLeft&) = delete;
  PagesLeft& operator=(const PagesLeft&) = delete;
};

struct Built {
  bool ok = false;
  bool truncated = false;
  uint16_t pages = 0;
  std::vector<std::string> words;
};

struct PageHeap : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / ("page_heap_" + std::to_string(static_cast<long>(getpid())) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  Built build(const size_t pagesAllowed, const std::string& chapter = body()) {
    auto epub = syntheticBook(work, books++, chapter,
                              {{"pic.png", "image/png", syntheticPngHeader(300, 120)},
                               {"tall.png", "image/png", syntheticPngHeader(300, 600)}});
    epub->loadImageManifest();
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    Built out;
    {
      const PagesLeft guard(pagesAllowed);
      out.ok = section.createSectionFile(params(), {}, /*skipEviction=*/true);
    }
    if (!out.ok) return out;
    out.truncated = section.isTruncatedCache();
    out.pages = section.pageCount;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      EXPECT_NE(nullptr, page) << "page " << p;
      if (!page) break;
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock* block = static_cast<const PageLine&>(*el).getBlock();
        if (!block) continue;
        for (uint16_t w = 0; w < block->wordCount(); ++w) out.words.emplace_back(block->wordText(w));
      }
    }
    return out;
  }
};

}  // namespace

// Refuse the n-th page, for every page the whole chapter takes: the build ends, never aborts, keeps the pages before
// the refusal (a prefix of the whole chapter's words) and says it is truncated, which the reader rebuilds from.
TEST_F(PageHeap, ARefusedPageStopsTheBuildAndKeepsThePagesBeforeIt) {
  const Built whole = build(SIZE_MAX);
  ASSERT_TRUE(whole.ok);
  ASSERT_FALSE(whole.truncated);
  ASSERT_GE(whole.pages, 8) << "the chapter must span pages for every element kind to start one";
  for (size_t allowed = 0; allowed < whole.pages; ++allowed) {
    SCOPED_TRACE("pages allowed: " + std::to_string(allowed));
    const Built cut = build(allowed);
    if (allowed == 0) {
      EXPECT_FALSE(cut.ok) << "no page at all: nothing to keep";
      continue;
    }
    ASSERT_TRUE(cut.ok) << "the pages before the refusal are kept";
    EXPECT_TRUE(cut.truncated) << "the chapter stopped early and must say so";
    EXPECT_LT(cut.pages, whole.pages);
    ASSERT_LE(cut.words.size(), whole.words.size());
    for (size_t i = 0; i < cut.words.size(); ++i) ASSERT_EQ(cut.words[i], whole.words[i]) << "word " << i;
  }
}

// The same refusal in a child process, which a bare `new Page()` (-fno-exceptions: abort) would kill. Every count of
// pages allowed, so each element kind meets the refusal at its own page.
TEST_F(PageHeap, ARefusedPageNeverAborts) {
  const Built whole = build(SIZE_MAX);
  ASSERT_TRUE(whole.ok);
  EXPECT_EXIT(
      {
        for (size_t allowed = 0; allowed < whole.pages; ++allowed) {
          const Built cut = build(allowed);
          if (allowed > 0 && !(cut.ok && cut.truncated)) std::exit(1);
        }
        std::exit(0);
      },
      testing::ExitedWithCode(0), "");
}

// A chapter that opens with each kind of element, its first page refused: each made that page itself, five of them with
// a bare `new Page()`. Nothing to keep, so the build fails, and the reader falls back as for any failed build.
TEST_F(PageHeap, EveryElementThatOpensAChapterTakesTheRefusal) {
  const char* const openers[] = {
      "<hr/><p>after</p>",
      "<p><img src=\"pic.png\" alt=\"pic\"/></p><p>after</p>",
      "<table><tr><td>a</td><td>b</td></tr></table><p>after</p>",
      "<p><img src=\"tall.png\" alt=\"tall\"/></p><p>after</p>",
      "<p><img src=\"pic.png\" alt=\"float\" style=\"float: left; width: 100px\"/>after</p>",
      "<p>after</p>",
  };
  for (const char* opener : openers) {
    SCOPED_TRACE(opener);
    const Built whole = build(SIZE_MAX, opener);
    ASSERT_TRUE(whole.ok);
    EXPECT_FALSE(whole.truncated);
    EXPECT_FALSE(build(0, opener).ok);
    if (whole.pages > 1) {
      const Built cut = build(1, opener);
      ASSERT_TRUE(cut.ok);
      EXPECT_TRUE(cut.truncated);
    }
  }
}
