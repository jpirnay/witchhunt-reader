// Boxes: CSS borders and background shades, drawn as a PageBox element around a block's slice of a page.
//
// PageBox: what it draws in each pass, and that it survives the section file. Then the parser's box scope.
#include <BufferedFileIO.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

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
    {
      serialization::BufferedFileWriter writer(out);
      ASSERT_TRUE(page.serialize(writer));
      ASSERT_TRUE(writer.flush());
    }
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

// The parser's box scope: where the frame goes, what it holds, and how a page break cuts it.
//
// The stub renderer's lines are 24 px, and the column is 400 px wide by 600 px high (25 lines). Every box test laid out
// in heap mode is laid out again with a lent arena (the reader's background build, where the page-fit test runs before
// the line exists), which must give the same pages.
namespace {

struct BoxView {
  size_t index;  // in the page's elements
  int x, y, w, h;
  int widths[4];  // top, right, bottom, left
  CssBorderStyle styles[4];
  bool shaded;
  int bottom() const { return y + h; }
  int right() const { return x + w; }
};
struct LineView {
  size_t index;
  int x, y;
  std::string text;  // the line's words, a space between
  int lastWordX;     // where its last word starts
};
struct PictureView {
  size_t index;
  int x, y, w, h;
};
struct TableView {
  size_t index;
  int x, y, w, h;
};
struct PageView {
  bool vertical = false;
  std::vector<BoxView> boxes;
  std::vector<LineView> lines;
  std::vector<PictureView> pictures;
  std::vector<TableView> tables;
  std::vector<int> ruleXs;
  // The first line holding `word`, or nullptr.
  const LineView* lineWith(const std::string& word) const {
    for (const LineView& line : lines) {
      if (line.text.find(word) != std::string::npos) return &line;
    }
    return nullptr;
  }
};

class BoxLayout : public testing::Test {
 protected:
  fs::path work;
  int books = 0;
  bool lentArena = false;
  bool extraParagraphSpacing = false;
  bool embeddedStyle = true;
  bool vertical = false;
  uint16_t viewportHeight = 600;
  float lineCompression = 1.0f;

  void SetUp() override {
    std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char& c : name)
      if (c == '/') c = '_';
    work = fs::temp_directory_path() / ("epub_boxes_" + std::to_string(getpid()) + "_" + name);
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::vector<PageView> layoutOnce(const std::string& body, const std::string& css, const bool arena,
                                   const std::vector<SyntheticFile>& files) {
    std::vector<SyntheticFile> extra = files;
    extra.push_back({"style.css", "text/css", css});
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
        body + "\n</body></html>\n";
    auto book = syntheticBookFromDocument(work, books++, doc, extra);
    Section::BuildParams params;
    params.viewportWidth = 400;
    params.viewportHeight = viewportHeight;
    params.lineCompression = lineCompression;
    params.embeddedStyle = embeddedStyle;
    params.extraParagraphSpacing = extraParagraphSpacing;
    if (vertical) params.verticalText = VerticalText::On;
    GfxRenderer renderer;
    Section section(book, 0, renderer);
    std::unique_ptr<BuildArena> lent;
    if (arena) {
      lent = std::make_unique<BuildArena>(52272);
      section.setExternalBuildScratch(lent.get());
    }
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    std::vector<PageView> pages;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      PageView view;
      if (!page) {
        ADD_FAILURE() << "page " << p << " did not load";
        pages.push_back(view);
        continue;
      }
      view.vertical = page->isVertical();
      for (size_t i = 0; i < page->elements.size(); ++i) {
        const PageElement& el = *page->elements[i];
        switch (el.getTag()) {
          case TAG_PageBox: {
            const auto& box = static_cast<const PageBox&>(el);
            BoxView b{i, box.xPos, box.yPos, box.getWidth(), box.getHeight(), {}, {}, box.isShaded()};
            for (uint8_t s = 0; s < 4; ++s) {
              b.widths[s] = box.getEdge(static_cast<CssBox::Side>(s)).width;
              b.styles[s] = box.getEdge(static_cast<CssBox::Side>(s)).style;
            }
            view.boxes.push_back(b);
            break;
          }
          case TAG_PageLine: {
            const TextBlock& block = *static_cast<const PageLine&>(el).getBlock();
            LineView line{i, el.xPos, el.yPos, "", 0};
            for (uint16_t w = 0; w < block.wordCount(); ++w) {
              line.text += (w ? " " : "") + std::string(block.wordText(w));
              line.lastWordX = el.xPos + block.wordXpos(w);
            }
            view.lines.push_back(line);
            break;
          }
          case TAG_PageImage: {
            const auto& img = static_cast<const PageImage&>(el).getImageBlock();
            view.pictures.push_back({i, el.xPos, el.yPos, img.getWidth(), img.getRenderedHeight()});
            break;
          }
          case TAG_PageTable: {
            const auto& t = static_cast<const PageTableFragment&>(el);
            view.tables.push_back({i, t.xPos, t.yPos, t.getTotalWidth(), t.getTotalHeight()});
            break;
          }
          case TAG_PageHR:
            view.ruleXs.push_back(el.xPos);
            break;
        }
      }
      pages.push_back(std::move(view));
    }
    return pages;
  }

  // Laid out in heap mode, and again in a lent arena, which must give the same boxes and lines.
  std::vector<PageView> layout(const std::string& body, const std::string& css,
                               const std::vector<SyntheticFile>& files = {}) {
    std::vector<PageView> heap = layoutOnce(body, css, false, files);
    std::vector<PageView> arena = layoutOnce(body, css, true, files);
    EXPECT_EQ(heap.size(), arena.size()) << "the lent-arena build paginated differently";
    for (size_t p = 0; p < std::min(heap.size(), arena.size()); ++p) {
      EXPECT_EQ(heap[p].boxes.size(), arena[p].boxes.size()) << "page " << p;
      for (size_t b = 0; b < std::min(heap[p].boxes.size(), arena[p].boxes.size()); ++b) {
        const BoxView& h = heap[p].boxes[b];
        const BoxView& a = arena[p].boxes[b];
        EXPECT_TRUE(h.x == a.x && h.y == a.y && h.w == a.w && h.h == a.h && h.index == a.index)
            << "page " << p << " box " << b << " differs in the lent-arena build";
      }
      EXPECT_EQ(heap[p].lines.size(), arena[p].lines.size()) << "page " << p;
    }
    return heap;
  }

  static std::string words(const char* stem, const int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out += (i ? " " : "") + std::string(stem) + std::to_string(i);
    return out;
  }
};

constexpr int kLine = 24;  // the stub's line height
constexpr int kGap = 4;    // ChapterHtmlSlimParser::kBoxMinGap
const char* const kReset = "p, div, h2, ul, li, pre { margin: 0; padding: 0; text-indent: 0; }\n";

}  // namespace

TEST_F(BoxLayout, ABorderedParagraphIsFramedWithItsPaddingInside) {
  const auto pages =
      layout("<p>before</p><p class=\"box\">inside</p><p>after</p>",
             std::string(kReset) + "p.box { border: 2px solid #000; padding: 10px; margin: 20px 30px; }");
  ASSERT_EQ(1u, pages.size());
  const PageView& page = pages[0];
  ASSERT_EQ(1u, page.boxes.size());
  const BoxView& box = page.boxes[0];
  const LineView* before = page.lineWith("before");
  const LineView* inside = page.lineWith("inside");
  const LineView* after = page.lineWith("after");
  ASSERT_TRUE(before && inside && after);
  EXPECT_LT(before->index, box.index);
  EXPECT_LT(box.index, inside->index) << "the frame goes before what it frames, so its shade is under it";
  EXPECT_EQ(30, box.x);
  EXPECT_EQ(400 - 60, box.w);
  for (const int width : box.widths) EXPECT_EQ(2, width);
  EXPECT_EQ(CssBorderStyle::Solid, box.styles[CssBox::Left]);
  EXPECT_EQ(before->y + kLine + 20, box.y) << "the margin is above the frame";
  EXPECT_EQ(box.x + 2 + 10, inside->x) << "border, then padding";
  EXPECT_EQ(box.y + 2 + 10, inside->y);
  EXPECT_EQ(inside->y + kLine + 10 + 2, box.bottom());
  EXPECT_EQ(box.bottom() + 20, after->y) << "the margin is below the frame";
  EXPECT_EQ(0, after->x);
}

TEST_F(BoxLayout, AShadeKeepsItsTextOffItsEdges) {
  const auto pages = layout("<div class=\"note\">shaded</div>", std::string(kReset) + ".note { background: #eee; }");
  ASSERT_EQ(1u, pages.size());
  ASSERT_EQ(1u, pages[0].boxes.size());
  const BoxView& box = pages[0].boxes[0];
  EXPECT_TRUE(box.shaded);
  for (const int width : box.widths) EXPECT_EQ(0, width);
  const LineView* line = pages[0].lineWith("shaded");
  ASSERT_TRUE(line);
  EXPECT_EQ(box.x + kGap, line->x);
  EXPECT_EQ(box.y + kGap, line->y);
  EXPECT_EQ(line->y + kLine + kGap, box.bottom());
}

// A child's margins are inside a bordered box: they do not collapse through its border with the box's own.
TEST_F(BoxLayout, AChildsMarginsStayInsideTheFrame) {
  const auto pages =
      layout("<div class=\"box\"><p>first</p><p>second</p></div><p>after</p>",
             std::string(kReset) + ".box { border: 1px solid; margin: 8px 0; }\n.box p { margin: 15px 0; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  const BoxView& box = pages[0].boxes[0];
  const LineView* first = pages[0].lineWith("first");
  const LineView* second = pages[0].lineWith("second");
  const LineView* after = pages[0].lineWith("after");
  ASSERT_TRUE(first && second && after);
  EXPECT_EQ(8, box.y);
  EXPECT_EQ(box.y + 1 + kGap + 15, first->y);
  EXPECT_EQ(first->y + kLine + 15, second->y) << "sibling margins still collapse inside";
  EXPECT_EQ(second->y + kLine + 15 + kGap + 1, box.bottom());
  EXPECT_EQ(box.bottom() + 8, after->y);
}

TEST_F(BoxLayout, APageBreakLeavesTheBoxOpenOnBothPages) {
  std::string body = "<div class=\"box\">";
  for (int i = 0; i < 40; ++i) body += "<p>line" + std::to_string(i) + "</p>";
  body += "</div><p>after</p>";
  const auto pages = layout(body, std::string(kReset) + ".box { border: 2px dashed; padding: 6px; }");
  ASSERT_EQ(2u, pages.size());
  ASSERT_EQ(1u, pages[0].boxes.size());
  ASSERT_EQ(1u, pages[1].boxes.size());
  const BoxView& upper = pages[0].boxes[0];
  const BoxView& lower = pages[1].boxes[0];
  EXPECT_EQ(2, upper.widths[CssBox::Top]);
  EXPECT_EQ(0, upper.widths[CssBox::Bottom]) << "no bottom edge where the break cuts the box";
  EXPECT_EQ(2, upper.widths[CssBox::Left]);
  EXPECT_EQ(upper.y + upper.h, pages[0].lines.back().y + kLine) << "the sides run to the last line";
  EXPECT_EQ(0, lower.widths[CssBox::Top]) << "no top edge on the page that goes on";
  EXPECT_EQ(2, lower.widths[CssBox::Bottom]);
  EXPECT_EQ(0, lower.y);
  ASSERT_FALSE(pages[1].lines.empty());
  EXPECT_EQ(0, pages[1].lines.front().y) << "the padding is the box's start and end, not the break's";
  EXPECT_EQ(upper.x, lower.x);
  EXPECT_EQ(upper.w, lower.w);
}

TEST_F(BoxLayout, BoxesNest) {
  const auto pages = layout("<div class=\"outer\"><p>out</p><div class=\"inner\"><p>in</p></div></div>",
                            std::string(kReset) +
                                ".outer { border: 1px solid; padding: 5px; }\n"
                                ".inner { border: 2px dotted; margin: 10px; background: #ddd; }");
  ASSERT_EQ(2u, pages[0].boxes.size());
  const BoxView& outer = pages[0].boxes[0];
  const BoxView& inner = pages[0].boxes[1];
  const LineView* out = pages[0].lineWith("out");
  const LineView* in = pages[0].lineWith("in");
  ASSERT_TRUE(out && in);
  EXPECT_LT(outer.index, out->index);
  EXPECT_LT(out->index, inner.index);
  EXPECT_LT(inner.index, in->index);
  EXPECT_EQ(outer.x + 1 + 5 + 10, inner.x);
  EXPECT_EQ(outer.right() - 1 - 5 - 10, inner.right());
  EXPECT_EQ(inner.x + 2 + kGap, in->x);
  EXPECT_EQ(inner.bottom() + 10 + 5 + 1, outer.bottom()) << "the outer frame holds the inner one and its margin";
  EXPECT_TRUE(inner.shaded);
  EXPECT_FALSE(outer.shaded);
}

TEST_F(BoxLayout, AnEmptyBoxIsARule) {
  const auto pages = layout("<p>above</p><div class=\"rule\"></div><p>below</p>",
                            std::string(kReset) + ".rule { border-top: 1px solid; margin: 10px 0; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  const BoxView& rule = pages[0].boxes[0];
  EXPECT_EQ(1, rule.widths[CssBox::Top]);
  EXPECT_EQ(0, rule.widths[CssBox::Bottom]);
  EXPECT_EQ(1 + kGap, rule.h) << "a drawn top edge and its gap; the other sides are not drawn";
  EXPECT_EQ(kLine + 10, rule.y);
  EXPECT_EQ(rule.bottom() + 10, pages[0].lineWith("below")->y);
}

TEST_F(BoxLayout, AnEmptyBoxThatDoesNotFitGoesToTheNextPage) {
  std::string body;
  for (int i = 0; i < 24; ++i) body += "<p>fill" + std::to_string(i) + "</p>";
  body += "<div class=\"frame\"></div><p>after</p>";
  const auto pages = layout(body, std::string(kReset) + ".frame { border: 3px double; padding: 20px; }");
  ASSERT_EQ(2u, pages.size());
  EXPECT_TRUE(pages[0].boxes.empty());
  ASSERT_EQ(1u, pages[1].boxes.size());
  EXPECT_EQ(0, pages[1].boxes[0].y);
  EXPECT_EQ(2 * (3 + 20), pages[1].boxes[0].h);
}

TEST_F(BoxLayout, APictureIsCentredInItsBox) {
  const auto pages = layout("<div class=\"box\"><p>caption</p><p><img src=\"pic.png\" alt=\"\"/></p></div>",
                            std::string(kReset) + ".box { border: 1px solid; margin-left: 100px; }",
                            {{"pic.png", "image/png", syntheticPngHeader(100, 40)}});
  ASSERT_EQ(1u, pages[0].boxes.size());
  ASSERT_EQ(1u, pages[0].pictures.size());
  const BoxView& box = pages[0].boxes[0];
  const PictureView& pic = pages[0].pictures[0];
  const int contentLeft = box.x + 1 + kGap;
  const int contentRight = box.right() - 1 - kGap;
  EXPECT_EQ(contentLeft + (contentRight - contentLeft - pic.w) / 2, pic.x);
  EXPECT_LE(pic.y + pic.h + 1, box.bottom()) << "the frame holds the picture";
}

TEST_F(BoxLayout, AListInABoxKeepsItsMarkersInside) {
  const auto pages = layout("<div class=\"box\"><ul><li>one</li><li>two</li></ul></div>",
                            std::string(kReset) + ".box { border: 2px solid; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  const BoxView& box = pages[0].boxes[0];
  for (const char* word : {"one", "two"}) {
    const LineView* line = pages[0].lineWith(word);
    ASSERT_TRUE(line) << word;
    EXPECT_GE(line->x, box.x + 2 + kGap) << word;
    EXPECT_LE(line->y + kLine, box.bottom() - 2 - kGap) << word;
  }
}

TEST_F(BoxLayout, ATableInABoxIsInsideTheFrame) {
  const auto pages = layout("<div class=\"box\"><table><tr><td>a</td><td>b</td></tr></table></div>",
                            std::string(kReset) + ".box { border: 2px solid; padding: 6px; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  ASSERT_EQ(1u, pages[0].tables.size());
  const BoxView& box = pages[0].boxes[0];
  const TableView& table = pages[0].tables[0];
  EXPECT_EQ(box.x + 2 + 6, table.x);
  EXPECT_EQ(box.right() - 2 - 6, table.x + table.w);
  EXPECT_EQ(box.y + 2 + 6, table.y);
  EXPECT_EQ(table.y + table.h + 6 + 2, box.bottom());
}

// A table's cells are laid out apart from the page, with no place for a frame: a box in a cell is not drawn, and its
// text stays in the cell.
TEST_F(BoxLayout, ABoxInATableCellIsNotDrawn) {
  const auto pages = layout("<table><tr><td><div class=\"box\">cell</div></td><td>other</td></tr></table>",
                            std::string(kReset) + ".box { border: 1px solid; background: #eee; }");
  ASSERT_EQ(1u, pages.size());
  EXPECT_TRUE(pages[0].boxes.empty());
  EXPECT_EQ(1u, pages[0].tables.size());
}

TEST_F(BoxLayout, AHeadingWithARuleBelowIt) {
  const auto pages = layout("<h2>Title</h2><p>text</p>", std::string(kReset) + "h2 { border-bottom: 2px solid; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  const BoxView& rule = pages[0].boxes[0];
  EXPECT_EQ(0, rule.widths[CssBox::Top]);
  EXPECT_EQ(2, rule.widths[CssBox::Bottom]);
  const LineView* title = pages[0].lineWith("Title");
  ASSERT_TRUE(title);
  EXPECT_LT(rule.index, title->index);
  EXPECT_GE(rule.bottom() - 2, title->y + kLine) << "the rule is under the heading's line";
}

TEST_F(BoxLayout, AFloatedPictureStaysInsideItsBox) {
  const auto pages = layout("<div class=\"box\"><span class=\"left\"><img src=\"pic.png\" alt=\"\"/></span><p>" +
                                words("word", 30) + "</p></div>",
                            std::string(kReset) + ".box { border: 1px solid; margin: 0 40px; }\n.left { float: left; }",
                            {{"pic.png", "image/png", syntheticPngHeader(60, 60)}});
  ASSERT_EQ(1u, pages[0].boxes.size());
  ASSERT_EQ(1u, pages[0].pictures.size());
  const BoxView& box = pages[0].boxes[0];
  const PictureView& pic = pages[0].pictures[0];
  EXPECT_EQ(box.x + 1 + kGap, pic.x);
  const LineView* first = pages[0].lineWith("word0");
  ASSERT_TRUE(first);
  EXPECT_GE(first->x, pic.x + pic.w) << "the text beside it starts after it";
  EXPECT_GE(box.bottom(), pic.y + pic.h) << "the frame holds the float";
}

TEST_F(BoxLayout, RightToLeftTextStaysInsideItsBox) {
  const auto pages = layout("<div class=\"box\" dir=\"rtl\"><p>שלום עולם</p></div>",
                            std::string(kReset) + ".box { border: 2px solid; padding: 8px; margin: 0 50px; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  ASSERT_EQ(1u, pages[0].lines.size());
  const BoxView& box = pages[0].boxes[0];
  const LineView& line = pages[0].lines[0];
  EXPECT_GE(line.x, box.x + 2 + 8);
  EXPECT_LT(line.lastWordX, box.right() - 2 - 8);
  EXPECT_GT(line.lastWordX, box.x + box.w / 2) << "right-aligned inside the frame, as right-to-left text sets";
}

// A vertical-rl page is laid out as a horizontal page in the turned frame, and the box with it: its CSS right side is
// where its lines start, the frame's top there.
TEST_F(BoxLayout, AVerticalPageTurnsItsBoxes) {
  vertical = true;
  const auto pages =
      layout("<div class=\"box\"><p>日本語の文章。</p></div>",
             std::string(kReset) + "html { writing-mode: vertical-rl; }\n.box { border-right: 3px solid; }");
  ASSERT_FALSE(pages.empty());
  EXPECT_TRUE(pages[0].vertical);
  ASSERT_EQ(1u, pages[0].boxes.size());
  const BoxView& box = pages[0].boxes[0];
  EXPECT_EQ(3, box.widths[CssBox::Top]) << "block-start, the right side of a vertical-rl box";
  EXPECT_EQ(0, box.widths[CssBox::Right]);
  EXPECT_EQ(0, box.widths[CssBox::Left]);
  ASSERT_FALSE(pages[0].lines.empty());
  EXPECT_EQ(box.y + 3 + kGap, pages[0].lines[0].y);
}

TEST_F(BoxLayout, APaperColouredBorderDrawsNothing) {
  const auto pages =
      layout("<p class=\"box\">text</p>", std::string(kReset) + ".box { border: 1px solid white; padding: 5px; }");
  EXPECT_TRUE(pages[0].boxes.empty());
  EXPECT_EQ(5, pages[0].lineWith("text")->x) << "its padding stays";
}

TEST_F(BoxLayout, WithTheBooksStyleOffNoBoxOfItsIsDrawn) {
  embeddedStyle = false;
  const auto pages = layout("<p class=\"box\">text</p><div style=\"border: 1px solid\">inline style</div>",
                            std::string(kReset) + ".box { border: 1px solid; background: #eee; }");
  EXPECT_TRUE(pages[0].boxes.empty());
}

// A fieldset's outline is the browser's styling, as a heading's size is: it stays with the book's style off, and then
// the book's CSS, which is not read, cannot take it away.
TEST_F(BoxLayout, AFieldsetIsOutlinedWithTheBooksStyleOff) {
  embeddedStyle = false;
  const auto pages = layout("<p>before</p><fieldset><legend>Legend</legend><p>field</p></fieldset>",
                            std::string(kReset) + "fieldset { border: none; padding: 0; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  const BoxView& box = pages[0].boxes[0];
  for (const int width : box.widths) EXPECT_EQ(1, width);
  for (const CssBorderStyle style : box.styles) EXPECT_EQ(CssBorderStyle::Solid, style);
  EXPECT_EQ(2, box.x) << "the browser's 2 px margin";
  const LineView* legend = pages[0].lineWith("Legend");
  ASSERT_TRUE(legend);
  EXPECT_LT(box.index, legend->index);
  EXPECT_GT(legend->x, box.x + 1) << "the browser's padding, inside the outline";
}

// The reader's extra paragraph spacing comes after the frame, as after a paragraph: inside, the bottom padding is the
// same as the top.
TEST_F(BoxLayout, TheExtraParagraphSpacingGoesAfterTheFrame) {
  extraParagraphSpacing = true;
  const auto pages = layout("<p class=\"box\">inside</p><p>after</p>",
                            std::string(kReset) + ".box { border: 1px solid; padding: 3px; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  const BoxView& box = pages[0].boxes[0];
  const LineView* inside = pages[0].lineWith("inside");
  const LineView* after = pages[0].lineWith("after");
  ASSERT_TRUE(inside && after);
  EXPECT_EQ(inside->y - box.y, box.bottom() - (inside->y + kLine));
  EXPECT_EQ(box.bottom() + kLine / 2, after->y);
}

TEST_F(BoxLayout, ACodeBoxIsShaded) {
  const auto pages = layout("<pre class=\"code\">int x;\nreturn x;</pre>",
                            std::string(kReset) + "pre.code { background-color: #f5f5f5; padding: 0.5em; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  EXPECT_TRUE(pages[0].boxes[0].shaded);
  const LineView* last = pages[0].lineWith("return");
  ASSERT_TRUE(last);
  EXPECT_LE(last->y + kLine, pages[0].boxes[0].bottom() - kGap);
}

TEST_F(BoxLayout, AFieldsetIsOutlined) {
  const auto pages = layout("<fieldset><legend>Legend</legend><p>field</p></fieldset>", kReset);
  ASSERT_EQ(1u, pages[0].boxes.size());
  for (const int width : pages[0].boxes[0].widths) EXPECT_EQ(1, width);
  // The book's CSS goes over the outline.
  const auto plain = layout("<fieldset><p>field</p></fieldset>", std::string(kReset) + "fieldset { border: none; }");
  EXPECT_TRUE(plain[0].boxes.empty());
}

TEST_F(BoxLayout, ARuleInABoxIsCentredInIt) {
  const auto pages = layout("<div class=\"box\"><p>a</p><hr/><p>b</p></div>",
                            std::string(kReset) + ".box { border: 1px solid; margin-left: 200px; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  ASSERT_EQ(1u, pages[0].ruleXs.size());
  const BoxView& box = pages[0].boxes[0];
  const int left = box.x + 1 + kGap;
  const int width = box.right() - 1 - kGap - left;
  EXPECT_EQ(left + width / 4, pages[0].ruleXs[0]);
}

// A book with no box pays nothing for them: no element, and the layout of its pages is the golden's.
TEST_F(BoxLayout, ABookWithoutBoxesHasNone) {
  const auto pages = layout("<p>a</p><hr/><p>b</p><div><p>c</p></div>",
                            std::string(kReset) + "hr { border: none; } p { border-color: red; }");
  EXPECT_TRUE(pages[0].boxes.empty());
  EXPECT_TRUE(pages[0].ruleXs.empty()) << "an <hr> with border none still draws nothing";
}

// A box whose first line does not fit where its frame would start begins on the next page, top edge and padding with
// it.
TEST_F(BoxLayout, ABoxThatStartsAtThePageFootStartsOnTheNextPage) {
  std::string body;
  for (int i = 0; i < 24; ++i) body += "<p>fill" + std::to_string(i) + "</p>";
  body += "<p class=\"box\">boxed</p>";
  const auto pages = layout(body, std::string(kReset) + ".box { border: 2px solid; padding: 10px; }");
  ASSERT_EQ(2u, pages.size());
  EXPECT_TRUE(pages[0].boxes.empty()) << "nothing of the box is on the first page";
  ASSERT_EQ(1u, pages[1].boxes.size());
  const BoxView& box = pages[1].boxes[0];
  EXPECT_EQ(0, box.y);
  EXPECT_EQ(2, box.widths[CssBox::Top]);
  EXPECT_EQ(2 + 10, pages[1].lineWith("boxed")->y);
}

// A paragraph over 96 words is laid out in parts (and, in the lent arena, line by line ahead of each line): the box
// still frames all of it, open where the page breaks.
TEST_F(BoxLayout, ALongParagraphInABoxRunsOverPages) {
  const auto pages = layout("<p class=\"box\">" + words("w", 700) + "</p><p>after</p>",
                            std::string(kReset) + ".box { border: 1px solid; }");
  ASSERT_GE(pages.size(), 2u);
  for (size_t p = 0; p < pages.size(); ++p) {
    if (pages[p].lines.empty() || !pages[p].lineWith("w")) continue;
    ASSERT_EQ(1u, pages[p].boxes.size()) << "page " << p;
    const BoxView& box = pages[p].boxes[0];
    EXPECT_EQ(p == 0 ? 1 : 0, box.widths[CssBox::Top]) << "page " << p;
    for (const LineView& line : pages[p].lines) {
      if (line.text.rfind("w", 0) != 0) continue;
      EXPECT_GE(line.y, box.y) << "page " << p;
      EXPECT_LE(line.y + kLine, box.bottom()) << "page " << p;
      EXPECT_GE(line.x, box.x + 1 + kGap) << "page " << p;
    }
  }
}

// Under a tight line spacing a line's glyphs reach below its advance: a frame under it starts below them.
TEST_F(BoxLayout, AFrameStartsBelowTheGlyphsOfTheLineAbove) {
  lineCompression = 0.75f;
  const auto pages =
      layout("<p>above</p><p class=\"box\">boxed</p>", std::string(kReset) + ".box { border: 1px solid; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  EXPECT_EQ(pages[0].lineWith("above")->y + kLine, pages[0].boxes[0].y) << "the full line height, not the advance";
}

// A display:block span is a block of its own, framed like one; one inside it stays inline, and has no frame.
TEST_F(BoxLayout, ABlockSpanIsFramedAndOneInsideItIsNot) {
  const auto pages = layout("<p>head <span class=\"b\">outer <span class=\"b\">inner</span> rest</span> tail</p>",
                            std::string(kReset) + "span.b { display: block; border: 1px solid; }");
  ASSERT_EQ(1u, pages[0].boxes.size());
  const BoxView& box = pages[0].boxes[0];
  const LineView* outer = pages[0].lineWith("outer");
  ASSERT_TRUE(outer);
  EXPECT_NE(std::string::npos, outer->text.find("inner")) << "the inner span stays in the line";
  EXPECT_EQ(box.y + 1 + kGap, outer->y);
  EXPECT_EQ(box.x + 1 + kGap, outer->x);
}
