// The section cache's content-offset LUT: for every page, the number of visible bytes of the
// chapter's source text before the page's first element (VisibleText.h's rule, counted by the
// layout parser), and the lookup the reader uses to turn an offset back into a page.
//
// The layout parser and the KOReader XPath mappers count with the same rule over the same SAX
// stream, so for every chapter the last page's start must not exceed the mapper's own count of
// the chapter, and the starts must be in page order. Two pages can share a start (a page whose
// first element is an image, a rule or a table fragment, followed by the text at the same
// offset); the lookup then answers the LAST page of the run, the one that holds the text at that
// offset.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "KOReaderSync/ChapterXPathIndexerInternal.h"
#include "KOReaderSync/ProgressMapper.h"
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
      // The last page of a run of equal starts is the one an offset resolves to.
      uint16_t last = page;
      while (last + 1 < section->pageCount && *section->getVisibleTextOffsetForPage(last + 1) == start) ++last;
      EXPECT_EQ(section->getPageForVisibleTextOffset(start), std::optional<uint16_t>(last))
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

  std::shared_ptr<Epub> book(const std::string& body, const std::vector<SyntheticFile>& extra = {}) {
    return syntheticBook(work, bookCount++, body, extra);
  }

  // Each run of two or more consecutive pages sharing one start, as {first page, last page}.
  static std::vector<std::pair<uint16_t, uint16_t>> tiedRuns(const Section& section) {
    std::vector<std::pair<uint16_t, uint16_t>> runs;
    for (uint16_t page = 1; page < section.pageCount; ++page) {
      if (*section.getVisibleTextOffsetForPage(page) != *section.getVisibleTextOffsetForPage(page - 1)) continue;
      if (!runs.empty() && runs.back().second == page - 1) {
        runs.back().second = page;
      } else {
        runs.emplace_back(static_cast<uint16_t>(page - 1), page);
      }
    }
    return runs;
  }

  // The page a push from `page` lands on when pulled back, as the reader resolves it.
  static std::optional<uint16_t> roundTrip(const std::shared_ptr<Epub>& epub, const Section& section,
                                           const uint16_t page) {
    CrossPointPosition pos{};
    pos.spineIndex = 0;
    pos.pageNumber = page;
    pos.totalPages = section.pageCount;
    pos.visibleTextOffset = *section.getVisibleTextOffsetForPage(page);
    pos.hasVisibleTextOffset = true;
    const auto ko = ProgressMapper::toKOReader(epub, pos);
    const auto back = ProgressMapper::toCrossPoint(epub, ko);
    EXPECT_TRUE(back.hasVisibleTextOffset) << "page " << page << " via " << ko.xpath;
    if (!back.hasVisibleTextOffset) return std::nullopt;
    return section.getPageForVisibleTextOffset(back.visibleTextOffset);
  }

  static bool holdsImage(Section& section, const uint16_t page) {
    section.currentPage = page;
    const auto loaded = section.loadPageFromSectionFile();
    if (!loaded) return false;
    for (const auto& el : loaded->elements) {
      if (el->getTag() == TAG_PageImage) return true;
    }
    return false;
  }

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

// A page past the LUT's end has no start: a bounds guard, the same for every book.
TEST_F(SyntheticLutFixture, PageBeyondTheLutHasNoOffset) {
  const auto section = build(book(paragraphs(80)));
  ASSERT_GT(section->pageCount, 1);
  EXPECT_EQ(section->getVisibleTextOffsetForPage(section->pageCount), std::nullopt);
  EXPECT_EQ(section->getVisibleTextOffsetForPage(0xFFFF), std::nullopt);
}

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

TEST_F(SyntheticLutFixture, RubyAndRpTextCountTowardTheContentOffsets) {
  // The parser drops <rp> from the layout and takes <rt> into a ruby group, but KOSync's offsets count
  // every text byte in the chapter, parentheses and readings included. Each paragraph is 42 visible
  // bytes (6 + 3 + 12 + 3 + 18) and one line, so each page starts at 42 x the lines on the pages before it.
  constexpr uint32_t kParagraphBytes = 42;
  constexpr int kParagraphs = 200;
  std::string body;
  for (int i = 0; i < kParagraphs; ++i) {
    body += "<p><ruby>吾輩<rp>（</rp><rt>わがはい</rt><rp>）</rp></ruby>は猫である。</p>\n";
  }
  const auto epub = book(body);
  const auto section = build(epub);
  ASSERT_GT(section->pageCount, 2);
  const auto counted = ChapterXPathIndexerInternal::countTotalTextBytes(epub, 0);
  ASSERT_TRUE(counted.has_value());
  EXPECT_EQ(*counted, kParagraphBytes * kParagraphs);
  uint32_t lines = 0;
  for (uint16_t page = 0; page < section->pageCount; ++page) {
    EXPECT_EQ(*section->getVisibleTextOffsetForPage(page), kParagraphBytes * lines) << "page " << page;
    EXPECT_EQ(roundTrip(epub, *section, page), std::optional<uint16_t>(page)) << "page " << page;
    section->currentPage = page;
    const auto loaded = section->loadPageFromSectionFile();
    ASSERT_TRUE(loaded);
    for (const auto& el : loaded->elements) {
      if (el->getTag() == TAG_PageLine) ++lines;
    }
  }
  EXPECT_EQ(lines, static_cast<uint32_t>(kParagraphs));
}

TEST_F(SyntheticLutFixture, KeptBidiMarksLeaveTheContentOffsetsAsTheyWere) {
  // The RLM opening each right-to-left paragraph stays in its first word for the line pass, but no word's offset starts
  // at it. Each page then starts where it did when the parser dropped the mark: past 3 bytes for every paragraph up to
  // its first. A paragraph is one line of 23 visible bytes ("שלום" + "עולם" + "טוב."), so every page opens on one.
  constexpr uint32_t kParagraphBytes = 23;
  std::string marked;
  std::string plain;
  for (int i = 0; i < 120; ++i) {
    marked += "<p>&#x200F;שלום עולם טוב.</p>\n";
    plain += "<p>שלום עולם טוב.</p>\n";
  }
  const auto withMarks = build(book(marked));
  const auto without = build(book(plain));
  ASSERT_GT(without->pageCount, 1);
  ASSERT_EQ(withMarks->pageCount, without->pageCount);
  for (uint16_t page = 0; page < without->pageCount; ++page) {
    const uint32_t start = *without->getVisibleTextOffsetForPage(page);
    EXPECT_EQ(*withMarks->getVisibleTextOffsetForPage(page), start + 3 * (1 + start / kParagraphBytes))
        << "page " << page;
  }
}

TEST_F(SyntheticLutFixture, LiteralIsolatesCountAsText) {
  // A literal LRI and PDI are text, not controls kept uncounted: each page starts at its first paragraph's LRI, and a
  // position at that paragraph's start is on the page. A paragraph is one line of 29 visible bytes, 23 of letters and a
  // period and 3 for each isolate, so every page opens on one.
  constexpr uint32_t kPlainBytes = 23;
  constexpr uint32_t kParagraphBytes = 29;
  std::string isolated;
  std::string plain;
  for (int i = 0; i < 120; ++i) {
    isolated += "<p>&#x2066;שלום עולם טוב.&#x2069;</p>\n";
    plain += "<p>שלום עולם טוב.</p>\n";
  }
  const auto withIsolates = build(book(isolated));
  const auto without = build(book(plain));
  ASSERT_GT(without->pageCount, 1);
  ASSERT_EQ(withIsolates->pageCount, without->pageCount);
  for (uint16_t page = 0; page < without->pageCount; ++page) {
    const uint32_t paragraphStart = *without->getVisibleTextOffsetForPage(page) / kPlainBytes * kParagraphBytes;
    EXPECT_EQ(*withIsolates->getVisibleTextOffsetForPage(page), paragraphStart) << "page " << page;
    EXPECT_EQ(withIsolates->getPageForVisibleTextOffset(paragraphStart), std::optional<uint16_t>(page))
        << "page " << page;
  }
}

TEST_F(SyntheticLutFixture, InsertedControlsCountNowhere) {
  // The span's RLI and PDI are markup the parser puts in the text, not source text: every page starts where it does
  // without the span.
  std::string spanned;
  std::string plain;
  for (int i = 0; i < 120; ++i) {
    spanned += "<p><span dir=\"rtl\">שלום</span> עולם טוב.</p>\n";
    plain += "<p>שלום עולם טוב.</p>\n";
  }
  const auto withSpans = build(book(spanned));
  const auto without = build(book(plain));
  ASSERT_GT(without->pageCount, 1);
  ASSERT_EQ(withSpans->pageCount, without->pageCount);
  for (uint16_t page = 0; page < without->pageCount; ++page) {
    EXPECT_EQ(withSpans->getVisibleTextOffsetForPage(page), without->getVisibleTextOffsetForPage(page))
        << "page " << page;
  }
}

TEST_F(SyntheticLutFixture, ATokenAfterAKeptJoinerKeepsItsSourceOffset) {
  // The word is split at an inline edge, 66 glued letters (the width of a line) in the first token and the joiner and
  // three more letters in the second, which the line breaker puts at a line's start. A page opens on that line in some
  // paragraphs, and its offset is the second token's letter's position in the source, as VisibleText counts it: the
  // joiner's 3 bytes are counted between the tokens, as the mappers count them. A paragraph is 60 one-byte words, 66
  // beh (132 bytes), a ZWNJ and three beh (9 bytes), and 100 one-byte words. The two tokens stay as the parser cut
  // them, in any font: their style keys differ, a beh and a ZWNJ do not meet inside a syllable
  // (indic::syllableBreakAllowed is true), and the second token is not marks only, so deviation 7 does not merge them
  // (and in this font, which cannot shape Arabic, nothing is merged at all). An Indic word cut after a virama is merged
  // whatever its styles (the glue falls inside a syllable), in a paragraph that needs bidi too since PR 4b, so it no
  // longer shows this.
  constexpr uint32_t kBefore = 60;
  constexpr uint32_t kSecondToken = kBefore + 132 + 3;
  constexpr uint32_t kParagraphBytes = kBefore + 132 + 9 + 100;
  std::string before;
  for (uint32_t i = 0; i < kBefore; ++i) before += "w ";
  std::string after;
  for (int i = 0; i < 100; ++i) after += "w ";
  std::string letters;
  for (int i = 0; i < 66; ++i) letters += "ب";
  std::string joined;
  for (int i = 0; i < 40; ++i) {
    joined += "<p dir=\"rtl\">" + before + letters + "<b>\xE2\x80\x8C" + "ببب</b> " + after + "</p>\n";
  }
  const auto section = build(book(joined));
  ASSERT_GT(section->pageCount, 1);
  int atSecondToken = 0;
  for (uint16_t page = 0; page < section->pageCount; ++page) {
    const uint32_t within = *section->getVisibleTextOffsetForPage(page) % kParagraphBytes;
    EXPECT_TRUE(within == 0 || within == kSecondToken) << "page " << page << " opens " << within << " bytes in";
    if (within == kSecondToken) ++atSecondToken;
  }
  EXPECT_GT(atSecondToken, 0);
}

TEST_F(SyntheticLutFixture, TheVisibleOffsetsAreUnchangedByAKeptArabicJoiner) {
  // The Indic test above with an Arabic word: 65 letters (2 bytes each), a ZWNJ cut at an inline edge and counted
  // between the tokens (3 bytes), then three letters. The Arabic letters are narrow, so the word is never cut by a
  // line: a page that opens after it opens at the next word's source position, the joiner's 3 bytes counted. A
  // paragraph is 60 one-byte words, 130 + 3 + 6 bytes, and 100 words.
  constexpr uint32_t kBefore = 60;
  constexpr uint32_t kAfterWord = kBefore + 130 + 3 + 6;
  constexpr uint32_t kParagraphBytes = kBefore + 130 + 3 + 6 + 100;
  std::string before;
  for (uint32_t i = 0; i < kBefore; ++i) before += "w ";
  std::string after;
  for (int i = 0; i < 100; ++i) after += "w ";
  std::string letters;
  for (int i = 0; i < 65; ++i) letters += "\u0628";
  std::string joined;
  for (int i = 0; i < 40; ++i) {
    joined += "<p dir=\"rtl\">" + before + letters + "<b>\xE2\x80\x8C" + "\u0628\u0628\u0628</b> " + after + "</p>\n";
  }
  const auto section = build(book(joined));
  ASSERT_GT(section->pageCount, 1);
  int afterWord = 0;
  for (uint16_t page = 0; page < section->pageCount; ++page) {
    const uint32_t within = *section->getVisibleTextOffsetForPage(page) % kParagraphBytes;
    EXPECT_TRUE(within == 0 || within == kAfterWord) << "page " << page << " opens " << within << " bytes in";
    if (within == kAfterWord) ++afterWord;
  }
  EXPECT_GT(afterWord, 0);
}

TEST_F(SyntheticLutFixture, AMergedWordSplitAfterItsJoinerStartsItsPageAtItsVisiblePrefix) {
  // A guard, not a RED test. The word of the test above with its joiner near its start, in a paragraph that shapes:
  // the inline edges are merged away (deviation 7: the glue after the virama falls inside a syllable, and the letters
  // after </b> are the word's first style), and the merged word, wider than the line, is split between syllables. A
  // page that opens on the remainder opens at the word's offset plus its prefix's visible bytes, the kept joiner left
  // out of them, as for any split inside a word. A paragraph is 60 one-byte words, a letter and its virama (6 bytes),
  // the joiner, a letter and 66 more (204 bytes), and 100 one-byte words.
  constexpr uint32_t kBefore = 60;
  constexpr uint32_t kParagraphBytes = kBefore + 6 + 204 + 100;
  const std::string kJoined = "\xE0\xA4\x95\xE0\xA5\x8D\xE2\x80\x8D\xE0\xA4\xB7";  // क्, ZWJ, ष
  std::string before;
  for (uint32_t i = 0; i < kBefore; ++i) before += "w ";
  std::string after;
  for (int i = 0; i < 100; ++i) after += "w ";
  std::string letters;
  for (int i = 0; i < 66; ++i) letters += "क";
  std::string joined;
  for (int i = 0; i < 40; ++i) {
    joined += "<p>" + before +
              "\xE0\xA4\x95\xE0\xA5\x8D<b>\xE2\x80\x8D"
              "\xE0\xA4\xB7</b>" +
              letters + " " + after + "</p>\n";
  }
  const auto section = build(book(joined));
  ASSERT_GT(section->pageCount, 1);
  int atSplit = 0;
  for (uint16_t page = 1; page < section->pageCount; ++page) {
    const uint32_t within = *section->getVisibleTextOffsetForPage(page) % kParagraphBytes;
    if (within == 0) continue;
    // The page before ends on the word's prefix, its hyphen added by the split.
    section->currentPage = page - 1;
    const auto previous = section->loadPageFromSectionFile();
    ASSERT_TRUE(previous);
    const TextBlock* last = nullptr;
    for (const auto& el : previous->elements) {
      if (el->getTag() == TAG_PageLine) last = static_cast<const PageLine&>(*el).getBlock();
    }
    ASSERT_NE(last, nullptr);
    std::string prefix = last->wordText(last->wordCount() - 1);
    if (prefix.empty() || prefix.back() != '-') continue;  // a page opening among the one-byte words
    prefix.pop_back();
    ASSERT_EQ(0u, prefix.rfind(kJoined, 0)) << "page " << page;
    EXPECT_EQ(within, kBefore + prefix.size() - 3) << "page " << page;
    ++atSplit;
  }
  EXPECT_GT(atSplit, 0);
}

// A guard, not a RED test: pins that a kept joiner moves no page start the LUT holds.
TEST_F(SyntheticLutFixture, KeptJoinersMoveNoPageStart) {
  // A kept joiner is in the source, so VisibleText counts its 3 bytes; no word's offset starts at it. Each paragraph is
  // one line of 24 visible bytes (क् 6, the ZWJ 3, ष 3, "कमल।" 12), so every page opens on a paragraph, at its index
  // times 24; the page breaks are those of the same book without the joiner (21 bytes a paragraph).
  constexpr uint32_t kPlainBytes = 21;
  constexpr uint32_t kParagraphBytes = 24;
  std::string joined;
  std::string plain;
  for (int i = 0; i < 120; ++i) {
    joined +=
        "<p>क्\xE2\x80\x8D"
        "ष कमल।</p>\n";
    plain += "<p>क्ष कमल।</p>\n";
  }
  const auto withJoiners = build(book(joined));
  const auto without = build(book(plain));
  ASSERT_GT(without->pageCount, 1);
  ASSERT_EQ(withJoiners->pageCount, without->pageCount);
  for (uint16_t page = 0; page < without->pageCount; ++page) {
    const uint32_t paragraph = *without->getVisibleTextOffsetForPage(page) / kPlainBytes;
    EXPECT_EQ(*withJoiners->getVisibleTextOffsetForPage(page), paragraph * kParagraphBytes) << "page " << page;
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

TEST_F(SyntheticLutFixture, TiesResolveToTheLastPageOfTheRun) {
  // A page break forced before a paragraph gives two consecutive pages the same start only when
  // the first holds no text; a page-break rule (<hr>) before the paragraph makes a rule-only page.
  // The lookup must answer the last page of the run, the one holding the text at that offset:
  // answering the rule-only page would put every position on the text page one page early. A
  // single rule lands mid-page; a run of them long enough to fill a page always makes a rule-only
  // page.
  std::string rules;
  for (int i = 0; i < 60; ++i) rules += "<hr/>\n";
  const auto epub = book(paragraphs(40) + rules + paragraphs(40));
  const auto section = build(epub);
  const auto runs = tiedRuns(*section);
  EXPECT_FALSE(runs.empty()) << "the rules no longer fill a page; the test checks nothing";
  for (const auto& [first, last] : runs) {
    const uint32_t start = *section->getVisibleTextOffsetForPage(first);
    EXPECT_EQ(section->getPageForVisibleTextOffset(start), std::optional<uint16_t>(last))
        << "run " << first << ".." << last;
    // A push from the run's last page, the text page, comes back on that page.
    EXPECT_EQ(roundTrip(epub, *section, last), std::optional<uint16_t>(last)) << "run " << first << ".." << last;
  }
}

TEST_F(SyntheticLutFixture, TheTextPageAfterAFullPageImageRoundTripsToItself) {
  // A picture as tall as the page fills a page of its own, which starts where the text after it
  // does. Every position on that text page must land on the text page, not on the picture.
  const auto epub = book(paragraphs(20) + "<div><img src=\"full.png\" alt=\"\"/></div>\n" + paragraphs(20),
                         {{"full.png", "image/png", syntheticPngHeader(480, 800)}});
  const auto section = build(epub);
  const auto runs = tiedRuns(*section);
  ASSERT_EQ(runs.size(), 1u) << "the picture no longer makes one page of its own";
  const auto [imagePage, textPage] = runs.front();
  ASSERT_EQ(textPage, imagePage + 1);
  EXPECT_TRUE(holdsImage(*section, imagePage)) << "page " << imagePage;
  EXPECT_FALSE(holdsImage(*section, textPage)) << "page " << textPage;
  const uint32_t start = *section->getVisibleTextOffsetForPage(textPage);
  EXPECT_EQ(section->getPageForVisibleTextOffset(start), std::optional<uint16_t>(textPage));
  EXPECT_EQ(roundTrip(epub, *section, textPage), std::optional<uint16_t>(textPage));
}

TEST_F(SyntheticLutFixture, AnUppercaseBodyIsCountedLikeALowercaseOne) {
  // The mappers lowercase tag names, so text under <BODY> is source text to them; the layout
  // parser's body test must agree, or every page of such a chapter would start at 0.
  const auto document = [](const char* bodyTag) {
    return std::string(
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
               "<head><title>C</title></head><") +
           bodyTag + ">\n" + paragraphs(80) + "\n</" + bodyTag + "></html>\n";
  };
  const auto lower = build(syntheticBookFromDocument(work, bookCount++, document("body")));
  const auto upper = build(syntheticBookFromDocument(work, bookCount++, document("BODY")));
  ASSERT_GT(lower->pageCount, 2);
  ASSERT_EQ(upper->pageCount, lower->pageCount);
  for (uint16_t page = 0; page < lower->pageCount; ++page) {
    EXPECT_EQ(upper->getVisibleTextOffsetForPage(page), lower->getVisibleTextOffsetForPage(page)) << "page " << page;
  }
}

}  // namespace
