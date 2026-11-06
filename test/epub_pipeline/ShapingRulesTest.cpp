// The shaping rules, checked on every page the host pipeline lays out for the four shaped-Indic books
// (test/epubs/make_test_shaped_indic.py), each with its font (test/fixtures/fonts) as the face of every font and style:
//  1. a glyph form sits only on a word whose text holds U+0900-0DFF;
//  2. a shaped word is as wide as it was measured: a word glued to a word with a glyph form (or one with a glyph form
//     glued to the word before it) starts where that word ends, the run's advance or a piece's text width, and a word
//     with a glyph form ends at or before the spaced word after it;
//  3. no line starts with a dependent sign, a ZWJ or a ZWNJ, and no line that ends inside a word (before the hyphen of
//     a word split across lines) ends its Indic text with a virama;
//  4. each glyph form is what IndicShaper::shapeToken makes of its word's text, at its drawn size (the face's ppem
//     times its scale), in the book's language, with the glued tokens kept apart from it (another style or size),
//     whole, as context: one Glyphs piece over the whole text, with the stored records;
//  5. every word is drawn inside the page.
// And a line without Indic text keeps its bytes: the same with the book's face as without one. And the build's word
// cache changes no line: a long chapter on a lent arena whose tail the parse overwrites dumps the same without it.
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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/WordShapeCache.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"
#include "IndicScripts.h"
#include "IndicShaper.h"
#include "LogObserver.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"
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
// TheWordCacheChangesNoLine's chapter: chapter 2 this many times over.
constexpr int kLongChapterRepeats = 16;

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

// The whole text of the token that word `last` ends, or word `first` starts: the layout's context for a token kept
// apart from it is that token's text (ParsedText::shapeToken's `before` and `after`), and a token is the run of glued
// words of one style around it, each of its pieces a word of the line.
std::string tokenEndingAt(const TextBlock& block, const uint16_t last) {
  uint16_t first = last;
  while (first > 0 && block.wordContinues(first) && sameStyle(block, static_cast<uint16_t>(first - 1), first)) --first;
  std::string text;
  for (uint16_t k = first; k <= last; ++k) text += block.wordText(k);
  return text;
}

std::string tokenStartingAt(const TextBlock& block, const uint16_t first) {
  std::string text = block.wordText(first);
  for (uint16_t k = static_cast<uint16_t>(first + 1);
       k < block.wordCount() && block.wordContinues(k) && sameStyle(block, static_cast<uint16_t>(k - 1), k); ++k) {
    text += block.wordText(k);
  }
  return text;
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
        // The glued tokens kept apart, whole, as the layout passed them.
        std::string before;
        std::string after;
        if (i > 0 && keptApart(block, static_cast<uint16_t>(i - 1), i)) {
          before = tokenEndingAt(block, static_cast<uint16_t>(i - 1));
        }
        if (i + 1 < block.wordCount() && keptApart(block, i, static_cast<uint16_t>(i + 1))) {
          after = tokenStartingAt(block, static_cast<uint16_t>(i + 1));
        }
        request.before = before;
        request.after = after;
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

// A line without Indic text is laid out as if no font could shape: the book's Latin lines are byte for byte the same,
// as the section file holds them, with its face and without one.
TEST_P(ShapingRules, TheLatinLinesKeepTheirBytes) {
  const fs::path lineFile = work / "line.bin";
  const auto latinLines = [&](const pipeline_harness::Profile& profile, const char* cache) {
    std::vector<std::string> lines;
    const bool ok = pipeline_harness::forEachPage(
        corpusBook(GetParam().name), (work / cache).string(), profile,
        [&](int, const Section&, const Page& page, GfxRenderer&) {
          forEachLine(page, [&](const TextBlock& block, int) {
            bool latin = false;
            for (uint16_t i = 0; i < block.wordCount(); ++i) {
              if (holdsIndic(block.wordText(i))) return;
              const std::vector<uint32_t> cps = codepointsOf(block.wordText(i));
              latin = latin || std::any_of(cps.begin(), cps.end(), [](const uint32_t cp) {
                        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
                      });
            }
            if (!latin) return;
            HalFile out;
            ASSERT_TRUE(out.openForWrite(lineFile.string()));
            ASSERT_TRUE(block.serialize(out));
            out.close();
            std::ifstream in(lineFile, std::ios::binary);
            lines.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            ASSERT_FALSE(lines.back().empty());
          });
        });
    EXPECT_TRUE(ok);
    return lines;
  };
  pipeline_harness::Profile unshaped = profileOf(GetParam());
  unshaped.shapingFontFile = nullptr;
  const std::vector<std::string> shapedLines = latinLines(profileOf(GetParam()), "shaped");
  const std::vector<std::string> unshapedLines = latinLines(unshaped, "unshaped");
  logCount("Latin lines", static_cast<int>(shapedLines.size()), 2);
  EXPECT_GE(shapedLines.size(), 2u);
  EXPECT_EQ(unshapedLines, shapedLines);
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

// The SCT lines of the builds in progress (Section's arena and word cache lines).
std::vector<std::string>* gSctLines = nullptr;
void collectSct(const char*, const char* origin, const char* message) {
  if (gSctLines != nullptr && std::strcmp(origin, "SCT") == 0) gSctLines->emplace_back(message);
}

// The first of `lines` holding `marker`, from the marker on; empty when none does.
std::string lineWith(const std::vector<std::string>& lines, const char* marker) {
  for (const std::string& line : lines) {
    const size_t at = line.find(marker);
    if (at != std::string::npos) return line.substr(at);
  }
  return {};
}

struct WordCacheLine {
  unsigned bytes = 0, hits = 0, misses = 0, inserts = 0, bypassed = 0, oversize = 0, clears = 0;
};

bool parseWordCacheLine(const std::string& line, WordCacheLine* out) {
  return std::sscanf(line.c_str(),
                     "word cache: bytes=%u hits=%u misses=%u inserts=%u bypassed=%u oversize=%u clears=%u", &out->bytes,
                     &out->hits, &out->misses, &out->inserts, &out->bypassed, &out->oversize, &out->clears) == 7;
}

// Restores the word cache's arming however the test leaves.
struct WordCacheOff {
  WordCacheOff() { WordShapeCache::setEnabled(false); }
  ~WordCacheOff() { WordShapeCache::setEnabled(true); }
};

// A paragraph that sets alone a syllable the book's cases glue between two others, when it shapes otherwise without
// them as context (rule 4's teeth): the Bengali case's bold হে. Shaped with its neighbours, its form must never be
// replayed for it alone, so a glued token must stay out of the word cache.
std::string keptApartAlone(const Book& book) {
  return std::string(book.name) == "test_shaped_bengali" ? "<p><b>\xE0\xA6\xB9\xE0\xA7\x87</b></p>\n" : "";
}

// One chapter of the book's chapter 2 `repeats` times over, as the long timing book repeats the declaration
// (make_test_shaped_indic.py --long): its words come again, with every case of chapter 2 among them, and
// keptApartAlone() after the first. Written beside `work` with the book's stylesheet and language; the path.
std::string longChapterBook(const Book& book, const fs::path& work, const int repeats) {
  const auto item = [&](const char* href) {
    Epub source(corpusBook(book.name), (work / "source").string());
    size_t size = 0;
    uint8_t* bytes = source.readItemContentsToBytes(href, &size, false);
    std::string out = bytes ? std::string(reinterpret_cast<const char*>(bytes), size) : std::string();
    free(bytes);
    return out;
  };
  const std::string chapter = item("OEBPS/ch2.xhtml");
  const std::string css = item("OEBPS/style.css");
  const size_t open = chapter.find("<body>");
  const size_t close = chapter.find("</body>");
  EXPECT_NE(std::string::npos, open);
  EXPECT_NE(std::string::npos, close);
  EXPECT_FALSE(css.empty());
  if (open == std::string::npos || close == std::string::npos) return {};
  const size_t bodyAt = open + std::strlen("<body>");
  const std::string body = chapter.substr(bodyAt, close - bodyAt);
  std::string xhtml = chapter.substr(0, bodyAt);
  for (int r = 0; r < repeats; ++r) {
    xhtml += body;
    // Early, while the cache is as it was armed: right after the first time its glued case is laid out.
    if (r == 0) xhtml += keptApartAlone(book);
  }
  xhtml += chapter.substr(close);

  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
          "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
          "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
  zip.add("content.opf",
          std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
                      "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
                      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">long</dc:identifier>"
                      "<dc:title>Long</dc:title><dc:language>") +
              book.language +
              "</dc:language></metadata>\n<manifest>\n"
              "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
              "<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/>\n"
              "</manifest>\n<spine><itemref idref=\"c\"/></spine>\n</package>\n");
  zip.add("chapter.xhtml", xhtml);
  zip.add("style.css", css);
  const fs::path path = work / "long.epub";
  zip.write(path.string());
  return path.string();
}

}  // namespace

// The word cache changes no line. A long chapter laid out on a lent arena sized so that the parse lane's page and call
// blocks reach the cache's tail and empty it more than once is the same with the cache as with the cache disarmed; and
// the cache did replay words. Bengali's glued bold syllable, set alone too, keeps a context-shaped form out of it.
TEST_P(ShapingRules, TheWordCacheChangesNoLine) {
  const Book& book = GetParam();
  const std::string path = longChapterBook(book, work, kLongChapterRepeats);
  ASSERT_FALSE(path.empty());
  pipeline_harness::Profile profile = profileOf(book);
  // Pages three times as tall as the harness's: a page block holds three times the lines, and the parse lane rises well
  // past the room the cache keeps below its tail.
  profile.viewportHeight = 2280;
  const auto run = [&](const char* cache, const size_t arenaBytes, std::vector<std::string>* lines) {
    pipeline_harness::Profile lent = profile;
    lent.lentArenaBytes = arenaBytes;
    std::ostringstream out;
    gSctLines = lines;
    setLogObserver(collectSct);
    const bool ok = pipeline_harness::runAndDump(path, (work / cache).string(), lent, out);
    setLogObserver(nullptr);
    gSctLines = nullptr;
    EXPECT_TRUE(ok) << out.str();
    return out.str();
  };

  // What the parse holds when the cache arms (the SAX state, the feed chunk, the ruleset), on a roomy arena.
  std::vector<std::string> probeLines;
  run("probe", 52272, &probeLines);
  unsigned cap = 0, highWater = 0, setup = 0, extract = 0, resident = 0, parse = 0;
  ASSERT_EQ(6, std::sscanf(lineWith(probeLines, "arena: ").c_str(),
                           "arena: cap=%u highWater=%u lanes(setup=%u extract=%u resident=%u parse=%u", &cap,
                           &highWater, &setup, &extract, &resident, &parse))
      << lineWith(probeLines, "arena: ");
  // An arena the cache arms on, sized from the parse lane: the cache arms with the cursor within `slack` of what the
  // parse holds (early on a page), and its tail starts kRoomBelow above that, halfway between the arming line and the
  // lane's peak, so a full page's page and call blocks reach it. Page after page, it is emptied and filled again.
  ASSERT_GT(parse, WordShapeCache::kRoomBelow + 1024) << "the lane must reach the tail for the test to mean anything";
  const size_t slack = (parse - WordShapeCache::kRoomBelow) / 2;
  const size_t arenaBytes = resident + WordShapeCache::kWordCacheBytes + WordShapeCache::kRoomBelow + slack;

  std::vector<std::string> onLines, offLines;
  const std::string with = run("on", arenaBytes, &onLines);
  std::string without;
  {
    const WordCacheOff off;
    without = run("off", arenaBytes, &offLines);
  }

  WordCacheLine on, disarmed;
  ASSERT_TRUE(parseWordCacheLine(lineWith(onLines, "word cache: "), &on)) << lineWith(onLines, "word cache: ");
  ASSERT_TRUE(parseWordCacheLine(lineWith(offLines, "word cache: "), &disarmed));
  std::printf(
      "[ ShapingRules ] %s: arena %zu B (resident %u, parse lane %u): word cache hits=%u misses=%u "
      "inserts=%u bypassed=%u oversize=%u clears=%u\n",
      book.name, arenaBytes, resident, parse, on.hits, on.misses, on.inserts, on.bypassed, on.oversize, on.clears);
  EXPECT_EQ(WordShapeCache::kWordCacheBytes, on.bytes);
  EXPECT_GE(on.clears, 2u);  // emptied, armed again, and emptied again
  EXPECT_GT(on.hits, 0u);
  EXPECT_EQ(0u, disarmed.bytes);
  EXPECT_EQ(0u, disarmed.hits);
  EXPECT_EQ(0u, disarmed.inserts);
  EXPECT_EQ(0u, disarmed.oversize);  // never armed: nothing was offered
  EXPECT_GT(disarmed.bypassed, 0u);
  EXPECT_EQ(without, with);
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
