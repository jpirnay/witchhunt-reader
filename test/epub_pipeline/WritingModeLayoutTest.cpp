// Boxes read along the CSS writing mode, text-align start/end along the direction, still set horizontally.
//
// A book written for vertical text names its margins physically for a vertical page: there, top is inline-start
// (down the column), right is block-start (before the first column), left block-end, and height the inline size.
// Layout reads them logically, so the parser turns each element's box by the writing mode its CSS was written for
// (CssStyle::resolveLogical). The "twin" tests need no arithmetic: a book written vertically must dump exactly like a
// horizontal book whose CSS names the same logical sides physically. The position tests derive their values from the
// host stub's metrics (GfxRenderer.h: ascender 18, the em layout resolves lengths against; line height 24) and block
// margins collapsing to the larger of two (ChapterHtmlSlimParser, the margin-top collapse before a block's first line).
#include <BidiUtils.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/blocks/BlockStyle.h"
#include "LogObserver.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

struct Word {
  int x = 0;
  std::string text;     // its logical text (t=)
  std::string display;  // its WD form, what a right-to-left line draws for it; empty when it has none
};

struct Line {
  int y = 0;
  int x = 0;
  int align = 0;
  int firstWordX = -1;  // the first word's x
  std::string text;     // the line's words joined by single spaces
  std::vector<Word> words;
  std::string order;  // the O record's positions ("1,0"); empty when the line reads in stored order
};

// The dump's text lines, in order (PipelineRunner's "LINE y= x= align=" with its "W ... t=" words, each word's "WD t="
// form and the line's "O order=").
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
      Word& word = lines.back().words.emplace_back();
      std::sscanf(row.c_str(), "   W x=%d", &word.x);
      word.text = row.substr(t + 3);
      if (lines.back().text.empty()) lines.back().firstWordX = word.x;
      if (!lines.back().text.empty()) lines.back().text += ' ';
      lines.back().text += word.text;
    } else if (row.rfind("    WD t=", 0) == 0 && !lines.empty() && !lines.back().words.empty()) {
      lines.back().words.back().display = row.substr(9);
    } else if (row.rfind("   O order=", 0) == 0 && !lines.empty()) {
      lines.back().order = row.substr(11);
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
                        const std::string& spineAttrs = "", const std::string& lang = "en") {
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
            "<dc:identifier id=\"id\">wm</dc:identifier><dc:title>WM</dc:title><dc:language>" +
                lang +
                "</dc:language>"
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
                   const std::vector<SyntheticFile>& extra = {}, const std::string& spineAttrs = "",
                   const std::string& lang = "en") {
    const std::string path = writeBook(css, xhtml, extra, spineAttrs, lang);
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

TEST_F(WritingModeLayout, TextAlignStartStillIndentsALatinParagraph) {
  // Start is kept for the layout now; an unset direction resolves it to Left, which still takes the indent.
  const auto lines = parseLines(dump(".s { text-align: start; text-indent: 1em }\n",
                                     document("<p class=\"s\">Alpha bravo</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(kLeft, lines[0].align);
  EXPECT_EQ(18, lines[0].firstWordX);
}

TEST(BlockStyle, TheChildsDirectionWinsElseTheParents) {
  BlockStyle parent;
  parent.direction = CssDirection::Rtl;
  BlockStyle child;
  EXPECT_EQ(CssDirection::Rtl, parent.getCombinedBlockStyle(child).direction);
  child.direction = CssDirection::Ltr;
  EXPECT_EQ(CssDirection::Ltr, parent.getCombinedBlockStyle(child).direction);
}

// ---- Right to left ----

using BidiLayout = WritingModeLayout;

TEST_F(BidiLayout, ARightToLeftParagraphTakesNoDropCap) {
  const std::string hebrew =
      "לום עולם זהו טקסט ארוך מספיק כדי לשבור את השורה הראשונה לכמה שורות מלאות ועוד מילים כדי שהפסקה "
      "תמשיך עוד ועוד ועוד ועוד ועוד ועוד ועוד ועוד ועוד ועוד ועוד ועוד ועוד ועוד";
  const std::string cap = "<span style=\"float: left; font-size: 3em\">ש</span>";
  const std::string shin = "ש";
  const auto withDir = parseLines(dump("", document("<p dir=\"rtl\">" + cap + hebrew + "</p>")));
  EXPECT_EQ(nullptr, lineWith(withDir, shin));
  const auto inHebrewBook =
      parseLines(dump("", document("<p>" + cap + hebrew + "</p>"), pipeline_harness::Profile{}, {}, "", "he"));
  EXPECT_EQ(nullptr, lineWith(inHebrewBook, shin));
  const auto latin = parseLines(dump(
      "",
      document("<p><span style=\"float: left; font-size: 3em\">A</span>lpha bravo charlie delta echo foxtrot golf "
               "hotel india juliet kilo lima mike november oscar papa quebec romeo sierra tango uniform victor "
               "whiskey xray yankee zulu alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo</p>")));
  EXPECT_NE(nullptr, lineWith(latin, "A"));
}

// The host stub's advances (GfxRenderer.h: 6 + cp % 7 px a character) give שלום 32, עולם 32, "(world)" 66, ".1998" 36,
// He 17 and said 35; a space is 6. The default viewport is 460 px wide.
constexpr const char* kShalom = "שלום";
constexpr const char* kShalomDrawn = "םולש";  // its letters in visual order
constexpr const char* kOlam = "עולם";
constexpr const char* kOlamDrawn = "םלוע";
constexpr const char* kMixedRtl = "<p dir=\"rtl\">שלום (world) 1998.</p>";

void expectWord(const Word& word, const int x, const std::string& text, const std::string& display) {
  EXPECT_EQ(x, word.x) << text;
  EXPECT_EQ(text, word.text);
  EXPECT_EQ(display, word.display) << text;
}

std::string repeated(const char* word, const int count) {
  std::string out;
  for (int i = 0; i < count; ++i) {
    if (i > 0) out += ' ';
    out += word;
  }
  return out;
}

TEST_F(BidiLayout, ARightToLeftLineRunsFromTheRightEdge) {
  const auto lines = parseLines(dump("", document("<p dir=\"rtl\">שלום עולם</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].words.size());
  // Stored left to right: 460 - (32 + 6 + 32) is where the second word in reading order starts.
  expectWord(lines[0].words[0], 390, kOlam, kOlamDrawn);
  expectWord(lines[0].words[1], 428, kShalom, kShalomDrawn);
  EXPECT_EQ("1,0", lines[0].order);
}

TEST_F(BidiLayout, AMixedLineInARightToLeftParagraph) {
  const auto lines = parseLines(dump("", document(kMixedRtl), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].words.size());
  // The period of 1998. is drawn on its left, and the parentheses, mirrored, still read (world).
  expectWord(lines[0].words[0], 314, "1998.", ".1998");
  expectWord(lines[0].words[1], 356, "(world)", "");
  expectWord(lines[0].words[2], 428, kShalom, kShalomDrawn);
  EXPECT_EQ("2,1,0", lines[0].order);
}

TEST_F(BidiLayout, AMixedLineInALeftToRightParagraph) {
  const auto lines = parseLines(dump("", document("<p dir=\"ltr\">He said שלום.</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].words.size());
  // The line is resolved as a whole (decision 5): the period ends the English sentence, right of the Hebrew word.
  expectWord(lines[0].words[0], 0, "He", "");
  expectWord(lines[0].words[1], 23, "said", "");
  expectWord(lines[0].words[2], 64, "שלום.", "םולש.");
  EXPECT_EQ("", lines[0].order);
}

TEST_F(BidiLayout, TheFirstLineIndentGoesOnTheRight) {
  const auto lines =
      parseLines(dump("", document("<p dir=\"rtl\" style=\"text-indent: 2em\">שלום עולם</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].words.size());
  // The line ends 2em (36 px) short of the right edge.
  EXPECT_EQ(354, lines[0].words[0].x);
  EXPECT_EQ(392, lines[0].words[1].x);
}

TEST_F(BidiLayout, AHangingIndentHangsIntoTheRightInset) {
  const auto lines = parseLines(dump(
      "", document("<p dir=\"rtl\" style=\"margin-right: 2em; text-indent: -1em\">שלום עולם</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].words.size());
  // The block is 424 px wide, 460 less its 36 px right margin; its first line hangs 18 px out of it on the right.
  EXPECT_EQ(0, lines[0].x);
  EXPECT_EQ(372, lines[0].words[0].x);
  EXPECT_EQ(410, lines[0].words[1].x);
}

TEST_F(BidiLayout, ARightToLeftParagraphTurnsTheUsersLeftAlignmentRight) {
  pipeline_harness::Profile userLeft;
  userLeft.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  const auto user = parseLines(dump("", document("<p dir=\"rtl\">שלום</p>"), userLeft));
  ASSERT_EQ(1u, user.size());
  EXPECT_EQ(kRight, user[0].align);
  EXPECT_EQ(428, user[0].firstWordX);
  // The book's own text-align: left stays left.
  const auto book =
      parseLines(dump("", document("<p dir=\"rtl\" style=\"text-align: left\">שלום</p>"), bookAlignment()));
  ASSERT_EQ(1u, book.size());
  EXPECT_EQ(kLeft, book[0].align);
  EXPECT_EQ(0, book[0].firstWordX);
}

TEST_F(BidiLayout, AJustifiedRightToLeftLineFillsTheMeasure) {
  // 472 px, so that 12 words (12 × 32 + 11 × 6 = 450 px) leave 22 px, 2 for each gap. At 460 the remainder of the
  // share would stay undistributed at the line's left, as it stays at the right of a left-to-right line.
  pipeline_harness::Profile justified;
  justified.viewportWidth = 472;
  const auto lines = parseLines(dump("", document("<p dir=\"rtl\">" + repeated(kShalom, 20) + "</p>"), justified));
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(12u, lines[0].words.size());
  EXPECT_EQ(0, lines[0].words.front().x);
  EXPECT_EQ(472, lines[0].words.back().x + 32);
  for (size_t i = 1; i < lines[0].words.size(); ++i) {
    EXPECT_EQ(32 + 6 + 2, lines[0].words[i].x - lines[0].words[i - 1].x) << i;
  }
  // The last line is not stretched: it ends at the right edge, 8 × 32 + 7 × 6 = 298 px long.
  ASSERT_EQ(8u, lines[1].words.size());
  EXPECT_EQ(472, lines[1].words.back().x + 32);
  EXPECT_EQ(174, lines[1].words.front().x);
}

TEST_F(BidiLayout, ALineOverTheBidiLimitStillRunsRightToLeft) {
  // 30 words and their 29 spaces are 149 slots, past the line pass's 128: the line takes upstream's per-word order.
  pipeline_harness::Profile wide;
  wide.viewportWidth = 1400;
  const auto lines = parseLines(dump("", document("<p dir=\"rtl\">" + repeated(kShalom, 30) + "</p>"), wide));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(30u, lines[0].words.size());
  // 1400 - (30 × 32 + 29 × 6) = 266.
  std::string order;
  for (int k = 0; k < 30; ++k) {
    expectWord(lines[0].words[k], 266 + 38 * k, kShalom, kShalomDrawn);
    order += (k == 0 ? "" : ",") + std::to_string(29 - k);
  }
  EXPECT_EQ(order, lines[0].order);
}

TEST_F(BidiLayout, ALentArenaBuildLaysOutAsAHeapBuild) {
  // With a lent arena the line pass borrows its workspace from it between a page's lines; at 16 KB the arena may have
  // no room and it takes the heap. Either way the book lays out as a heap build does.
  const std::string corpus = (fs::path(CORPUS_DIR) / "test_rtl_markup.epub").string();
  const std::string mixed = writeBook("", document(kMixedRtl));
  int run = 0;
  for (const std::string& book : {corpus, mixed}) {
    std::string heap;
    for (const size_t lent : {size_t{0}, size_t{49152}, size_t{16384}}) {
      pipeline_harness::Profile profile = bookAlignment();
      profile.lentArenaBytes = lent;
      const fs::path cache = work / ("cache" + std::to_string(run++));
      std::ostringstream out;
      EXPECT_TRUE(pipeline_harness::runAndDump(book, cache.string(), profile, out)) << out.str();
      if (lent == 0) {
        heap = out.str();
        EXPECT_NE(std::string::npos, heap.find(" WD t=")) << heap;
      } else {
        EXPECT_EQ(heap, out.str()) << book << ", lent " << lent;
      }
    }
  }
}

TEST_F(BidiLayout, AParagraphInADrainedBlockDecidesItsDirectionAnew) {
  // <hr> lays the block out and keeps it, empty, and the next paragraph merges into it rather than resetting it.
  const auto lines =
      parseLines(dump("", document("<p dir=\"rtl\">שלום</p><hr/><p dir=\"ltr\">Alpha bravo</p>"), bookAlignment()));
  const Line* latin = lineWith(lines, "Alpha bravo");
  ASSERT_NE(nullptr, latin);
  EXPECT_EQ(0, latin->firstWordX);
  EXPECT_EQ("", latin->order);
}

// What drains a block and keeps it for the next paragraph to merge into: a rule, a table, a table with a caption.
const char* const kDrainingElements[] = {
    "<hr/>",
    "<table><tr><td>x</td></tr></table>",
    "<table><caption>Cap</caption><tr><td>x</td></tr></table>",
};

// The book's alignment, and the reader's Left (which a right-to-left paragraph turns Right): with each, the paragraph
// after the element sets no text-align, so its alignment comes from the block it merges into.
struct AlignmentCase {
  const char* name;
  pipeline_harness::Profile profile;
  int paragraphAlign;  // what a paragraph without text-align is laid out at
};
std::vector<AlignmentCase> alignmentCases() {
  pipeline_harness::Profile userLeft;
  userLeft.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Left);
  return {{"book alignment", bookAlignment(), static_cast<int>(CssTextAlign::Justify)},
          {"reader's Left", userLeft, kLeft}};
}

TEST_F(BidiLayout, AParagraphAfterARuleTakesItsContainersDirection) {
  // An English book: the paragraph after the element sets no direction, and neither does anything around it.
  for (const AlignmentCase& alignment : alignmentCases()) {
    for (const char* element : kDrainingElements) {
      for (const std::string& before :
           {std::string("<p dir=\"rtl\">א</p>"), std::string("<div dir=\"rtl\"><p>א</p>")}) {
        const std::string close = before.rfind("<div", 0) == 0 ? "</div>" : "";
        const auto lines =
            parseLines(dump("", document(before + element + close + "<p>Alpha bravo</p>"), alignment.profile));
        const Line* latin = lineWith(lines, "Alpha bravo");
        ASSERT_NE(nullptr, latin) << element;
        EXPECT_EQ(0, latin->firstWordX) << alignment.name << ": " << before << element << close;
        EXPECT_EQ(alignment.paragraphAlign, latin->align) << alignment.name << ": " << before << element << close;
      }
    }
  }
}

TEST_F(BidiLayout, AParagraphAfterARuleInARightToLeftContainerStaysRightToLeft) {
  for (const AlignmentCase& alignment : alignmentCases()) {
    for (const char* element : kDrainingElements) {
      const auto lines = parseLines(
          dump("", document(std::string("<div dir=\"rtl\"><p>א</p>") + element + "<p>ב</p></div>"), alignment.profile));
      const Line* second = lineWith(lines, "ב");
      ASSERT_NE(nullptr, second) << element;
      EXPECT_EQ(460 - 11, second->firstWordX) << alignment.name << ": " << element;  // ב is 11 px, at the right edge
      // The reader's Left turns Right again, in this paragraph's own right-to-left layout.
      EXPECT_EQ(alignment.paragraphAlign == kLeft ? kRight : alignment.paragraphAlign, second->align)
          << alignment.name << ": " << element;
    }
  }
}

TEST_F(BidiLayout, TheBidiWorkspaceIsGoneWhenTheBuildEnds) {
  // One held before the build goes too: the build ends by releasing it (deviation 1).
  std::string visual;
  ASSERT_TRUE(BidiUtils::applyBidiVisual("שלום", visual, 1));
  ASSERT_TRUE(BidiUtils::heapWorkspaceHeld());
  const std::string out = dump("", document(kMixedRtl), bookAlignment());
  EXPECT_NE(std::string::npos, out.find(" WD t=")) << out;
  EXPECT_FALSE(BidiUtils::heapWorkspaceHeld());
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
