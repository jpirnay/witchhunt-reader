// A section decides its writing mode at <body> (FlowContext::verticalFor) and, set vertical-rl, lays out as a
// horizontal page in the turned frame (spec 5.2): the viewport's width and height swap, every page keeps the reader's
// width as its block extent, every line its column bit. The twin test needs no arithmetic: a vertical section at
// 460 x 760 dumps exactly like the same chapter set horizontally at 760 x 460, but for the markers.
#include <Arduino.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/ImageBlock.h"
#include "LogObserver.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"
#include "SyntheticBook.h"

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
                        const std::string& language = "ja", const std::vector<SyntheticFile>& extra = {}) {
    const int index = books++;
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    std::string items;
    for (size_t i = 0; i < extra.size(); ++i) {
      items += "<item id=\"x" + std::to_string(i) + "\" href=\"" + extra[i].href + "\" media-type=\"" +
               extra[i].mediaType + "\"/>\n";
      zip.add(extra[i].href, extra[i].bytes);
    }
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"id\">vl</dc:identifier><dc:title>VL</dc:title><dc:language>" +
                language +
                "</dc:language></metadata>\n<manifest>\n"
                "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
                "<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/>\n" +
                items + "</manifest>\n<spine" + spineAttrs + "><itemref idref=\"c\"/></spine>\n</package>\n");
    zip.add("chapter.xhtml", xhtml);
    zip.add("style.css", std::string(kBaseCss) + css);
    const fs::path path = work / ("book" + std::to_string(index) + ".epub");
    zip.write(path.string());
    return path.string();
  }

  // The book built and dumped in a cache of its own.
  std::string dump(const std::string& css, const std::string& xhtml, const pipeline_harness::Profile& profile,
                   const std::string& spineAttrs = "", const std::vector<SyntheticFile>& extra = {}) {
    const std::string path = writeBook(css, xhtml, spineAttrs, "ja", extra);
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

  // The section files of spine `spineIndex` in its bucket `dir`.
  static std::vector<fs::path> variants(const std::string& dir, const int spineIndex) {
    const std::string prefix = std::to_string(spineIndex) + "_";
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
      const std::string name = e.path().filename().string();
      if (name.rfind(prefix, 0) == 0 && e.path().extension() == ".bin") files.push_back(e.path());
    }
    return files;
  }
  static int countVariants(const std::string& dir, const int spineIndex) {
    return static_cast<int>(variants(dir, spineIndex).size());
  }

  // Steps a sliced build of `section` under `p` until it is live with a page written, so its parser is past <body>.
  static bool stepToAPage(Section& section, const Section::BuildParams& p) {
    // A ticking clock, or the slice budget never runs out and the build ends in one call.
    const host_clock::Ticking tick(1);
    for (int i = 0; i < 20000; ++i) {
      const Section::BuildStep step = section.stepSectionBuild(p, /*budgetMs=*/1);
      if (section.hasActiveBuild() && section.pageCount > 0) return true;
      if (step == Section::BuildStep::Done || step == Section::BuildStep::Failed) return false;
    }
    return false;
  }
};

void expectFlow(const Section& section, const bool vertical, const bool rtl, const char* what) {
  const FlowContext flow = section.flowContext();
  EXPECT_EQ(vertical, flow.vertical) << what;
  EXPECT_EQ(rtl, flow.pageProgressionRtl) << what;
}

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

// A load that fails leaves the section horizontal under the request it was asked for, and a cleared one is a fresh
// section's, never the request of a cache it no longer holds. The book turns right to left by its spine, so a
// horizontal section of it turns right to left under Automatic and left to right under Off.
TEST_F(VerticalLayout, AFailedLoadTakesTheRequestItWasGiven) {
  const std::string path = writeBook(kVerticalBodyCss, document(kShortBody, "", " class=\"v\""), kRtl);
  auto epub = std::make_shared<Epub>(path, path + ".cache");
  ASSERT_TRUE(epub->load(true));
  const std::string bucket = Epub::spineCacheDir(epub->getCachePath(), 0);
  GfxRenderer renderer;
  const auto build = [&](const VerticalText request) {
    Section builder(epub, 0, renderer);
    return builder.createSectionFile(params(request), {}, /*skipEviction=*/true);
  };
  ASSERT_TRUE(build(VerticalText::Automatic));

  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.loadSectionFile(params(VerticalText::Automatic)));
  expectFlow(section, true, true, "loaded vertical");
  EXPECT_FALSE(section.loadSectionFile(params(VerticalText::Off))) << "no Off variant on disk";
  EXPECT_FALSE(section.isVertical());
  expectFlow(section, false, false, "a missing Off variant");
  ASSERT_TRUE(section.loadSectionFile(params(VerticalText::Automatic)));
  section.clearCache();
  EXPECT_FALSE(section.isVertical());
  expectFlow(section, false, false, "cleared");

  // A header whose request does not match: the On variant's file (the same pages: the book is vertical either way)
  // under the Automatic variant's name.
  ASSERT_TRUE(build(VerticalText::Automatic));
  const std::vector<fs::path> automatic = variants(bucket, 0);
  ASSERT_EQ(1u, automatic.size());
  ASSERT_TRUE(build(VerticalText::On));
  std::vector<fs::path> both = variants(bucket, 0);
  ASSERT_EQ(2u, both.size());
  const fs::path on = both[0] == automatic[0] ? both[1] : both[0];
  fs::copy_file(on, automatic[0], fs::copy_options::overwrite_existing);
  Section mismatched(epub, 0, renderer);
  EXPECT_FALSE(mismatched.loadSectionFile(params(VerticalText::Automatic))) << "the header's request is matched";
  EXPECT_FALSE(fs::exists(automatic[0])) << "a mismatched cache is removed, for a rebuild";
  EXPECT_FALSE(mismatched.isVertical());
  expectFlow(mismatched, false, true, "a mismatched header, under Automatic");
}

// The colophon of test_vertical_text sets horizontal-tb on its <body>: it stays horizontal under Automatic, and still
// turns right to left with the book. Off keeps both sections as they were before the step.
TEST_F(VerticalLayout, AColophonStaysHorizontalAndTurnsRightToLeft) {
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_vertical_text.epub", (work / "cache").string());
  ASSERT_TRUE(epub->load(true));
  ASSERT_EQ(2, epub->getSpineItemsCount());
  GfxRenderer renderer;
  const auto flowOf = [&](const int spine, const VerticalText request) {
    Section section(epub, spine, renderer);
    EXPECT_TRUE(section.createSectionFile(params(request), {}, /*skipEviction=*/true));
    return section.flowContext();
  };
  const FlowContext chapter = flowOf(0, VerticalText::Automatic);
  EXPECT_TRUE(chapter.vertical);
  EXPECT_TRUE(chapter.pageProgressionRtl);
  const FlowContext colophon = flowOf(1, VerticalText::Automatic);
  EXPECT_FALSE(colophon.vertical);
  EXPECT_TRUE(colophon.pageProgressionRtl);
  for (const int spine : {0, 1}) {
    const FlowContext off = flowOf(spine, VerticalText::Off);
    EXPECT_FALSE(off.vertical) << "spine " << spine;
    EXPECT_FALSE(off.pageProgressionRtl) << "spine " << spine;
  }
}

// XHTML names its elements in lower case, but a book written as HTML may not: the section still decides at its body,
// not at an element of an upper-case head.
TEST_F(VerticalLayout, AnUppercaseHeadIsNotTheBody) {
  const std::string xhtml =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<HTML xmlns=\"http://www.w3.org/1999/xhtml\"><HEAD><TITLE>C</TITLE>"
      "<LINK rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></HEAD><BODY class=\"v\">\n" +
      std::string(kShortBody) + "\n</BODY></HTML>\n";
  EXPECT_TRUE(firstSpineVertical(dump(kVerticalBodyCss, xhtml, profileOf(VerticalText::Automatic))));
}

// While a build runs, the section answers from its parser: vertical once the parser has decided, and never from the
// cache it loaded before.
TEST_F(VerticalLayout, ABuildInProgressAnswersFromItsParser) {
  // Several 1 KB parse slices long, so the build is still live after its first page.
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_cjk_ja_soseki.epub", (work / "cache").string());
  ASSERT_TRUE(epub->load(true));
  GfxRenderer renderer;
  {
    Section building(epub, 0, renderer);
    ASSERT_TRUE(stepToAPage(building, params(VerticalText::On)));
    EXPECT_TRUE(building.isVertical()) << "the live parser decided vertical";
    // Finished rather than aborted: an abort mid-parse removes the anchor spill the parser still holds open, which
    // Windows refuses.
    ASSERT_EQ(Section::BuildStep::Done, building.stepSectionBuild(params(VerticalText::On), /*budgetMs=*/0));
    EXPECT_TRUE(building.isVertical()) << "the build's outcome";
  }
  {
    Section builder(epub, 0, renderer);
    ASSERT_TRUE(builder.createSectionFile(params(VerticalText::On), {}, /*skipEviction=*/true));
  }
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.loadSectionFile(params(VerticalText::On)));
  ASSERT_TRUE(section.isVertical());
  ASSERT_TRUE(stepToAPage(section, params(VerticalText::Off)));
  EXPECT_FALSE(section.isVertical()) << "an Off build in progress is horizontal, whatever was loaded before";
  ASSERT_EQ(Section::BuildStep::Done, section.stepSectionBuild(params(VerticalText::Off), /*budgetMs=*/0));
  EXPECT_FALSE(section.isVertical());
}

// forEachPage builds on the heap alone: it refuses a profile that asks for a lent arena rather than ignore it.
TEST_F(VerticalLayout, ForEachPageRefusesALentArena) {
  const std::string path = writeBook("", document(kShortBody));
  pipeline_harness::Profile lent = profileOf(VerticalText::Off);
  lent.lentArenaBytes = 64 * 1024;
  int visits = 0;
  EXPECT_FALSE(pipeline_harness::forEachPage(path, path + ".cache", lent,
                                             [&](int, const Section&, const Page&, GfxRenderer&) { ++visits; }));
  EXPECT_EQ(0, visits);
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

namespace {
int g_boundsRejected = 0;
void countBoundsRejected(const char*, const char*, const char* message) {
  if (std::string(message).find("Render bounds rejected") != std::string::npos) ++g_boundsRejected;
}

std::vector<SyntheticFile> png(const uint32_t width, const uint32_t height) {
  return {{"img.png", "image/png", syntheticPngHeader(width, height)}};
}
constexpr const char* kImageBody = "<p><img src=\"img.png\" alt=\"\"/></p>";

// The y= of the first "  IMG " record in a dump, -1 when there is none.
int firstImageY(const std::string& dump) {
  const size_t at = dump.find("  IMG y=");
  return at == std::string::npos ? -1 : std::atoi(dump.c_str() + at + 8);
}
}  // namespace

// The 200 x 400 image is a 400 x 200 box in the turned frame; alone on a page 460 wide it moves to (460 - 200) / 2.
TEST_F(VerticalLayout, ALoneImageIsCentredAcrossAVerticalPage) {
  const std::string on = dump("", document(kImageBody), profileOf(VerticalText::On), "", png(200, 400));
  EXPECT_NE(std::string::npos, on.find("IMG y=130 x=180 w=400 h=200 ")) << on;
}

// Beside text an image keeps its place in the flow: the same block position as on the horizontal page.
TEST_F(VerticalLayout, AnImageBesideTextKeepsItsPlace) {
  const std::string body = std::string("<p>本文</p>") + kImageBody;
  const int on = firstImageY(dump("", document(body), profileOf(VerticalText::On), "", png(200, 400)));
  const int off = firstImageY(dump("", document(body), profileOf(VerticalText::Off), "", png(200, 400)));
  EXPECT_GT(on, 0);
  EXPECT_EQ(off, on);
}

// An image as wide as the block extent stays where it is (the acceptance book's full-page maps).
TEST_F(VerticalLayout, AnImageSpanningThePageIsNotMoved) {
  const std::string on = dump("", document(kImageBody), profileOf(VerticalText::On), "", png(460, 400));
  EXPECT_EQ(0, firstImageY(on)) << on;
  EXPECT_NE(std::string::npos, on.find(" h=460 ")) << on;
}

TEST_F(VerticalLayout, APortraitImageIsSizedTurnedIntoTheColumns) {
  const std::string on = dump("", document(kImageBody), profileOf(VerticalText::On), "", png(200, 400));
  EXPECT_NE(std::string::npos, on.find("x=180 w=400 h=200 ")) << on;
  EXPECT_NE(std::string::npos, on.find(" turned=1\n")) << on;
  const std::string path = writeBook("", document(kImageBody), "", "ja", png(200, 400));
  int images = 0;
  EXPECT_TRUE(pipeline_harness::forEachPage(
      path, path + ".cache", profileOf(VerticalText::On), [&](int, const Section&, const Page& page, GfxRenderer&) {
        for (const auto& e : page.elements) {
          if (e->getTag() != TAG_PageImage) continue;
          const ImageBlock& ib = static_cast<const PageImage&>(*e).getImageBlock();
          EXPECT_TRUE(ib.isTurned());
          EXPECT_EQ(200, ib.drawnWidth());
          EXPECT_EQ(400, ib.drawnHeight());
          ++images;
        }
      }));
  EXPECT_EQ(1, images);
}

TEST_F(VerticalLayout, ALandscapeImageIsSizedTurned) {
  const std::string path = writeBook("", document(kImageBody), "", "ja", png(400, 200));
  const std::string on = dump("", document(kImageBody), profileOf(VerticalText::On), "", png(400, 200));
  EXPECT_NE(std::string::npos, on.find("x=280 w=200 h=400 ")) << on;
  EXPECT_NE(std::string::npos, on.find(" turned=1\n")) << on;
  int images = 0;
  EXPECT_TRUE(pipeline_harness::forEachPage(
      path, path + ".cache", profileOf(VerticalText::On), [&](int, const Section&, const Page& page, GfxRenderer&) {
        for (const auto& e : page.elements) {
          if (e->getTag() != TAG_PageImage) continue;
          const ImageBlock& ib = static_cast<const PageImage&>(*e).getImageBlock();
          EXPECT_EQ(400, ib.drawnWidth());
          EXPECT_EQ(200, ib.drawnHeight());
          ++images;
        }
      }));
  EXPECT_EQ(1, images);
}

TEST_F(VerticalLayout, AnImageTallerThanThePageIsScaledNotSliced) {
  const std::string on = dump("", document(kImageBody), profileOf(VerticalText::On), "", png(230, 1520));
  EXPECT_NE(std::string::npos, on.find("x=0 w=760 h=115 ")) << on;
  size_t images = 0;
  for (size_t at = on.find("  IMG "); at != std::string::npos; at = on.find("  IMG ", at + 1)) ++images;
  EXPECT_EQ(1u, images) << on;
  const std::string path = writeBook("", document(kImageBody), "", "ja", png(230, 1520));
  EXPECT_TRUE(pipeline_harness::forEachPage(
      path, path + ".cache", profileOf(VerticalText::On), [&](int, const Section&, const Page& page, GfxRenderer&) {
        for (const auto& e : page.elements) {
          if (e->getTag() != TAG_PageImage) continue;
          const ImageBlock& ib = static_cast<const PageImage&>(*e).getImageBlock();
          EXPECT_EQ(115, ib.drawnWidth());
          EXPECT_EQ(760, ib.drawnHeight());
        }
      }));
}

TEST_F(VerticalLayout, ACellImageIsTurned) {
  const std::string body = "<table><tr><td><img src=\"img.png\" alt=\"\"/></td></tr></table>";
  const std::string dumped = dump("", document(body), profileOf(VerticalText::On), "", png(200, 400));
  EXPECT_NE(std::string::npos, dumped.find("CELLIMG ")) << dumped;
  EXPECT_NE(std::string::npos, dumped.find(" turned=1\n")) << dumped;
  const std::string path = writeBook("", document(body), "", "ja", png(200, 400));
  int cellImages = 0;
  EXPECT_TRUE(pipeline_harness::forEachPage(
      path, path + ".cache", profileOf(VerticalText::On), [&](int, const Section&, const Page& page, GfxRenderer&) {
        for (const auto& e : page.elements) {
          if (e->getTag() != TAG_PageTable) continue;
          for (const auto& row : static_cast<const PageTableFragment&>(*e).getRows()) {
            for (const auto& cell : row.cells) {
              if (!cell.image) continue;
              EXPECT_TRUE(cell.image->isTurned());
              EXPECT_EQ(cell.image->getHeight(), cell.image->drawnWidth());
              EXPECT_EQ(cell.image->getWidth(), cell.image->drawnHeight());
              ++cellImages;
            }
          }
        }
      }));
  EXPECT_EQ(1, cellImages);
}

TEST_F(VerticalLayout, ATurnedImageDrawsInsideTheReadersScreen) {
  const std::string path = writeBook("", document(kImageBody), "", "ja", png(230, 1520));
  g_boundsRejected = 0;
  setLogObserver(countBoundsRejected);
  int pages = 0;
  const bool ok = pipeline_harness::forEachPage(path, path + ".cache", profileOf(VerticalText::On),
                                                [&](int, const Section&, const Page& page, GfxRenderer& r) {
                                                  page.render(r, 1, 10, 20, /*forceLoadLargeImages=*/true);
                                                  EXPECT_EQ(GfxRenderer::Portrait, r.getOrientation());
                                                  ++pages;
                                                });
  setLogObserver(nullptr);
  EXPECT_TRUE(ok);
  EXPECT_GT(pages, 0);
  EXPECT_EQ(0, g_boundsRejected);
}

TEST_F(VerticalLayout, AHorizontalImageIsNotTurned) {
  const std::string off = dump("", document(kImageBody), profileOf(VerticalText::Off), "", png(200, 400));
  EXPECT_NE(std::string::npos, off.find("w=200 h=400 ")) << off;
  EXPECT_EQ(std::string::npos, off.find("turned")) << off;
}

namespace {
struct DumpedWord {
  int line;
  int x;
  std::string text;
};

// Every W record of the dump with the index of the line it is on.
std::vector<DumpedWord> dumpedWords(const std::string& dump) {
  std::vector<DumpedWord> words;
  std::istringstream in(dump);
  std::string row;
  int line = -1;
  while (std::getline(in, row)) {
    if (row.rfind("  LINE ", 0) == 0) {
      ++line;
    } else if (row.rfind("   W x=", 0) == 0 && line >= 0) {
      const size_t t = row.find(" t=");
      if (t == std::string::npos) continue;
      words.push_back({line, std::stoi(row.substr(7)), row.substr(t + 3)});
    }
  }
  return words;
}

int occurrences(const std::string& text, const std::string& what) {
  int count = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++count;
  return count;
}
}  // namespace

// × and § are upright in a column (one em, 16 in the stub) and 11 and 12 wide across a line.
TEST_F(VerticalLayout, AnUprightCharacterAdvancesOneEm) {
  const std::string xhtml = document("<p>あ×い §を×え</p>");
  pipeline_harness::Profile on = profileOf(VerticalText::On, 460, 760);
  pipeline_harness::Profile off = profileOf(VerticalText::Off, 760, 460);
  on.paragraphAlignment = 1;
  off.paragraphAlignment = 1;
  const std::vector<DumpedWord> vertical = dumpedWords(dump("", xhtml, on));
  const std::vector<DumpedWord> horizontal = dumpedWords(dump("", xhtml, off));
  ASSERT_FALSE(vertical.empty());
  ASSERT_EQ(horizontal.size(), vertical.size());
  int shift = 0;
  int lineOfShift = -1;
  int moved = 0;
  for (size_t i = 0; i < vertical.size(); ++i) {
    ASSERT_EQ(horizontal[i].text, vertical[i].text);
    ASSERT_EQ(horizontal[i].line, vertical[i].line);
    if (vertical[i].line != lineOfShift) {
      lineOfShift = vertical[i].line;
      shift = 0;
    }
    EXPECT_EQ(horizontal[i].x + shift, vertical[i].x) << "word " << i << " " << vertical[i].text;
    if (shift != 0) ++moved;
    shift += 5 * occurrences(vertical[i].text, "\xC3\x97") + 4 * occurrences(vertical[i].text, "\xC2\xA7");
  }
  EXPECT_GT(moved, 0);
}
