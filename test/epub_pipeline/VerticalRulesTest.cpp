// The vertical rules (spec 7), checked on every page the host pipeline lays out for the vertical books: each upright
// character is drawn in the reader's frame and each sideways run through the turned one, kinsoku holds at the end of
// every column the layout broke, and every line and image of a vertical section is turned.
//
// The books run through forEachPage: Automatic for the two that decide by themselves (test_vertical_text,
// test_writing_modes), On for the two Aozora-style books that declare no mode.
#include <GfxRenderer.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub/CjkLineBreak.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
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
        for (const GfxRenderer::TextDraw& draw : draws) {
          ASSERT_FALSE(draw.text.empty());
          if (draw.frame == GfxRenderer::Portrait) {
            bool pieceIsUpright = false;
            EXPECT_EQ(draw.text.size(), nextColumnPiece(draw.text.c_str(), pieceIsUpright))
                << "an upright draw is one piece: " << draw.text;
            EXPECT_TRUE(drawsUpright(firstCodepointOf(draw.text))) << "sideways in the reader's frame: " << draw.text;
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
  const TextBlock* previous = nullptr;
  int previousSpine = -1;
  const bool ok = pipeline_harness::forEachPage(
      epub(), cache(), profileOf(GetParam()), [&](int spine, const Section&, const Page& page, GfxRenderer&) {
        if (!page.isVertical()) {
          previous = nullptr;
          return;
        }
        if (spine != previousSpine) previous = nullptr;
        previousSpine = spine;
        for (const auto& element : page.elements) {
          if (element->getTag() != TAG_PageLine) {
            previous = nullptr;  // an image, rule or table sits between the columns
            continue;
          }
          const TextBlock* block = static_cast<const PageLine&>(*element).getBlock();
          if (block->wordCount() == 0) continue;
          const TextBlock* before = previous;
          previous = block;
          if (!before || before->wordCount() == 0 || block->wordXpos(0) >= kEm) continue;
          const uint32_t end = lastCodepointOf(lineText(*before));
          const uint32_t start = firstCodepointOf(lineText(*block));
          if (!utf8IsUnspacedCjk(end) && !utf8IsUnspacedCjk(start)) continue;
          ++checked;
          EXPECT_FALSE(CjkLineBreak::isNoLineEnd(end))
              << "column ends with an opening bracket: " << lineText(*before) << " before: " << lineText(*block);
          EXPECT_FALSE(CjkLineBreak::isNoLineStart(start))
              << "column starts with closing punctuation: " << lineText(*block) << " after: " << lineText(*before);
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

namespace {

std::vector<Book> verticalBooks() {
  return {{"test_vertical_text", VerticalText::Automatic, 10, 20},
          {"test_writing_modes", VerticalText::Automatic, 1, 0},
          {"test_cjk_ja_soseki", VerticalText::On, 1, 20},
          {"test_cjk_ko_hyeon", VerticalText::On, 10, 0}};
}

std::string bookName(const testing::TestParamInfo<Book>& info) { return info.param.name; }

INSTANTIATE_TEST_SUITE_P(VerticalBooks, VerticalRules, testing::ValuesIn(verticalBooks()), bookName);

}  // namespace
