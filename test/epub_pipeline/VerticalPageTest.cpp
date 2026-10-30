// A page of a vertical-rl section draws in the turned frame (spec 5.2) and gives the reader's frame back on every exit.
// Against the pipeline shim: em 16 for a CJK character, ascender 18, a 480 px wide Portrait screen.
#include <CooperativeAbort.h>
#include <GfxRenderer.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub/Page.h"

namespace {

constexpr int kFontId = 1;

std::unique_ptr<TextBlock> textLine(std::vector<std::string> words, std::vector<int16_t> xpos, const bool vertical) {
  const std::vector<EpdFontFamily::Style> styles(words.size(), EpdFontFamily::REGULAR);
  TextBlock::WordRange range;
  range.words = &words;
  range.styles = &styles;
  range.first = 0;
  range.count = words.size();

  BlockStyle blockStyle;
  blockStyle.alignment = CssTextAlign::Left;
  blockStyle.textIndent = 0;
  blockStyle.textIndentDefined = true;
  std::unique_ptr<TextBlock> block(new TextBlock(range, xpos, blockStyle));
  block->setVertical(vertical);
  return block;
}

// One line at (0, 0): the word 日A down a column when vertical, else the word AB.
Page pageOf(const bool vertical) {
  Page page;
  page.elements.push_back(std::unique_ptr<PageElement>(
      new PageLine(vertical ? textLine({"日A"}, {0}, true) : textLine({"AB"}, {0}, false), 0, 0)));
  if (vertical) page.verticalBlockExtent = 460;
  return page;
}

bool alwaysAbort() { return true; }

std::string tempPath(const std::string& name) {
  const auto dir = std::filesystem::temp_directory_path() / "vertical_page_test";
  std::filesystem::create_directories(dir);
  return (dir / name).string();
}

// serialize() then deserialize() through a file.
std::unique_ptr<Page> roundTrip(const Page& page, const std::string& name) {
  const std::string path = tempPath(name);
  FsFile out;
  if (!Storage.openFileForWrite("TST", path, out) || !page.serialize(out)) return nullptr;
  out.close();
  FsFile in;
  if (!Storage.openFileForRead("TST", path, in)) return nullptr;
  auto loaded = Page::deserialize(in);
  in.close();
  return loaded;
}

}  // namespace

// The reader's offsets (10, 20) turn into content offsets (20, 10): a 480 px screen less 10 less the 460 px block.
// Em 16, ascender 18: baseline 28, axis 28 - 6 = 22, x 479 - 22 - 8 = 449, y 20 + 14 - 18 = 16; A is sideways at the
// pen after 日, x 20 + 16 = 36, y 10.
TEST(VerticalPage, ATurnedPageDrawsInItsContentFrame) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  std::vector<GfxRenderer::TextDraw> draws;
  r.textDraws = &draws;
  const Page page = pageOf(true);
  EXPECT_TRUE(page.isVertical());
  page.render(r, kFontId, 10, 20);
  ASSERT_EQ(2u, draws.size());
  EXPECT_EQ("日", draws[0].text);
  EXPECT_EQ(GfxRenderer::Portrait, draws[0].frame);
  EXPECT_EQ(449, draws[0].x);
  EXPECT_EQ(16, draws[0].y);
  EXPECT_EQ("A", draws[1].text);
  EXPECT_EQ(GfxRenderer::LandscapeCounterClockwise, draws[1].frame);
  EXPECT_EQ(36, draws[1].x);
  EXPECT_EQ(10, draws[1].y);
  EXPECT_EQ(GfxRenderer::Portrait, r.getOrientation());
}

TEST(VerticalPage, RenderTextOnlyAndAnAbortRestoreTheFrame) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  std::vector<GfxRenderer::TextDraw> draws;
  r.textDraws = &draws;
  const Page page = pageOf(true);

  EXPECT_TRUE(page.renderTextOnly(r, kFontId, 10, 20));
  EXPECT_EQ(2u, draws.size());
  EXPECT_EQ(GfxRenderer::Portrait, r.getOrientation());

  draws.clear();
  CooperativeAbort::setLongTaskAbortPredicate(alwaysAbort);
  EXPECT_FALSE(page.renderTextOnly(r, kFontId, 10, 20, true));
  CooperativeAbort::setLongTaskAbortPredicate(nullptr);
  EXPECT_TRUE(draws.empty());
  EXPECT_EQ(GfxRenderer::Portrait, r.getOrientation());
}

TEST(VerticalPage, AHorizontalPageDrawsAsBefore) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  std::vector<GfxRenderer::TextDraw> draws;
  r.textDraws = &draws;
  const Page page = pageOf(false);
  EXPECT_FALSE(page.isVertical());
  page.render(r, kFontId, 10, 20);
  ASSERT_EQ(1u, draws.size());
  EXPECT_EQ(GfxRenderer::Portrait, draws[0].frame);
  EXPECT_EQ(10, draws[0].x);
  EXPECT_EQ(20, draws[0].y);
  EXPECT_EQ(GfxRenderer::Portrait, r.getOrientation());
}

TEST(VerticalPage, TheBlockExtentSurvivesTheSectionFile) {
  const auto loaded = roundTrip(pageOf(true), "vertical.bin");
  ASSERT_TRUE(loaded);
  EXPECT_EQ(460u, loaded->verticalBlockExtent);
  EXPECT_TRUE(loaded->isVertical());
  EXPECT_EQ(1u, loaded->elements.size());
}

TEST(VerticalPage, AHorizontalPageStaysHorizontalThroughTheSectionFile) {
  const auto loaded = roundTrip(pageOf(false), "horizontal.bin");
  ASSERT_TRUE(loaded);
  EXPECT_EQ(0u, loaded->verticalBlockExtent);
  EXPECT_FALSE(loaded->isVertical());
  EXPECT_EQ(1u, loaded->elements.size());
}

TEST(VerticalPage, AFileCutAfterTheElementCountDoesNotLoad) {
  const std::string path = tempPath("cut.bin");
  FsFile out;
  ASSERT_TRUE(Storage.openFileForWrite("TST", path, out));
  serialization::writePod(out, static_cast<uint16_t>(1));
  out.close();
  FsFile in;
  ASSERT_TRUE(Storage.openFileForRead("TST", path, in));
  EXPECT_FALSE(Page::deserialize(in));
  in.close();
}
