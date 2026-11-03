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

#include <algorithm>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/BlockStyle.h"
#include "GfxRenderer.h"
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

TEST_F(WritingModeLayout, DirAutoTakesTheFirstStrongLetter) {
  // Auto is decided from the text (UAX #9 P2/P3): the first letter that has a direction gives the paragraph its own.
  const auto hebrew = parseLines(dump(kStartEndCss, document("<p class=\"s\" dir=\"auto\">שלום</p>"), bookAlignment()));
  ASSERT_EQ(1u, hebrew.size());
  EXPECT_EQ(kRight, hebrew[0].align);
  const auto latinFirst =
      parseLines(dump(kStartEndCss, document("<p class=\"s\" dir=\"auto\">abc שלום</p>"), bookAlignment()));
  ASSERT_EQ(1u, latinFirst.size());
  EXPECT_EQ(kLeft, latinFirst[0].align);
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

TEST_F(BidiLayout, ACentredRightToLeftLineSitsInTheMiddle) {
  const auto lines =
      parseLines(dump("", document("<p dir=\"rtl\" style=\"text-align: center\">שלום עולם</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(static_cast<int>(CssTextAlign::Center), lines[0].align);
  ASSERT_EQ(2u, lines[0].words.size());
  // (460 - 70) / 2, in visual order: the second word in reading order is on the left.
  expectWord(lines[0].words[0], 195, kOlam, kOlamDrawn);
  expectWord(lines[0].words[1], 233, kShalom, kShalomDrawn);
  EXPECT_EQ("1,0", lines[0].order);
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

TEST_F(BidiLayout, AJustifiedBidiLineDoesNotStretchTheGapBeforeClosingPunctuation) {
  // Two left-to-right paragraphs alike but for their first word: the Hebrew one is laid out by the line pass, the Latin
  // one by the ordinary path. Justified, neither stretches the space before a closing mark (here a spaced em dash).
  const std::string tail =
      " alpha \xE2\x80\x94 bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november oscar papa";
  const GfxRenderer renderer;
  for (const std::string& first : {std::string(kShalom), std::string("Shalom")}) {
    const auto lines = parseLines(dump("", document("<p dir=\"ltr\">" + first + tail + "</p>")));
    ASSERT_GE(lines.size(), 2u) << first;
    const std::vector<Word>& words = lines[0].words;
    ASSERT_GE(words.size(), 4u) << first;
    ASSERT_EQ("\xE2\x80\x94", words[2].text) << first;
    const auto end = [&](const size_t i) {
      return words[i].x + renderer.getTextAdvanceX(1, words[i].text.c_str(), EpdFontFamily::REGULAR);
    };
    EXPECT_GT(words[3].x - end(2), 6) << first << ": the line is stretched";
    EXPECT_EQ(6, words[2].x - end(1)) << first << ": the space before the dash keeps its width";
  }
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

// Whether a heap workspace was held at any measurement since it was last cleared (GfxRenderer::onTextAdvance).
bool gHeapWorkspaceSeen = false;
void noteHeapWorkspace() { gHeapWorkspaceSeen = gHeapWorkspaceSeen || BidiUtils::heapWorkspaceHeld(); }

TEST_F(BidiLayout, ALentArenaBuildLaysOutAsAHeapBuild) {
  // With a lent arena the line pass borrows its workspace from it between a page's lines; at 16 KB the arena has no
  // room and it takes the heap. Either way the book lays out as a heap build does. Which one ran shows in whether a
  // heap workspace is held while the build measures: never at 48 KB, as in every heap build at 16 KB and without one.
  const std::string corpus = (fs::path(CORPUS_DIR) / "test_rtl_markup.epub").string();
  const std::string mixed = writeBook("", document(kMixedRtl));
  GfxRenderer::onTextAdvance = noteHeapWorkspace;
  int run = 0;
  for (const std::string& book : {corpus, mixed}) {
    std::string heap;
    for (const size_t lent : {size_t{0}, size_t{49152}, size_t{16384}}) {
      pipeline_harness::Profile profile = bookAlignment();
      profile.lentArenaBytes = lent;
      const fs::path cache = work / ("cache" + std::to_string(run++));
      std::ostringstream out;
      BidiUtils::releaseWorkspace();
      gHeapWorkspaceSeen = false;
      EXPECT_TRUE(pipeline_harness::runAndDump(book, cache.string(), profile, out)) << out.str();
      EXPECT_EQ(lent != 49152, gHeapWorkspaceSeen) << book << ", lent " << lent;
      if (lent == 0) {
        heap = out.str();
        EXPECT_NE(std::string::npos, heap.find(" WD t=")) << heap;
      } else {
        EXPECT_EQ(heap, out.str()) << book << ", lent " << lent;
      }
    }
  }
  GfxRenderer::onTextAdvance = nullptr;
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

TEST_F(BidiLayout, AParagraphAfterABackgroundPictureTakesItsContainersDirection) {
  // The picture is placed once its element closes, after the element's text: that drains the block and keeps it.
  const std::vector<SyntheticFile> picture = {{"pic.png", "image/png", syntheticPngHeader(200, 150)}};
  for (const AlignmentCase& alignment : alignmentCases()) {
    const std::string out =
        dump("", document("<p dir=\"rtl\" style=\"background: url(pic.png) no-repeat\">א</p><p>Alpha bravo</p>"),
             alignment.profile, picture);
    EXPECT_NE(std::string::npos, out.find("  IMG ")) << out;
    const auto lines = parseLines(out);
    const Line* latin = lineWith(lines, "Alpha bravo");
    ASSERT_NE(nullptr, latin) << alignment.name;
    EXPECT_EQ(0, latin->firstWordX) << alignment.name;
    EXPECT_EQ(alignment.paragraphAlign, latin->align) << alignment.name;
  }
}

TEST_F(BidiLayout, ABlockOfOnlyControlsLeavesNoDirectionBehind) {
  // The LRI and PDI paragraph needs no bidi, so its controls are dropped and nothing is left to lay out; <hr> drains
  // the block and keeps it, and the right-to-left paragraph merges into it.
  const auto lines = parseLines(
      dump("", document("<p>Alpha</p><p>&#x2066;&#x2069;</p><hr/><p dir=\"rtl\">שלום עולם</p>"), bookAlignment()));
  const Line* hebrew = lineWith(lines, "עולם שלום");
  ASSERT_NE(nullptr, hebrew);
  ASSERT_EQ(2u, hebrew->words.size());
  EXPECT_EQ(390, hebrew->words[0].x);
  EXPECT_EQ(428, hebrew->words[1].x);
}

int gBidiErrors = 0;
void countBidiErrors(const char* level, const char* origin, const char* /*message*/) {
  if (std::string(origin) == "BIDI" && std::string(level) == "ERR") ++gBidiErrors;
}

TEST(BidiFallback, ALineThePassCannotRunOnIsLaidOutWithoutItsControls) {
  // No workspace (1.5 KB) and no scratch (5.9 KB) can be had: the line is laid out without bidi, logged, and as a pass
  // that needs no bidi lays it out, its RLE and PDF dropped from the text, the drawn form and the widths.
  BlockStyle style;
  style.direction = CssDirection::Rtl;
  style.textIndent = 0;
  style.textIndentDefined = true;
  ParsedText text(false, false, style);
  text.addWord("\xE2\x80\xAB\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\xE2\x80\xAC", EpdFontFamily::REGULAR);  // RLE שלום PDF
  text.addWord(kOlam, EpdFontFamily::REGULAR);
  const GfxRenderer renderer;
  std::vector<std::unique_ptr<TextBlock>> lines;
  BidiUtils::releaseWorkspace();
  gBidiErrors = 0;
  setLogObserver(countBidiErrors);
  {
    const AllocationRefusal refusal(1024);
    text.layoutAndExtractLines(renderer, 1, 460, [&](std::unique_ptr<TextBlock> line, bool, bool) {
      lines.push_back(std::move(line));
      return ParsedText::LineProcessResult::Accepted;
    });
  }
  setLogObserver(nullptr);
  EXPECT_GT(gBidiErrors, 0);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0]->wordCount());
  EXPECT_STREQ(kShalom, lines[0]->wordText(0));
  EXPECT_STREQ(kShalom, lines[0]->wordDrawText(0));
  EXPECT_EQ(0, lines[0]->wordXpos(0));
  EXPECT_EQ(32 + 6, lines[0]->wordXpos(1));  // שלום measured without its controls
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

// ---- The direction of a paragraph without dir (decision 6) ----

TEST_F(BidiLayout, AHebrewBooksParagraphRunsRightToLeftWhenItHasAHebrewLetter) {
  const auto lines = parseLines(dump("", document("<p>Wiki שלום</p>"), bookAlignment(), {}, "", "he"));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].words.size());
  // Wiki (29 px) reads first, at the right edge: 460 - 29 = 431, and שלום a space to its left.
  expectWord(lines[0].words[0], 393, kShalom, kShalomDrawn);
  expectWord(lines[0].words[1], 431, "Wiki", "");
  EXPECT_EQ("1,0", lines[0].order);
}

TEST_F(BidiLayout, AHebrewBooksParagraphWithoutAHebrewLetterRunsLeftToRight) {
  const auto lines = parseLines(dump("", document("<p>Only English, 2024.</p>"), bookAlignment(), {}, "", "he"));
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(0, lines[0].firstWordX);
  for (const Word& word : lines[0].words) EXPECT_EQ("", word.display) << word.text;
  EXPECT_EQ("", lines[0].order);
}

TEST_F(BidiLayout, AnEnglishBookQuotingHebrewStaysLeftToRight) {
  const auto lines = parseLines(dump("", document("<p>He said שלום.</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].words.size());
  expectWord(lines[0].words[0], 0, "He", "");
  expectWord(lines[0].words[1], 23, "said", "");
  expectWord(lines[0].words[2], 64, "שלום.", "םולש.");
  EXPECT_EQ("", lines[0].order);
}

TEST_F(BidiLayout, AHebrewOnlyParagraphInAnEnglishBookTurnsRightToLeft) {
  const auto lines = parseLines(dump("", document("<p>שלום עולם</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].words.size());
  expectWord(lines[0].words[0], 390, kOlam, kOlamDrawn);
  expectWord(lines[0].words[1], 428, kShalom, kShalomDrawn);
  EXPECT_EQ("1,0", lines[0].order);
}

TEST_F(BidiLayout, ALetterlessParagraphTakesTheBooksDirection) {
  // Each * is 6 px, as is each space: the line is 30 px long.
  const auto hebrew = parseLines(dump("", document("<p>* * *</p>"), bookAlignment(), {}, "", "he"));
  ASSERT_EQ(1u, hebrew.size());
  ASSERT_EQ(3u, hebrew[0].words.size());
  EXPECT_EQ(430, hebrew[0].words[0].x);
  EXPECT_EQ(442, hebrew[0].words[1].x);
  EXPECT_EQ(454, hebrew[0].words[2].x);
  EXPECT_EQ("2,1,0", hebrew[0].order);
  const auto english = parseLines(dump("", document("<p>* * *</p>"), bookAlignment(), {}, "", "en"));
  ASSERT_EQ(1u, english.size());
  ASSERT_EQ(3u, english[0].words.size());
  EXPECT_EQ(0, english[0].words[0].x);
  EXPECT_EQ(12, english[0].words[1].x);
  EXPECT_EQ(24, english[0].words[2].x);
  EXPECT_EQ("", english[0].order);
}

TEST_F(BidiLayout, StartAndEndFollowTheDecidedDirection) {
  const auto lines = parseLines(
      dump(kStartEndCss, document("<p class=\"s\">שלום</p><p class=\"e\">שלום</p><p class=\"s\">Only English</p>"),
           bookAlignment(), {}, "", "he"));
  ASSERT_EQ(3u, lines.size());
  EXPECT_EQ(kRight, lines[0].align);
  EXPECT_EQ(kLeft, lines[1].align);
  EXPECT_EQ(kLeft, lines[2].align);
}

TEST_F(BidiLayout, PlaintextOnABlockTakesTheFirstStrongLetter) {
  const auto lines = parseLines(dump(std::string(kStartEndCss) + ".pt { unicode-bidi: plaintext }\n",
                                     document("<p class=\"pt s\">שלום</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(kRight, lines[0].align);
}

TEST_F(BidiLayout, TheDirectionHoldsAcrossTheLongBlockSplit) {
  // 121 tokens, past the 96-token flush: the passes after the first hold Hebrew words only, and keep its direction.
  const std::string body = "<p>Wiki " + repeated(kShalom, 120) + "</p>";
  const GfxRenderer renderer;
  const auto width = [&](const Word& word) {
    const std::string& drawn = word.display.empty() ? word.text : word.display;
    return renderer.getTextAdvanceX(1, drawn.c_str(), EpdFontFamily::REGULAR);
  };
  for (const std::string& lang : {std::string("he"), std::string("en")}) {
    const auto lines = parseLines(dump("", document(body), pipeline_harness::Profile{}, {}, "", lang));
    ASSERT_GT(lines.size(), 8u) << lang;
    for (size_t i = 0; i < lines.size(); ++i) {
      ASSERT_FALSE(lines[i].words.empty()) << lang << " line " << i;
      int left = std::numeric_limits<int>::max();
      int right = std::numeric_limits<int>::min();
      for (const Word& word : lines[i].words) {
        left = std::min(left, word.x);
        right = std::max(right, word.x + width(word));
      }
      if (lang == "he") {
        EXPECT_EQ(460, right) << lang << " line " << i;
      } else {
        EXPECT_EQ(0, left) << lang << " line " << i;
      }
    }
  }
}

// ---- Literal controls in the text ----

constexpr const char* kLrm = "\xE2\x80\x8E";
constexpr const char* kRlm = "\xE2\x80\x8F";

TEST_F(BidiLayout, AnLrmInAnEnglishParagraphIsDroppedAsBefore) {
  // An LRM is a left-to-right letter's class: the paragraph needs no bidi, and drops it.
  const std::string out = dump("", document("<p>abc&#x200E; def</p>"));
  const auto lines = parseLines(out);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].words.size());
  expectWord(lines[0].words[0], 0, "abc", "");
  expectWord(lines[0].words[1], 31, "def", "");
  EXPECT_EQ(dump("", document("<p>abc def</p>")), out);
}

TEST_F(BidiLayout, AnRlmInAnEnglishParagraphIsLaidOutByUax9) {
  // An RLM is a right-to-left letter's class: the paragraph needs bidi, and UAX #9 lays it out, the mark drawn as
  // nothing. After "see" (27 px) it makes the number and the space before it a right-to-left run, so 12 sits against
  // "see" and the space follows it. Without a number to turn, the line keeps its places, each mark in a drawn form.
  const auto number = parseLines(dump("", document("<p>see&#x200F; 12 items here</p>")));
  ASSERT_EQ(1u, number.size());
  ASSERT_EQ(4u, number[0].words.size());
  expectWord(number[0].words[0], 0, std::string("see") + kRlm, "see");
  expectWord(number[0].words[1], 27, "12", "");
  expectWord(number[0].words[2], 46, "items", "");
  expectWord(number[0].words[3], 96, "here", "");
  EXPECT_EQ("", number[0].order);
  const auto words = parseLines(dump("", document("<p>abc&#x200F; def</p>")));
  ASSERT_EQ(1u, words.size());
  ASSERT_EQ(2u, words[0].words.size());
  expectWord(words[0].words[0], 0, std::string("abc") + kRlm, "abc");
  expectWord(words[0].words[1], 31, "def", "");
  EXPECT_EQ("", words[0].order);
}

TEST_F(BidiLayout, AWordOfMarksInAnEnglishParagraphChangesNothingAroundIt) {
  // A word of marks is held for the next word. It glues that word to nothing, waits out a no-break space, leaves a drop
  // cap its place, and ends with its table cell or where a table or a picture empties its block: each paragraph lays
  // out byte for byte as without the mark.
  const std::vector<SyntheticFile> picture = {{"pic.png", "image/png", syntheticPngHeader(200, 150)}};
  const std::string capText =
      "lpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november oscar papa quebec romeo "
      "sierra tango uniform victor whiskey xray yankee zulu alpha bravo charlie delta echo foxtrot golf hotel india";
  const std::string cap = "<span style=\"float: left; font-size: 3em\">A</span>";
  const std::pair<std::string, std::string> cases[] = {
      {"<p>abc &#x200E;<i>def</i></p>", "<p>abc <i>def</i></p>"},
      {"<p>abc <b>&#x200E;</b>def</p>", "<p>abc <b></b>def</p>"},
      {"<p>abc &#x200E;&#xA0;def</p>", "<p>abc &#xA0;def</p>"},
      {"<p>&#x200E;" + cap + capText + "</p>", "<p>" + cap + capText + "</p>"},
      {"<table><tr><td>abc &#x200F;</td><td>def</td></tr></table>",
       "<table><tr><td>abc </td><td>def</td></tr></table>"},
      {"<div>abc &#x200F;<table><tr><td>def</td></tr></table></div>",
       "<div>abc <table><tr><td>def</td></tr></table></div>"},
      {"<div>&#x200F;<img src=\"pic.png\" alt=\"\"/>def</div>", "<div><img src=\"pic.png\" alt=\"\"/>def</div>"},
  };
  const pipeline_harness::Profile profile;
  for (const auto& [marked, plain] : cases) {
    EXPECT_EQ(dump("", document(plain), profile, picture), dump("", document(marked), profile, picture)) << marked;
  }
}

TEST_F(BidiLayout, AMarkAtASourceLineBreakInCjkTextLeavesItJoined) {
  // A line break between unspaced CJK in the source vanishes. The mark before or after it is no character the text on
  // either side ends or starts with.
  const pipeline_harness::Profile profile;
  const std::string joined = dump("", document("<p>日本\n語</p>"), profile, {}, "", "ja");
  for (const char* marked : {"<p>日本&#x200E;\n語</p>", "<p>日本\n&#x200E;語</p>"}) {
    EXPECT_EQ(joined, dump("", document(marked), profile, {}, "", "ja")) << marked;
  }
}

TEST_F(BidiLayout, AMarkAfterADashKeepsTheBreakAfterIt) {
  // The dash still ends the text before <i>: the word after it is not glued on, and the line may break there.
  EXPECT_EQ(dump("", document("<p>foo—<i>bar</i></p>")), dump("", document("<p>foo—&#x200E;<i>bar</i></p>")));
}

TEST_F(BidiLayout, AMarkBetweenLatinAndCjkKeepsTheirGap) {
  // Glued across a tag, Latin and CJK meet at a breakable gap; a mark on either side of the tag changes nothing.
  const pipeline_harness::Profile profile;
  const std::pair<const char*, const char*> cases[] = {
      {"<p>abc<b>&#x200E;日本</b></p>", "<p>abc<b>日本</b></p>"},
      {"<p><b>日本&#x200E;</b>abc</p>", "<p><b>日本</b>abc</p>"},
  };
  for (const auto& [marked, plain] : cases) {
    EXPECT_EQ(dump("", document(plain), profile, {}, "", "ja"), dump("", document(marked), profile, {}, "", "ja"))
        << marked;
  }
}

TEST_F(BidiLayout, AStrayByteAfterAMarkIsReadAsItsReplacement) {
  // The parser takes any byte: a stray continuation byte after an LRM makes the token E2 80 8E 80, which a backward
  // scan for the token's last character reads as one sequence. The word glued on after it is set as after the
  // replacement character a well-formed book would hold there.
  const auto stray = parseLines(dump("", document("<p>&#x200E;\x80<i>x</i></p>")));
  const auto twin = parseLines(dump("", document("<p>&#x200E;&#xFFFD;<i>x</i></p>")));
  ASSERT_EQ(1u, stray.size());
  ASSERT_EQ(1u, twin.size());
  ASSERT_EQ(2u, stray[0].words.size());
  ASSERT_EQ(twin[0].words.size(), stray[0].words.size());
  for (size_t i = 0; i < twin[0].words.size(); ++i) EXPECT_EQ(twin[0].words[i].x, stray[0].words[i].x) << i;
  EXPECT_EQ("x", stray[0].words[1].text);
}

TEST_F(BidiLayout, AMarkInAHebrewParagraphIsKeptAndNotDrawn) {
  // C++ is 24 px: the LRM keeps its pluses on its right, and draws as nothing.
  const auto lines = parseLines(dump("", document("<p dir=\"rtl\">C++&#x200E; שלום</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].words.size());
  expectWord(lines[0].words[0], 398, kShalom, kShalomDrawn);
  expectWord(lines[0].words[1], 436, std::string("C++") + kLrm, "C++");
}

TEST_F(BidiLayout, AWordOfMarksJoinsTheNextWord) {
  const auto lines = parseLines(dump("", document("<p dir=\"rtl\">שלום &#x200F; עולם</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].words.size());
  EXPECT_EQ(std::string(kRlm) + kOlam, lines[0].words[0].text);
  EXPECT_EQ(kOlamDrawn, lines[0].words[0].display);
  EXPECT_EQ(kShalom, lines[0].words[1].text);
}

TEST_F(BidiLayout, AMarkEndingAParagraphLeavesTheNextOneClean) {
  const auto lines =
      parseLines(dump("", document("<p dir=\"rtl\">שלום &#x200F;</p><p dir=\"rtl\">עולם</p>"), bookAlignment()));
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(1u, lines[0].words.size());
  EXPECT_EQ(kShalom, lines[0].words[0].text);
  ASSERT_EQ(1u, lines[1].words.size());
  EXPECT_EQ(kOlam, lines[1].words[0].text);
}

// ---- Bidi markup in the text (deviation 5) ----

// dir, <bdo>, <bdi> and unicode-bidi become the controls CSS Writing Modes 3 (2.4.2) gives them, in the text at the
// element's edges. The host stub's widths: מילה 37, Latin 51, 123 21, סוף 22, abc 25, fed 27, ghi 29, "םולש:" 40, 3 8,
// posts 46.
const std::string kRle = "\xE2\x80\xAB";
const std::string kPdf = "\xE2\x80\xAC";
const std::string kRlo = "\xE2\x80\xAE";
const std::string kLri = "\xE2\x81\xA6";
const std::string kRli = "\xE2\x81\xA7";
const std::string kFsi = "\xE2\x81\xA8";
const std::string kPdi = "\xE2\x81\xA9";
constexpr const char* kEmbedCss =
    ".o { unicode-bidi: bidi-override; direction: rtl } .e { unicode-bidi: embed; direction: rtl }\n";

// How many times `needle` occurs in `text`.
size_t occurrences(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

TEST_F(BidiLayout, AnInlineDirSpanBecomesAnIsolate) {
  const auto lines =
      parseLines(dump("", document("<p dir=\"rtl\">מילה <span dir=\"ltr\">Latin 123</span> סוף</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(4u, lines[0].words.size());
  expectWord(lines[0].words[0], 311, "סוף", "ףוס");
  expectWord(lines[0].words[1], 339, kLri + "Latin", "Latin");
  expectWord(lines[0].words[2], 396, "123" + kPdi, "123");
  expectWord(lines[0].words[3], 423, "מילה", "הלימ");
  EXPECT_EQ("3,1,2,0", lines[0].order);
}

TEST_F(BidiLayout, AnIsolateKeepsANumberAfterItOutside) {
  // Without the isolate, 123 would join abc's left-to-right run; isolated, abc is a neutral and the number reads after
  // it, on its left.
  const auto lines =
      parseLines(dump("", document("<p dir=\"rtl\">שלום <span dir=\"ltr\">abc</span> 123</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].words.size());
  expectWord(lines[0].words[0], 370, "123", "");
  expectWord(lines[0].words[1], 397, kLri + "abc" + kPdi, "abc");
  expectWord(lines[0].words[2], 428, kShalom, kShalomDrawn);
  EXPECT_EQ("2,1,0", lines[0].order);
}

TEST_F(BidiLayout, ABdoOverridesLatinText) {
  const auto lines =
      parseLines(dump("", document("<p dir=\"ltr\">abc <bdo dir=\"rtl\">def</bdo> ghi</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].words.size());
  expectWord(lines[0].words[0], 0, "abc", "");
  expectWord(lines[0].words[1], 31, kFsi + kRlo + "def" + kPdf + kPdi, "fed");
  expectWord(lines[0].words[2], 64, "ghi", "");
}

TEST_F(BidiLayout, ABdiKeepsAUserNameApart) {
  const auto lines = parseLines(dump("", document("<p dir=\"ltr\"><bdi>שלום</bdi>: 3 posts</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].words.size());
  expectWord(lines[0].words[0], 0, kFsi + kShalom + kPdi + ":", "םולש:");
  expectWord(lines[0].words[1], 46, "3", "");
  expectWord(lines[0].words[2], 60, "posts", "");
  EXPECT_EQ("", lines[0].order);
}

TEST_F(BidiLayout, UnicodeBidiInCss) {
  const auto lines = parseLines(dump(
      kEmbedCss, document("<p><span class=\"o\">def</span></p><p><span class=\"e\">def</span></p>"), bookAlignment()));
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(1u, lines[0].words.size());
  EXPECT_EQ(kRlo + "def" + kPdf, lines[0].words[0].text);
  EXPECT_EQ("fed", lines[0].words[0].display);
  ASSERT_EQ(1u, lines[1].words.size());
  EXPECT_EQ(kRle + "def" + kPdf, lines[1].words[0].text);
  EXPECT_EQ("def", lines[1].words[0].display);
}

TEST_F(BidiLayout, AnEmbeddingLetsItsNeighboursIn) {
  // Deviation 12: the PDF takes the level of the Hebrew before it, so the colon joins the right-to-left run and the
  // number reads before it. ABdiKeepsAUserNameApart is the isolate's contrast.
  const auto lines = parseLines(
      dump(kEmbedCss, document("<p dir=\"ltr\"><span class=\"e\">שלום</span>: 3 posts</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].words.size());
  expectWord(lines[0].words[0], 0, "3", "");
  expectWord(lines[0].words[1], 14, kRle + kShalom + kPdf + ":", ":םולש");
  expectWord(lines[0].words[2], 60, "posts", "");
  EXPECT_EQ("1,0,2", lines[0].order);
}

TEST_F(BidiLayout, AnLtrSpanInAnEnglishBookChangesNothing) {
  // An LRI is no right-to-left class: the paragraph needs no bidi and drops the isolate.
  const std::string out = dump("", document("<p>abc <span dir=\"ltr\">def</span> ghi</p>"));
  const auto lines = parseLines(out);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].words.size());
  expectWord(lines[0].words[0], 0, "abc", "");
  expectWord(lines[0].words[1], 31, "def", "");
  expectWord(lines[0].words[2], 64, "ghi", "");
  EXPECT_EQ(dump("", document("<p>abc <span>def</span> ghi</p>")), out);
}

TEST_F(BidiLayout, AnLtrSpanLeavesTheBreaksAtItsEdgesAsTheyWere) {
  // The parser's reads of a word's edges look past the controls it inserted: a dash before a tag still allows a break
  // after it, and a source line break between CJK characters still vanishes, as without the dir.
  const pipeline_harness::Profile profile;
  const std::pair<const char*, const char*> latin[] = {
      {"<p><span dir=\"ltr\">foo\xE2\x80\x94</span><i>bar</i></p>", "<p><span>foo\xE2\x80\x94</span><i>bar</i></p>"},
      {"<p><i dir=\"ltr\">foo\xE2\x80\x94</i>bar</p>", "<p><i>foo\xE2\x80\x94</i>bar</p>"},
  };
  for (const auto& [marked, plain] : latin) {
    EXPECT_EQ(dump("", document(plain), profile), dump("", document(marked), profile)) << marked;
  }
  const std::pair<const char*, const char*> cjk[] = {
      {"<p><span dir=\"ltr\">日本</span>\n語</p>", "<p><span>日本</span>\n語</p>"},
      {"<p>日本\n<span dir=\"ltr\">語</span></p>", "<p>日本\n<span>語</span></p>"},
  };
  for (const auto& [marked, plain] : cjk) {
    EXPECT_EQ(dump("", document(plain), profile, {}, "", "ja"), dump("", document(marked), profile, {}, "", "ja"))
        << marked;
  }
}

TEST_F(BidiLayout, AnIsolateAcrossALineBreakKeepsItsOrder) {
  // 210 px: the first line ends after "three", and the second holds the span's last four words and עולם. The isolate
  // opened on the first line runs on: its words read left to right, and the Hebrew after it reads on their left.
  pipeline_harness::Profile narrow = bookAlignment();
  narrow.viewportWidth = 210;
  const char* const span[] = {"one", "two", "three", "four", "five", "six", "seven"};
  const std::string out = dump(
      "", document("<p dir=\"rtl\">שלום <span dir=\"ltr\">one two three four five six seven</span> עולם</p>"), narrow);
  const auto lines = parseLines(out);
  ASSERT_GE(lines.size(), 2u) << out;
  const auto drawn = [](const Word& word) { return word.display.empty() ? word.text : word.display; };
  const Line* last = nullptr;
  for (const Line& line : lines) {
    for (const Word& word : line.words) {
      if (word.text == kOlam) last = &line;
    }
  }
  ASSERT_NE(nullptr, last);
  int olamX = -1;
  std::vector<std::pair<int, int>> spanWords;  // source index, x
  for (const Word& word : last->words) {
    if (word.text == kOlam) olamX = word.x;
    for (int i = 0; i < 7; ++i) {
      if (drawn(word) == span[i]) spanWords.emplace_back(i, word.x);
    }
  }
  ASSERT_GE(spanWords.size(), 2u) << out;
  for (size_t i = 1; i < spanWords.size(); ++i) {
    EXPECT_GT(spanWords[i].first, spanWords[i - 1].first) << "visual order " << i;
  }
  EXPECT_LT(olamX, spanWords.front().second);
}

TEST_F(BidiLayout, DirAutoSkipsAnIsolatedName) {
  // UAX #9 P2: the first letter with a direction outside every isolate decides an auto paragraph, not a name in a bdi.
  const auto ltr = parseLines(dump("", document("<p dir=\"auto\"><bdi>שלום</bdi> abc</p>"), bookAlignment()));
  ASSERT_EQ(1u, ltr.size());
  ASSERT_EQ(2u, ltr[0].words.size());
  expectWord(ltr[0].words[0], 0, kFsi + kShalom + kPdi, kShalomDrawn);
  expectWord(ltr[0].words[1], 38, "abc", "");
  EXPECT_EQ("", ltr[0].order);
  const auto rtl = parseLines(dump("", document("<p dir=\"auto\"><bdi>abc</bdi> שלום</p>"), bookAlignment()));
  ASSERT_EQ(1u, rtl.size());
  ASSERT_EQ(2u, rtl[0].words.size());
  expectWord(rtl[0].words[0], 397, kShalom, kShalomDrawn);
  expectWord(rtl[0].words[1], 435, kFsi + "abc" + kPdi, "abc");
  EXPECT_EQ("1,0", rtl[0].order);
}

TEST_F(BidiLayout, ABlockTakesOnlyTheOverride) {
  // A block's isolate is its paragraph's direction (no control); its override is an RLO over its text.
  const auto lines = parseLines(
      dump("", document("<p dir=\"rtl\" style=\"unicode-bidi: bidi-override\">abc def</p>"), bookAlignment()));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0].words.size());
  expectWord(lines[0].words[0], 402, "def" + kPdf, "fed");
  expectWord(lines[0].words[1], 435, kRlo + "abc", "cba");
  EXPECT_EQ("1,0", lines[0].order);
}

TEST_F(BidiLayout, AHiddenElementInsertsNothing) {
  // Hidden, or with its text transparent, an element lays out no text, and leaves no control for the next word.
  const pipeline_harness::Profile profile = bookAlignment();
  const std::pair<const char*, const char*> cases[] = {
      {"<p dir=\"rtl\">שלום <span dir=\"ltr\" hidden=\"\">abc</span> עולם</p>",
       "<p dir=\"rtl\">שלום <span hidden=\"\">abc</span> עולם</p>"},
      {"<p dir=\"rtl\">שלום <span style=\"color: transparent\"><span dir=\"ltr\">abc</span></span> עולם</p>",
       "<p dir=\"rtl\">שלום <span style=\"color: transparent\"><span>abc</span></span> עולם</p>"},
  };
  for (const auto& [marked, plain] : cases) {
    EXPECT_EQ(dump("", document(plain), profile), dump("", document(marked), profile)) << marked;
  }
}

TEST_F(BidiLayout, AnElementPastTheHeldControlsCapInsertsNothing) {
  // Four nested bdo open 24 bytes of controls at the word's start, as many as are held for one word: the fifth inserts
  // neither its opener nor its closer, so the word stays balanced.
  const std::string bdo = "<bdo dir=\"rtl\">";
  std::string body = "<p>x ";
  for (int i = 0; i < 5; ++i) body += bdo;
  body += "abc";
  for (int i = 0; i < 5; ++i) body += "</bdo>";
  body += " y</p>";
  const auto lines = parseLines(dump("", document(body)));
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0].words.size());
  std::string expected;
  for (int i = 0; i < 4; ++i) expected += kFsi + kRlo;
  expected += "abc";
  for (int i = 0; i < 4; ++i) expected += kPdf + kPdi;
  EXPECT_EQ(expected, lines[0].words[1].text);
  EXPECT_EQ("cba", lines[0].words[1].display);
}

TEST_F(BidiLayout, ACloserGoesInPastTheHeldControlsCap) {
  // The RLI and seven RLMs fill the held run (an eighth is cut), and the span's PDI still closes it, across a no-break
  // space too: a closer is never dropped.
  std::string marks;
  for (int i = 0; i < 8; ++i) marks += "&#x200F;";
  std::string held = kRli;
  for (int i = 0; i < 7; ++i) held += kRlm;
  held += kPdi;
  for (const char* between : {"", "&#xA0;"}) {
    const auto lines =
        parseLines(dump("", document("<p>x <span dir=\"rtl\">" + marks + "</span>" + between + "abc y</p>")));
    ASSERT_EQ(1u, lines.size()) << between;
    const Word* abc = nullptr;
    for (const Word& word : lines[0].words) {
      if (word.text.size() > 3 && word.text.compare(word.text.size() - 3, 3, "abc") == 0) abc = &word;
    }
    ASSERT_NE(nullptr, abc) << between;
    EXPECT_EQ(held + "abc", abc->text) << between;
  }
}

TEST_F(BidiLayout, AParagraphEndSpendsTheOpenMarkup) {
  // An isolate open where its paragraph ends closes with it: the paragraph after a line break or a block inside the
  // span gets no PDI for an isolate it does not hold.
  const std::pair<const char*, size_t> cases[] = {
      {"<p dir=\"rtl\">שלום <span dir=\"ltr\">abc<br/>def</span> עולם</p>", 2},
      {"<div><span dir=\"rtl\">abc<p>def</p>ghi</span></div>", 2},
  };
  for (const auto& [body, lineCount] : cases) {
    const std::string out = dump("", document(body), bookAlignment());
    const auto lines = parseLines(out);
    ASSERT_EQ(lineCount, lines.size()) << out;
    for (size_t i = 0; i < lines.size(); ++i) {
      std::string text;
      for (const Word& word : lines[i].words) text += word.text;
      // The span's opener goes out with its first line's paragraph.
      EXPECT_EQ(i == 0 ? 1u : 0u, occurrences(text, kLri) + occurrences(text, kRli)) << body << " line " << i;
      EXPECT_EQ(0u, occurrences(text, kPdi)) << body << " line " << i;
    }
  }
}

TEST_F(BidiLayout, ACellDrainedUnderAWordKeepsItsIsolateClosed) {
  // 253 words leave the cell 16 bytes short of its row budget (ChapterHtmlSlimParser), so the CJK word opening the span
  // drains the cell's text so far as a paragraph before the word goes in. The word takes its RLI into the cell's fresh
  // text, and the span's PDI follows it there: no paragraph ended under the word. (The RLI is a token of its own, cut
  // before 日 by the CJK rules; it draws nothing, so the dump lists no word for it.)
  std::string cell;
  for (int i = 0; i < 253; ++i) cell += "word ";
  const auto lines = parseLines(
      dump("", document("<table><tr><td>" + cell + "<span dir=\"rtl\">日本 more</span> end</td></tr></table>"),
           bookAlignment()));
  ASSERT_FALSE(lines.empty());
  std::string last;
  for (const Word& word : lines.back().words) last += word.text + " ";
  EXPECT_EQ("日 本 more" + kPdi + " end ", last);
}

TEST_F(BidiLayout, AnElementNestedPastTheStackInsertsNothing) {
  // Eight elements open their isolates; a ninth inside them inserts neither its opener nor its closer.
  std::string body = "<p>";
  for (int i = 0; i < 9; ++i) body += "w <span dir=\"rtl\">";
  body += "z";
  for (int i = 0; i < 9; ++i) body += "</span>";
  body += " end</p>";
  const auto lines = parseLines(dump("", document(body)));
  std::string all;
  for (const Line& line : lines) {
    for (const Word& word : line.words) all += word.text;
  }
  EXPECT_EQ(8u, occurrences(all, kRli));
  EXPECT_EQ(8u, occurrences(all, kPdi));
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
