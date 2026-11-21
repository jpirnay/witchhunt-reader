// A page the heap cannot hold stops the build the way any build the heap refuses stops: the pages laid out before it
// are kept as a truncated section, and nothing aborts. Eight places made their page with a bare `new Page()`, which
// under -fno-exceptions aborts the firmware mid-build; the next open of the chapter rebuilt into the same abort. The
// rest made it with nothrow new and carried on with no page, to dereference it a few lines on. PAGE_FAULT_INJECTION
// refuses the n-th page (ChapterHtmlSlimParser::ensurePage). ELEMENT_FAULT_INJECTION does the same for the n-th element
// a page takes (ChapterHtmlSlimParser::addPageElement): `Page::elements` grew with a bare push_back, which aborts too.
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
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
  p.lineCompression = TextScale();
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

// Restores "never refuse" however the test ends.
struct ElementsLeft {
  ElementsLeft(const size_t n, const bool recovers) {
    elementsLeft() = n;
    elementRefusalRecovers() = recovers;
  }
  ~ElementsLeft() {
    elementsLeft() = SIZE_MAX;
    elementRefusalRecovers() = false;
  }
  ElementsLeft(const ElementsLeft&) = delete;
  ElementsLeft& operator=(const ElementsLeft&) = delete;
};

struct Built {
  bool ok = false;
  bool truncated = false;
  uint16_t pages = 0;
  size_t elements = 0;               // over all pages
  size_t firstPage = 0;              // elements on page 0
  std::vector<size_t> pageElements;  // per page
  size_t widestPage = 0;             // most elements on one page
  size_t imageElements = 0;
  size_t lineImageElements = 0;  // of those, the ones inside a line of text
  size_t ruleElements = 0;
  size_t tableElements = 0;
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

  Built build(const size_t pagesAllowed, const std::string& chapter = body(), const size_t elementsAllowed = SIZE_MAX,
              const uint16_t viewportHeight = 160, const std::string& css = {}, const bool recovers = false) {
    std::vector<SyntheticFile> extra = {{"pic.png", "image/png", syntheticPngHeader(300, 120)},
                                        {"tall.png", "image/png", syntheticPngHeader(300, 600)},
                                        {"huge.png", "image/png", syntheticPngHeader(300, 1500)},
                                        {"icon.png", "image/png", syntheticPngHeader(24, 24)}};
    std::shared_ptr<Epub> epub;
    if (css.empty()) {
      epub = syntheticBook(work, books++, chapter, extra);
    } else {
      extra.push_back({"style.css", "text/css", css});
      epub = syntheticBookFromDocument(
          work, books++,
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
          "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
              chapter + "\n</body></html>\n",
          extra);
    }
    epub->loadImageManifest();
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    Built out;
    {
      const PagesLeft guard(pagesAllowed);
      const ElementsLeft elementGuard(elementsAllowed, recovers);
      Section::BuildParams p = params();
      p.viewportHeight = viewportHeight;
      out.ok = section.createSectionFile(p, {}, /*skipEviction=*/true);
    }
    if (!out.ok) return out;
    out.truncated = section.isTruncatedCache();
    out.pages = section.pageCount;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      EXPECT_NE(nullptr, page) << "page " << p;
      if (!page) break;
      out.elements += page->elements.size();
      if (p == 0) out.firstPage = page->elements.size();
      out.pageElements.push_back(page->elements.size());
      out.widestPage = std::max(out.widestPage, page->elements.size());
      for (const auto& el : page->elements) {
        switch (el->getTag()) {
          case TAG_PageImage:
            ++out.imageElements;
            if (static_cast<const PageImage&>(*el).isInLine()) ++out.lineImageElements;
            break;
          case TAG_PageHR:
            ++out.ruleElements;
            break;
          case TAG_PageTable:
            ++out.tableElements;
            break;
          default:
            break;
        }
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

// `Page::elements` grows through ChapterHtmlSlimParser::addPageElement, and every element kind adds through it. A page
// of more than 32 elements (Page::TYPICAL_ELEMENTS, the reserve each page starts with) makes the table grow by a
// doubling. Refuse the n-th element add over a chapter that has such a page as its SECOND page, so a refusal anywhere
// on it, or on a page after it, leaves the first page to keep.
namespace {
constexpr uint16_t kElementViewport = 1200;

const char* const kElementCss =
    ".il { initial-letter: 3 }\n"
    ".bg { background-image: url(pic.png); background-repeat: no-repeat }\n"
    ".bgtall { background-image: url(huge.png); background-repeat: no-repeat }\n";

std::string prose(const int first, const int words) {
  std::string out;
  for (int i = 0; i < words; ++i) out += word(first + i) + " ";
  return out;
}

// Page 0 is three lines, then a forced break. Page 1 opens with 34 one-line paragraphs (a table of 34 elements, past
// the 32 it starts with) and a long paragraph, and goes on with one of each other element kind, so that each of the
// nine places that add an element meets the refusal while page 0 is there to keep:
//   lines (addLineToPage), a rule, a block picture, a floated picture, a drop cap, a picture inside a line of text, two
//   background pictures (placeImageBlockAsBlock's block branch), a grid table (flushTableFragment); a last paragraph
//   long enough to cross page breaks. Not reached: placeImageBlockAsBlock's slice branch. Every caller sizes the
//   picture to fit the viewport (buildImageFromEpubPath), so no picture is taller than a page and a 1500 px one comes
//   out 240 px wide at 1200 px high; the branch is dead on the callers there are today.
std::string elementBody() {
  std::string out;
  for (int i = 0; i < 3; ++i) out += "<p>" + word(i) + "</p>\n";
  out += "<p style=\"page-break-before: always\">" + word(100) + "</p>\n";
  for (int i = 1; i < 34; ++i) out += "<p>" + word(100 + i) + "</p>\n";
  out += "<p>" + prose(200, 70) + "</p>\n";
  out += "<hr/>\n<p>" + word(300) + "</p>\n";
  out += "<p><img src=\"pic.png\" alt=\"block\"/></p>\n";
  out += "<p><img src=\"pic.png\" alt=\"float\" style=\"float: left; width: 100px\"/>" + word(301) + "</p>\n";
  out += "<p><span class=\"il\">T</span>" + prose(400, 60) + "</p>\n";
  out += "<p>" + word(302) + " <img src=\"icon.png\" alt=\"icon\"/> " + word(303) + "</p>\n";
  out += "<div class=\"bg\"><p>" + word(304) + "</p></div>\n";
  out += "<table><tr><td>" + word(305) + "</td><td>" + word(306) + "</td></tr></table>\n";
  out += "<div class=\"bgtall\"><p>" + word(307) + "</p></div>\n";
  for (int i = 0; i < 3; ++i) out += "<p>" + word(500 + i) + "</p>\n";
  out += "<p>" + prose(600, 700) + "</p>\n";  // runs over several pages: its lines are the ones in flight at a break
  return out;
}

// Every count from 0 through the doubling (page 0's elements, then 32 + a few), then every `stride`th.
bool sweeps(const size_t allowed, const size_t firstPage, const size_t stride) {
  return allowed <= firstPage + 36 || (allowed - firstPage) % stride == 0;
}
}  // namespace

TEST_F(PageHeap, ARefusedElementStopsTheBuildAndKeepsThePagesBeforeIt) {
  const Built whole = build(SIZE_MAX, elementBody(), SIZE_MAX, kElementViewport, kElementCss);
  ASSERT_TRUE(whole.ok);
  ASSERT_FALSE(whole.truncated);
  ASSERT_GE(whole.pages, 2);
  ASSERT_GT(whole.widestPage, Page::TYPICAL_ELEMENTS) << "a page must outgrow its first reserve";
  ASSERT_GT(whole.firstPage, 0u);
  ASSERT_LT(whole.firstPage, whole.widestPage) << "the page with more than 32 elements must be a later page";
  // Each of the nine sites has an element in the chapter to refuse.
  EXPECT_GE(whole.imageElements, 5u) << "block, float, two backgrounds, inline";
  EXPECT_GE(whole.lineImageElements, 1u);
  EXPECT_GE(whole.ruleElements, 1u);
  EXPECT_GE(whole.tableElements, 1u);
  for (size_t allowed = 0; allowed < whole.elements; ++allowed) {
    if (!sweeps(allowed, whole.firstPage, 1)) continue;
    SCOPED_TRACE("elements allowed: " + std::to_string(allowed));
    const Built cut = build(SIZE_MAX, elementBody(), allowed, kElementViewport, kElementCss);
    if (allowed == 0) {
      EXPECT_FALSE(cut.ok) << "nothing laid out: nothing to keep";
      continue;
    }
    // A refusal on page 0 may or may not leave a page (the page break in flight emits what it holds); whatever is kept
    // must be truncated and a prefix. From page 1 on, page 0 is there to keep.
    if (allowed < whole.firstPage && !cut.ok) continue;
    ASSERT_TRUE(cut.ok) << "the pages before the refusal are kept";
    EXPECT_TRUE(cut.truncated) << "the chapter stopped early and must say so";
    ASSERT_LE(cut.words.size(), whole.words.size());
    for (size_t i = 0; i < cut.words.size(); ++i) ASSERT_EQ(cut.words[i], whole.words[i]) << "word " << i;
  }
}

// A build the heap refused is over: the paragraph being laid out when the refusal came goes on through its lines (the
// SAX parser stops, the line loop does not), and a heap that has recovered by then must not take them, nor a later
// page's. Refuse exactly the n-th add and admit every one after it: what is kept is still a prefix of the whole
// chapter.
TEST_F(PageHeap, NothingLandsAfterARefusedElementWhenTheHeapComesBack) {
  const Built whole = build(SIZE_MAX, elementBody(), SIZE_MAX, kElementViewport, kElementCss);
  ASSERT_TRUE(whole.ok);
  ASSERT_GT(whole.widestPage, Page::TYPICAL_ELEMENTS);
  // The lines in flight are the ones around a page break: refuse each of the last three elements of every page.
  std::vector<size_t> refusals;
  size_t end = 0;
  for (size_t p = 0; p + 1 < whole.pageElements.size(); ++p) {
    end += whole.pageElements[p];
    for (size_t back = 1; back <= 3 && back <= end; ++back) {
      if (end - back >= whole.firstPage) refusals.push_back(end - back);
    }
  }
  ASSERT_GE(refusals.size(), 6u);
  for (const size_t refused : refusals) {
    SCOPED_TRACE("the refused element: " + std::to_string(refused));
    const Built cut = build(SIZE_MAX, elementBody(), refused, kElementViewport, kElementCss, /*recovers=*/true);
    ASSERT_TRUE(cut.ok);
    EXPECT_TRUE(cut.truncated);
    ASSERT_LE(cut.words.size(), whole.words.size());
    for (size_t i = 0; i < cut.words.size(); ++i) ASSERT_EQ(cut.words[i], whole.words[i]) << "word " << i;
    // The page the refusal came on, and every page after it, are not in the section: the whole's page count bounds it.
    EXPECT_LE(cut.elements, refused) << "an element landed after the refused one";
  }
}

// The same refusal in a child process, which a bare push_back (-fno-exceptions: abort) would kill.
TEST_F(PageHeap, ARefusedElementNeverAborts) {
  const Built whole = build(SIZE_MAX, elementBody(), SIZE_MAX, kElementViewport, kElementCss);
  ASSERT_TRUE(whole.ok);
  EXPECT_EXIT(
      {
        for (size_t allowed = 0; allowed < whole.elements; ++allowed) {
          if (allowed % 4 != 0) continue;
          const Built cut = build(SIZE_MAX, elementBody(), allowed, kElementViewport, kElementCss);
          if (cut.ok && !cut.truncated) std::exit(1);
        }
        std::exit(0);
      },
      testing::ExitedWithCode(0), "");
}
