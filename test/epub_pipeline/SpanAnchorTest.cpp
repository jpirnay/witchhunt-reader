// Cover for ids on <span> in the anchor map.
//
// They used to be left out altogether, to keep converter-injected ids (Kobo's kobo.N.M, one per
// sentence) out of the map. Some books put their note targets on spans, though -- FB2 conversions
// wrap every note in one -- and every jump to such a note missed the map and landed on the
// fallback page, the notes chapter's first. Pinned here: span targets are in the map and name the
// page their note is on, in each shape books use; Kobo's ids stay out; and span ids have a budget
// of their own that leaves the other elements' cap whole.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr int kNotes = 60;

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = TextScale();
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.hyphenationEnabled = false;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  p.bionicReadingEnabled = false;
  p.inlineFootnotePreviews = false;
  p.imageRendering = 0;
  return p;
}

// Notes of uneven length, so that some of them start exactly at a page break: that is where an id
// recorded against the wrong line names the wrong page.
std::string filler(const int note) {
  std::string out;
  for (int w = 0; w < (note % 7 + 1) * 9; ++w) out += " word";
  return out;
}

struct SpanAnchorTest : testing::Test {
  fs::path work;
  GfxRenderer renderer;

  void SetUp() override {
    work =
        fs::temp_directory_path() / "span_anchor_test" / testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::unique_ptr<Section> build(const std::shared_ptr<Epub>& epub) {
    auto section = std::make_unique<Section>(epub, 0, renderer);
    EXPECT_TRUE(section->createSectionFile(params(), {}, /*skipEviction=*/true));
    EXPECT_TRUE(section->loadSectionFile(params()));
    return section;
  }

  // The page each marker word first appears on.
  static std::map<std::string, int> markerPages(Section& section, const std::string& prefix) {
    std::map<std::string, int> pages;
    for (int pg = 0; pg < section.pageCount; ++pg) {
      section.currentPage = pg;
      const auto page = section.loadPageFromSectionFile();
      EXPECT_TRUE(page) << "page " << pg;
      if (!page) continue;
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const auto& block = static_cast<const PageLine&>(*el).getBlock();
        if (!block || !block->valid()) continue;
        for (uint16_t w = 0; w < block->wordCount(); ++w) {
          const std::string word(block->wordText(w));
          if (word.rfind(prefix, 0) == 0) pages.emplace(word, pg);
        }
      }
    }
    return pages;
  }

  // Every note id must be in the map and name the page its marker word rendered on.
  void expectEveryNoteOnItsPage(const std::string& body, const std::string& marker) {
    auto epub = syntheticBook(work, 0, body);
    const auto section = build(epub);
    ASSERT_GT(section->pageCount, 3) << "the notes must run over several pages";
    const auto pages = markerPages(*section, marker);
    int checked = 0;
    for (int i = 1; i <= kNotes; ++i) {
      const auto seen = pages.find(marker + std::to_string(i));
      ASSERT_NE(seen, pages.end()) << marker << i << " not laid out";
      const auto anchored = section->getPageForAnchor("n" + std::to_string(i));
      ASSERT_TRUE(anchored.has_value()) << "n" << i << " missing from the anchor map";
      EXPECT_EQ(static_cast<int>(*anchored), seen->second) << "n" << i;
      ++checked;
    }
    EXPECT_EQ(checked, kNotes);
  }
};

}  // namespace

// <p><span id="n5">5.</span> Note text</p>: the span opens the note's paragraph.
TEST_F(SpanAnchorTest, ASpanOpeningTheNoteNamesItsPage) {
  std::string body;
  for (int i = 1; i <= kNotes; ++i) {
    body += "<p><span id=\"n" + std::to_string(i) + "\">MARK" + std::to_string(i) + "</span>" + filler(i) + "</p>\n";
  }
  expectEveryNoteOnItsPage(body, "MARK");
}

// <p><span id="n5"/>5. Note text</p>: an empty span as a bare anchor.
TEST_F(SpanAnchorTest, AnEmptyMarkerSpanNamesItsPage) {
  std::string body;
  for (int i = 1; i <= kNotes; ++i) {
    body += "<p><span id=\"n" + std::to_string(i) + "\"></span>MARK" + std::to_string(i) + filler(i) + "</p>\n";
  }
  expectEveryNoteOnItsPage(body, "MARK");
}

// <span id="n5"><div><p>5</p></div><p>Note text</p></span>: the whole note inside the span, as FB2
// conversions write it (crosspoint-reader #3798). The span opens after the previous note's block
// has closed, so the page is the one the note's number lands on, not the previous note's.
TEST_F(SpanAnchorTest, ASpanWrappingTheWholeNoteNamesThePageItOpensOn) {
  std::string body;
  for (int i = 1; i <= kNotes; ++i) {
    body += "<span id=\"n" + std::to_string(i) + "\"><div class=\"title\"><p>NOTE" + std::to_string(i) +
            "</p></div><p>Note" + filler(i) + "</p></span>\n";
  }
  expectEveryNoteOnItsPage(body, "NOTE");
}

// Kobo's per-sentence ids are bookkeeping, not targets, and stay out of the map.
TEST_F(SpanAnchorTest, KoboSentenceSpansAreNotRecorded) {
  std::string body;
  for (int i = 1; i <= kNotes; ++i) {
    body += "<p id=\"p" + std::to_string(i) + "\"><span class=\"koboSpan\" id=\"kobo." + std::to_string(i) +
            ".1\">Sentence" + filler(i) + "</span></p>\n";
  }
  auto epub = syntheticBook(work, 0, body);
  const auto section = build(epub);
  for (int i = 1; i <= kNotes; ++i) {
    EXPECT_FALSE(section->getPageForAnchor("kobo." + std::to_string(i) + ".1").has_value()) << i;
    EXPECT_TRUE(section->getPageForAnchor("p" + std::to_string(i)).has_value()) << i;
  }
}

// Span ids past their budget are dropped, and the ids of other elements after them are still
// recorded: the spans did not take their slots.
TEST_F(SpanAnchorTest, SpanIdsHaveABudgetOfTheirOwn) {
  constexpr int kSpans = 600;  // past the 512 span budget
  std::string body = "<p>";
  for (int i = 1; i <= kSpans; ++i) body += "<span id=\"s" + std::to_string(i) + "\">w</span> ";
  body += "</p>\n";
  // Enough ordinary ids after them to need the general cap's full 1024 slots minus a few.
  constexpr int kBlocks = 1000;
  for (int i = 1; i <= kBlocks; ++i) body += "<p id=\"b" + std::to_string(i) + "\">x</p>\n";
  auto epub = syntheticBook(work, 0, body);
  const auto section = build(epub);

  EXPECT_TRUE(section->getPageForAnchor("s1").has_value());
  EXPECT_TRUE(section->getPageForAnchor("s512").has_value());
  EXPECT_FALSE(section->getPageForAnchor("s513").has_value());
  EXPECT_FALSE(section->getPageForAnchor("s600").has_value());
  EXPECT_TRUE(section->getPageForAnchor("b1").has_value());
  EXPECT_TRUE(section->getPageForAnchor("b" + std::to_string(kBlocks)).has_value());
}

// An id between two blocks, after a grid table, a rule or a bordered box (#846). Each lays the paragraph before it out
// without starting a block, so that paragraph's count of tokens stood when the id was queued: the id waited for as
// many tokens more of the next paragraph, a page or more into a long one. It names the page that paragraph starts on.
TEST_F(SpanAnchorTest, AnIdBetweenBlocksAfterATableARuleOrABoxNamesTheNextParagraphsPage) {
  // 600 tokens, more than a page of 480x800 holds: an id counted on from them lands at least a page late.
  const auto paragraph = [](const std::string& marker, const int words) {
    std::string out = "<p>" + marker;
    for (int w = 1; w < words; ++w) out += " word";
    return out + "</p>\n";
  };
  const std::string body = paragraph("LEADA", 600) +
                           "<table><tr><td>cell</td><td>cell</td></tr></table><span id=\"after-table\"></span>" +
                           paragraph("NEXTA", 900) + paragraph("LEADB", 600) + "<hr/><span id=\"after-rule\"></span>" +
                           paragraph("NEXTB", 900) + "<div class=\"box\">" + paragraph("LEADC", 600) +
                           "</div><span id=\"after-box\"></span>" + paragraph("NEXTC", 900);
  const std::string xhtml =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
      "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
      body + "\n</body></html>\n";
  auto epub = syntheticBookFromDocument(work, 0, xhtml,
                                        {{"style.css", "text/css", ".box { border: 2px solid; padding: 6px; }\n"}});
  Section::BuildParams p = params();
  p.embeddedStyle = true;  // the box is the book's CSS
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
  ASSERT_TRUE(section.loadSectionFile(p));
  const auto pages = markerPages(section, "NEXT");
  const std::vector<std::pair<std::string, std::string>> idBeforeMarker = {
      {"after-table", "NEXTA"}, {"after-rule", "NEXTB"}, {"after-box", "NEXTC"}};
  for (const auto& [id, marker] : idBeforeMarker) {
    const auto seen = pages.find(marker);
    ASSERT_NE(seen, pages.end()) << marker << " not laid out";
    const auto anchored = section.getPageForAnchor(id);
    ASSERT_TRUE(anchored.has_value()) << id << " missing from the anchor map";
    EXPECT_EQ(static_cast<int>(*anchored), seen->second) << id;
  }
}
