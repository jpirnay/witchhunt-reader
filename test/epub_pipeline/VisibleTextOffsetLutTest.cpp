// The section cache's content-offset LUT: for every page, the number of visible bytes of the
// chapter's source text before the page's first element (VisibleText.h's rule, counted by the
// layout parser), and the lookup the reader uses to turn an offset back into a page.
//
// The layout parser and the KOReader XPath mappers count with the same rule over the same SAX
// stream, so for every chapter the last page's start must not exceed the mapper's own count of
// the chapter, and the starts must be in page order. Two pages can share a start (a page whose
// first element is an image, a rule or a table fragment, followed by the text at the same
// offset); the lookup then answers the FIRST page of the run, the one the reader reaches first.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "KOReaderSync/ChapterXPathIndexerInternal.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  return p;
}

std::vector<std::string> corpusBooks() {
  std::vector<std::string> books;
  for (const auto& entry : fs::directory_iterator(CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") books.push_back(entry.path().string());
  }
  std::sort(books.begin(), books.end());
  return books;
}

struct LutFixture : testing::TestWithParam<std::string> {
  fs::path work;
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void SetUp() override {
    std::string tag = testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char& c : tag) {
      if (c == '/') c = '_';
    }
    work = fs::temp_directory_path() / "visible_text_lut" / tag;
    fs::remove_all(work);
    fs::create_directories(work);
    epub = std::make_shared<Epub>(GetParam(), (work / "cache").string());
    ASSERT_TRUE(epub->load(true)) << GetParam();
    epub->setupCacheDir();
  }
  void TearDown() override {
    epub.reset();
    fs::remove_all(work);
  }

  std::unique_ptr<Section> build(const int spine) {
    auto section = std::make_unique<Section>(epub, spine, renderer);
    EXPECT_TRUE(section->createSectionFile(params(), {}, /*skipEviction=*/true)) << "spine " << spine;
    EXPECT_TRUE(section->loadSectionFile(params())) << "spine " << spine;
    return section;
  }
};

TEST_P(LutFixture, StartsAreInPageOrderAndWithinTheChapter) {
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine);
    const auto counted = ChapterXPathIndexerInternal::countTotalTextBytes(epub, spine);
    ASSERT_TRUE(counted.has_value()) << "spine " << spine;
    const size_t total = *counted;
    // The chapter's first offset is on its first page, even when that page opens past it (a
    // chapter that starts with hidden text).
    if (section->pageCount > 0) {
      EXPECT_EQ(section->getPageForVisibleTextOffset(0), std::optional<uint16_t>(0)) << "spine " << spine;
    }
    uint32_t previous = 0;
    for (uint16_t page = 0; page < section->pageCount; ++page) {
      const auto start = section->getVisibleTextOffsetForPage(page);
      ASSERT_TRUE(start.has_value()) << "spine " << spine << " page " << page;
      EXPECT_GE(*start, previous) << "spine " << spine << " page " << page;
      EXPECT_LE(*start, total) << "spine " << spine << " page " << page;
      previous = *start;
    }
  }
}

TEST_P(LutFixture, EveryWindowEndsAtTheFirstLaterDistinctStart) {
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine);
    for (uint16_t page = 0; page < section->pageCount; ++page) {
      const uint32_t start = *section->getVisibleTextOffsetForPage(page);
      std::optional<uint32_t> oracle;
      for (uint16_t later = page + 1; later < section->pageCount && !oracle; ++later) {
        const uint32_t laterStart = *section->getVisibleTextOffsetForPage(later);
        if (laterStart > start) oracle = laterStart;
      }
      EXPECT_EQ(section->getVisibleTextOffsetAfterPage(page), oracle) << "spine " << spine << " page " << page;
    }
  }
}

TEST_P(LutFixture, EveryStartLooksUpItsOwnPage) {
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine);
    for (uint16_t page = 0; page < section->pageCount; ++page) {
      const uint32_t start = *section->getVisibleTextOffsetForPage(page);
      // The first page of a run of equal starts is the one an offset resolves to.
      uint16_t first = page;
      while (first > 0 && *section->getVisibleTextOffsetForPage(first - 1) == start) --first;
      EXPECT_EQ(section->getPageForVisibleTextOffset(start), std::optional<uint16_t>(first))
          << "spine " << spine << " page " << page << " start " << start;
      // An offset inside the page (one byte before the next start) resolves to the page itself
      // or to the last page of its run.
      if (page + 1 < section->pageCount) {
        const uint32_t next = *section->getVisibleTextOffsetForPage(page + 1);
        if (next > start) {
          EXPECT_EQ(section->getPageForVisibleTextOffset(next - 1), std::optional<uint16_t>(page))
              << "spine " << spine << " page " << page;
        }
      }
    }
  }
}

TEST_P(LutFixture, PageBeyondTheLutHasNoOffset) {
  const auto section = build(0);
  EXPECT_EQ(section->getVisibleTextOffsetForPage(section->pageCount), std::nullopt);
  EXPECT_EQ(section->getVisibleTextOffsetForPage(0xFFFF), std::nullopt);
}

INSTANTIATE_TEST_SUITE_P(Corpus, LutFixture, testing::ValuesIn(corpusBooks()),
                         [](const testing::TestParamInfo<std::string>& info) {
                           std::string name = fs::path(info.param).stem().string();
                           for (char& c : name) {
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           }
                           return name;
                         });

// --- Shapes the corpus does not pin ---------------------------------------------------------------

struct SyntheticLutFixture : testing::Test {
  fs::path work;
  GfxRenderer renderer;
  int bookCount = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("visible_text_lut_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::shared_ptr<Epub> book(const std::string& body) { return syntheticBook(work, bookCount++, body); }

  std::unique_ptr<Section> build(const std::shared_ptr<Epub>& epub, const bool embeddedStyle = false) {
    Section::BuildParams p = params();
    p.embeddedStyle = embeddedStyle;
    auto section = std::make_unique<Section>(epub, 0, renderer);
    EXPECT_TRUE(section->createSectionFile(p, {}, true));
    EXPECT_TRUE(section->loadSectionFile(p));
    return section;
  }

  static std::string paragraphs(const int count) {
    std::string out;
    for (int i = 1; i <= count; ++i) {
      out += "<p>Paragraph " + std::to_string(i) +
             " carries enough ordinary words to give it some weight on the page and more.</p>\n";
    }
    return out;
  }
};

TEST_F(SyntheticLutFixture, StyleTextInsideBodyIsNotCounted) {
  // The mapper skips <style> wherever it is; so must the parser, or the two drift apart by the
  // length of the stylesheet for every page after it.
  const auto plainBook = book(paragraphs(80));
  const auto plain = build(plainBook);
  const auto styledBook = book("<style>p { margin: 0 } " + std::string(400, 'x') + "</style>\n" + paragraphs(80));
  const auto styled = build(styledBook);
  // The mapper's count of the styled chapter leaves the stylesheet out; a counted stylesheet
  // would push the later starts past it.
  const auto counted = ChapterXPathIndexerInternal::countTotalTextBytes(styledBook, 0);
  ASSERT_TRUE(counted.has_value());
  const size_t total = *counted;
  for (uint16_t page = 0; page < styled->pageCount; ++page) {
    EXPECT_LE(*styled->getVisibleTextOffsetForPage(page), total) << "page " << page;
  }
  // The layout may still lay the stylesheet out as words (it skips only <head>), which can move
  // the page breaks; the starts can only be compared page by page when it did not.
  if (plain->pageCount == styled->pageCount) {
    for (uint16_t page = 0; page < plain->pageCount; ++page) {
      EXPECT_EQ(styled->getVisibleTextOffsetForPage(page), plain->getVisibleTextOffsetForPage(page)) << "page " << page;
    }
  }
}

TEST_F(SyntheticLutFixture, HiddenTextIsCountedLikeTheMapperCounts) {
  // display:none text is not laid out but it IS source text: the mappers count it, so the LUT
  // must too, or a page after it would be reported too early by the hidden text's length.
  // The `hidden` attribute is the parser's display:none; an inline style="display:none" would
  // be ignored here, these params building without embedded CSS.
  const auto plain = build(book(paragraphs(80)));
  const std::string hidden = "<p hidden=\"hidden\">" + std::string(300, 'h') + "</p>\n";
  const auto withHidden = build(book(hidden + paragraphs(80)));
  ASSERT_EQ(plain->pageCount, withHidden->pageCount);
  for (uint16_t page = 1; page < plain->pageCount; ++page) {
    EXPECT_EQ(*withHidden->getVisibleTextOffsetForPage(page), *plain->getVisibleTextOffsetForPage(page) + 300)
        << "page " << page;
  }
  // Page 0 opens past the hidden text, at 300; a position inside that text is still on page 0.
  EXPECT_EQ(*withHidden->getVisibleTextOffsetForPage(0), 300u);
  EXPECT_EQ(withHidden->getPageForVisibleTextOffset(0), std::optional<uint16_t>(0));
  EXPECT_EQ(withHidden->getPageForVisibleTextOffset(299), std::optional<uint16_t>(0));
}

TEST_F(SyntheticLutFixture, MarkerLedPagesStartAtTheItemsFirstWord) {
  // A list marker goes in while the item's first word is flushed, inside a text chunk, where the
  // chapter counter already holds the chunk's end. The <b> splits every item into two chunks, so a
  // marker that took the counter would start its line, and any page that line opens, 8 bytes late
  // (the end of "Item1001 "). Every item is one line of the same visible length, so every page
  // opens on an item: its start must be a whole number of items.
  constexpr uint32_t kItemBytes = 23;  // "Item1001" + "alpha" + "beta" + "gamma."
  std::string items = "<ul>\n";
  for (int i = 1; i <= 200; ++i) {
    items += "<li>Item" + std::to_string(1000 + i) + " <b>alpha</b> beta gamma.</li>\n";
  }
  items += "</ul>\n";
  const auto section = build(book(items));
  ASSERT_GT(section->pageCount, 2);
  for (uint16_t page = 0; page < section->pageCount; ++page) {
    EXPECT_EQ(*section->getVisibleTextOffsetForPage(page) % kItemBytes, 0u) << "page " << page;
  }
}

TEST_F(SyntheticLutFixture, EmptyPageStartsWhereTheNextElementDoes) {
  // A top margin taller than the page pushes the opening paragraph's first line off page 0, which
  // goes out with nothing on it. That happens while the paragraph is laid out, after its text was
  // counted, so the counter is already past the line that opens page 1; the empty page must start
  // where that line does. Inline styles need embedded CSS.
  const auto section =
      build(book("<p style=\"margin-top: 2000px\">Opening paragraph below a tall margin.</p>\n" + paragraphs(20)),
            /*embeddedStyle=*/true);
  ASSERT_GT(section->pageCount, 2);
  EXPECT_EQ(*section->getVisibleTextOffsetForPage(1), 0u) << "page 1 no longer opens the chapter's text";
  EXPECT_EQ(*section->getVisibleTextOffsetForPage(0), 0u);
  for (uint16_t page = 1; page < section->pageCount; ++page) {
    EXPECT_GE(*section->getVisibleTextOffsetForPage(page), *section->getVisibleTextOffsetForPage(page - 1))
        << "page " << page;
  }
}

TEST_F(SyntheticLutFixture, OffsetAfterAPageSkipsPagesThatShareItsStart) {
  // The window of a page is [start(p), after(p)). Pages 0 and 1 tie (the empty page is back-filled
  // with the next element's offset), so neither may report the other's start as its end.
  const auto section =
      build(book("<p style=\"margin-top: 2000px\">Opening paragraph below a tall margin.</p>\n" + paragraphs(20)),
            /*embeddedStyle=*/true);
  ASSERT_GE(section->pageCount, 3);
  const uint32_t tied = *section->getVisibleTextOffsetForPage(0);
  ASSERT_EQ(*section->getVisibleTextOffsetForPage(1), tied) << "the fixture no longer produces a tie";
  uint16_t firstBeyond = 0;
  while (firstBeyond < section->pageCount && *section->getVisibleTextOffsetForPage(firstBeyond) == tied) ++firstBeyond;
  ASSERT_LT(firstBeyond, section->pageCount);
  const uint32_t beyond = *section->getVisibleTextOffsetForPage(firstBeyond);
  ASSERT_GT(beyond, tied);
  for (uint16_t page = 0; page < firstBeyond; ++page) {
    EXPECT_EQ(section->getVisibleTextOffsetAfterPage(page), std::optional<uint32_t>(beyond)) << "page " << page;
  }
  for (uint16_t page = firstBeyond; page + 1 < section->pageCount; ++page) {
    const auto after = section->getVisibleTextOffsetAfterPage(page);
    ASSERT_TRUE(after.has_value()) << "page " << page;
    EXPECT_GT(*after, *section->getVisibleTextOffsetForPage(page)) << "page " << page;
  }
  EXPECT_FALSE(section->getVisibleTextOffsetAfterPage(section->pageCount - 1).has_value());
  EXPECT_FALSE(section->getVisibleTextOffsetAfterPage(section->pageCount).has_value());
}

TEST_F(SyntheticLutFixture, DropCapPageStartsAtTheCapLetter) {
  // The cap line is placed when its span closes, after the captured letter was counted; a page it
  // opens starts at the letter, not one byte later. Inline styles need embedded CSS.
  const auto section = build(book("<p><span style=\"float: left; font-size: 3em\">A</span>ll of this opening paragraph "
                                  "wraps beside a drop cap.</p>\n" +
                                  paragraphs(20)),
                             /*embeddedStyle=*/true);
  ASSERT_GT(section->pageCount, 0);
  EXPECT_EQ(*section->getVisibleTextOffsetForPage(0), 0u);
}

TEST_F(SyntheticLutFixture, RowDegradedMidCellClosesItsFragmentAtTheRowStart) {
  // A cell past the row budget (256 words) degrades its row while a word is being flushed, mid
  // chunk. The wordless fragment packed before it is closed then; it stands before the row's first
  // word, not before the word in hand or the chunk's end.
  std::string cell;
  for (int i = 0; i < 300; ++i) cell += "word ";
  const auto section = build(book("<table><tr><td></td><td></td></tr><tr><td>" + cell + "</td></tr></table>\n"));
  ASSERT_GT(section->pageCount, 0);
  EXPECT_EQ(*section->getVisibleTextOffsetForPage(0), 0u);
}

TEST_F(SyntheticLutFixture, TableFragmentStartsAtItsFirstRow) {
  // A table's opening header row is lifted out to be repeated before the row is packed; the first
  // fragment must still start at the header's first word, not at the first data row's. A
  // continuation fragment repeats the header but starts at its first data row.
  constexpr uint32_t kHeaderBytes = 9;  // "Name" + "Value"
  constexpr uint32_t kRowBytes = 12;    // "Row1001" + "value"
  std::string table = "<table><thead><tr><th>Name</th><th>Value</th></tr></thead><tbody>\n";
  for (int i = 1; i <= 120; ++i) {
    table += "<tr><td>Row" + std::to_string(1000 + i) + "</td><td>value</td></tr>\n";
  }
  table += "</tbody></table>\n";
  const auto section = build(book(table));
  ASSERT_GT(section->pageCount, 1);
  EXPECT_EQ(*section->getVisibleTextOffsetForPage(0), 0u);
  for (uint16_t page = 1; page < section->pageCount; ++page) {
    const uint32_t start = *section->getVisibleTextOffsetForPage(page);
    ASSERT_GT(start, kHeaderBytes) << "page " << page;
    EXPECT_EQ((start - kHeaderBytes) % kRowBytes, 0u) << "page " << page;
  }
}

TEST_F(SyntheticLutFixture, TiesResolveToTheFirstPageOfTheRun) {
  // A page break forced before a paragraph gives two consecutive pages the same start only when
  // the first holds no text; a page-break rule (<hr>) before the paragraph makes a rule-only page.
  // The lookup must answer that page, the one the reader reaches first. A single rule lands
  // mid-page; a run of them long enough to fill a page always makes a rule-only page.
  std::string rules;
  for (int i = 0; i < 60; ++i) rules += "<hr/>\n";
  const auto section = build(book(paragraphs(40) + rules + paragraphs(40)));
  bool sawTie = false;
  for (uint16_t page = 1; page < section->pageCount; ++page) {
    const uint32_t start = *section->getVisibleTextOffsetForPage(page);
    if (*section->getVisibleTextOffsetForPage(page - 1) == start) {
      sawTie = true;
      EXPECT_LT(*section->getPageForVisibleTextOffset(start), page);
    }
  }
  EXPECT_TRUE(sawTie) << "the rules no longer fill a page; the test checks nothing";
}

}  // namespace
