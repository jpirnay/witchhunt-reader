// The right-to-left rules, checked on every page the host pipeline lays out for the right-to-left books: every word is
// drawn inside the page, glued pieces touch and spaced words keep a space, every mark stays with its letter, a Hebrew
// word is drawn right to left while Latin letters and digits keep their order, and two note markers on one line are
// read in order.
//
// The books run through forEachPage: test_bidi_text at 460 px and at 1,600 px, where its 111-token paragraph's lines
// pass BidiLine::kMaxCodepoints slots and take upstream's fallback, and test_rtl_markup. Every rule walks every page
// line and every table cell line. forEachPage frees each page when its callback returns, so a rule copies what it
// needs (text, numbers) inside the callback and keeps no pointer into a page.
#include <BidiUtils.h>
#include <TextShaping.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "Epub/BidiLine.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

struct Book {
  const char* name;   // the corpus book
  const char* label;  // the case's name
  uint16_t viewportWidth;
  int minOrderedLines;  // lines with a logical order the gap rule checks
  int minMarks;         // marks the mark rule checks
  int minHebrewWords;   // Hebrew words the right-to-left rule checks
  int minLatinWords;    // words the Latin-and-digits rule checks
};

void PrintTo(const Book& b, std::ostream* os) { *os << b.label; }

constexpr int kFontId = 1;      // the harness profile's body font
constexpr int kSpaceWidth = 6;  // the stub's space advance

pipeline_harness::Profile profileOf(const Book& book) {
  pipeline_harness::Profile p;
  p.viewportWidth = book.viewportWidth;
  p.viewportHeight = 760;
  return p;
}

fs::path workDirFor(const std::string& label) {
  // Per test as well as per book: ctest runs the cases of one book side by side.
  const fs::path dir = fs::temp_directory_path() / ("epub_bidi_rules_" + label + "_" +
                                                    testing::UnitTest::GetInstance()->current_test_info()->name());
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::string corpusBook(const char* name) { return (fs::path(CORPUS_DIR) / (std::string(name) + ".epub")).string(); }

class BidiRules : public testing::TestWithParam<Book> {
 protected:
  fs::path work;

  void SetUp() override { work = workDirFor(GetParam().label); }
  void TearDown() override { fs::remove_all(work); }

  std::string epub() const { return corpusBook(GetParam().name); }
  std::string cache() const { return (work / "cache").string(); }
  void logCount(const char* what, const int count, const int floor) const {
    std::printf("[ BidiRules ] %s: %d %s checked (floor %d)\n", GetParam().label, count, what, floor);
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

bool isHebrewLetter(const uint32_t cp) { return (cp >= 0x05D0 && cp <= 0x05EA) || (cp >= 0x05EF && cp <= 0x05F2); }

bool isAsciiLetterOrDigit(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

std::vector<uint32_t> hebrewLettersOf(const std::vector<uint32_t>& cps) {
  std::vector<uint32_t> out;
  for (const uint32_t cp : cps) {
    if (isHebrewLetter(cp)) out.push_back(cp);
  }
  return out;
}

std::vector<uint32_t> asciiLettersAndDigitsOf(const std::vector<uint32_t>& cps) {
  std::vector<uint32_t> out;
  for (const uint32_t cp : cps) {
    if (isAsciiLetterOrDigit(cp)) out.push_back(cp);
  }
  return out;
}

// Whether the word's letters hold the divine name, yod he vav he: TextShaping's Hebrew module draws it ה׳.
bool holdsTheDivineName(const std::vector<uint32_t>& cps) {
  static const std::vector<uint32_t> kName = {0x05D9, 0x05D4, 0x05D5, 0x05D4};
  const std::vector<uint32_t> letters = hebrewLettersOf(cps);
  return std::search(letters.begin(), letters.end(), kName.begin(), kName.end()) != letters.end();
}

// A base with the transparent marks after it, its bracket taken back to the opening one: L4 mirrors a bracket in a
// right-to-left run, so the drawn bracket may be the other of its pair.
using Cluster = std::vector<uint32_t>;

uint32_t unmirrored(const uint32_t cp) {
  switch (cp) {
    case ')':
      return '(';
    case ']':
      return '[';
    case '}':
      return '{';
    case '>':
      return '<';
    case 0x00BB:  // »
      return 0x00AB;
    default:
      return cp;
  }
}

// The clusters of `cps`, sorted (a multiset), without the bidi controls and soft hyphens no draw text holds. Leading
// marks with no base form a cluster of their own.
std::vector<Cluster> clustersOf(const std::vector<uint32_t>& cps, int* marks = nullptr) {
  std::vector<Cluster> out;
  for (const uint32_t cp : cps) {
    if (BidiLine::isControl(cp) || cp == 0x00AD) continue;
    if (BidiUtils::isTransparentMark(cp) && !out.empty()) {
      out.back().push_back(cp);
      if (marks != nullptr) ++*marks;
      continue;
    }
    out.push_back({unmirrored(cp)});
  }
  std::sort(out.begin(), out.end());
  return out;
}

// What the word's text shapes to (TextShaping), else its own code points: what its draw text must hold, in some order.
std::vector<uint32_t> shapedCodepointsOf(const char* text) {
  const std::vector<uint32_t> own = codepointsOf(text);
  std::vector<TextShaping::Unit> units(own.size() + 1);
  size_t count = 0;
  if (!TextShaping::shape(text, std::char_traits<char>::length(text), units.data(), units.size(), count)) return own;
  std::vector<uint32_t> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) out.push_back(units[i].cp);
  return out;
}

std::string lineText(const TextBlock& block) {
  std::string text;
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    if (i > 0 && !block.wordContinues(i)) text += ' ';
    text += block.wordText(i);
  }
  return text;
}

std::string where(const int spine, const TextBlock& block, const uint16_t i) {
  return "spine " + std::to_string(spine) + ", word " + std::to_string(i) + " '" + block.wordText(i) + "' drawn '" +
         block.wordDrawText(i) + "' in: " + lineText(block);
}

}  // namespace

// Every word starts at or right of the page's left edge and ends at or left of its right edge.
TEST_P(BidiRules, EveryWordIsDrawnInsideThePage) {
  const int width = GetParam().viewportWidth;
  int checked = 0;
  const auto visit = [&](int spine, const Section&, const Page& page, GfxRenderer& r) {
    forEachLine(page, [&](const TextBlock& block, const int x) {
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        const TextBlock::WordBox box = block.wordBox(r, i, kFontId, x, 0);
        ++checked;
        EXPECT_GE(box.x, 0) << where(spine, block, i);
        EXPECT_LE(box.x + box.width, width) << where(spine, block, i);
      }
    });
  };
  ASSERT_TRUE(pipeline_harness::forEachPage(epub(), cache(), profileOf(GetParam()), visit));
  logCount("words", checked, 1);
  EXPECT_GT(checked, 0);
}

// In stored order, a word glued to the one before (wordContinues) starts where that one's draw text ends; any other
// word starts at least a space after it.
TEST_P(BidiRules, GluedWordsTouchAndSpacedWordsKeepASpace) {
  int ordered = 0;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section&, const Page& page, GfxRenderer& r) {
        forEachLine(page, [&](const TextBlock& block, const int x) {
          if (block.hasLogicalOrder()) ++ordered;
          for (uint16_t i = 1; i < block.wordCount(); ++i) {
            const TextBlock::WordBox before = block.wordBox(r, i - 1, kFontId, x, 0);
            const TextBlock::WordBox box = block.wordBox(r, i, kFontId, x, 0);
            const int end = before.x + before.width;
            if (block.wordContinues(i)) {
              EXPECT_EQ(end, box.x) << "a glued word apart from the one before: " << where(spine, block, i);
            } else {
              EXPECT_GE(box.x, end + kSpaceWidth) << "a spaced word without its space: " << where(spine, block, i);
            }
          }
        });
      });
  ASSERT_TRUE(ok);
  logCount("lines with a logical order", ordered, GetParam().minOrderedLines);
  EXPECT_GE(ordered, GetParam().minOrderedLines);
}

// A word's draw text holds the clusters (a base and the transparent marks after it) of its text, shaped, as a
// multiset: no mark leaves its letter, and no draw text starts with a mark.
TEST_P(BidiRules, EveryMarkStaysWithItsLetter) {
  int marks = 0;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section&, const Page& page, GfxRenderer&) {
        forEachLine(page, [&](const TextBlock& block, int) {
          for (uint16_t i = 0; i < block.wordCount(); ++i) {
            const std::vector<uint32_t> drawn = codepointsOf(block.wordDrawText(i));
            if (!drawn.empty()) {
              EXPECT_FALSE(BidiUtils::isTransparentMark(drawn.front()))
                  << "a draw text starts with a mark: " << where(spine, block, i);
            }
            EXPECT_EQ(clustersOf(shapedCodepointsOf(block.wordText(i)), &marks), clustersOf(drawn))
                << "a mark apart from its letter: " << where(spine, block, i);
          }
        });
      });
  ASSERT_TRUE(ok);
  logCount("marks", marks, GetParam().minMarks);
  EXPECT_GE(marks, GetParam().minMarks);
}

// A word with a Hebrew letter and no ASCII letter or digit, whose letters do not hold the divine name, is drawn with
// its Hebrew letters reversed.
TEST_P(BidiRules, HebrewWordsAreDrawnRightToLeft) {
  int checked = 0;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section&, const Page& page, GfxRenderer&) {
        forEachLine(page, [&](const TextBlock& block, int) {
          for (uint16_t i = 0; i < block.wordCount(); ++i) {
            const std::vector<uint32_t> text = codepointsOf(block.wordText(i));
            std::vector<uint32_t> letters = hebrewLettersOf(text);
            if (letters.empty() || !asciiLettersAndDigitsOf(text).empty() || holdsTheDivineName(text)) continue;
            ++checked;
            std::reverse(letters.begin(), letters.end());
            EXPECT_EQ(letters, hebrewLettersOf(codepointsOf(block.wordDrawText(i))))
                << "a Hebrew word not drawn right to left: " << where(spine, block, i);
          }
        });
      });
  ASSERT_TRUE(ok);
  logCount("Hebrew words", checked, GetParam().minHebrewWords);
  EXPECT_GE(checked, GetParam().minHebrewWords);
}

// A word with no right-to-left letter (class R or AL) and no override (LRO, RLO) in its text is drawn with its ASCII
// letters and digits in their order.
TEST_P(BidiRules, LatinAndDigitsKeepTheirOrder) {
  int checked = 0;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section&, const Page& page, GfxRenderer&) {
        forEachLine(page, [&](const TextBlock& block, int) {
          for (uint16_t i = 0; i < block.wordCount(); ++i) {
            const std::vector<uint32_t> text = codepointsOf(block.wordText(i));
            const bool rightToLeft = std::any_of(text.begin(), text.end(), [](const uint32_t cp) {
              return cp == 0x202D || cp == 0x202E || (!BidiLine::isControl(cp) && BidiUtils::strongLevel(cp) == 1);
            });
            if (rightToLeft) continue;
            ++checked;
            EXPECT_EQ(asciiLettersAndDigitsOf(text), asciiLettersAndDigitsOf(codepointsOf(block.wordDrawText(i))))
                << "Latin or digits out of order: " << where(spine, block, i);
          }
        });
      });
  ASSERT_TRUE(ok);
  logCount("left-to-right words", checked, GetParam().minLatinWords);
  EXPECT_GE(checked, GetParam().minLatinWords);
}

// The two note markers on one line of test_bidi_text: "1" is read before "2" and, the line running right to left,
// drawn right of it. Without inline previews, so the line holds the markers and its own text only.
TEST(BidiRulesMarkers, TwoMarkersOnALineAreReadInOrder) {
  const fs::path work = workDirFor("test_bidi_text");
  pipeline_harness::Profile profile;
  profile.inlineFootnotePreviews = false;
  int lines = 0;
  int firstPosition = -1;
  int secondPosition = -1;
  int firstX = -1;
  int secondX = -1;
  std::string text;
  const auto visit = [&](int, const Section&, const Page& page, GfxRenderer&) {
    forEachLine(page, [&](const TextBlock& block, int) {
      int first = -1;
      int second = -1;
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        const std::string token = block.wordText(i);
        if (token == "1") first = i;
        if (token == "2") second = i;
      }
      if (first < 0 || second < 0) return;
      ++lines;
      firstPosition = block.logicalPosition(static_cast<uint16_t>(first));
      secondPosition = block.logicalPosition(static_cast<uint16_t>(second));
      firstX = block.wordXpos(static_cast<uint16_t>(first));
      secondX = block.wordXpos(static_cast<uint16_t>(second));
      text = lineText(block);
    });
  };
  const bool ok =
      pipeline_harness::forEachPage(corpusBook("test_bidi_text"), (work / "cache").string(), profile, visit);
  fs::remove_all(work);
  ASSERT_TRUE(ok);
  ASSERT_EQ(1, lines) << "lines holding both markers";
  EXPECT_LT(firstPosition, secondPosition) << text;
  EXPECT_GT(firstX, secondX) << text;
}

namespace {

std::vector<Book> bidiBooks() {
  return {{"test_bidi_text", "test_bidi_text", 460, 25, 80, 160, 35},
          {"test_bidi_text", "test_bidi_text_1600", 1600, 8, 80, 160, 35},
          {"test_rtl_markup", "test_rtl_markup", 460, 3, 0, 10, 5}};
}

std::string bookLabel(const testing::TestParamInfo<Book>& info) { return info.param.label; }

INSTANTIATE_TEST_SUITE_P(BidiBooks, BidiRules, testing::ValuesIn(bidiBooks()), bookLabel);

}  // namespace
