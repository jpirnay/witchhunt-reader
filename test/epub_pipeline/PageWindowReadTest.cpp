// A page loads through a window smaller than itself (Section's one sector, serialization::BufferedFileReader): refills
// between the fields, and a word arena or an alt text larger than the window read past it, give back what was written.
#include <BufferedFileIO.h>
#include <Serialization.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/blocks/ImageBlock.h"

namespace {

std::unique_ptr<TextBlock> textLine(std::vector<std::string> words) {
  const std::vector<EpdFontFamily::Style> styles(words.size(), EpdFontFamily::REGULAR);
  std::vector<int16_t> xpos;
  for (size_t i = 0; i < words.size(); i++) xpos.push_back(static_cast<int16_t>(i * 40));
  TextBlock::WordRange range;
  range.words = &words;
  range.styles = &styles;
  range.first = 0;
  range.count = words.size();
  BlockStyle blockStyle;
  blockStyle.alignment = CssTextAlign::Left;
  blockStyle.textIndent = 0;
  blockStyle.textIndentDefined = true;
  return std::unique_ptr<TextBlock>(new TextBlock(range, xpos, blockStyle));
}

// About 2 KB: 30 lines of six words, a rule, a picture with a 100-byte alt text, three notes. `longLine` adds a line
// whose word arena alone is larger than the window.
Page bigPage(const int seed, const bool longLine) {
  Page page;
  for (int line = 0; line < 30; line++) {
    std::vector<std::string> words;
    for (int w = 0; w < 6; w++) {
      words.push_back("w" + std::to_string(seed) + "_" + std::to_string(line) + "_" + std::to_string(w));
    }
    page.elements.push_back(
        std::unique_ptr<PageElement>(new PageLine(textLine(words), 0, static_cast<int16_t>(line * 20))));
  }
  if (longLine) {
    std::vector<std::string> words;
    for (int w = 0; w < 60; w++) words.push_back("long" + std::to_string(seed) + "_" + std::to_string(w));
    page.elements.push_back(std::unique_ptr<PageElement>(new PageLine(textLine(words), 0, 600)));
  }
  page.elements.push_back(std::unique_ptr<PageElement>(new PageHR(0, 620, 300)));
  page.elements.push_back(std::unique_ptr<PageElement>(
      new PageImage(std::unique_ptr<ImageBlock>(new ImageBlock("/cache/img_" + std::to_string(seed) + ".pxc", 120, 80,
                                                               std::string(100, static_cast<char>('a' + seed)))),
                    0, 640)));
  for (int n = 1; n <= 3; n++) {
    EXPECT_TRUE(page.addFootnote(std::to_string(n).c_str(),
                                 ("notes.xhtml#s" + std::to_string(seed) + "n" + std::to_string(n)).c_str()));
  }
  return page;
}

// Everything the comparison looks at, in order.
std::vector<std::string> contents(const Page& page) {
  std::vector<std::string> out;
  for (const auto& el : page.elements) {
    out.push_back("tag " + std::to_string(static_cast<int>(el->getTag())));
    if (el->getTag() == TAG_PageLine) {
      const TextBlock* block = static_cast<const PageLine&>(*el).getBlock();
      for (uint16_t w = 0; block && w < block->wordCount(); w++) out.emplace_back(block->wordText(w));
    } else if (el->getTag() == TAG_PageImage) {
      const ImageBlock& image = static_cast<const PageImage&>(*el).getImageBlock();
      out.push_back(image.getImagePath());
      out.push_back(image.getAltText());
    }
  }
  for (const auto& note : page.footnotes) out.push_back(std::string(note.number) + " " + note.href);
  return out;
}

std::string tempPath(const std::string& name) {
  const auto dir =
      std::filesystem::temp_directory_path() / ("page_window_read_" + std::to_string(static_cast<long>(::getpid())));
  std::filesystem::create_directories(dir);
  return (dir / name).string();
}

}  // namespace

TEST(PageWindowRead, PagesLargerThanTheWindowLoadWhole) {
  const std::string path = tempPath("pages.bin");
  const Page pages[] = {bigPage(1, false), bigPage(2, true), bigPage(3, false)};
  std::vector<uint32_t> offsets;
  {
    FsFile out;
    ASSERT_TRUE(Storage.openFileForWrite("TST", path, out));
    serialization::BufferedFileWriter writer(out);
    for (const Page& page : pages) {
      offsets.push_back(writer.position());
      ASSERT_TRUE(page.serialize(writer));
    }
    ASSERT_TRUE(writer.flush());
    out.close();
  }
  ASSERT_GT(offsets[1] - offsets[0], 1024u) << "a page must be larger than the window for this to test anything";

  FsFile in;
  ASSERT_TRUE(Storage.openFileForRead("TST", path, in));
  std::array<uint8_t, 512> window{};
  // Section's window; one far smaller than any field run, so nearly every read refills or goes straight to the file;
  // and none at all (a reader that reads through).
  for (const size_t size : {static_cast<size_t>(512), static_cast<size_t>(16), static_cast<size_t>(0)}) {
    // Backwards, so every load seeks, as a page turn back does.
    for (int p = 2; p >= 0; p--) {
      ASSERT_TRUE(in.seek(offsets[p]));
      serialization::BufferedFileReader reader(in, size ? window.data() : nullptr, size);
      const std::unique_ptr<Page> loaded = Page::deserialize(reader);
      ASSERT_TRUE(loaded) << "window " << size << ", page " << p;
      EXPECT_EQ(contents(pages[p]), contents(*loaded)) << "window " << size << ", page " << p;
      if (p < 2) EXPECT_EQ(offsets[p + 1], reader.position()) << "the page ends where the next begins";
    }
  }
  in.close();
}
