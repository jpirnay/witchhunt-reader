// Loading a page or a chapter on a heap that is low or split into small blocks.
//
// The device is built with -fno-exceptions: a std::vector or std::string that cannot grow does not
// throw, it aborts and reboots the reader. Where the size of an allocation comes from the cache
// file -- a page's footnote count, a chapter's page count, a stored alt text -- the loaders ask
// for the block first (HeapFit.h) and carry on without it. These tests put the loaders under a
// heap whose largest block is too small and check what comes out still reads.
#include <Arduino.h>
#include <Serialization.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/ImageBlock.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

std::string freshDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "low_heap_load_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  return p;
}

std::vector<std::string> wordsOn(const Page& page) {
  std::vector<std::string> out;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine&>(*el).getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t w = 0; w < block->wordCount(); ++w) out.emplace_back(block->wordText(w));
  }
  return out;
}

}  // namespace

TEST(LowHeapLoad, PageWithoutABlockForItsFootnotesLoadsWithoutThem) {
  const std::string path = freshDir("footnotes") + "/page.bin";
  {
    Page page;
    ASSERT_TRUE(page.addFootnote("1", "notes.xhtml#n1"));
    ASSERT_TRUE(page.addFootnote("2", "notes.xhtml#n2"));
    ASSERT_TRUE(page.addFootnote("3", "notes.xhtml#n3"));
    FsFile out;
    ASSERT_TRUE(Storage.openFileForWrite("TST", path, out));
    ASSERT_TRUE(page.serialize(out));
    out.close();
  }

  FsFile in;
  ASSERT_TRUE(Storage.openFileForRead("TST", path, in));
  const auto full = Page::deserialize(in);
  ASSERT_TRUE(full);
  ASSERT_EQ(3u, full->footnotes.size());
  EXPECT_STREQ("notes.xhtml#n2", full->footnotes[1].href);

  // Three entries need 384 bytes in one block.
  const HostLargestFreeBlock fragmented(256);
  ASSERT_TRUE(in.seek(0));
  const auto bare = Page::deserialize(in);
  ASSERT_TRUE(bare) << "the page itself must still load";
  EXPECT_TRUE(bare->footnotes.empty());
  in.close();
}

// Without a block for the page-offset table the chapter still opens, and every page reads its
// offset from the file instead -- the same pages, word for word. (The TOC boundaries and page-break
// labels give way under the same heap; they have their own fallbacks and are not checked here.)
TEST(LowHeapLoad, ChapterWithoutABlockForItsPageTableReadsOffsetsFromTheFile) {
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_anchor_pagebreak.epub", freshDir("lut"));
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();
  GfxRenderer renderer;
  const auto p = params();

  std::vector<std::vector<std::string>> expected;
  {
    Section section(epub, 0, renderer);
    ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
    ASSERT_TRUE(section.loadSectionFile(p));
    ASSERT_GT(section.pageCount, 3);
    for (int pg = 0; pg < section.pageCount; ++pg) {
      section.currentPage = pg;
      const auto page = section.loadPageFromSectionFile();
      ASSERT_TRUE(page);
      expected.push_back(wordsOn(*page));
    }
  }

  Section section(epub, 0, renderer);
  {
    const HostLargestFreeBlock fragmented(16);
    ASSERT_TRUE(section.loadSectionFile(p)) << "a good cache must not be refused, let alone deleted";
  }
  ASSERT_EQ(expected.size(), static_cast<size_t>(section.pageCount));
  // Backwards, so every load seeks rather than following on from the previous one.
  for (int pg = section.pageCount - 1; pg >= 0; --pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    ASSERT_TRUE(page) << "page " << pg;
    EXPECT_EQ(expected[pg], wordsOn(*page)) << "page " << pg;
  }
  section.currentPage = section.pageCount;
  EXPECT_FALSE(section.loadPageFromSectionFile()) << "the range check still applies";
}

TEST(LowHeapLoad, AltTextIsCutToOneLineAtACharacterBoundary) {
  // 127 ASCII bytes, then a two-byte character straddling the 128-byte cut.
  const std::string alt = std::string(127, 'a') + "\xC3\xA9" + std::string(50, 'b');
  const ImageBlock block("/img.pxc", 10, 10, alt);
  EXPECT_EQ(std::string(127, 'a'), block.getAltText());

  const ImageBlock shortAlt("/img.pxc", 10, 10, "A map");
  EXPECT_EQ("A map", shortAlt.getAltText());
}

// A cache written before the cap can hold an alt text of up to 4 KB. Loading it keeps only what
// the placeholder can show, and the fields after it still line up.
TEST(LowHeapLoad, LongStoredAltTextIsCutOnLoad) {
  const std::string path = freshDir("alt") + "/image.bin";
  const std::string alt = std::string(1000, 'x');
  {
    FsFile out;
    ASSERT_TRUE(Storage.openFileForWrite("TST", path, out));
    serialization::writeString(out, std::string("/cache/img_1.pxc"));
    serialization::writePod(out, static_cast<int16_t>(320));
    serialization::writePod(out, static_cast<int16_t>(200));
    serialization::writeString(out, alt);
    serialization::writeString(out, std::string("/books/a.epub"));
    serialization::writeString(out, std::string("OEBPS/img/1.jpg"));
    serialization::writePod(out, static_cast<int16_t>(0));
    serialization::writePod(out, static_cast<int16_t>(120));
    serialization::writePod(out, static_cast<uint8_t>(0));          // flags
    serialization::writePod(out, static_cast<uint32_t>(0xC0FFEE));  // whatever follows the block
    out.close();
  }
  FsFile in;
  ASSERT_TRUE(Storage.openFileForRead("TST", path, in));
  const auto block = ImageBlock::deserialize(in);
  ASSERT_TRUE(block);
  EXPECT_EQ(std::string(ImageBlock::MAX_ALT_TEXT_BYTES, 'x'), block->getAltText());
  EXPECT_EQ("/cache/img_1.pxc", block->getImagePath());
  EXPECT_EQ(320, block->getWidth());
  EXPECT_EQ(120, block->getRenderedHeight());
  uint32_t next = 0;
  serialization::readPod(in, next);
  EXPECT_EQ(0xC0FFEEu, next);
  in.close();
}
