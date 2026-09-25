// Block images inside a percentage-width wrapper.
//
// Publishers wrap figures in <div class="full60"> (width: 60%) for a tablet-sized column, where
// 60% still leaves the picture near its native resolution. On a 480 px column the same rule
// turned the riddle diagrams in "Strange Pictures" into ~270 px thumbnails. A percentage wrapper
// may therefore enlarge an image but never shrink it below min(native width, unwrapped column).
//
// What has to hold:
//   1. A large image in a percentage wrapper fills the column, as if unwrapped.
//   2. An image smaller than the column keeps its native width (the floor never upscales).
//   3. An absolute-width wrapper (<div style="width:100px">) still constrains, as before.
//   4. An image the publisher sized itself (px/em width) is left alone inside a percentage wrapper.
//   5. A floated percentage box keeps its width, or the text could no longer wrap beside it.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/ImageBlock.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

uint32_t crc32(const std::string& data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char c : data) {
    crc ^= c;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void putBe32(std::string& out, const uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 0xFF));
}

// Signature + IHDR is all the dimension probe reads; the image is never decoded here.
std::string pngHeader(const uint32_t width, const uint32_t height) {
  std::string png("\x89PNG\r\n\x1a\n", 8);
  std::string chunk = "IHDR";
  putBe32(chunk, width);
  putBe32(chunk, height);
  chunk += std::string("\x08\x00\x00\x00\x00", 5);  // 8-bit grayscale, deflate, no filter, no interlace
  putBe32(png, 13);
  png += chunk;
  putBe32(png, crc32(chunk));
  return png;
}

struct ImageWrapperWidthFixture : testing::Test {
  static constexpr int kViewportWidth = 480;
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_imgwrap_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // One chapter, one <img> per test so the page order cannot confuse which width is which.
  std::string makeBook(const std::string& body) {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">wrap</dc:identifier>"
            "<dc:title>Wrap</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n"
            "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/>\n"
            "<item id=\"big\" href=\"big.png\" media-type=\"image/png\"/>\n"
            "<item id=\"small\" href=\"small.png\" media-type=\"image/png\"/>\n"
            "</manifest>\n<spine><itemref idref=\"c\"/></spine>\n</package>\n");
    zip.add("style.css",
            "div.full60 { width: 60%; margin: 0.5em auto; text-align: center; }\n"
            "div.box { width: 100px; }\n"
            "img { max-width: 100%; }\n"
            "img.sized { width: 50px; }\n"
            "div.side { float: left; width: 30%; }\n");
    zip.add("chapter.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
            "<head><title>C</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
                body + "\n</body></html>\n");
    zip.add("big.png", pngHeader(1200, 600));
    zip.add("small.png", pngHeader(120, 60));
    const std::string path = (work / "wrap.epub").string();
    zip.write(path);
    return path;
  }

  // Width of the single laid-out image, or -1 if the chapter produced none.
  int imageWidth(const std::string& body) {
    auto epub = std::make_shared<Epub>(makeBook(body), (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->loadImageManifest();
    Section::BuildParams params;
    params.viewportWidth = kViewportWidth;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;  // the wrapper widths ARE publisher CSS
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      for (const auto& el : page->elements) {
        if (el->getTag() == TAG_PageImage) return static_cast<const PageImage&>(*el).getImageBlock().getWidth();
      }
    }
    return -1;
  }
};

TEST_F(ImageWrapperWidthFixture, UnwrappedLargeImageFillsTheColumn) {
  // Baseline the other cases are measured against: whatever the column is on this build.
  EXPECT_EQ(imageWidth("<div><img src=\"big.png\" alt=\"\"/></div>"), kViewportWidth);
}

TEST_F(ImageWrapperWidthFixture, PercentWrapperNoLongerShrinksALargeImage) {
  EXPECT_EQ(imageWidth("<div class=\"full60\"><img src=\"big.png\" alt=\"\"/></div>"), kViewportWidth)
      << "60% of a 480 px column is a thumbnail; the wrapper must not shrink below the column";
}

TEST_F(ImageWrapperWidthFixture, PercentWrapperKeepsASmallImageNative) {
  EXPECT_EQ(imageWidth("<div class=\"full60\"><img src=\"small.png\" alt=\"\"/></div>"), 120)
      << "the floor is min(native, column): it must never upscale";
}

TEST_F(ImageWrapperWidthFixture, AbsoluteWrapperStillConstrains) {
  EXPECT_EQ(imageWidth("<div class=\"box\"><img src=\"big.png\" alt=\"\"/></div>"), 100);
}

TEST_F(ImageWrapperWidthFixture, PercentWrapperInsideAbsoluteBoxFloorsToTheBox) {
  EXPECT_EQ(imageWidth("<div class=\"box\"><div class=\"full60\"><img src=\"big.png\" alt=\"\"/></div></div>"), 100);
}

TEST_F(ImageWrapperWidthFixture, PublisherSizedImageIsLeftAlone) {
  EXPECT_EQ(imageWidth("<div class=\"full60\"><img class=\"sized\" src=\"big.png\" alt=\"\"/></div>"), 50);
}

TEST_F(ImageWrapperWidthFixture, FloatedPercentBoxKeepsItsWidth) {
  const int w =
      imageWidth("<div class=\"side\"><img src=\"big.png\" alt=\"\"/></div><p>Text that wraps beside the figure.</p>");
  EXPECT_GT(w, 0);
  EXPECT_LE(w, kViewportWidth * 30 / 100 + 1) << "a floated figure must keep room for the text beside it";
}

}  // namespace
