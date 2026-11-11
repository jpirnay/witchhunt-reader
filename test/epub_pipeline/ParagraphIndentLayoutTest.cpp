// The reader's Paragraph Indent through the real pipeline: which blocks take the setting and which keep the book's
// first line. Built by the parser and laid out by ParsedText, read back from the section files. The host stub's font
// has an 18 px ascender, which is the em a paragraph indent is measured in (0.5 em = 9 px, 2 em = 36 px), and 16 px CJK
// characters, the em of the CJK grid.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/ParagraphIndent.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

constexpr uint8_t kBooksStyle = static_cast<uint8_t>(ParagraphIndent::FromBook);
constexpr uint8_t kNone = static_cast<uint8_t>(ParagraphIndent::None);
constexpr uint8_t kHalfEm = static_cast<uint8_t>(ParagraphIndent::HalfEm);
constexpr uint8_t kOneEm = static_cast<uint8_t>(ParagraphIndent::OneEm);
constexpr uint8_t kOneAndHalfEm = static_cast<uint8_t>(ParagraphIndent::OneAndHalfEm);
constexpr uint8_t kTwoEm = static_cast<uint8_t>(ParagraphIndent::TwoEm);
constexpr uint8_t kAllSettings[] = {kNone, kHalfEm, kOneEm, kOneAndHalfEm, kTwoEm, kBooksStyle};

struct Word {
  int x = 0;
  std::string text;
};
struct Line {
  int x = 0;
  std::vector<Word> words;
};

// Every text line of the dump in order, page lines ("LINE") and table cell lines ("LN") alike.
std::vector<Line> parseLines(const std::string& dump) {
  std::vector<Line> lines;
  std::istringstream in(dump);
  std::string row;
  while (std::getline(in, row)) {
    if (row.rfind("  LINE ", 0) == 0) {
      Line line;
      int y = 0;
      std::sscanf(row.c_str(), "  LINE y=%d x=%d", &y, &line.x);
      lines.push_back(line);
    } else if (row.rfind("     LN ", 0) == 0) {
      lines.emplace_back();
    } else if ((row.rfind("   W ", 0) == 0 || row.rfind("      W ", 0) == 0) && !lines.empty()) {
      Word word;
      std::sscanf(row.c_str() + row.find("W x="), "W x=%d", &word.x);
      const size_t t = row.find(" t=");
      if (t == std::string::npos) continue;
      word.text = row.substr(t + 3);
      lines.back().words.push_back(word);
    }
  }
  return lines;
}

// The x of `text` as the first word of a line; INT_MIN when no line starts with it.
int firstWordX(const std::vector<Line>& lines, const std::string& text) {
  for (const Line& line : lines) {
    if (!line.words.empty() && line.words.front().text == text) return line.words.front().x;
  }
  ADD_FAILURE() << "no line starts with \"" << text << "\"";
  return INT32_MIN;
}

// The x of `text` wherever it is on a line.
int wordX(const std::vector<Line>& lines, const std::string& text) {
  for (const Line& line : lines) {
    for (const Word& word : line.words) {
      if (word.text == text) return word.x;
    }
  }
  ADD_FAILURE() << "no word \"" << text << "\"";
  return INT32_MIN;
}

constexpr const char* kBaseCss = "body { margin: 0; padding: 0 } p { margin: 0 }\n";

struct ParagraphIndentLayout : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / ("epub_paragraph_indent_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::string writeBook(const std::string& css, const std::string& body, const std::string& language = "en") {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"id\">pi</dc:identifier><dc:title>PI</dc:title><dc:language>" +
                language +
                "</dc:language></metadata>\n<manifest>\n<item id=\"c\" href=\"chapter.xhtml\" "
                "media-type=\"application/xhtml+xml\"/>\n<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/>\n"
                "</manifest>\n<spine><itemref idref=\"c\"/></spine>\n</package>\n");
    zip.add("chapter.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
            "<title>C</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
                body + "\n</body></html>\n");
    zip.add("style.css", std::string(kBaseCss) + css);
    const fs::path path = work / ("book" + std::to_string(books++) + ".epub");
    zip.write(path.string());
    return path.string();
  }

  std::vector<Line> layOut(const std::string& path, const pipeline_harness::Profile& profile) {
    std::ostringstream out;
    const std::string cache = path + ".cache" + std::to_string(books++);
    EXPECT_TRUE(pipeline_harness::runAndDump(path, cache, profile, out)) << out.str();
    return parseLines(out.str());
  }

  static pipeline_harness::Profile profile(const uint8_t indent, const bool extraParagraphSpacing = false) {
    pipeline_harness::Profile p;
    p.paragraphIndent = indent;
    p.extraParagraphSpacing = extraParagraphSpacing;
    return p;
  }

  // The first-line x of the paragraph starting with `first`, under each setting in kAllSettings order.
  std::vector<int> underEverySetting(const std::string& path, const std::string& first,
                                     pipeline_harness::Profile base = {}) {
    std::vector<int> xs;
    for (const uint8_t indent : kAllSettings) {
      base.paragraphIndent = indent;
      xs.push_back(firstWordX(layOut(path, base), first));
    }
    return xs;
  }

  // Where each list item's marker starts, in reading order.
  std::vector<int> markersAt(const std::string& path, const pipeline_harness::Profile& profile) {
    std::vector<int> xs;
    for (const Line& line : layOut(path, profile)) {
      if (!line.words.empty() && line.words.front().text == "\xE2\x80\xA2") xs.push_back(line.words.front().x);
    }
    return xs;
  }

  // The section files a build under `profile` leaves, by name (<spine>_<property hash>.bin).
  std::set<std::string> sectionFiles(const pipeline_harness::Profile& profile) {
    const std::string path = writeBook("", "<p>Alpha</p>");
    std::ostringstream out;
    const std::string cache = path + ".cache";
    EXPECT_TRUE(pipeline_harness::runAndDump(path, cache, profile, out)) << out.str();
    const std::regex name("[0-9]+_[0-9a-f]{8}\\.bin");
    std::set<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(cache)) {
      const std::string file = entry.path().filename().string();
      if (std::regex_match(file, name)) files.insert(file);
    }
    return files;
  }
};

// --- body paragraphs: the setting's ---------------------------------------------------------------------------------

TEST_F(ParagraphIndentLayout, AParagraphWithoutAnIndentOfItsOwnTakesTheSetting) {
  const std::string path = writeBook("", "<p>Alpha bravo.</p>");
  // None, 0.5 em, 1 em, 1.5 em, 2 em, then Book's Style: one em, as before the setting.
  EXPECT_EQ(underEverySetting(path, "Alpha"), (std::vector<int>{0, 9, 18, 27, 36, 18}));
}

TEST_F(ParagraphIndentLayout, ExtraParagraphSpacingDoesNotStopTheSetting) {
  const std::string path = writeBook("", "<p>Alpha bravo.</p>");
  pipeline_harness::Profile spaced;
  spaced.extraParagraphSpacing = true;
  // Book's Style indents nothing with the spacing on; a fixed step still does.
  EXPECT_EQ(underEverySetting(path, "Alpha", spaced), (std::vector<int>{0, 9, 18, 27, 36, 0}));
}

TEST_F(ParagraphIndentLayout, TheSettingReplacesTheBooksPositiveIndent) {
  const std::string path = writeBook("p { text-indent: 2.5em }", "<p>Alpha bravo.</p>");
  EXPECT_EQ(underEverySetting(path, "Alpha"), (std::vector<int>{0, 9, 18, 27, 36, 45}));
}

TEST_F(ParagraphIndentLayout, TheSettingReplacesTheBooksZeroIndent) {
  const std::string path = writeBook("p { text-indent: 0 }", "<p>Alpha bravo.</p>");
  EXPECT_EQ(underEverySetting(path, "Alpha"), (std::vector<int>{0, 9, 18, 27, 36, 0}));
}

TEST_F(ParagraphIndentLayout, OnlyTheFirstLineIsIndented) {
  const std::string path = writeBook("", "<p>Alpha<br/>Bravo charlie.</p>");
  for (const uint8_t indent : kAllSettings) {
    const auto lines = layOut(path, profile(indent));
    EXPECT_EQ(firstWordX(lines, "Bravo"), 0) << "a <br> continuation, setting " << int{indent};
  }
}

// A right-to-left paragraph is indented at its start, the right: a one-line paragraph sits at the right edge less the
// indent, so each step moves its words left by its size.
TEST_F(ParagraphIndentLayout, ARightToLeftParagraphIsIndentedOnTheRight) {
  const std::string path = writeBook("", "<p dir=\"rtl\">\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D.</p>", "he");
  const std::string word = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D.";
  const int none = wordX(layOut(path, profile(kNone)), word);
  EXPECT_EQ(wordX(layOut(path, profile(kHalfEm)), word), none - 9);
  EXPECT_EQ(wordX(layOut(path, profile(kTwoEm)), word), none - 36);
  EXPECT_EQ(wordX(layOut(path, profile(kBooksStyle)), word), none - 18);
}

// --- what keeps the book's first line
// ---------------------------------------------------------------------------------

TEST_F(ParagraphIndentLayout, AHangingIndentIsKept) {
  const std::string path = writeBook("p.h { margin-left: 2em; text-indent: -1em }", "<p class=\"h\">Alpha bravo.</p>");
  const auto xs = underEverySetting(path, "Alpha");
  for (size_t i = 0; i < xs.size(); ++i) EXPECT_EQ(xs[i], xs.back()) << "setting " << int{kAllSettings[i]};
  EXPECT_LT(xs.back(), 0) << "the first line hangs out of the margin";
}

TEST_F(ParagraphIndentLayout, ACentredParagraphIsNotIndented) {
  const std::string path = writeBook("p.c { text-align: center }", "<p class=\"c\">Alpha bravo.</p>");
  const auto xs = underEverySetting(path, "Alpha");
  for (size_t i = 0; i < xs.size(); ++i) EXPECT_EQ(xs[i], xs.back()) << "setting " << int{kAllSettings[i]};
}

// Under the book's own alignment, so a left-aligned heading stays left (a heading is centred otherwise).
TEST_F(ParagraphIndentLayout, AHeadingKeepsTheBooksFirstLine) {
  const std::string path = writeBook("h2 { text-align: left }", "<h2>Alpha bravo</h2><p>Charlie delta.</p>");
  pipeline_harness::Profile booksAlignment;
  booksAlignment.paragraphAlignment = 4;  // Book's Style
  const auto xs = underEverySetting(path, "Alpha", booksAlignment);
  for (size_t i = 0; i < xs.size(); ++i) EXPECT_EQ(xs[i], xs.back()) << "setting " << int{kAllSettings[i]};
  // The paragraph after it does take the setting.
  EXPECT_EQ(underEverySetting(path, "Charlie", booksAlignment), (std::vector<int>{0, 9, 18, 27, 36, 18}));
}

// The first item's marker on its own line, the second's inside its <p>. Book's Style indents each item's marker by one
// em where the book sets no indent; every fixed step, None to 2 em, drops that em, so the markers start one em further
// left. The paragraph after the list takes the step as usual.
TEST_F(ParagraphIndentLayout, AFixedStepDropsTheEmBeforeAListItemsMarker) {
  const std::string path = writeBook("", "<ul><li>Alpha bravo.</li><li><p>Charlie delta.</p></li></ul><p>Echo.</p>");
  const std::vector<int> booksStyle = markersAt(path, profile(kBooksStyle));
  ASSERT_EQ(booksStyle.size(), 2u);
  for (const uint8_t indent : {kNone, kHalfEm, kOneEm, kOneAndHalfEm, kTwoEm}) {
    EXPECT_EQ(markersAt(path, profile(indent)), (std::vector<int>{booksStyle[0] - 18, booksStyle[1] - 18}))
        << "setting " << int{indent};
  }
  EXPECT_EQ(underEverySetting(path, "Echo."), (std::vector<int>{0, 9, 18, 27, 36, 18}));
}

// An indent the book gives its list items stands under every setting.
TEST_F(ParagraphIndentLayout, AListItemKeepsTheBooksOwnIndent) {
  const std::string path = writeBook("li { text-indent: 1.5em }", "<ul><li>Alpha bravo.</li></ul><p>Echo.</p>");
  const std::vector<int> booksStyle = markersAt(path, profile(kBooksStyle));
  ASSERT_EQ(booksStyle.size(), 1u);
  for (const uint8_t indent : kAllSettings) {
    EXPECT_EQ(markersAt(path, profile(indent)), booksStyle) << "setting " << int{indent};
  }
}

// A code block or a verse set in <pre>: no line of it is indented under any setting, Book's Style included, with
// Extra Paragraph Spacing on or off.
TEST_F(ParagraphIndentLayout, PreformattedTextTakesNoIndent) {
  const std::string path = writeBook("", "<pre>Alpha bravo\nCharlie delta</pre><p>Echo.</p>");
  for (const bool extraSpacing : {false, true}) {
    pipeline_harness::Profile base;
    base.extraParagraphSpacing = extraSpacing;
    EXPECT_EQ(underEverySetting(path, "Alpha", base), (std::vector<int>{0, 0, 0, 0, 0, 0})) << extraSpacing;
    EXPECT_EQ(underEverySetting(path, "Charlie", base), (std::vector<int>{0, 0, 0, 0, 0, 0})) << extraSpacing;
  }
  // The paragraph after it is running text again.
  EXPECT_EQ(underEverySetting(path, "Echo."), (std::vector<int>{0, 9, 18, 27, 36, 18}));
}

TEST_F(ParagraphIndentLayout, APreKeepsTheIndentItsBookSets) {
  const std::string path = writeBook("pre { text-indent: 2em }", "<pre>Alpha bravo</pre>");
  EXPECT_EQ(underEverySetting(path, "Alpha"), (std::vector<int>{36, 36, 36, 36, 36, 36}));
}

TEST_F(ParagraphIndentLayout, ATableCellKeepsTheBooksFirstLine) {
  const std::string path =
      writeBook("", "<table><tr><td>Alpha bravo</td><td>Charlie</td></tr></table><p>Delta echo.</p>");
  const auto xs = underEverySetting(path, "Alpha");
  for (size_t i = 0; i < xs.size(); ++i) EXPECT_EQ(xs[i], xs.back()) << "setting " << int{kAllSettings[i]};
}

TEST_F(ParagraphIndentLayout, ADropCapLineKeepsTheBooksFirstLine) {
  const std::string path = writeBook(".dc { float: left; font-size: 3em }",
                                     "<p><span class=\"dc\">A</span>lpha bravo charlie delta echo foxtrot golf hotel "
                                     "india juliet kilo lima mike november oscar papa quebec romeo sierra.</p>");
  const auto xs = underEverySetting(path, "lpha");
  for (size_t i = 0; i < xs.size(); ++i) EXPECT_EQ(xs[i], xs.back()) << "setting " << int{kAllSettings[i]};
}

// --- CJK: the grid's whole characters --------------------------------------------------------------------------------

// A Japanese book's lines sit on a grid of whole characters (16 px here), centred in the column: an indent rounds down
// to whole ems, at least one, horizontal or vertical alike. Measured from where None puts the first character.
TEST_F(ParagraphIndentLayout, AJapaneseIndentIsWholeCharacters) {
  const std::string text = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE6\x96\x87\xE7\xAB\xA0";  // 日本語の文章
  const std::string path = writeBook("", "<p>" + text + "</p>", "ja");
  for (const VerticalText vertical : {VerticalText::Off, VerticalText::On}) {
    pipeline_harness::Profile base;
    base.verticalText = vertical;
    std::vector<int> xs;
    for (const uint8_t indent : kAllSettings) {
      base.paragraphIndent = indent;
      const auto lines = layOut(path, base);
      ASSERT_FALSE(lines.empty());
      ASSERT_FALSE(lines.front().words.empty());
      xs.push_back(lines.front().words.front().x);
    }
    const int none = xs.front();
    for (int& x : xs) x -= none;
    EXPECT_EQ(xs, (std::vector<int>{0, 16, 16, 16, 32, 16})) << (vertical == VerticalText::On ? "vertical" : "");
  }
}

// --- the cache key
// -----------------------------------------------------------------------------------------------------

TEST_F(ParagraphIndentLayout, BooksStyleKeepsTheCacheKeyAndEachStepHasItsOwn) {
  // The default profile's one section file, as LineSpacingLayoutTest measured it before the setting existed.
  EXPECT_EQ(sectionFiles(profile(kBooksStyle)), std::set<std::string>{"0_9fe217c6.bin"});
  std::set<std::string> seen{"0_9fe217c6.bin"};
  for (const uint8_t indent : {kNone, kHalfEm, kOneEm, kOneAndHalfEm, kTwoEm}) {
    const auto files = sectionFiles(profile(indent));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_TRUE(seen.insert(*files.begin()).second) << "setting " << int{indent} << " shares " << *files.begin();
  }
}

}  // namespace
