// A section decides its writing mode at <body> (FlowContext::verticalFor) and, set vertical-rl, lays out as a
// horizontal page in the turned frame (spec 5.2): the viewport's width and height swap, every page keeps the reader's
// width as its block extent, every line its column bit. The twin test needs no arithmetic: a vertical section at
// 460 x 760 dumps exactly like the same chapter set horizontally at 760 x 460, but for the markers.
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

struct Line {
  std::string text;  // the line's words joined by single spaces
};

// The dump's body text lines, in order (PipelineRunner's "LINE" with its "W ... t=" words).
std::vector<Line> parseLines(const std::string& dump) {
  std::vector<Line> lines;
  std::istringstream in(dump);
  std::string row;
  while (std::getline(in, row)) {
    if (row.rfind("  LINE ", 0) == 0) {
      lines.emplace_back();
    } else if (row.rfind("   W ", 0) == 0 && !lines.empty()) {
      const size_t t = row.find(" t=");
      if (t == std::string::npos) continue;
      if (!lines.back().text.empty()) lines.back().text += ' ';
      lines.back().text += row.substr(t + 3);
    }
  }
  return lines;
}

bool hasLine(const std::vector<Line>& lines, const std::string& text) {
  for (const Line& line : lines) {
    if (line.text == text) return true;
  }
  return false;
}

// The first spine's SPINE line says it was set vertically.
bool firstSpineVertical(const std::string& dump) {
  const size_t spine = dump.find("SPINE 0 ");
  if (spine == std::string::npos) return false;
  const std::string line = dump.substr(spine, dump.find('\n', spine) - spine);
  return line.find(" vertical=1") != std::string::npos;
}

std::string without(std::string text, const std::string& marker) {
  for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at))
    text.erase(at, marker.size());
  return text;
}

constexpr const char* kBaseCss = "body { margin: 0; padding: 0 } p { margin: 0; text-indent: 0 }\n";

std::string document(const std::string& body, const std::string& htmlAttrs = "", const std::string& bodyAttrs = "") {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"" + htmlAttrs +
         "><head><title>C</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body" +
         bodyAttrs + ">\n" + body + "\n</body></html>\n";
}

pipeline_harness::Profile profileOf(const VerticalText request, const uint16_t width = 460,
                                    const uint16_t height = 760) {
  pipeline_harness::Profile p;
  p.verticalText = request;
  p.viewportWidth = width;
  p.viewportHeight = height;
  return p;
}

constexpr const char* kRtl = " page-progression-direction=\"rtl\"";

// The twin: a heading, two indented paragraphs (kana, kanji, 「」、。, a Latin word and digits, a ruby group), a rule
// and a 2 x 2 table. Every upright character is a full-width one, one em wide, so measuring along the column changes
// no width.
constexpr const char* kTwinCss = "p.i { text-indent: 1em }\n";
constexpr const char* kTwinBody =
    "<h2>第一章</h2>\n"
    "<p class=\"i\">吾輩は猫である。名前はまだ無い。「どこで生れたか」とんと見当がつかぬ。Natsume は1905年に"
    "これを書いた。何でも薄暗いじめじめした所で泣いていた事だけは記憶している。吾輩はここで始めて人間というものを"
    "見た。</p>\n"
    "<p class=\"i\"><ruby>吾輩<rt>わがはい</rt></ruby>は猫である。しかもあとで聞くとそれは書生という人間中で一番"
    "獰悪な種族であったそうだ。この書生というのは時々我々を捕えて煮て食うという話である。</p>\n"
    "<hr/>\n"
    "<table><tr><td>一行一列</td><td>一行二列</td></tr><tr><td>二行一列</td><td>二行二列</td></tr></table>\n";

constexpr const char* kVerticalBodyCss = "body.v { -webkit-writing-mode: vertical-rl }\n";
constexpr const char* kShortBody = "<p>吾輩は猫である。名前はまだ無い。</p>";

struct VerticalLayout : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_vertical_layout_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // A one-chapter book of `xhtml` with `css` as its stylesheet, in `language`; `spineAttrs` go on its OPF's <spine>.
  std::string writeBook(const std::string& css, const std::string& xhtml, const std::string& spineAttrs = "",
                        const std::string& language = "ja") {
    const int index = books++;
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"id\">vl</dc:identifier><dc:title>VL</dc:title><dc:language>" +
                language +
                "</dc:language></metadata>\n<manifest>\n"
                "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
                "<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/>\n</manifest>\n<spine" +
                spineAttrs + "><itemref idref=\"c\"/></spine>\n</package>\n");
    zip.add("chapter.xhtml", xhtml);
    zip.add("style.css", std::string(kBaseCss) + css);
    const fs::path path = work / ("book" + std::to_string(index) + ".epub");
    zip.write(path.string());
    return path.string();
  }

  // The book built and dumped in a cache of its own.
  std::string dump(const std::string& css, const std::string& xhtml, const pipeline_harness::Profile& profile,
                   const std::string& spineAttrs = "") {
    const std::string path = writeBook(css, xhtml, spineAttrs);
    std::ostringstream out;
    EXPECT_TRUE(pipeline_harness::runAndDump(path, path + ".cache", profile, out)) << out.str();
    return out.str();
  }

  // Parameters for a Section driven by hand, at the profiles' 460 x 760.
  static Section::BuildParams params(const VerticalText request) {
    Section::BuildParams p;
    p.fontId = 1;
    p.viewportWidth = 460;
    p.viewportHeight = 760;
    p.lineCompression = 1.0f;
    p.embeddedStyle = true;
    p.verticalText = request;
    return p;
  }

  static int countVariants(const std::string& dir, const int spineIndex) {
    const std::string prefix = std::to_string(spineIndex) + "_";
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
      const std::string name = e.path().filename().string();
      if (name.rfind(prefix, 0) == 0 && e.path().extension() == ".bin") ++n;
    }
    return n;
  }
};

}  // namespace

TEST_F(VerticalLayout, AVerticalSectionIsItsHorizontalTwinTurned) {
  const std::string vertical = dump(kTwinCss, document(kTwinBody), profileOf(VerticalText::On, 460, 760));
  const std::string horizontal = dump(kTwinCss, document(kTwinBody), profileOf(VerticalText::Off, 760, 460));
  EXPECT_TRUE(firstSpineVertical(vertical)) << vertical;
  EXPECT_NE(std::string::npos, vertical.find(" turned=460\n")) << vertical;
  EXPECT_NE(std::string::npos, vertical.find("  TABLE ")) << vertical;
  EXPECT_NE(std::string::npos, vertical.find("  HR ")) << vertical;
  EXPECT_NE(std::string::npos, vertical.find("   R first=")) << vertical;
  EXPECT_FALSE(firstSpineVertical(horizontal));
  EXPECT_EQ(horizontal, without(without(vertical, " vertical=1"), " turned=460"));
}

TEST_F(VerticalLayout, TheBookDecidesUnderAutomatic) {
  const pipeline_harness::Profile automatic = profileOf(VerticalText::Automatic);
  // A calibre conversion: the mode only as -webkit-writing-mode, through a class on <body>.
  EXPECT_TRUE(firstSpineVertical(dump(kVerticalBodyCss, document(kShortBody, "", " class=\"v\""), automatic)));
  // A colophon: horizontal-tb on its <body> outranks the book's right-to-left progression.
  EXPECT_FALSE(firstSpineVertical(
      dump("", document(kShortBody, "", " style=\"writing-mode: horizontal-tb\""), automatic, kRtl)));
  // No writing mode: a right-to-left book is the Japanese vertical convention.
  EXPECT_TRUE(firstSpineVertical(dump("", document(kShortBody), automatic, kRtl)));
  EXPECT_FALSE(firstSpineVertical(dump("", document(kShortBody), automatic)));
  // The mode set on <html> reaches <body>, but only through the book's CSS.
  constexpr const char* kHtmlVertical = "html { writing-mode: vertical-rl }\n";
  EXPECT_TRUE(firstSpineVertical(dump(kHtmlVertical, document(kShortBody), automatic)));
  pipeline_harness::Profile noStyle = automatic;
  noStyle.embeddedStyle = false;
  EXPECT_FALSE(firstSpineVertical(dump(kHtmlVertical, document(kShortBody), noStyle)));
}

TEST_F(VerticalLayout, OffKeepsAVerticalBookHorizontal) {
  const std::string xhtml = document(kShortBody, "", " class=\"v\"");
  const std::string off = dump(kVerticalBodyCss, xhtml, profileOf(VerticalText::Off));
  EXPECT_FALSE(firstSpineVertical(off)) << off;
  EXPECT_EQ(std::string::npos, off.find(" turned=")) << off;
  EXPECT_EQ(dump(kVerticalBodyCss, xhtml, pipeline_harness::Profile{}), off);
  EXPECT_TRUE(firstSpineVertical(dump(kVerticalBodyCss, xhtml, profileOf(VerticalText::Automatic))));
}

TEST_F(VerticalLayout, EveryPageAndLineOfAVerticalSectionIsTurned) {
  const std::string path = writeBook(kTwinCss, document(kTwinBody));
  for (const VerticalText request : {VerticalText::On, VerticalText::Off}) {
    const bool vertical = request == VerticalText::On;
    int pages = 0;
    int lines = 0;
    int cellLines = 0;
    const bool ok = pipeline_harness::forEachPage(
        path, path + (vertical ? ".on" : ".off"), profileOf(request),
        [&](const int spine, const Section& section, const Page& page, GfxRenderer&) {
          EXPECT_EQ(vertical, section.isVertical()) << "spine " << spine;
          EXPECT_EQ(vertical ? 460 : 0, page.verticalBlockExtent) << "page " << pages;
          ++pages;
          for (const auto& element : page.elements) {
            if (element->getTag() == TAG_PageLine) {
              EXPECT_EQ(vertical, static_cast<const PageLine&>(*element).getBlock()->isVertical());
              ++lines;
            } else if (element->getTag() == TAG_PageTable) {
              for (const auto& row : static_cast<const PageTableFragment&>(*element).getRows()) {
                for (const auto& cell : row.cells) {
                  for (const auto& line : cell.lines) {
                    EXPECT_EQ(vertical, line->isVertical());
                    ++cellLines;
                  }
                }
              }
            }
          }
        });
    EXPECT_TRUE(ok);
    EXPECT_GT(pages, 0);
    EXPECT_GT(lines, 0);
    EXPECT_EQ(4, cellLines);
  }
}

TEST_F(VerticalLayout, TheHeaderKeepsItsRequest) {
  const std::string path = writeBook(kVerticalBodyCss, document(kShortBody, "", " class=\"v\""));
  auto epub = std::make_shared<Epub>(path, path + ".cache");
  ASSERT_TRUE(epub->load(true));
  GfxRenderer renderer;
  {
    Section built(epub, 0, renderer);
    ASSERT_TRUE(built.createSectionFile(params(VerticalText::Automatic), {}, /*skipEviction=*/true));
    EXPECT_TRUE(built.isVertical());
  }
  {
    Section loaded(epub, 0, renderer);
    ASSERT_TRUE(loaded.loadSectionFile(params(VerticalText::Automatic)));
    EXPECT_TRUE(loaded.isVertical()) << "the outcome is the header's status bit";
    const FlowContext flow = loaded.flowContext();
    EXPECT_TRUE(flow.vertical);
    EXPECT_TRUE(flow.pageProgressionRtl) << "a vertical section turns right to left";
  }
  {
    Section forced(epub, 0, renderer);
    EXPECT_FALSE(forced.loadSectionFile(params(VerticalText::On))) << "another request is another variant: a rebuild";
  }
  {
    Section off(epub, 0, renderer);
    ASSERT_TRUE(off.createSectionFile(params(VerticalText::Off), {}, /*skipEviction=*/true));
    EXPECT_FALSE(off.isVertical());
    const FlowContext flow = off.flowContext();
    EXPECT_FALSE(flow.vertical);
    EXPECT_FALSE(flow.pageProgressionRtl);
  }
  // The request is in the property hash, so Off and Automatic are two files of the spine.
  EXPECT_EQ(2, countVariants(Epub::spineCacheDir(epub->getCachePath(), 0), 0));
}

TEST_F(VerticalLayout, ADropCapStaysInItsLine) {
  const std::string xhtml = document(
      "<p><span style=\"float: left; font-size: 3em\">吾</span>輩は猫である。名前はまだ無い。どこで生れたかとんと"
      "見当がつかぬ。何でも薄暗いじめじめした所でニャーニャー泣いていた事だけは記憶している。吾輩はここで始めて人間と"
      "いうものを見た。しかもあとで聞くとそれは書生という人間中で一番獰悪な種族であったそうだ。</p>");
  EXPECT_TRUE(hasLine(parseLines(dump("", xhtml, profileOf(VerticalText::Off))), "吾"));
  const std::string vertical = dump("", xhtml, profileOf(VerticalText::On));
  EXPECT_TRUE(firstSpineVertical(vertical));
  EXPECT_FALSE(hasLine(parseLines(vertical), "吾")) << "a drop cap's ink metrics are horizontal";
}
