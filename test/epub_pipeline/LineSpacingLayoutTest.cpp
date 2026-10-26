// Book's style line spacing: a block's CSS line-height, inherited as CSS has it, sets its line pitch, never tighter
// than the floor; a block without one keeps the reader's spacing. Values derive from the host stub's metrics
// (GfxRenderer.h: line height 24) and the profile below: em 20 px and floor 0.95, so a line-height of L ems is a
// compression of L × 20/24 and a pitch of int(24 × max(L × 20/24, 0.95) + 0.5) (effectiveLineHeight).
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "PipelineRunner.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

struct Line {
  int y = 0;
  std::string text;  // the line's words joined by single spaces
};

// The dump's text lines, in order (PipelineRunner's "LINE y=" with its "W ... t=" words).
std::vector<Line> parseLines(const std::string& dump) {
  std::vector<Line> lines;
  std::istringstream in(dump);
  std::string row;
  while (std::getline(in, row)) {
    if (row.rfind("  LINE ", 0) == 0) {
      Line line;
      std::sscanf(row.c_str(), "  LINE y=%d", &line.y);
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

// The pitch inside the paragraph whose first line is `first`: the next line's y minus its own.
int pitchAfter(const std::vector<Line>& lines, const std::string& first) {
  for (size_t i = 0; i + 1 < lines.size(); ++i) {
    if (lines[i].text == first) return lines[i + 1].y - lines[i].y;
  }
  ADD_FAILURE() << "no line \"" << first << "\" with a line after it";
  return -1;
}

// Two lines per paragraph, whatever the viewport: a <br> ends the first.
std::string para(const std::string& word, const std::string& attrs = "") {
  return "<p" + attrs + ">" + word + "<br/>" + word + "2</p>";
}

constexpr const char* kBaseCss = "body { margin: 0; padding: 0 } p { margin: 0; text-indent: 0 }\n";

pipeline_harness::Profile bookStyle() {
  pipeline_harness::Profile p;
  p.bookLineHeightFloor = 0.95f;
  p.lineHeightEmPx = 20;
  return p;
}

struct LineSpacingLayout : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_line_spacing_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::string writeBook(const std::string& css, const std::string& body, const std::string& htmlAttrs = "") {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"id\">ls</dc:identifier><dc:title>LS</dc:title><dc:language>en</dc:language>"
            "</metadata>\n<manifest>\n<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/>\n</manifest>\n"
            "<spine><itemref idref=\"c\"/></spine>\n</package>\n");
    zip.add("chapter.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"" + htmlAttrs +
                "><head><title>C</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head>"
                "<body>\n" +
                body + "\n</body></html>\n");
    zip.add("style.css", std::string(kBaseCss) + css);
    const fs::path path = work / ("book" + std::to_string(books++) + ".epub");
    zip.write(path.string());
    return path.string();
  }

  std::vector<Line> lines(const std::string& css, const std::string& body,
                          const pipeline_harness::Profile& profile = bookStyle(), const std::string& htmlAttrs = "") {
    const std::string path = writeBook(css, body, htmlAttrs);
    std::ostringstream out;
    EXPECT_TRUE(pipeline_harness::runAndDump(path, path + ".cache", profile, out)) << out.str();
    return parseLines(out.str());
  }

  // The section files a build of `body` under `profile` leaves, by name (<spine>_<property hash>.bin).
  std::vector<std::string> sectionFiles(const pipeline_harness::Profile& profile) {
    const std::string path = writeBook("", para("Alpha"));
    std::ostringstream out;
    EXPECT_TRUE(pipeline_harness::runAndDump(path, path + ".cache", profile, out)) << out.str();
    const std::regex name("[0-9]+_[0-9a-f]{8}\\.bin");
    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(path + ".cache")) {
      const std::string file = entry.path().filename().string();
      if (std::regex_match(file, name)) files.push_back(file);
    }
    return files;
  }
};

TEST_F(LineSpacingLayout, AParagraphsOwnValue) {
  // 1.75 × 20/24 = 1.4583; 24 × 1.4583 = 35.
  EXPECT_EQ(pitchAfter(lines("p { line-height: 1.75 }", para("Alpha")), "Alpha"), 35);
}

TEST_F(LineSpacingLayout, ATightValueIsFloored) {
  // 1.0 × 20/24 = 0.833, below the floor: 24 × 0.95 = 22.8, laid out as 23.
  EXPECT_EQ(pitchAfter(lines("p { line-height: 1.0 }", para("Alpha")), "Alpha"), 23);
}

TEST_F(LineSpacingLayout, NoValueIsTheReadersSpacing) {
  // lineCompression 1.0: the font's 24.
  EXPECT_EQ(pitchAfter(lines("", para("Alpha")), "Alpha"), 24);
}

TEST_F(LineSpacingLayout, InheritedFromHtml) {
  // The EBPAJ template sets it on the root element: every paragraph takes 1.75, pitch 35.
  EXPECT_EQ(pitchAfter(lines("html { line-height: 1.75 }", para("Alpha") + para("Bravo")), "Bravo"), 35);
}

TEST_F(LineSpacingLayout, AChildOverridesItsParent) {
  // body 2 → 2 × 20/24 = 1.667, pitch 40; p.n 1.5 → 1.25, pitch 30.
  const auto ls =
      lines("body { line-height: 2 } p.n { line-height: 1.5 }", para("Alpha", " class=\"n\"") + para("Bravo"));
  EXPECT_EQ(pitchAfter(ls, "Alpha"), 30);
  EXPECT_EQ(pitchAfter(ls, "Bravo"), 40);
}

TEST_F(LineSpacingLayout, AHiddenElementsValueDoesNotLeak) {
  // The hidden div's line-height must not reach the paragraph after it.
  EXPECT_EQ(
      pitchAfter(lines("", "<div style=\"display: none; line-height: 3\">" + para("Hidden") + "</div>" + para("Alpha")),
                 "Alpha"),
      24);
}

TEST_F(LineSpacingLayout, ASpentSiblingDoesNotLendItsValue) {
  // An empty block with a line-height merges into the next paragraph's block, which keeps its own (none).
  EXPECT_EQ(pitchAfter(lines(".lh { line-height: 3 }", "<div class=\"lh\"></div>" + para("Alpha")), "Alpha"), 24);
}

TEST_F(LineSpacingLayout, OneClassUnderTwoAncestors) {
  // One memo entry (p|x) serves both paragraphs; only the first sits under .a (2 → pitch 40).
  const auto ls = lines(".a { line-height: 2 }",
                        "<div class=\"a\">" + para("Alpha", " class=\"x\"") + "</div>" + para("Bravo", " class=\"x\""));
  EXPECT_EQ(pitchAfter(ls, "Alpha"), 40);
  EXPECT_EQ(pitchAfter(ls, "Bravo"), 24);
}

TEST_F(LineSpacingLayout, BookStyleOffIgnoresTheValue) {
  EXPECT_EQ(pitchAfter(lines("p { line-height: 1.75 }", para("Alpha"), pipeline_harness::Profile{}), "Alpha"), 24);
}

TEST_F(LineSpacingLayout, WithoutTheBooksCss) {
  pipeline_harness::Profile p = bookStyle();
  p.embeddedStyle = false;
  EXPECT_EQ(pitchAfter(lines("p { line-height: 1.75 }", para("Alpha"), p), "Alpha"), 24);
}

TEST_F(LineSpacingLayout, BookStyleOffKeepsTheCacheKey) {
  // Measured on master e978c0f2c, before Book's style: the default profile's one section file.
  EXPECT_EQ(sectionFiles(pipeline_harness::Profile{}), std::vector<std::string>{"0_9fe217c6.bin"});
  pipeline_harness::Profile emOnly;
  emOnly.lineHeightEmPx = 29;  // no floor: not hashed
  EXPECT_EQ(sectionFiles(emOnly), std::vector<std::string>{"0_9fe217c6.bin"});
}

TEST_F(LineSpacingLayout, BookStyleHasAKeyPerEm) {
  pipeline_harness::Profile a = bookStyle();
  pipeline_harness::Profile b = bookStyle();
  b.lineHeightEmPx = 29;
  const auto off = sectionFiles(pipeline_harness::Profile{});
  const auto onA = sectionFiles(a);
  const auto onB = sectionFiles(b);
  ASSERT_EQ(onA.size(), 1u);
  ASSERT_EQ(onB.size(), 1u);
  EXPECT_NE(onA, off);
  EXPECT_NE(onA, onB);
}

}  // namespace
