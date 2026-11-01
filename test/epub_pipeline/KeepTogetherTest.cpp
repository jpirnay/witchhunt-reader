// Widow and orphan control, and headings kept with what follows them.
//
// A page used to end wherever the next line no longer fit: a paragraph's first line could sit
// alone at the foot of a page (an orphan), its last line alone at the head of the next (a widow),
// and a heading could end a page with its text overleaf. The page now ends earlier where that
// would happen -- decided before each line is placed, since a placed line is never moved again
// (its bytes live in the page's arena block, rewound when the page is handed off).
//
// The geometry makes every count exact: the column is 100 px wide and every word ("aaaaNNN",
// 66-84 px in the stub renderer) fills a line of its own, so a paragraph of N words is N lines;
// a page of 240 px holds exactly ten 24 px body lines. One-word paragraphs fill a page to a known
// height first.
//
// What has to hold (CSS defaults orphans: 2, widows: 2):
//   1. A paragraph whose first line alone would fit moves to the next page.
//   2. A paragraph whose last line alone would go over leaves one more line with it.
//   3. A paragraph that cannot split 2/2 (three lines, two fitting) moves whole.
//   4. A paragraph that splits 3/2 anyway is left alone; so is one that fits.
//   5. A heading that would end the page without two lines of what follows moves with them.
//   6. A paragraph longer than a page still splits (no page is left empty, nothing loops), and
//      its last page gets two lines, also when it is laid out in parts (over 96 words).
//   7. The build lent an arena (the reader's background build, where the page-fit test runs
//      before the line exists) lays out the same pages.
//   8. With the reader's "Avoid Widows & Orphans" off, every page fills (#579: on a page of ten
//      lines, two-line paragraphs that may not split left whole lines empty). A heading still
//      keeps with its text, and the chapter is cached apart from the one laid out with the rule.
//   9. A vertical section (the viewport is swapped, so the frame is the same 100 px column by
//      240 px block extent) fills its pages whatever the setting says; a heading still keeps
//      with its text.
//
// test_keep_together.epub sets shapes 1-5 and its golden pins them in heap mode, the harness
// default, so those run here in the lent arena only. The rest run in both modes: the book has no
// two-line paragraph and no heading with room, and its long paragraph ends with lines to spare,
// so no golden reaches the widow on the last page of a paragraph longer than a page.
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

struct Layout {
  uint16_t pages = 0;
  std::map<std::string, uint16_t> pageOf;  // each word's page
  std::map<uint16_t, int> linesOn;         // text lines per page
};

class KeepTogetherLayout : public testing::Test {
 protected:
  fs::path work;
  int books = 0;
  int words = 0;
  bool lentArena = false;
  bool widowOrphanControl = true;
  bool vertical = false;
  std::shared_ptr<Epub> book;  // the last one laid out

  Section::BuildParams buildParams() const {
    Section::BuildParams params;
    params.viewportWidth = 100;
    params.viewportHeight = 240;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    params.widowOrphanControl = widowOrphanControl;
    if (vertical) {
      params.verticalText = VerticalText::On;
      params.viewportWidth = 240;
      params.viewportHeight = 100;
    }
    return params;
  }

  void SetUp() override {
    std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char& c : name)
      if (c == '/') c = '_';
    work = fs::temp_directory_path() / ("epub_keeptogether_" + name);
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override {
    book.reset();  // it holds its cache files open, which Windows will not remove
    fs::remove_all(work);
  }

  // The next unique one-line word.
  std::string word() {
    const int n = words++;
    return "aaaa" + std::to_string(n / 100 % 10) + std::to_string(n / 10 % 10) + std::to_string(n % 10);
  }
  // A paragraph of `lines` lines; `first`/`last` receive its first and last word.
  std::string para(const int lines, std::string* first = nullptr, std::string* last = nullptr) {
    std::string out = "<p>";
    for (int i = 0; i < lines; ++i) {
      const std::string w = word();
      if (i == 0 && first) *first = w;
      if (i == lines - 1 && last) *last = w;
      out += (i ? " " : "") + w;
    }
    return out + "</p>";
  }
  std::string fill(const int lines) {
    std::string out;
    for (int i = 0; i < lines; ++i) out += para(1);
    return out;
  }

  Layout layout(const std::string& body) {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
        body + "\n</body></html>\n";
    book = syntheticBookFromDocument(work, books++, doc,
                                     {{"style.css", "text/css", "p, h2 { margin: 0; text-indent: 0; }"}});
    const Section::BuildParams params = buildParams();
    GfxRenderer renderer;
    Section section(book, 0, renderer);
    std::unique_ptr<BuildArena> lent;
    if (lentArena) {
      lent = std::make_unique<BuildArena>(52272);
      section.setExternalBuildScratch(lent.get());
    }
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    Layout out;
    out.pages = section.pageCount;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      if (vertical) EXPECT_TRUE(page->isVertical());
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
        out.linesOn[p]++;
        for (uint16_t w = 0; w < block.wordCount(); ++w) out.pageOf[block.wordText(w)] = p;
      }
    }
    return out;
  }
};

class KeepTogetherInArena : public KeepTogetherLayout {
 protected:
  void SetUp() override {
    lentArena = true;
    KeepTogetherLayout::SetUp();
  }
};

class KeepTogetherFixture : public KeepTogetherLayout, public testing::WithParamInterface<bool> {
 protected:
  void SetUp() override {
    lentArena = GetParam();
    KeepTogetherLayout::SetUp();
  }
};

}  // namespace

TEST_F(KeepTogetherInArena, OrphanMovesTheParagraphToTheNextPage) {
  std::string first;
  const auto l = layout(fill(9) + para(4, &first));
  EXPECT_EQ(l.pageOf.at(first), 1) << "the paragraph's first line was left alone at the foot of the page";
  EXPECT_EQ(l.linesOn.at(0), 9);
}

TEST_F(KeepTogetherInArena, WidowTakesALineWithIt) {
  std::string first, last;
  const auto l = layout(fill(7) + para(4, &first, &last));
  EXPECT_EQ(l.pageOf.at(first), 0);
  EXPECT_EQ(l.linesOn.at(0), 9) << "three lines fit, but only two may stay: the last must not go over alone";
  EXPECT_EQ(l.linesOn.at(1), 2);
  EXPECT_EQ(l.pageOf.at(last), 1);
}

TEST_F(KeepTogetherInArena, ThreeLinesThatCannotSplitMoveWhole) {
  std::string first;
  const auto l = layout(fill(8) + para(3, &first));
  EXPECT_EQ(l.pageOf.at(first), 1);
  EXPECT_EQ(l.linesOn.at(1), 3);
}

TEST_P(KeepTogetherFixture, TwoLineParagraphMovesWhole) {
  std::string first;
  const auto l = layout(fill(9) + para(2, &first));
  EXPECT_EQ(l.pageOf.at(first), 1);
}

TEST_F(KeepTogetherInArena, AGoodSplitAndAFittingParagraphAreLeftAlone) {
  std::string first, last;
  auto l = layout(fill(7) + para(5, &first, &last));
  EXPECT_EQ(l.linesOn.at(0), 10) << "a 3/2 split breaks no rule";
  EXPECT_EQ(l.pageOf.at(first), 0);
  EXPECT_EQ(l.linesOn.at(1), 2);
  l = layout(fill(6) + para(4, &first));
  EXPECT_EQ(l.pages, 1);
}

TEST_F(KeepTogetherInArena, HeadingMovesWithWhatFollows) {
  std::string first;
  // The h2 is 34 px (1.4 x 24): after eight lines it fits at 192..226, its paragraph does not.
  const auto l = layout(fill(8) + "<h2>Hdg</h2>" + para(3, &first));
  EXPECT_EQ(l.pageOf.at("Hdg"), 1) << "the heading ended the page with its text overleaf";
  EXPECT_EQ(l.pageOf.at(first), 1);
}

TEST_P(KeepTogetherFixture, HeadingWithRoomStays) {
  const auto l = layout(fill(5) + "<h2>Hdg</h2>" + para(3));
  EXPECT_EQ(l.pages, 1);
}

TEST_P(KeepTogetherFixture, ParagraphLongerThanAPageStillSplits) {
  std::string first, last;
  const auto l = layout(fill(9) + para(21, &first, &last));
  EXPECT_EQ(l.pageOf.at(first), 1);
  EXPECT_EQ(l.linesOn.at(1), 10);
  EXPECT_EQ(l.linesOn.at(2), 9) << "the paragraph's last line went over alone";
  EXPECT_EQ(l.linesOn.at(3), 2);
  EXPECT_EQ(l.pageOf.at(last), 3);
}

TEST_P(KeepTogetherFixture, ParagraphLaidOutInPartsKeepsItsLastLines) {
  // 101 words: the parser lays out the first 96 lines before the paragraph ends (its 96-word
  // split), the last five once it does. Pages of ten: ten full pages and one line over.
  std::string last;
  const auto l = layout(para(101, nullptr, &last));
  EXPECT_EQ(l.pages, 11);
  EXPECT_EQ(l.linesOn.at(9), 9);
  EXPECT_EQ(l.linesOn.at(10), 2);
  EXPECT_EQ(l.pageOf.at(last), 10);
}

TEST_P(KeepTogetherFixture, ControlOffFillsThePage) {
  widowOrphanControl = false;
  std::string first, last;
  auto l = layout(fill(9) + para(2, &first, &last));
  EXPECT_EQ(l.pageOf.at(first), 0) << "the first line may stay alone at the foot of the page";
  EXPECT_EQ(l.linesOn.at(0), 10);
  EXPECT_EQ(l.pageOf.at(last), 1);
  l = layout(fill(8) + para(3, &first));
  EXPECT_EQ(l.linesOn.at(0), 10) << "a paragraph that cannot split 2/2 splits 2/1";
  l = layout(fill(7) + para(4, &first, &last));
  EXPECT_EQ(l.linesOn.at(0), 10) << "the last line may go over alone";
  EXPECT_EQ(l.linesOn.at(1), 1);
  EXPECT_EQ(l.pageOf.at(last), 1);
}

TEST_P(KeepTogetherFixture, ControlOffStillKeepsAHeadingWithItsText) {
  widowOrphanControl = false;
  std::string first;
  const auto l = layout(fill(8) + "<h2>Hdg</h2>" + para(3, &first));
  EXPECT_EQ(l.pageOf.at("Hdg"), 1);
  EXPECT_EQ(l.pageOf.at(first), 1);
}

TEST_P(KeepTogetherFixture, ControlOffIsCachedApart) {
  layout(fill(9) + para(2));  // cached with the rule on
  Section::BuildParams params = buildParams();
  GfxRenderer renderer;
  Section section(book, 0, renderer);
  EXPECT_TRUE(section.loadSectionFile(params));
  params.widowOrphanControl = false;
  EXPECT_FALSE(section.loadSectionFile(params)) << "pages laid out with the rule were reused without it";
}

// 9 lines then 1 of 4 fit: the horizontal rule would move the paragraph; a vertical page fills.
TEST_P(KeepTogetherFixture, AVerticalSectionLeavesAnOrphan) {
  vertical = true;
  std::string first;
  const auto l = layout(fill(9) + para(4, &first));
  EXPECT_EQ(l.pageOf.at(first), 0) << "the column fits: a vertical page fills it";
  EXPECT_EQ(l.linesOn.at(0), 10);
}

// 7 lines then 3 of 4 fit: the horizontal rule stops at 2, leaving two for the next page.
TEST_P(KeepTogetherFixture, AVerticalSectionLeavesAWidow) {
  vertical = true;
  std::string first, last;
  const auto l = layout(fill(7) + para(4, &first, &last));
  EXPECT_EQ(l.linesOn.at(0), 10);
  EXPECT_EQ(l.linesOn.at(1), 1);
  EXPECT_EQ(l.pageOf.at(last), 1);
}

// The h2 is 34 px at 192..226 with no room for two lines below (keep-with-next, unchanged).
TEST_P(KeepTogetherFixture, AVerticalHeadingStillMovesWithWhatFollows) {
  vertical = true;
  std::string first;
  const auto l = layout(fill(8) + "<h2>Hdg</h2>" + para(3, &first));
  EXPECT_EQ(l.pageOf.at("Hdg"), 1);
  EXPECT_EQ(l.pageOf.at(first), 1);
}

// 1 + 10 + 10 lines of 21 on pages 0-2.
TEST_P(KeepTogetherFixture, AVerticalParagraphLongerThanAPageStillSplits) {
  vertical = true;
  std::string first, last;
  const auto l = layout(fill(9) + para(21, &first, &last));
  EXPECT_EQ(l.linesOn.at(0), 10);
  EXPECT_EQ(l.pageOf.at(first), 0);
  EXPECT_EQ(l.pageOf.at(last), 2);
}

// 9 lines then 1 of 4 fit (as the orphan case above): the page ends the same with the setting on and off.
TEST_P(KeepTogetherFixture, AVerticalSectionIgnoresTheSetting) {
  vertical = true;
  widowOrphanControl = true;
  std::string first;
  const auto on = layout(fill(9) + para(4, &first));
  const int onPage = on.pageOf.at(first);
  const int onLines = on.linesOn.at(0);
  widowOrphanControl = false;
  const auto off = layout(fill(9) + para(4, &first));
  EXPECT_EQ(off.pageOf.at(first), onPage);
  EXPECT_EQ(off.linesOn.at(0), onLines);
}

INSTANTIATE_TEST_SUITE_P(HeapAndLentArena, KeepTogetherFixture, testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) { return info.param ? "Arena" : "Heap"; });
