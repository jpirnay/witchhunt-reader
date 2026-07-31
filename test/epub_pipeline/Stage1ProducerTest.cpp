// Stage-1 producer tap (docs/stage1-extraction-design.md, step 2): drives a real
// section build with a capturing BlockSink attached and checks the materialized
// compiled::Blocks. The fused layout output is asserted unchanged by the existing
// EpubPipelineTest goldens; this test validates the NEW producer path.

#include <GfxRenderer.h>
#include <gtest/gtest.h>
#include <process.h>  // _getpid - per-process temp isolation under parallel ctest

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/content/BlockSink.h"
#include "Epub/content/CompiledContent.h"

namespace fs = std::filesystem;

namespace {

// Records every block (with its resolved CssStyle) the walk emits.
struct CapturingSink : compiled::BlockSink {
  struct Captured {
    compiled::Block block;
    CssStyle style;
  };
  struct Chapter {
    uint8_t level;
    std::string title;
    size_t blockIndex;  // block emitted immediately before this onChapter
  };
  struct Anchor {
    std::string id;
    size_t blockIndex;  // block this anchor introduces (sink block count at emit time)
  };
  struct Footnote {
    int wordIndex;
    std::string number;
    std::string href;
    size_t blockIndex;  // block the footnote anchors into (current block at emit time)
  };
  std::vector<Captured> blocks;
  std::vector<Chapter> chapters;
  std::vector<Anchor> anchors;
  std::vector<Footnote> footnotes;
  std::vector<std::pair<std::string, size_t>> labels;  // printed-page label, block index
  int spineEnds = 0;

  void onBlock(compiled::Block&& b, const CssStyle& s) override { blocks.push_back({std::move(b), s}); }
  void onAnchor(const std::string& id) override { anchors.push_back({id, blocks.size()}); }
  void onChapter(uint8_t level, const std::string& title) override {
    chapters.push_back({level, title, blocks.empty() ? 0 : blocks.size() - 1});
  }
  void onPageBreakLabel(const std::string& label) override { labels.push_back({label, blocks.size()}); }
  void onFootnote(int wordIndex, const FootnoteEntry& e) override {
    footnotes.push_back({wordIndex, e.number, e.href, blocks.size()});
  }
  void onSpineEnd() override { ++spineEnds; }
};

// The i-th word's text within a block (words are NUL-terminated back-to-back).
std::string wordText(const compiled::Block& b, size_t i) { return std::string(&b.text[b.words[i].textOff]); }

std::vector<std::string> allWords(const compiled::Block& b) {
  std::vector<std::string> out;
  for (size_t i = 0; i < b.words.size(); ++i) out.push_back(wordText(b, i));
  return out;
}

// A table cell's text (words joined by spaces, honoring attach-to-previous).
std::string cellText(const compiled::TableCell& c) {
  std::string s;
  for (size_t i = 0; i < c.words.size(); ++i) {
    if (i != 0 && (c.words[i].styleSpan & compiled::kSpanAttachPrev) == 0) s.push_back(' ');
    s.append(&c.text[c.words[i].textOff]);
  }
  return s;
}

// Words joined honoring the attach-to-previous bit (matches the producer's title build).
std::string joinWords(const compiled::Block& b) {
  std::string s;
  for (size_t i = 0; i < b.words.size(); ++i) {
    if (i != 0 && (b.words[i].styleSpan & compiled::kSpanAttachPrev) == 0) s.push_back(' ');
    s += wordText(b, i);
  }
  return s;
}

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / ("stage1_producer_test_" + std::to_string(_getpid())) / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

// Build the given spine of an EPUB at `epubPath` with a capturing sink attached.
void compileSpineAt(const std::string& epubPath, int spineIndex, const std::string& cacheDir, CapturingSink& sink) {
  GfxRenderer renderer;
  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  ASSERT_TRUE(epub->load(true));
  epub->loadImageManifest();

  Section section(epub, spineIndex, renderer);
  section.setStage1Sink(&sink);
  // Default profile (matches EpubPipelineTest's golden profile).
  ASSERT_TRUE(section.createSectionFile(/*fontId=*/0, /*lineCompression=*/1.0f, /*extraParagraphSpacing=*/false,
                                        /*paragraphAlignment=*/0, /*viewportWidth=*/300, /*viewportHeight=*/400,
                                        /*hyphenationEnabled=*/false, /*embeddedStyle=*/true,
                                        /*bionicReadingEnabled=*/false, /*inlineFootnotePreviews=*/false,
                                        /*imageRendering=*/0, {}, /*skipEviction=*/true, {}));
}

// Build a corpus book's spine (path derived from CORPUS_DIR).
void compileSpine(const std::string& epubName, int spineIndex, const std::string& cacheDir, CapturingSink& sink) {
  compileSpineAt(std::string(CORPUS_DIR) + "/" + epubName, spineIndex, cacheDir, sink);
}

void compileSpine0(const std::string& epubName, const std::string& cacheDir, CapturingSink& sink) {
  compileSpine(epubName, 0, cacheDir, sink);
}

// The producer's word sequence for a spine, in document order: text-block words, and
// table-cell words row-major (image blocks contribute none).
std::vector<std::string> producerWords(const CapturingSink& sink) {
  std::vector<std::string> out;
  for (const auto& cap : sink.blocks) {
    const auto& b = cap.block;
    if (b.type == compiled::BlockType::Text) {
      for (size_t i = 0; i < b.words.size(); ++i) out.push_back(wordText(b, i));
    } else if (b.type == compiled::BlockType::Table) {
      for (const auto& row : b.rows)
        for (const auto& cell : row.cells)
          for (size_t i = 0; i < cell.words.size(); ++i) out.emplace_back(&cell.text[cell.words[i].textOff]);
    }
  }
  return out;
}

// The layout's word sequence for a spine, read from a section cache built via the LayoutSink
// (no external stage1 sink attached — post-unify that is the ONLY layout path). cacheDir must be
// fresh; we build then read back.
std::vector<std::string> layoutWords(const std::string& epubName, int spineIndex, const std::string& cacheDir) {
  std::vector<std::string> out;
  GfxRenderer renderer;
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/" + epubName, cacheDir);
  EXPECT_TRUE(epub->load(true));
  epub->loadImageManifest();
  Section section(epub, spineIndex, renderer);
  // Build via the internal LayoutSink (no setStage1Sink), then read back.
  if (!section.createSectionFile(0, 1.0f, false, 0, 300, 400, false, true, false, false, 0, {}, true, {})) return out;
  if (!section.loadSectionFile(0, 1.0f, false, 0, 300, 400, false, true, false, false, 0)) return out;
  for (uint16_t p = 0; p < section.pageCount; ++p) {
    section.currentPage = p;
    const auto page = section.loadPageFromSectionFile();
    if (!page) break;
    for (const auto& el : page->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& block = *static_cast<const PageLine&>(*el).getBlock();
        for (uint16_t w = 0; w < block.wordCount(); ++w) out.emplace_back(block.wordText(w));
      } else if (el->getTag() == TAG_PageTable) {
        // Grid tables: cell text lives in the fragment, not PageLines. Walk row-major.
        for (const auto& row : static_cast<const PageTableFragment&>(*el).getRows()) {
          for (const auto& cell : row.cells) {
            for (const auto& line : cell.lines) {
              for (uint16_t w = 0; w < line->wordCount(); ++w) out.emplace_back(line->wordText(w));
            }
          }
        }
      }
    }
  }
  return out;
}

// Find the spine index whose href contains `needle`.
int spineIndexForHref(const std::string& epubName, const std::string& cacheDir, const std::string& needle) {
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/" + epubName, cacheDir);
  EXPECT_TRUE(epub->load(true));
  for (int i = 0; i < epub->getSpineItemsCount(); ++i) {
    if (epub->getSpineItem(i).href.find(needle) != std::string::npos) return i;
  }
  return -1;
}

}  // namespace

TEST(Stage1Producer, EmitsBlocksForHeadings) {
  CapturingSink sink;
  compileSpine0("test_headings.epub", freshCacheDir("headings"), sink);

  EXPECT_EQ(sink.spineEnds, 1) << "onSpineEnd must fire exactly once per spine build";
  ASSERT_GE(sink.blocks.size(), 4u) << "test_headings has several headings + paragraphs";

  // The first NON-EMPTY block is the h1 heading (empty wrapper transcript blocks may precede).
  size_t firstContent = 0;
  while (firstContent < sink.blocks.size() && sink.blocks[firstContent].block.words.empty()) ++firstContent;
  ASSERT_LT(firstContent, sink.blocks.size());
  EXPECT_EQ(allWords(sink.blocks[firstContent].block),
            (std::vector<std::string>{"H1", "Heading", "(default", "multiplier", "1.6x)"}));

  // Structural invariants across every captured block. Empty blocks are the
  // wrapper/spacer/<br> transcript (legal, word-free); word invariants apply to the rest.
  uint32_t prevCharOffset = 0;
  for (const auto& cap : sink.blocks) {
    const auto& b = cap.block;
    EXPECT_EQ(b.type, compiled::BlockType::Text);
    if (!b.words.empty()) {
      EXPECT_FALSE(b.text.empty());
      // First word of a block starts a new paragraph — never an attach-to-previous.
      EXPECT_EQ(b.words[0].styleSpan & compiled::kSpanAttachPrev, 0);
      for (const auto& w : b.words) {
        ASSERT_LT(w.textOff, b.text.size());
        EXPECT_NE(b.text[w.textOff], '\0') << "each word points at non-empty NUL-terminated text";
      }
    } else {
      EXPECT_TRUE(b.text.empty()) << "an empty transcript block carries no text bytes";
    }
    EXPECT_GE(b.charOffset, prevCharOffset) << "char offsets are monotonic in document order";
    prevCharOffset = b.charOffset;
  }
}

TEST(Stage1Producer, EmitsChaptersForHeadings) {
  CapturingSink sink;
  compileSpine0("test_headings.epub", freshCacheDir("chapters"), sink);

  ASSERT_FALSE(sink.chapters.empty()) << "test_headings has headings";

  // First chapter is the h1 (level 1) with the heading's text as title.
  EXPECT_EQ(sink.chapters[0].level, 1);
  EXPECT_EQ(sink.chapters[0].title, "H1 Heading (default multiplier 1.6x)");

  // Every chapter references a heading block: the block carries kStartsChapter and its
  // joined text equals the chapter title. Chapters and heading-blocks are 1:1.
  size_t headingBlocks = 0;
  for (const auto& cap : sink.blocks) {
    if (cap.block.flags & compiled::kStartsChapter) ++headingBlocks;
  }
  EXPECT_EQ(headingBlocks, sink.chapters.size());
  for (const auto& ch : sink.chapters) {
    ASSERT_LT(ch.blockIndex, sink.blocks.size());
    const auto& b = sink.blocks[ch.blockIndex].block;
    EXPECT_NE(b.flags & compiled::kStartsChapter, 0) << "chapter must point at a heading block";
    EXPECT_GE(ch.level, 1);
    EXPECT_LE(ch.level, 6);
    EXPECT_EQ(ch.title, joinWords(b));
  }
}

TEST(Stage1Producer, EmitsAnchorsForContentIds) {
  const std::string cacheDir = freshCacheDir("anchors");
  // frontmatter.xhtml carries <h2 id="dedication">, id="epigraph", id="foreword".
  const int spine = spineIndexForHref("test_spine_toc_edges.epub", freshCacheDir("anchors_find"), "frontmatter");
  ASSERT_GE(spine, 0);

  CapturingSink sink;
  compileSpine("test_spine_toc_edges.epub", spine, cacheDir, sink);

  // Collect the emitted anchor ids.
  std::vector<std::string> ids;
  for (const auto& a : sink.anchors) ids.push_back(a.id);
  for (const char* want : {"dedication", "epigraph", "foreword"}) {
    EXPECT_NE(std::find(ids.begin(), ids.end(), want), ids.end()) << "missing anchor id: " << want;
  }

  // Each anchor introduces a block position; the first CONTENT block at or after it is
  // the anchored element (empty wrapper transcript blocks may sit in between at the same
  // charOffset). 'dedication' -> the "Dedication" heading block.
  for (const auto& a : sink.anchors) {
    ASSERT_LE(a.blockIndex, sink.blocks.size());
    if (a.id != "dedication") continue;
    size_t i = a.blockIndex;
    while (i < sink.blocks.size() && sink.blocks[i].block.words.empty()) ++i;
    ASSERT_LT(i, sink.blocks.size());
    EXPECT_EQ(wordText(sink.blocks[i].block, 0), "Dedication");
  }
}

TEST(Stage1Producer, EmitsImageBlocks) {
  // chapter2.xhtml embeds <img src="images/png_format.png">.
  const int spine = spineIndexForHref("test_png_images.epub", freshCacheDir("img_find"), "chapter2");
  ASSERT_GE(spine, 0);
  CapturingSink sink;
  compileSpine("test_png_images.epub", spine, freshCacheDir("images"), sink);

  size_t imageBlocks = 0;
  bool sawPngFormat = false;
  for (const auto& cap : sink.blocks) {
    const auto& b = cap.block;
    if (b.type != compiled::BlockType::Image) continue;
    ++imageBlocks;
    EXPECT_FALSE(b.entryPath.empty()) << "image block must carry an EPUB entry path";
    EXPECT_GT(b.width, 0) << "intrinsic width";
    EXPECT_GT(b.height, 0) << "intrinsic height";
    EXPECT_EQ(b.floatSide, 0) << "block images are centered, not floated";
    EXPECT_TRUE(b.words.empty()) << "image blocks carry no text words";
    if (b.entryPath.find("png_format.png") != std::string::npos) sawPngFormat = true;
  }
  EXPECT_GT(imageBlocks, 0u) << "chapter2 has a block image";
  EXPECT_TRUE(sawPngFormat) << "the image block's entryPath is the EPUB path, not the display cache path";
}

// Strong equivalence: the producer's text-word sequence must exactly equal the layout's,
// in reading order, for every construct the producer handles. This is the intermediate gate
// until step 6 (full Stage-1->Stage-2 golden diff). Add (book, href) pairs as producer
// coverage grows; a book with a construct the producer does not yet emit (e.g. table cells)
// would fail here, which is the point.
TEST(Stage1Producer, TextMatchesLayoutWords) {
  struct Case {
    const char* book;
    const char* href;  // spine href fragment (headings, paragraphs, block images, lists)
  };
  const std::vector<Case> cases = {
      {"test_headings.epub", "chapter1"},      // headings + paragraphs + a list
      {"test_font_sizes.epub", "chapter1"},    // inline font-size spans + a list
      {"test_png_images.epub", "chapter2"},    // text around a block image
      {"test_tables.epub", "ch001"},           // grid + paragraph-fallback tables
      {"test_float_images.epub", "ch1"},       // CSS-floated images beside paragraphs
      {"test_display_none.epub", "chapter1"},  // display:none content must be absent from BOTH
  };
  for (const auto& c : cases) {
    const int spine = spineIndexForHref(c.book, freshCacheDir(std::string("eqv_find_") + c.book), c.href);
    ASSERT_GE(spine, 0) << c.book << " " << c.href;
    const std::string cacheDir = freshCacheDir(std::string("eqv_") + c.book + "_" + c.href);
    CapturingSink sink;
    compileSpine(c.book, spine, cacheDir, sink);
    // layoutWords builds its own LayoutSink-backed section in a separate fresh dir (post-unify the
    // sink is the only layout path; the producer stream that fed `sink` also feeds that section).
    const std::string layoutDir = freshCacheDir(std::string("eqv_layout_") + c.book + "_" + c.href);
    EXPECT_EQ(producerWords(sink), layoutWords(c.book, spine, layoutDir))
        << "producer vs layout word mismatch: " << c.book << " " << c.href;
  }
}

TEST(Stage1Producer, EmitsTableBlocks) {
  const int spine = spineIndexForHref("test_tables.epub", freshCacheDir("tbl_find"), "ch001");
  ASSERT_GE(spine, 0);
  CapturingSink sink;
  compileSpine("test_tables.epub", spine, freshCacheDir("tables"), sink);

  std::vector<const compiled::Block*> tables;
  for (const auto& cap : sink.blocks) {
    if (cap.block.type == compiled::BlockType::Table) tables.push_back(&cap.block);
  }
  ASSERT_GE(tables.size(), 2u) << "ch001 has two tables";

  // First table: header row [Col 1, Col 2], data row [Some, Text].
  const auto& t = *tables[0];
  ASSERT_EQ(t.rows.size(), 2u);
  ASSERT_EQ(t.rows[0].cells.size(), 2u);
  EXPECT_TRUE(t.rows[0].isHeaderRow);
  EXPECT_TRUE(t.rows[0].cells[0].isHeader);
  EXPECT_EQ(cellText(t.rows[0].cells[0]), "Col 1");
  EXPECT_EQ(cellText(t.rows[0].cells[1]), "Col 2");
  ASSERT_EQ(t.rows[1].cells.size(), 2u);
  EXPECT_FALSE(t.rows[1].cells[0].isHeader);
  EXPECT_EQ(cellText(t.rows[1].cells[0]), "Some");
  EXPECT_EQ(cellText(t.rows[1].cells[1]), "Text");

  // Second table is 3 columns.
  EXPECT_EQ(tables[1]->rows[0].cells.size(), 3u);
}

TEST(Stage1Producer, EmitsFootnotes) {
  const char* book = "test_spine_toc_edges.epub";  // the corpus book with recognized footnote links
  int spineCount = 0;
  {
    auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/" + book, freshCacheDir("fn_count"));
    ASSERT_TRUE(epub->load(true));
    spineCount = epub->getSpineItemsCount();
  }
  ASSERT_GT(spineCount, 0);

  std::vector<CapturingSink> sinks(spineCount);
  size_t total = 0;
  bool sawNumberAndHref = false;
  for (int i = 0; i < spineCount; ++i) {
    compileSpine(book, i, freshCacheDir("fn_" + std::to_string(i)), sinks[i]);
    total += sinks[i].footnotes.size();
    for (const auto& f : sinks[i].footnotes) {
      if (!f.number.empty() && !f.href.empty()) sawNumberAndHref = true;
      EXPECT_LT(f.blockIndex, sinks[i].blocks.size() + 1);  // anchors into (or just past) a real block
    }
  }
  EXPECT_GT(total, 0u) << "test_font_sizes has footnote/cross-reference links";
  EXPECT_TRUE(sawNumberAndHref) << "footnotes carry a marker number and an href";
}

TEST(Stage1Producer, EmitsInlineImagesForFloats) {
  // test_float_images has CSS float:left/right images in every chapter.
  int spineCount = 0;
  {
    auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_float_images.epub", freshCacheDir("flt_count"));
    ASSERT_TRUE(epub->load(true));
    spineCount = epub->getSpineItemsCount();
  }
  ASSERT_GT(spineCount, 0);

  size_t inlineImages = 0;
  bool sawLeft = false;
  bool sawRight = false;
  for (int i = 0; i < spineCount; ++i) {
    CapturingSink sink;
    compileSpine("test_float_images.epub", i, freshCacheDir("flt_" + std::to_string(i)), sink);
    for (const auto& cap : sink.blocks) {
      const auto& b = cap.block;
      if (b.type != compiled::BlockType::Text || b.inlineImageEntryPath.empty()) continue;
      ++inlineImages;
      EXPECT_GT(b.inlineImageWidth, 0) << "intrinsic width";
      EXPECT_GT(b.inlineImageHeight, 0) << "intrinsic height";
      ASSERT_TRUE(b.inlineImageSide == 1 || b.inlineImageSide == 2) << "left or right float";
      if (b.inlineImageSide == 1) sawLeft = true;
      if (b.inlineImageSide == 2) sawRight = true;
      EXPECT_FALSE(b.words.empty()) << "the float attaches to a paragraph that has text";
    }
  }
  EXPECT_GT(inlineImages, 0u) << "test_float_images has floated images";
  EXPECT_TRUE(sawLeft && sawRight) << "corpus exercises both float sides";
}

TEST(Stage1Producer, IsDeterministic) {
  CapturingSink a;
  CapturingSink b;
  compileSpine0("test_headings.epub", freshCacheDir("det_a"), a);
  compileSpine0("test_headings.epub", freshCacheDir("det_b"), b);

  ASSERT_EQ(a.blocks.size(), b.blocks.size());
  for (size_t i = 0; i < a.blocks.size(); ++i) {
    EXPECT_EQ(a.blocks[i].block.text, b.blocks[i].block.text) << "block " << i << " text";
    EXPECT_EQ(a.blocks[i].block.charOffset, b.blocks[i].block.charOffset) << "block " << i << " charOffset";
    ASSERT_EQ(a.blocks[i].block.words.size(), b.blocks[i].block.words.size());
    for (size_t w = 0; w < a.blocks[i].block.words.size(); ++w) {
      EXPECT_EQ(a.blocks[i].block.words[w].styleSpan, b.blocks[i].block.words[w].styleSpan);
      EXPECT_EQ(a.blocks[i].block.words[w].sizePct, b.blocks[i].block.words[w].sizePct);
    }
  }
}
