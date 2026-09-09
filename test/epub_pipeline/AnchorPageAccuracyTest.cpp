// Cover for WHICH PAGE an anchor names.
//
// AnchorMapTest pins that every anchor is in the map. This pins that the page beside it is the
// page the anchor's content actually rendered on -- a different property, and the one a footnote
// jump depends on. An anchor that is present but one page early sends the reader to the page
// before the note, which reads as "the link is broken" just as much as a missing entry does.
//
// The failure it was written for: an anchor's page was taken when its text block STARTED, which
// is before that block's first line has been placed. A block whose first line does not fit on the
// page in progress moves to the next page, and the anchor kept the old count. Anything beginning
// exactly at a page boundary was therefore recorded one page early -- ordinary prose included,
// though a table makes it far likelier, since every fragment flush is a page boundary.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const std::string kFixture = std::string(CORPUS_DIR) + "/test_anchor_pagebreak.epub";
constexpr int kAnchorCount = 60;

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.hyphenationEnabled = false;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  p.bionicReadingEnabled = false;
  p.inlineFootnotePreviews = false;
  p.imageRendering = 0;
  return p;
}

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "anchor_page_accuracy" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

// Every word on `page`, so a marker token can be located in the laid-out text rather than in the
// source. Mirrors the walk PipelineRunner's dump uses.
void collectWords(const Page& page, std::vector<std::string>& out) {
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    const auto& block = line.getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t w = 0; w < block->wordCount(); ++w) out.emplace_back(block->wordText(w));
  }
}

}  // namespace

// For every anchor: the page the map names must be the page its content is on. Checked over all
// 60 rather than a chosen one, because which paragraphs happen to start at a page boundary is a
// property of the layout, not something the fixture can pin from outside.
TEST(AnchorPageAccuracy, AnchorNamesThePageItsContentIsOn) {
  auto epub = std::make_shared<Epub>(kFixture, freshCacheDir("pages"));
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();

  GfxRenderer renderer;
  Section section(epub, 0, renderer);
  const auto p = params();
  ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
  ASSERT_TRUE(section.loadSectionFile(p));
  ASSERT_GT(section.pageCount, 3) << "fixture must span several pages to place starts at breaks";

  // Where each MARKnn actually rendered.
  std::map<std::string, int> markPage;
  for (int pg = 0; pg < section.pageCount; ++pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    ASSERT_TRUE(page) << "page " << pg << " failed to load";
    std::vector<std::string> words;
    collectWords(*page, words);
    for (const auto& w : words) {
      if (w.rfind("MARK", 0) == 0 && markPage.find(w) == markPage.end()) markPage[w] = pg;
    }
  }

  int checked = 0;
  int offBy = 0;
  for (int i = 1; i <= kAnchorCount; ++i) {
    const std::string mark = "MARK" + std::to_string(i);
    const auto seen = markPage.find(mark);
    if (seen == markPage.end()) continue;  // token split across lines; not this test's business
    const auto anchored = section.getPageForAnchor("a" + std::to_string(i));
    ASSERT_TRUE(anchored.has_value()) << "a" << i << " missing from the anchor map";
    ++checked;
    if (static_cast<int>(*anchored) != seen->second) {
      ++offBy;
      EXPECT_EQ(static_cast<int>(*anchored), seen->second)
          << "anchor a" << i << " names page " << *anchored << " but its text is on page " << seen->second;
    }
  }
  EXPECT_GT(checked, kAnchorCount / 2) << "too few anchors located to be meaningful";
  EXPECT_EQ(offBy, 0) << offBy << " of " << checked << " anchors name the wrong page";
}
