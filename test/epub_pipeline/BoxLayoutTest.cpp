// Boxes: CSS borders and background shades, drawn as a PageBox element around a block's slice of a page.
//
// PageBox: what it draws in each pass, and that it survives the section file.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

struct Fill {
  int x, y, w, h, color;
  bool operator==(const Fill& o) const { return x == o.x && y == o.y && w == o.w && h == o.h && color == o.color; }
};
std::ostream& operator<<(std::ostream& os, const Fill& f) {
  return os << "{" << f.x << "," << f.y << " " << f.w << "x" << f.h << " c" << f.color << "}";
}

std::vector<Fill>* recordedFills = nullptr;
void recordFill(const int x, const int y, const int w, const int h, const int color) {
  if (recordedFills) recordedFills->push_back({x, y, w, h, color});
}

// What `box` draws at offset (10, 20) in `mode`.
std::vector<Fill> fillsOf(PageBox& box, const GfxRenderer::RenderMode mode = GfxRenderer::BW) {
  std::vector<Fill> fills;
  recordedFills = &fills;
  GfxRenderer::onFill = recordFill;
  GfxRenderer::renderModeForTest = mode;
  GfxRenderer renderer;
  box.render(renderer, 0, 10, 20);
  GfxRenderer::onFill = nullptr;
  GfxRenderer::renderModeForTest = GfxRenderer::BW;
  recordedFills = nullptr;
  return fills;
}

constexpr int kSolidFill = -1;
constexpr int kShadeKeepingInk = -2;

}  // namespace

TEST(PageBox, DrawsItsShadeUnderItsEdges) {
  const PageBox::Edge edges[4] = {
      {1, CssBorderStyle::Solid}, {0, CssBorderStyle::None}, {2, CssBorderStyle::Solid}, {3, CssBorderStyle::Solid}};
  PageBox box(5, 7, 100, 50, edges, /*shaded=*/true);
  const std::vector<Fill> want = {
      {15, 27, 100, 50, kShadeKeepingInk},  // the shade first, adding ink only
      {15, 27, 100, 1, kSolidFill},         // top
      {15, 75, 100, 2, kSolidFill},         // bottom, inside the box
      {15, 27, 3, 50, kSolidFill},          // left
  };
  EXPECT_EQ(want, fillsOf(box));
}

TEST(PageBox, DoubleDottedAndDashedEdges) {
  const PageBox::Edge edges[4] = {{3, CssBorderStyle::Double},
                                  {1, CssBorderStyle::Dotted},
                                  {2, CssBorderStyle::Double},
                                  {1, CssBorderStyle::Dashed}};
  PageBox box(0, 0, 12, 10, edges, /*shaded=*/false);
  const std::vector<Fill> fills = fillsOf(box);
  // Top: a 3 px double is two 1 px lines with a gap.
  ASSERT_GE(fills.size(), 2u);
  EXPECT_EQ((Fill{10, 20, 12, 1, kSolidFill}), fills[0]);
  EXPECT_EQ((Fill{10, 22, 12, 1, kSolidFill}), fills[1]);
  // Bottom: a 2 px double has no room for a gap and is solid.
  EXPECT_EQ((Fill{10, 28, 12, 2, kSolidFill}), fills[2]);
  // Left, dashed 1 px: 3 px dashes, 2 px gaps, down 10 px.
  EXPECT_EQ((Fill{10, 20, 1, 3, kSolidFill}), fills[3]);
  EXPECT_EQ((Fill{10, 25, 1, 3, kSolidFill}), fills[4]);
  // Right, dotted 1 px: 1 px dots, 2 px gaps, at the right edge.
  const std::vector<Fill> right(fills.begin() + 5, fills.end());
  ASSERT_EQ(4u, right.size()) << "dots at 0, 3, 6, 9";
  EXPECT_EQ((Fill{21, 20, 1, 1, kSolidFill}), right.front());
  EXPECT_EQ((Fill{21, 29, 1, 1, kSolidFill}), right.back());
}

// The anti-aliasing planes and the one-pass gray mask draw the same element list: a box draws nothing there, or its
// shade would turn gray and its lines would clear the text's plane bits.
TEST(PageBox, DrawsInTheBlackAndWhitePassOnly) {
  const PageBox::Edge edges[4] = {
      {1, CssBorderStyle::Solid}, {1, CssBorderStyle::Solid}, {1, CssBorderStyle::Solid}, {1, CssBorderStyle::Solid}};
  PageBox box(0, 0, 40, 40, edges, /*shaded=*/true);
  EXPECT_EQ(5u, fillsOf(box, GfxRenderer::BW).size());
  EXPECT_TRUE(fillsOf(box, GfxRenderer::GRAYSCALE_LSB).empty());
  EXPECT_TRUE(fillsOf(box, GfxRenderer::GRAYSCALE_MSB).empty());
}

TEST(PageBox, SurvivesTheSectionFile) {
  const fs::path dir = fs::temp_directory_path() / ("page_box_" + std::to_string(getpid()));
  fs::create_directories(dir);
  const std::string path = (dir / "page.bin").string();
  {
    Page page;
    const PageBox::Edge edges[4] = {{2, CssBorderStyle::Dashed},
                                    {0, CssBorderStyle::None},
                                    {3, CssBorderStyle::Double},
                                    {1, CssBorderStyle::Dotted}};
    page.elements.push_back(std::make_unique<PageBox>(12, 34, 300, 120, edges, true));
    FsFile out;
    ASSERT_TRUE(Storage.openFileForWrite("TST", path, out));
    ASSERT_TRUE(page.serialize(out));
    out.close();
  }
  FsFile in;
  ASSERT_TRUE(Storage.openFileForRead("TST", path, in));
  const std::unique_ptr<Page> back = Page::deserialize(in);
  in.close();
  ASSERT_TRUE(back);
  ASSERT_EQ(1u, back->elements.size());
  ASSERT_EQ(TAG_PageBox, back->elements[0]->getTag());
  const auto& box = static_cast<const PageBox&>(*back->elements[0]);
  EXPECT_EQ(12, box.xPos);
  EXPECT_EQ(34, box.yPos);
  EXPECT_EQ(300, box.getWidth());
  EXPECT_EQ(120, box.getHeight());
  EXPECT_TRUE(box.isShaded());
  EXPECT_EQ(2, box.getEdge(CssBox::Top).width);
  EXPECT_EQ(CssBorderStyle::Dashed, box.getEdge(CssBox::Top).style);
  EXPECT_EQ(0, box.getEdge(CssBox::Right).width);
  EXPECT_EQ(CssBorderStyle::Double, box.getEdge(CssBox::Bottom).style);
  EXPECT_EQ(CssBorderStyle::Dotted, box.getEdge(CssBox::Left).style);
  fs::remove_all(dir);
}
