// The vertical rules (spec 7), checked on every page the host pipeline lays out for the vertical books: each upright
// character is drawn in the reader's frame and each sideways run through the turned one, kinsoku holds at the end of
// every column the layout broke, every line and image of a vertical section is turned, and a placed 、。 keeps its ink
// in its own place down the column.
//
// The books run through forEachPage: Automatic for the three that decide by themselves (test_vertical_text, its
// Traditional Chinese sibling test_vertical_text_zh_hant, test_writing_modes), On for the two Aozora-style books that
// declare no mode.
#include <GfxRenderer.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Epub/CjkLineBreak.h"
#include "Epub/CjkPunctuation.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/TurnedFrame.h"
#include "Epub/VerticalDraw.h"
#include "Epub/VerticalOrientation.h"
#include "Epub/blocks/ImageBlock.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

struct Book {
  const char* name;
  VerticalText request;
  int minSideways;  // sideways draws a book must hold
  int minColumns;   // kinsoku columns a book must check; 0 where its columns are not unspaced CJK or too few break
  int minPlacedMarks = 0;  // placed 、。 a book must check; 0 where it sets none
  int minCombined = 0;     // combined words a book must check; 0 where it sets none
};

// The stub's em: a CJK character is 16 px wide, so a first word less than 16 px in is a column the layout broke.
constexpr int kEm = 16;
constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 800;

void PrintTo(const Book& b, std::ostream* os) { *os << b.name; }

pipeline_harness::Profile profileOf(const Book& book) {
  pipeline_harness::Profile p;
  p.verticalText = book.request;
  p.viewportWidth = 460;
  p.viewportHeight = 760;
  return p;
}

class VerticalRules : public testing::TestWithParam<Book> {
 protected:
  fs::path work;

  void SetUp() override {
    // Per test as well as per book: ctest runs the cases of one book side by side.
    work = fs::temp_directory_path() / (std::string("epub_vertical_rules_") + GetParam().name + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::string epub() const { return (fs::path(CORPUS_DIR) / (std::string(GetParam().name) + ".epub")).string(); }
  std::string cache() const { return (work / "cache").string(); }
};

// Marks and selectors that follow a character and draw with it, whichever way the run it ends is set.
bool isMarkOrSelector(const uint32_t cp) {
  return utf8IsCombiningMark(cp) || cp == 0x3099 || cp == 0x309A || (cp >= 0xFE00 && cp <= 0xFE0F) ||
         (cp >= 0xE0100 && cp <= 0xE01EF);
}

std::vector<uint32_t> codepointsOf(const std::string& text) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*p) out.push_back(utf8NextCodepoint(&p));
  return out;
}

uint32_t firstCodepointOf(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  return utf8NextCodepoint(&p);
}

uint32_t lastCodepointOf(const std::string& text) {
  size_t i = text.size();
  while (i > 0 && (static_cast<unsigned char>(text[i - 1]) & 0xC0) == 0x80) --i;
  if (i == 0) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str() + i - 1);
  return utf8NextCodepoint(&p);
}

std::string lineText(const TextBlock& block) {
  std::string text;
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    if (i > 0 && !block.wordContinues(i)) text += ' ';
    text += block.wordText(i);
  }
  return text;
}

}  // namespace

// Every upright character is drawn in the reader's frame as a piece of its own, every sideways run through the turned
// one, and nothing in another frame; the reader's frame is back after each page.
TEST_P(VerticalRules, UprightCharactersAreDrawnInTheReadersFrameAndRunsSideways) {
  GfxRenderer::Orientation sideways = GfxRenderer::LandscapeCounterClockwise;
  int upright = 0;
  int runs = 0;
  int pages = 0;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section&, const Page& page, GfxRenderer& r) {
        if (!page.isVertical()) return;
        ++pages;
        r.setOrientation(GfxRenderer::Portrait);
        std::vector<GfxRenderer::TextDraw> draws;
        r.textDraws = &draws;
        page.render(r, 1, 10, 20, /*forceLoadLargeImages=*/true);
        r.textDraws = nullptr;
        EXPECT_EQ(GfxRenderer::Portrait, r.getOrientation()) << "spine " << spine;
        // A combined word (tate-chu-yoko) is the one upright draw that holds several characters, digits among them:
        // only a draw at a combined word's box, in the reader's frame, is exempt (values copied out of the page).
        std::vector<turnedframe::Rect> combined;
        const turnedframe::Offsets origin =
            turnedframe::contentOffsets(kScreenWidth, 10, 20, static_cast<int>(page.verticalBlockExtent));
        for (const auto& element : page.elements) {
          if (element->getTag() != TAG_PageLine) continue;
          const auto& line = static_cast<const PageLine&>(*element);
          const TextBlock& block = *line.getBlock();
          for (uint16_t i = 0; i < block.wordCount(); ++i) {
            if (!block.isCombined(i)) continue;
            const TextBlock::WordBox box = block.wordBox(r, i, 1, line.xPos + origin.x, line.yPos + origin.y);
            combined.push_back(turnedframe::toReader(kScreenWidth, {box.x, box.y, box.width, box.height}));
          }
        }
        for (const GfxRenderer::TextDraw& draw : draws) {
          ASSERT_FALSE(draw.text.empty());
          if (draw.frame == GfxRenderer::Portrait) {
            const bool isCombined =
                std::any_of(combined.begin(), combined.end(), [&draw](const turnedframe::Rect& cell) {
                  return draw.x >= cell.x && draw.x < cell.x + cell.width && draw.y >= cell.y - kEm &&
                         draw.y < cell.y + cell.height + kEm;
                });
            bool pieceIsUpright = false;
            if (!isCombined) {
              EXPECT_EQ(draw.text.size(), nextColumnPiece(draw.text.c_str(), pieceIsUpright))
                  << "an upright draw is one piece: " << draw.text;
              EXPECT_TRUE(drawsUpright(firstCodepointOf(draw.text))) << "sideways in the reader's frame: " << draw.text;
            }
            EXPECT_GE(draw.x, 0) << draw.text;
            EXPECT_LT(draw.x, kScreenWidth) << draw.text;
            EXPECT_GE(draw.y, 0) << draw.text;
            EXPECT_LT(draw.y, kScreenHeight) << draw.text;
            ++upright;
          } else if (draw.frame == sideways) {
            for (const uint32_t cp : codepointsOf(draw.text)) {
              if (isMarkOrSelector(cp)) continue;
              EXPECT_FALSE(drawsUpright(cp)) << "upright in a sideways run: " << draw.text;
            }
            ++runs;
          } else {
            ADD_FAILURE() << "drawn in another frame: " << draw.text;
          }
        }
      });
  ASSERT_TRUE(ok);
  EXPECT_GT(pages, 0);
  EXPECT_GE(upright, 100);
  EXPECT_GE(runs, std::max(1, GetParam().minSideways));
}

// At a column end with unspaced CJK on either side, the column after it does not start with closing punctuation and
// the one before it does not end with an opening bracket. A paragraph's indented first column, one em or more in, is
// not a layout break; a broken column starts at the whole-em grid's inset (4 px in a 760 px zh/ja column, 0 in Korean)
// or a few pixels further out where a pushed-in opening mark starts it.
TEST_P(VerticalRules, KinsokuHoldsAtEveryColumnEnd) {
  int checked = 0;
  // The previous column's text, not its block: the page holding it is freed before the next page loads.
  std::string previous;
  int previousSpine = -1;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section&, const Page& page, GfxRenderer&) {
        if (!page.isVertical()) {
          previous.clear();
          return;
        }
        if (spine != previousSpine) previous.clear();
        previousSpine = spine;
        for (const auto& element : page.elements) {
          if (element->getTag() != TAG_PageLine) {
            previous.clear();  // an image, rule or table sits between the columns
            continue;
          }
          const TextBlock* block = static_cast<const PageLine&>(*element).getBlock();
          if (block->wordCount() == 0) continue;
          const std::string before = std::move(previous);
          previous = lineText(*block);
          if (before.empty() || block->wordXpos(0) >= kEm) continue;
          const uint32_t end = lastCodepointOf(before);
          const uint32_t start = firstCodepointOf(previous);
          if (!utf8IsUnspacedCjk(end) && !utf8IsUnspacedCjk(start)) continue;
          ++checked;
          EXPECT_FALSE(CjkLineBreak::isNoLineEnd(end))
              << "column ends with an opening bracket: " << before << " before: " << previous;
          EXPECT_FALSE(CjkLineBreak::isNoLineStart(start))
              << "column starts with closing punctuation: " << previous << " after: " << before;
        }
      });
  ASSERT_TRUE(ok);
  EXPECT_GE(checked, GetParam().minColumns) << "only " << checked << " columns checked";
}

// On a page of a vertical section every line and cell line is turned, every image and cell image too; on every other
// page none is.
TEST_P(VerticalRules, EveryLineAndImageOfAVerticalPageIsTurned) {
  int verticalPages = 0;
  int lines = 0;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section& section, const Page& page, GfxRenderer&) {
        const bool vertical = section.isVertical();
        EXPECT_EQ(vertical, page.isVertical()) << "spine " << spine;
        if (vertical) ++verticalPages;
        for (const auto& element : page.elements) {
          switch (element->getTag()) {
            case TAG_PageLine:
              EXPECT_EQ(vertical, static_cast<const PageLine&>(*element).getBlock()->isVertical());
              ++lines;
              break;
            case TAG_PageImage:
              EXPECT_EQ(vertical, static_cast<const PageImage&>(*element).getImageBlock().isTurned());
              break;
            case TAG_PageTable:
              for (const auto& row : static_cast<const PageTableFragment&>(*element).getRows()) {
                for (const auto& cell : row.cells) {
                  for (const auto& line : cell.lines) EXPECT_EQ(vertical, line->isVertical());
                  if (!cell.image) continue;
                  EXPECT_EQ(vertical, cell.image->isTurned());
                }
              }
              break;
            default:
              break;
          }
        }
      });
  ASSERT_TRUE(ok);
  EXPECT_GT(verticalPages, 0);
  EXPECT_GT(lines, 0);
}

// A placed mark (a 、。，． drawn upright, VerticalOrientation.h) puts its ink where columnInk says, from its token's
// start down the column: it must end by the next token's start, or by the column's end (the content frame's width in
// the turned frame) where it closes the column, and must not start before the token before it. A mark push-in narrows
// is drawn back by what its ink needs (ParsedText's ink shift), so the check holds for narrowed marks too.
//
// Page lines only: a table cell's column is the cell's, not the frame's. columnInk is the ink at scale 1, so a mark
// drawn at another size is left out; the books set their marks at the body size.
TEST_P(VerticalRules, APlacedMarksInkStaysInItsColumnPlace) {
  const int columnLength = profileOf(GetParam()).viewportHeight;
  int checked = 0;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section&, const Page& page, GfxRenderer& r) {
        if (!page.isVertical()) return;
        for (const auto& element : page.elements) {
          if (element->getTag() != TAG_PageLine) continue;
          const TextBlock& block = *static_cast<const PageLine&>(*element).getBlock();
          if (block.getRenderStyle().fontSizeMultiplier != 1.0f) continue;
          for (uint16_t i = 0; i < block.wordCount(); ++i) {
            const std::vector<uint32_t> cps = codepointsOf(block.wordText(i));
            if (cps.size() != 1 || CjkPunctuation::classOf(cps[0]) == CjkPunctuation::Class::None ||
                uprightPlacementOf(cps[0]) == UprightPlacement::None || block.wordSizePct(i) != 100 ||
                block.wordFont(i) != nullptr) {
              continue;
            }
            int inkStart = 0;
            int inkSize = 0;
            int cell = 0;
            if (!columnInk(r, 1, cps[0], block.wordStyle(i), block.centredMarks(), inkStart, inkSize, cell)) continue;
            ++checked;
            const int start = block.wordXpos(i) + inkStart;
            const int end = start + inkSize;
            const std::string where = "spine " + std::to_string(spine) + ", " + block.wordText(i) + " in " +
                                      lineText(block) + " at " + std::to_string(block.wordXpos(i));
            if (i + 1 < block.wordCount()) {
              EXPECT_LE(end, block.wordXpos(i + 1)) << "ink past the next token: " << where;
            } else {
              EXPECT_LE(element->xPos + end, columnLength) << "ink past the column's end: " << where;
            }
            if (i > 0) EXPECT_GE(start, block.wordXpos(i - 1)) << "ink before the token before: " << where;
          }
        }
      });
  ASSERT_TRUE(ok);
  EXPECT_GE(checked, GetParam().minPlacedMarks) << "only " << checked << " placed marks checked";
}

// A combined word (tate-chu-yoko) is drawn as one piece, upright, in the reader's frame: the one draw of its text that
// falls in its word box mapped to the reader's frame, centred across the column, its baseline (the draw's top plus
// the ascender) inside the cell [pen, pen + em] along it.
TEST_P(VerticalRules, ACombinedWordIsOneUprightDrawInItsCell) {
  int checked = 0;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section&, const Page& page, GfxRenderer& r) {
        if (!page.isVertical()) return;
        r.setOrientation(GfxRenderer::Portrait);
        std::vector<GfxRenderer::TextDraw> draws;
        r.textDraws = &draws;
        page.render(r, 1, 10, 20, /*forceLoadLargeImages=*/true);
        r.textDraws = nullptr;
        const turnedframe::Offsets origin =
            turnedframe::contentOffsets(kScreenWidth, 10, 20, static_cast<int>(page.verticalBlockExtent));
        for (const auto& element : page.elements) {
          if (element->getTag() != TAG_PageLine) continue;
          const auto& line = static_cast<const PageLine&>(*element);
          const TextBlock& block = *line.getBlock();
          for (uint16_t i = 0; i < block.wordCount(); ++i) {
            if (!block.isCombined(i)) continue;
            ++checked;
            const std::string text = block.wordText(i);
            const std::string where = "spine " + std::to_string(spine) + ", " + text + " in " + lineText(block);
            const TextBlock::WordBox box = block.wordBox(r, i, 1, line.xPos + origin.x, line.yPos + origin.y);
            // A heading's em is scaled, so the em is the word's own, not kEm.
            EXPECT_EQ(columnEm(r, box.fontId, box.style, box.scale), box.width)
                << "a combined word is one em down the column: " << where;
            const turnedframe::Rect cell = turnedframe::toReader(kScreenWidth, {box.x, box.y, box.width, box.height});
            int inCell = 0;
            int sameText = 0;
            for (const GfxRenderer::TextDraw& draw : draws) {
              if (draw.text != text) continue;
              ++sameText;
              if (draw.frame != GfxRenderer::Portrait) continue;
              if (draw.x < cell.x || draw.x >= cell.x + cell.width) continue;
              if (draw.y < cell.y - kEm || draw.y >= cell.y + cell.height + kEm) continue;
              ++inCell;
              const int baseline = draw.y + r.getFontAscenderSizeScaled(box.fontId, box.scale);
              EXPECT_GE(baseline, cell.y) << where;
              EXPECT_LE(baseline, cell.y + cell.height) << where;
            }
            EXPECT_EQ(1, inCell) << "one draw in the reader's frame at the word's box: " << where << " (" << sameText
                                 << " draws of that text)";
          }
        }
      });
  ASSERT_TRUE(ok);
  EXPECT_GE(checked, GetParam().minCombined) << "only " << checked << " combined words checked";
}

namespace {

std::vector<Book> verticalBooks() {
  return {{"test_vertical_text", VerticalText::Automatic, 10, 20, 100, 3},
          {"test_vertical_text_zh_hant", VerticalText::Automatic, 10, 20, 100},
          {"test_writing_modes", VerticalText::Automatic, 1, 0, 1},
          {"test_cjk_ja_soseki", VerticalText::On, 1, 20, 50},
          {"test_cjk_ko_hyeon", VerticalText::On, 10, 0}};
}

std::string bookName(const testing::TestParamInfo<Book>& info) { return info.param.name; }

INSTANTIATE_TEST_SUITE_P(VerticalBooks, VerticalRules, testing::ValuesIn(verticalBooks()), bookName);

}  // namespace
