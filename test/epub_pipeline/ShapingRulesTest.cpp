// The shaping rules, checked on every page the host pipeline lays out for the four shaped-Indic books
// (test/epubs/make_test_shaped_indic.py), each with its font (test/fixtures/fonts) as the face of every font and style:
//  1. a glyph form sits only on a word whose text holds U+0900-0DFF;
//  2. a shaped word is as wide as it was measured: a word glued to a word with a glyph form (or one with a glyph form
//     glued to the word before it) starts where that word ends, the run's advance or a piece's text width, and a word
//     with a glyph form ends at or before the spaced word after it;
//  3. no line starts with a dependent sign, a ZWJ or a ZWNJ, and no line that ends inside a word (before the hyphen of
//     a word split across lines) ends its Indic text with a virama;
//  4. each glyph form is what IndicShaper::shapeToken makes of its word's text, at its drawn size (the face's ppem
//     times its scale), in the book's language, with its glued neighbours kept apart (another style or size) as
//     context: one Glyphs piece over the whole text, with the stored records;
//  5. every word is drawn inside the page.
// Each book's counts are printed against floors: glyph words, merged words (the cases' tokens that merge), kept-apart
// pairs, glyph words glued to punctuation and, in Malayalam, words past one shaping call's code points and words split
// across lines. No section of the four is laid out degraded.
//
// Every rule walks every page line and every table cell line. forEachPage frees each page when its callback returns,
// so a rule copies what it needs inside the callback and keeps no pointer into a page.
#include <GlyphRun.h>
#include <OtShaper.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"
#include "IndicScripts.h"
#include "IndicShaper.h"
#include "PipelineRunner.h"
#include "TextShaping.h"

namespace fs = std::filesystem;

namespace {

struct Book {
  const char* name;                      // the corpus book
  const char* font;                      // its font, in test/fixtures/fonts
  const char* language;                  // its dc:language
  std::vector<std::string> mergedWords;  // the words the cases' tokens merge into, each once in the book
  int minGlyphWords;
  int minKeptApart;
  int minGluedToPunctuation;
  int minPastTheCap;
  int minSplitWords;
};

void PrintTo(const Book& b, std::ostream* os) { *os << b.name; }

constexpr int kFontId = 1;  // the harness profile's body font
constexpr uint16_t kViewportWidth = 460;
constexpr uint32_t kZwnj = 0x200C;
constexpr uint32_t kZwj = 0x200D;

pipeline_harness::Profile profileOf(const Book& book) {
  pipeline_harness::Profile p;
  p.viewportWidth = kViewportWidth;
  p.viewportHeight = 760;
  p.shapingFontFile = book.font;
  return p;
}

std::string corpusBook(const char* name) { return (fs::path(CORPUS_DIR) / (std::string(name) + ".epub")).string(); }

class ShapingRules : public testing::TestWithParam<Book> {
 protected:
  fs::path work;

  void SetUp() override {
    // Per test as well as per book: ctest runs the cases of one book side by side.
    work = fs::temp_directory_path() / (std::string("epub_shaping_rules_") + GetParam().name + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override {
    TextShaping::releaseBuildMemory();
    fs::remove_all(work);
  }

  template <typename Visit>
  bool eachPage(Visit&& visit) const {
    return pipeline_harness::forEachPage(corpusBook(GetParam().name), (work / "cache").string(), profileOf(GetParam()),
                                         visit);
  }
  void logCount(const char* what, const int count, const int floor) const {
    std::printf("[ ShapingRules ] %s: %d %s (floor %d)\n", GetParam().name, count, what, floor);
  }
};

// Calls visit(block, x) for every text line of the page: each page line at its element's x, each table cell line at
// its cell's, as PageTableFragment::render places them.
template <typename Visit>
void forEachLine(const Page& page, Visit&& visit) {
  for (const auto& element : page.elements) {
    if (element->getTag() == TAG_PageLine) {
      visit(*static_cast<const PageLine&>(*element).getBlock(), static_cast<int>(element->xPos));
      continue;
    }
    if (element->getTag() != TAG_PageTable) continue;
    const auto& table = static_cast<const PageTableFragment&>(*element);
    const uint8_t columns = table.getColumnCount();
    for (const auto& row : table.getRows()) {
      uint8_t col = 0;
      for (const auto& cell : row.cells) {
        if (col >= columns) break;
        const int cellX = element->xPos +
                          static_cast<int>(static_cast<uint32_t>(table.getTotalWidth()) * col / columns) +
                          TABLE_CELL_PADDING;
        for (const auto& line : cell.lines) visit(*line, cellX);
        col = static_cast<uint8_t>(col + std::min<uint8_t>(cell.colSpan ? cell.colSpan : 1, columns - col));
      }
    }
  }
}

std::vector<uint32_t> codepointsOf(const char* text) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) out.push_back(utf8NextCodepoint(&p));
  return out;
}

bool holdsIndic(const char* text) {
  const std::vector<uint32_t> cps = codepointsOf(text);
  return std::any_of(cps.begin(), cps.end(), [](const uint32_t cp) { return cp >= 0x0900 && cp <= 0x0DFF; });
}

// Punctuation alone: ASCII other than letters and digits, or General Punctuation (dashes, quotes).
bool isPunctuation(const char* text) {
  const std::vector<uint32_t> cps = codepointsOf(text);
  return !cps.empty() && std::all_of(cps.begin(), cps.end(), [](const uint32_t cp) {
    const bool ascii =
        cp > 0x20 && cp < 0x7F && !(cp >= '0' && cp <= '9') && !(cp >= 'A' && cp <= 'Z') && !(cp >= 'a' && cp <= 'z');
    return ascii || (cp >= 0x2010 && cp <= 0x205E);
  });
}

// Whether two words of a line are in one style, size and font: the pieces of one token always are, so glued words
// that differ were kept apart.
bool sameStyle(const TextBlock& block, const uint16_t a, const uint16_t b) {
  const TextBlock::WordFont* fa = block.wordFont(a);
  const TextBlock::WordFont* fb = block.wordFont(b);
  const bool sameFont =
      fa == fb || (fa != nullptr && fb != nullptr && fa->fontId == fb->fontId && fa->scale == fb->scale);
  return block.wordStyle(a) == block.wordStyle(b) && block.wordSizePct(a) == block.wordSizePct(b) && sameFont;
}

bool keptApart(const TextBlock& block, const uint16_t a, const uint16_t b) {
  return block.wordContinues(b) && !sameStyle(block, a, b);
}

struct Glyph {
  uint16_t glyph;
  int16_t advance;
  int8_t dx;
  int8_t dy;
  bool operator==(const Glyph& o) const { return glyph == o.glyph && advance == o.advance && dx == o.dx && dy == o.dy; }
};

std::ostream& operator<<(std::ostream& os, const Glyph& g) {
  return os << g.glyph << ":" << g.advance << ":" << static_cast<int>(g.dx) << "," << static_cast<int>(g.dy);
}

std::vector<Glyph> recordsOf(const GlyphRun& run) {
  std::vector<Glyph> out;
  forEachGlyph(run, 0, false, [&out](const GlyphRecord& r, int) { out.push_back({r.glyph, r.advance, r.dx, r.dy}); });
  return out;
}

struct Piece {
  IndicShaper::PieceKind kind;
  uint16_t begin;
  uint16_t end;
  std::vector<Glyph> glyphs;
};

class RecordingSink final : public IndicShaper::PieceSink {
 public:
  std::vector<Piece> pieces;
  bool beginPiece(const IndicShaper::PieceKind kind, const uint16_t begin, const uint16_t end) override {
    pieces.push_back({kind, begin, end, {}});
    return true;
  }
  bool addGlyph(const GlyphRecord& r) override {
    if (pieces.empty()) return false;
    pieces.back().glyphs.push_back({r.glyph, r.advance, r.dx, r.dy});
    return true;
  }
  bool addBytes(const char*, size_t) override { return true; }
};

std::string lineText(const TextBlock& block) {
  std::string text;
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    if (i > 0 && !block.wordContinues(i)) text += ' ';
    text += block.wordText(i);
  }
  return text;
}

std::string where(const int spine, const TextBlock& block, const uint16_t i) {
  return "spine " + std::to_string(spine) + ", word " + std::to_string(i) + " '" + block.wordText(i) +
         "' in: " + lineText(block);
}

}  // namespace

// Rule 1.
TEST_P(ShapingRules, GlyphFormsSitOnIndicWordsOnly) {
  int glyphWords = 0;
  ASSERT_TRUE(eachPage([&](int spine, const Section&, const Page& page, GfxRenderer&) {
    forEachLine(page, [&](const TextBlock& block, int) {
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        if (!block.hasGlyphForm(i)) continue;
        ++glyphWords;
        EXPECT_TRUE(holdsIndic(block.wordText(i)))
            << "a glyph form on a word without Indic text: " << where(spine, block, i);
      }
    });
  }));
  logCount("glyph words", glyphWords, GetParam().minGlyphWords);
  EXPECT_GE(glyphWords, GetParam().minGlyphWords);
}

// Rule 2.
TEST_P(ShapingRules, ShapedWordsAreAsWideAsMeasured) {
  int glued = 0;
  int spaced = 0;
  ASSERT_TRUE(eachPage([&](int spine, const Section&, const Page& page, GfxRenderer& r) {
    forEachLine(page, [&](const TextBlock& block, const int x) {
      for (uint16_t i = 1; i < block.wordCount(); ++i) {
        const bool shaped = block.hasGlyphForm(i - 1) || block.hasGlyphForm(i);
        if (!shaped) continue;
        const TextBlock::WordBox before = block.wordBox(r, i - 1, kFontId, x, 0);
        const TextBlock::WordBox box = block.wordBox(r, i, kFontId, x, 0);
        if (block.wordContinues(i)) {
          ++glued;
          EXPECT_EQ(before.x + before.width, box.x)
              << "a glued word apart from a shaped one: " << where(spine, block, i);
        } else if (block.hasGlyphForm(i - 1)) {
          ++spaced;
          EXPECT_LE(before.x + before.width, box.x)
              << "a shaped word over the spaced word after it: " << where(spine, block, i);
        }
      }
    });
  }));
  logCount("glued pairs with a shaped word", glued, 1);
  logCount("shaped words before a spaced one", spaced, 1);
  EXPECT_GT(glued, 0);
  EXPECT_GT(spaced, 0);
}

// Rule 3.
TEST_P(ShapingRules, NoLineBreaksInsideASyllable) {
  int lines = 0;
  ASSERT_TRUE(eachPage([&](int spine, const Section&, const Page& page, GfxRenderer&) {
    forEachLine(page, [&](const TextBlock& block, int) {
      const uint16_t n = block.wordCount();
      if (n == 0) return;
      ++lines;
      const std::vector<uint32_t> first = codepointsOf(block.wordText(0));
      if (!first.empty()) {
        EXPECT_FALSE(indic::isDependentSign(first.front())) << "a line starts with a sign: " << where(spine, block, 0);
        EXPECT_NE(kZwj, first.front()) << "a line starts with a ZWJ: " << where(spine, block, 0);
        EXPECT_NE(kZwnj, first.front()) << "a line starts with a ZWNJ: " << where(spine, block, 0);
      }
      // A line that ends inside a word ends with the split's hyphen, a piece glued to the word's Indic part.
      const auto last = static_cast<uint16_t>(n - 1);
      if (n >= 2 && std::string_view(block.wordText(last)) == "-" && block.wordContinues(last) &&
          holdsIndic(block.wordText(last - 1))) {
        const std::vector<uint32_t> part = codepointsOf(block.wordText(last - 1));
        EXPECT_FALSE(indic::isVirama(part.back())) << "a line ends with a virama: " << where(spine, block, last - 1);
      }
    });
  }));
  logCount("lines", lines, 1);
  EXPECT_GT(lines, 0);
}

// Rule 4.
TEST_P(ShapingRules, GlyphFormsAreTheEnginesShaping) {
  const uint32_t* const language = ot::languageTagsFor(GetParam().language);
  int checked = 0;
  ASSERT_TRUE(eachPage([&](int spine, const Section&, const Page& page, GfxRenderer& r) {
    forEachLine(page, [&](const TextBlock& block, const int x) {
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        if (!block.hasGlyphForm(i)) continue;
        ++checked;
        const TextBlock::WordBox box = block.wordBox(r, i, kFontId, x, 0);
        uint32_t facePpem = 0;
        IndicShaper::Request request;
        request.face = r.shapingFace(
            box.fontId, static_cast<EpdFontFamily::Style>(box.style & EpdFontFamily::BOLD_ITALIC), &facePpem);
        ASSERT_NE(nullptr, request.face) << where(spine, block, i);
        // Records are in drawn pixels: the word is shaped at the size it is drawn at.
        request.ppem26_6 =
            static_cast<uint32_t>(static_cast<float>(facePpem) * r.fontBaseScale(box.fontId) * box.scale + 0.5f);
        request.languageTags = language;
        if (i > 0 && keptApart(block, static_cast<uint16_t>(i - 1), i)) request.before = block.wordText(i - 1);
        if (i + 1 < block.wordCount() && keptApart(block, i, static_cast<uint16_t>(i + 1))) {
          request.after = block.wordText(i + 1);
        }
        const std::string_view text(block.wordText(i), block.wordTextLen(i));
        RecordingSink sink;
        IndicShaper::Outcome outcome;
        {
          IndicShaper::CallMemory memory(nullptr, text.size());
          outcome = IndicShaper::shapeToken(text, request, memory, sink);
        }
        ASSERT_EQ(IndicShaper::Outcome::Formed, outcome) << where(spine, block, i);
        ASSERT_EQ(1u, sink.pieces.size()) << where(spine, block, i);
        const Piece& piece = sink.pieces.front();
        EXPECT_EQ(IndicShaper::PieceKind::Glyphs, piece.kind) << where(spine, block, i);
        EXPECT_EQ(0, piece.begin) << where(spine, block, i);
        EXPECT_EQ(text.size(), piece.end) << where(spine, block, i);
        EXPECT_EQ(piece.glyphs, recordsOf(block.glyphForm(i))) << where(spine, block, i);
      }
    });
    TextShaping::releaseBuildMemory();
  }));
  logCount("glyph forms", checked, GetParam().minGlyphWords);
  EXPECT_GE(checked, GetParam().minGlyphWords);
}

// Rule 5.
TEST_P(ShapingRules, EveryWordIsDrawnInsideThePage) {
  int checked = 0;
  ASSERT_TRUE(eachPage([&](int spine, const Section&, const Page& page, GfxRenderer& r) {
    forEachLine(page, [&](const TextBlock& block, const int x) {
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        const TextBlock::WordBox box = block.wordBox(r, i, kFontId, x, 0);
        ++checked;
        EXPECT_GE(box.x, 0) << where(spine, block, i);
        EXPECT_LE(box.x + box.width, kViewportWidth) << where(spine, block, i);
      }
    });
  }));
  logCount("words", checked, 1);
  EXPECT_GT(checked, 0);
}

// The cases the rules must have met, against their floors; and no section laid out degraded.
TEST_P(ShapingRules, TheCasesAreOnThePages) {
  const Book& book = GetParam();
  int merged = 0;
  int keptApartPairs = 0;
  int gluedToPunctuation = 0;
  int pastTheCap = 0;
  int split = 0;
  ASSERT_TRUE(eachPage([&](int spine, const Section& section, const Page& page, GfxRenderer&) {
    EXPECT_FALSE(section.isShapingDegraded()) << "spine " << spine;
    forEachLine(page, [&](const TextBlock& block, int) {
      const uint16_t n = block.wordCount();
      for (uint16_t i = 0; i < n; ++i) {
        const char* text = block.wordText(i);
        if (i > 0 && keptApart(block, static_cast<uint16_t>(i - 1), i) && holdsIndic(block.wordText(i - 1)) &&
            holdsIndic(text)) {
          ++keptApartPairs;
        }
        if (!block.hasGlyphForm(i)) continue;
        if (std::find(book.mergedWords.begin(), book.mergedWords.end(), text) != book.mergedWords.end()) ++merged;
        const bool punctuationBefore = i > 0 && block.wordContinues(i) && isPunctuation(block.wordText(i - 1));
        const bool punctuationAfter = i + 1 < n && block.wordContinues(i + 1) && isPunctuation(block.wordText(i + 1));
        if (punctuationBefore || punctuationAfter) ++gluedToPunctuation;
        if (codepointsOf(text).size() > IndicShaper::kMaxRunCodepoints) ++pastTheCap;
        if (i + 2 == n && std::string_view(block.wordText(i + 1)) == "-" && block.wordContinues(i + 1)) ++split;
      }
    });
  }));
  const auto mergedFloor = static_cast<int>(book.mergedWords.size());
  logCount("merged words", merged, mergedFloor);
  logCount("kept-apart pairs", keptApartPairs, book.minKeptApart);
  logCount("glyph words glued to punctuation", gluedToPunctuation, book.minGluedToPunctuation);
  logCount("words past the cap", pastTheCap, book.minPastTheCap);
  logCount("words split across lines", split, book.minSplitWords);
  EXPECT_EQ(mergedFloor, merged);
  EXPECT_GE(keptApartPairs, book.minKeptApart);
  EXPECT_GE(gluedToPunctuation, book.minGluedToPunctuation);
  EXPECT_GE(pastTheCap, book.minPastTheCap);
  EXPECT_GE(split, book.minSplitWords);
}

namespace {

std::vector<Book> shapedBooks() {
  return {
      {"test_shaped_devanagari", "NotoSansDevanagari-Book-Regular.ttf", "hi", {"हिन्दी", "कि"}, 150, 3, 29, 0, 0},
      {"test_shaped_bengali", "NotoSansBengali-Book-Regular.ttf", "bn", {"বাংলা", "কি"}, 150, 6, 22, 0, 0},
      {"test_shaped_tamil", "NotoSansTamil-Book-Regular.ttf", "ta", {"தமிழ்", "கி"}, 150, 3, 36, 0, 0},
      {"test_shaped_malayalam", "NotoSansMalayalam-Book-Regular.ttf", "ml", {"മലയാളം", "കി"}, 150, 3, 24, 2, 2},
  };
}

std::string bookLabel(const testing::TestParamInfo<Book>& info) { return info.param.name; }

INSTANTIATE_TEST_SUITE_P(ShapedBooks, ShapingRules, testing::ValuesIn(shapedBooks()), bookLabel);

}  // namespace
