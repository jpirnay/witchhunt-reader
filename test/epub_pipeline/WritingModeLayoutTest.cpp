// Boxes read along the CSS writing mode, text-align start/end along the direction, still set horizontally.
//
// A book written for vertical text names its margins physically for a vertical page: there, top is inline-start
// (down the column), right is block-start (before the first column), left block-end, and height the inline size.
// Layout reads them logically, so the parser turns each element's box by the writing mode its CSS was written for
// (CssStyle::resolveLogical). The "twin" tests need no arithmetic: a book written vertically must dump exactly like a
// horizontal book whose CSS names the same logical sides physically. The position tests derive their values from the
// host stub's metrics (GfxRenderer.h: ascender 18, the em layout resolves lengths against; line height 24) and block
// margins collapsing to the larger of two (ChapterHtmlSlimParser, the margin-top collapse before a block's first line).
#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "LogObserver.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

struct Line {
  int y = 0;
  int x = 0;
  int align = 0;
  std::string text;  // the line's words joined by single spaces
};

// The dump's text lines, in order (PipelineRunner's "LINE y= x= align=" with its "W ... t=" words).
std::vector<Line> parseLines(const std::string& dump) {
  std::vector<Line> lines;
  std::istringstream in(dump);
  std::string row;
  while (std::getline(in, row)) {
    if (row.rfind("  LINE ", 0) == 0) {
      Line line;
      std::sscanf(row.c_str(), "  LINE y=%d x=%d align=%d", &line.y, &line.x, &line.align);
      lines.push_back(line);
    } else if (row.rfind("   W ", 0) == 0 && !lines.empty()) {
      const size_t t = row.find(" t=");
      if (t == std::string::npos) continue;
      if (!lines.back().text.empty()) lines.back().text += ' ';
      lines.back().text += row.substr(t + 3);
    }
  }
  return lines;
}

const Line* lineWith(const std::vector<Line>& lines, const std::string& text) {
  for (const Line& line : lines) {
    if (line.text == text) return &line;
  }
  return nullptr;
}

constexpr const char* kBaseCss = "body { margin: 0; padding: 0 } p { margin: 0; text-indent: 0 }\n";

std::string document(const std::string& body, const std::string& htmlAttrs = "", const std::string& bodyAttrs = "") {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"" + htmlAttrs +
         "><head><title>C</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body" +
         bodyAttrs + ">\n" + body + "\n</body></html>\n";
}

// The book's text set with the book's own alignment, so text-align reaches the layout (the default profile's justify
// overrides it).
pipeline_harness::Profile bookAlignment() {
  pipeline_harness::Profile p;
  p.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::None);
  return p;
}

struct WritingModeLayout : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_writing_mode_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // A one-chapter book of `xhtml` with `css` as its stylesheet; `spineAttrs` go on its OPF's <spine>.
  std::string writeBook(const std::string& css, const std::string& xhtml, const std::vector<SyntheticFile>& extra = {},
                        const std::string& spineAttrs = "") {
    const int index = books++;
    std::vector<SyntheticFile> files = extra;
    files.push_back({"style.css", "text/css", std::string(kBaseCss) + css});
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    std::string manifest = "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
    for (size_t i = 0; i < files.size(); ++i) {
      manifest += "<item id=\"x" + std::to_string(i) + "\" href=\"" + files[i].href + "\" media-type=\"" +
                  files[i].mediaType + "\"/>\n";
    }
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"id\">wm</dc:identifier><dc:title>WM</dc:title><dc:language>en</dc:language>"
            "</metadata>\n<manifest>\n" +
                manifest + "</manifest>\n<spine" + spineAttrs + "><itemref idref=\"c\"/></spine>\n</package>\n");
    zip.add("chapter.xhtml", xhtml);
    for (const auto& file : files) zip.add(file.href, file.bytes);
    const fs::path path = work / ("book" + std::to_string(index) + ".epub");
    zip.write(path.string());
    return path.string();
  }

  // The book built and dumped in a cache of its own.
  std::string dump(const std::string& css, const std::string& xhtml,
                   const pipeline_harness::Profile& profile = pipeline_harness::Profile{},
                   const std::vector<SyntheticFile>& extra = {}, const std::string& spineAttrs = "") {
    const std::string path = writeBook(css, xhtml, extra, spineAttrs);
    std::ostringstream out;
    EXPECT_TRUE(pipeline_harness::runAndDump(path, path + ".cache", profile, out)) << out.str();
    return out.str();
  }
};

// ---- Boxes along the writing mode ----

constexpr const char* kTwinBody = "<p>Alpha</p><p class=\"ind\">Bravo</p><p class=\"gap\">Charlie</p><p>Delta</p>";
constexpr const char* kVerticalCss =
    ".v { -webkit-writing-mode: vertical-rl } p { margin: 1em 0 } .ind { padding-top: 2em } "
    ".gap { margin-right: 1em; margin-left: 2em }\n";
constexpr const char* kHorizontalTwinCss =
    "p { margin: 0 1em } .ind { padding-left: 2em } .gap { margin-top: 1em; margin-bottom: 2em }\n";

TEST_F(WritingModeLayout, AVerticalBookLaysOutAsItsHorizontalTwin) {
  const std::string vertical = dump(kVerticalCss, document(kTwinBody, "", " class=\"v\""));
  const std::string horizontal = dump(kHorizontalTwinCss, document(kTwinBody));
  EXPECT_EQ(horizontal, vertical);
}

TEST_F(WritingModeLayout, AVerticalLrBookLaysOutAsItsHorizontalTwin) {
  // vertical-lr: block-start is the left side, block-end the right.
  const std::string vertical = dump(".v { writing-mode: vertical-lr } .gap { margin-left: 1em; margin-right: 2em }\n",
                                    document(kTwinBody, "", " class=\"v\""));
  const std::string horizontal = dump(".gap { margin-top: 1em; margin-bottom: 2em }\n", document(kTwinBody));
  EXPECT_EQ(horizontal, vertical);
}

TEST_F(WritingModeLayout, AVerticalBooksPositions) {
  const auto lines = parseLines(dump(kVerticalCss, document(kTwinBody, "", " class=\"v\"")));
  const Line* alpha = lineWith(lines, "Alpha");
  const Line* bravo = lineWith(lines, "Bravo");
  const Line* charlie = lineWith(lines, "Charlie");
  ASSERT_TRUE(alpha && bravo && charlie);
  // Turned, p { margin: 1em 0 } is margin-left and margin-right 18 and no vertical margin: Alpha at the top, inset 18.
  EXPECT_EQ(0, alpha->y);
  EXPECT_EQ(18, alpha->x);
  // One 24 px line later; .ind's padding-top 2em is a padding-left of 36 on top of the 18.
  EXPECT_EQ(24, bravo->y);
  EXPECT_EQ(54, bravo->x);
  // Bravo's line ends at 48. .gap's margin-right 1em is Charlie's margin-top, 18, and Bravo left no margin-bottom for
  // it to collapse into.
  EXPECT_EQ(66, charlie->y);
  EXPECT_EQ(18, charlie->x);

  // The same CSS without the writing mode, read horizontally as before.
  const auto control = parseLines(dump(kVerticalCss, document(kTwinBody)));
  const Line* alpha0 = lineWith(control, "Alpha");
  const Line* bravo0 = lineWith(control, "Bravo");
  ASSERT_TRUE(alpha0 && bravo0);
  EXPECT_EQ(18, alpha0->y);  // Alpha's margin-top
  EXPECT_EQ(0, alpha0->x);
  // Alpha: 18 + 24 + its margin-bottom 18 = 60; Bravo's margin-top 18 collapses into it; then its padding-top 36.
  EXPECT_EQ(96, bravo0->y);
  EXPECT_EQ(0, bravo0->x);
}

TEST_F(WritingModeLayout, AVerticalImageHeightIsAHorizontalWidth) {
  // Half the column is half the line once turned: imageWidth, resolved against the container width.
  const std::vector<SyntheticFile> image = {{"tall.png", "image/png", syntheticPngHeader(100, 200)}};
  const std::string vertical =
      dump(".v { writing-mode: vertical-rl }\n",
           document("<p><img src=\"tall.png\" alt=\"\" style=\"height: 50%\"/></p>", "", " class=\"v\""),
           pipeline_harness::Profile{}, image);
  const std::string horizontal = dump("", document("<p><img src=\"tall.png\" alt=\"\" style=\"width: 50%\"/></p>"),
                                      pipeline_harness::Profile{}, image);
  EXPECT_NE(std::string::npos, vertical.find("  IMG ")) << vertical;
  EXPECT_EQ(horizontal, vertical);
}

TEST_F(WritingModeLayout, AVerticalSpanIndentIsAHorizontalOne) {
  const std::string body = "<p><span class=\"i\">Alpha</span> Bravo</p>";
  const std::string vertical =
      dump(".v { writing-mode: vertical-rl } span.i { margin-top: 2em }\n", document(body, "", " class=\"v\""));
  const std::string horizontal = dump("span.i { margin-left: 2em }\n", document(body));
  EXPECT_EQ(horizontal, vertical);
}

TEST_F(WritingModeLayout, TheModeComesFromHtmlOrBody) {
  const std::string horizontal = dump(kHorizontalTwinCss, document(kTwinBody));
  // The EBPAJ template sets it on <html>, a calibre conversion through a class on <body>, prefixed only.
  const std::string ebpaj = dump(std::string("html.vrtl { writing-mode: vertical-rl } ") + kVerticalCss,
                                 document(kTwinBody, " class=\"vrtl\""));
  const std::string calibre = dump(std::string(".calibre { -webkit-writing-mode: vertical-rl } ") + kVerticalCss,
                                   document(kTwinBody, "", " class=\"calibre\""));
  EXPECT_EQ(horizontal, ebpaj);
  EXPECT_EQ(horizontal, calibre);
}

TEST_F(WritingModeLayout, ANestedHorizontalBlockIsNotTurned) {
  const auto lines = parseLines(
      dump(".v { writing-mode: vertical-rl } p { margin: 1em 0 }\n",
           document("<p>Alpha</p><div style=\"writing-mode: horizontal-tb\"><p>Bravo</p></div><p>Charlie</p>", "",
                    " class=\"v\"")));
  const Line* alpha = lineWith(lines, "Alpha");
  const Line* bravo = lineWith(lines, "Bravo");
  const Line* charlie = lineWith(lines, "Charlie");
  ASSERT_TRUE(alpha && bravo && charlie);
  EXPECT_EQ(0, alpha->y);  // turned: inset 18, no vertical margin
  EXPECT_EQ(18, alpha->x);
  EXPECT_EQ(42, bravo->y);  // not turned: its margin-top 18 after Alpha's line
  EXPECT_EQ(0, bravo->x);
  EXPECT_EQ(84, charlie->y);  // after Bravo's line (66) and its margin-bottom 18; turned again
  EXPECT_EQ(18, charlie->x);
}

TEST_F(WritingModeLayout, OneClassUnderTwoModes) {
  // The parser memoises styles by tag and class: the turn must apply to the element's copy, not the memo's.
  const auto lines =
      parseLines(dump(".v { writing-mode: vertical-rl } .x { margin: 1em 0 }\n",
                      document("<div class=\"v\"><p class=\"x\">Alpha</p></div><p class=\"x\">Bravo</p>")));
  const Line* alpha = lineWith(lines, "Alpha");
  const Line* bravo = lineWith(lines, "Bravo");
  ASSERT_TRUE(alpha && bravo);
  EXPECT_EQ(18, alpha->x);
  EXPECT_EQ(0, bravo->x);
}

TEST_F(WritingModeLayout, AHiddenElementsModeDoesNotLeak) {
  const auto lines = parseLines(dump(
      ".x { margin: 1em 0 }\n",
      document("<div style=\"display: none; writing-mode: vertical-rl\"><p>Gone</p></div><p class=\"x\">Alpha</p>")));
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ("Alpha", lines[0].text);
  EXPECT_EQ(18, lines[0].y);
  EXPECT_EQ(0, lines[0].x);
}

// ---- text-align start and end along the direction ----

constexpr const char* kStartEndCss = ".s { text-align: start } .e { text-align: end } .r { direction: rtl }\n";
constexpr int kLeft = static_cast<int>(CssTextAlign::Left);
constexpr int kRight = static_cast<int>(CssTextAlign::Right);

TEST_F(WritingModeLayout, StartAndEndFollowDir) {
  const std::string body =
      "<p class=\"s\">Alpha</p><p class=\"e\">Bravo</p><div dir=\"ltr\"><p class=\"s\">Charlie</p></div>";
  const auto rtl = parseLines(dump(kStartEndCss, document(body, " dir=\"rtl\""), bookAlignment()));
  ASSERT_EQ(3u, rtl.size());
  EXPECT_EQ(kRight, rtl[0].align);
  EXPECT_EQ(kLeft, rtl[1].align);
  EXPECT_EQ(kLeft, rtl[2].align);  // the inner dir="ltr" resets it
  const auto ltr = parseLines(dump(kStartEndCss, document(body), bookAlignment()));
  ASSERT_EQ(3u, ltr.size());
  EXPECT_EQ(kLeft, ltr[0].align);  // as master set start and end before they were kept logical
  EXPECT_EQ(kRight, ltr[1].align);
}

TEST_F(WritingModeLayout, StartAndEndFollowCssDirection) {
  const auto lines = parseLines(dump(kStartEndCss,
                                     document("<div class=\"r\"><p class=\"s\">Alpha</p></div>"
                                              "<p class=\"s\" dir=\"rtl\" style=\"direction: ltr\">Bravo</p>"),
                                     bookAlignment()));
  ASSERT_EQ(2u, lines.size());
  EXPECT_EQ(kRight, lines[0].align);  // inherited from the div's direction
  EXPECT_EQ(kLeft, lines[1].align);   // the author's CSS beats the attribute
}

TEST_F(WritingModeLayout, DirAutoReadsAsLtrForNow) {
  // Auto is decided from the text (UAX #9 P2/P3), which the right-to-left step brings; until then it reads as ltr.
  const auto lines = parseLines(dump(
      kStartEndCss, document("<p class=\"s\" dir=\"auto\">\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(kLeft, lines[0].align);
}

TEST_F(WritingModeLayout, AnInlineBdoLeavesTheBlockAlone) {
  const auto lines = parseLines(
      dump(kStartEndCss, document("<p class=\"s\">abc <bdo dir=\"rtl\">def</bdo> ghi</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(kLeft, lines[0].align);
}

std::vector<std::string>* gEhpLines = nullptr;
void collectEhp(const char* /*level*/, const char* origin, const char* message) {
  if (gEhpLines != nullptr && std::string(origin) == "EHP") gEhpLines->push_back(message);
}

TEST_F(WritingModeLayout, DirHoldsWithoutTheBooksCss) {
  // dir is markup, not style: with the book's CSS off the parser still takes the direction from it.
  std::vector<std::string> ehp;
  gEhpLines = &ehp;
  setLogObserver(collectEhp);
  pipeline_harness::Profile noCss = bookAlignment();
  noCss.embeddedStyle = false;
  dump(kStartEndCss, document("<p>Alpha</p>", " dir=\"rtl\""), noCss);
  setLogObserver(nullptr);
  gEhpLines = nullptr;
  bool sawRtl = false;
  for (const std::string& message : ehp)
    sawRtl = sawRtl || message.find("Text runs horizontal, rtl") != std::string::npos;
  EXPECT_TRUE(sawRtl);
}

// ---- The spine's page progression ----

TEST_F(WritingModeLayout, ThePageProgressionSurvivesBookBin) {
  const std::string path = writeBook("", document("<p>Alpha</p>"), {}, " page-progression-direction=\"rtl\"");
  const std::string cache = path + ".cache";
  {
    Epub built(path, cache);
    ASSERT_TRUE(built.load(true));
    EXPECT_EQ(PageProgression::Rtl, built.pageProgression());
  }
  {
    Epub reloaded(path, cache);  // from book.bin this time
    ASSERT_TRUE(reloaded.load(true));
    EXPECT_EQ(PageProgression::Rtl, reloaded.pageProgression());
  }
}

TEST_F(WritingModeLayout, TheDumpNamesADeclaredPageProgressionOnly) {
  EXPECT_NE(std::string::npos,
            dump("", document("<p>Alpha</p>"), {}, {}, " page-progression-direction=\"rtl\"").find(" ppd=rtl\n"));
  EXPECT_EQ(std::string::npos, dump("", document("<p>Alpha</p>")).find("ppd="));
}

}  // namespace
