// ChapterHtmlSlimParser's carried document (beginCarried / endCarried): the body of a spine file that holds only its
// chapter's heading, fed after the next file's <body> start tag. Laid out, it must be what one file holding both would
// be, while it counts nothing of the file it is laid out in: no visible text, no paragraph, no anchor.
#include <GfxRenderer.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr uint16_t kWidth = 214;
constexpr uint16_t kHeight = 358;

// The heading file's body content, as calibre leaves it: the number with its printed-page marker, the split marker.
const std::string kCarried =
    "\n<div class=\"c1\">\n<h1 id=\"h13\"><span epub:type=\"pagebreak\" id=\"page_127\" title=\"127\"></span>13</h1>\n"
    "<div id=\"calibre_pb_0\"></div>\n</div>\n";

std::string paragraphs(const int count) {
  std::string out;
  for (int i = 0; i < count; ++i) {
    out += "<p id=\"p" + std::to_string(i) + "\">Paragraph " + std::to_string(i) +
           " of the chapter, long enough to wrap over a few lines of a narrow page and fill it.</p>\n";
  }
  return out;
}

const std::string kHead =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
    "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>T</title></head><body>";
const std::string kTail = "</body></html>\n";

struct Laid {
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<std::string> anchors;
  std::vector<std::pair<uint16_t, std::string>> labels;
  std::vector<uint32_t> pageOffsets;
  uint16_t lastParagraph = 0;
};

class CarriedDocumentTest : public testing::Test {
 protected:
  void SetUp() override {
    work = fs::temp_directory_path() / ("carried_document_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    // A stylesheet, so the book has a CSS parser: inline styles, the page breaks among them, need one.
    epub = syntheticBook(work, 0, "<p>x</p>", {{"style.css", "text/css", "p { margin: 0; }"}});
    css = epub->getCssParser();
    ASSERT_NE(nullptr, css);
    css->setLeanResolve(true);
    ASSERT_TRUE(css->loadFromCache());
  }
  void TearDown() override { fs::remove_all(work); }

  // Lays out `prefix`, then `carried` in the carried mode (none when empty), then `rest`, as one parse.
  Laid lay(const std::string& prefix, const std::string& carried, const std::string& rest) {
    Laid run;
    ChapterHtmlSlimParser parser(
        epub, renderer, /*fontId=*/1, /*lineCompression=*/1.0f, /*extraParagraphSpacing=*/false,
        /*paragraphAlignment=*/0, kWidth, kHeight, /*hyphenationEnabled=*/false, /*fontSizeNormalization=*/false,
        /*bionicReadingEnabled=*/false,
        [&run](std::unique_ptr<Page> page) {
          run.pages.push_back(std::move(page));
          return true;
        },
        /*embeddedStyle=*/true, /*contentBase=*/"", (work / "img_").string(), /*imageRendering=*/0,
        /*tocAnchors=*/{}, /*progressFn=*/nullptr, css);
    const auto feed = [&parser](const std::string& bytes) {
      EXPECT_EQ(bytes.size(), parser.write(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    };
    EXPECT_TRUE(parser.setup(prefix.size() + carried.size() + rest.size()));
    feed(prefix);
    if (!carried.empty()) {
      parser.beginCarried();
      feed(carried);
      parser.endCarried();
    }
    feed(rest);
    EXPECT_TRUE(parser.finalize());
    for (const auto& [id, page] : parser.getAnchors()) run.anchors.push_back(id);
    parser.forEachPageBreakLabel([&run](const uint16_t page, const char* label, const size_t len) {
      run.labels.emplace_back(page, std::string(label, len));
    });
    for (size_t i = 0; i < parser.getParagraphLutPerPage().size(); ++i) {
      run.pageOffsets.push_back(parser.getParagraphLutPerPage()[i].visibleTextOffset);
      run.lastParagraph = parser.getParagraphLutPerPage()[i].paragraphIndex;
    }
    return run;
  }

  // Every line's place and words, page by page.
  static std::string dump(const Laid& run) {
    std::ostringstream out;
    for (size_t p = 0; p < run.pages.size(); ++p) {
      out << "PAGE " << p << "\n";
      for (const auto& el : run.pages[p]->elements) {
        out << " tag=" << static_cast<int>(el->getTag()) << " x=" << el->xPos << " y=" << el->yPos;
        if (el->getTag() == TAG_PageLine) {
          const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
          for (uint16_t w = 0; w < block.wordCount(); ++w) out << " " << block.wordText(w);
        }
        out << "\n";
      }
    }
    return out.str();
  }

  fs::path work;
  GfxRenderer renderer;
  std::shared_ptr<Epub> epub;
  CssParser* css = nullptr;
};

}  // namespace

TEST_F(CarriedDocumentTest, LaysOutAsOneFileWould) {
  const std::string body = "<div class=\"chapter\"></div><p>This Summer</p>\n" + paragraphs(12);
  const Laid carried = lay(kHead, kCarried, body + kTail);
  const Laid oneFile = lay(kHead + kCarried, "", body + kTail);
  ASSERT_GT(carried.pages.size(), 1u);
  EXPECT_EQ(dump(oneFile), dump(carried));
  // The heading tops the first page, the chapter's text follows on it.
  const std::string first = dump(carried).substr(0, dump(carried).find("PAGE 1"));
  EXPECT_NE(std::string::npos, first.find(" 13\n"));
  EXPECT_NE(std::string::npos, first.find("Paragraph 0"));
}

TEST_F(CarriedDocumentTest, CountsNothingOfTheFileItIsLaidOutIn) {
  const std::string body = paragraphs(12);
  const Laid carried = lay(kHead, kCarried, body + kTail);
  const Laid alone = lay(kHead, "", body + kTail);
  ASSERT_FALSE(carried.pageOffsets.empty());
  // The first page starts at the file's first visible byte, and each page's text is where it is in the file alone.
  EXPECT_EQ(0u, carried.pageOffsets.front());
  EXPECT_EQ(alone.lastParagraph, carried.lastParagraph);
  // No anchor of the carried file: a link to one of them names the other file.
  for (const std::string& id : carried.anchors) {
    EXPECT_NE("h13", id);
    EXPECT_NE("page_127", id);
    EXPECT_NE("calibre_pb_0", id);
  }
  EXPECT_EQ(alone.anchors, carried.anchors);
  // Its printed page stays, on the first page.
  ASSERT_EQ(1u, carried.labels.size());
  EXPECT_EQ(0, carried.labels.front().first);
  EXPECT_EQ("127", carried.labels.front().second);
}

TEST_F(CarriedDocumentTest, DropsTheCutAndKeepsTheBreaksAfterIt) {
  // The converter's cut, still asked for by the next file's first block, and a break further on.
  const std::string body =
      "<div style=\"page-break-before: always\"><p>FIRSTTEXT under the heading.</p></div>\n"
      "<p style=\"page-break-before: always\">LATERBREAK on a page of its own.</p>\n";
  const Laid carried = lay(kHead, kCarried, body + kTail);
  ASSERT_EQ(2u, carried.pages.size());
  const std::string text = dump(carried);
  const size_t pageOne = text.find("PAGE 1");
  EXPECT_LT(text.find(" 13\n"), pageOne);
  EXPECT_LT(text.find("FIRSTTEXT"), pageOne);
  EXPECT_GT(text.find("LATERBREAK"), pageOne);
}

TEST_F(CarriedDocumentTest, IgnoresTheCarriedFilesOwnBreaks) {
  const std::string breaking = "\n<h1 style=\"page-break-after: always; page-break-before: always\">13</h1>\n";
  const Laid carried = lay(kHead, breaking, "<p>FIRSTTEXT under the heading.</p>" + kTail);
  ASSERT_EQ(1u, carried.pages.size());
}
