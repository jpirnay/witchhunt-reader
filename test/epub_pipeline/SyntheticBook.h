#pragma once
// A one-chapter EPUB built from a <body> fragment (syntheticBook) or a whole chapter document
// (syntheticBookFromDocument), plus any extra files the chapter refers to: STORED zip at
// work/book<index>.epub, loaded, cache directory set up. One file per book: the cache is keyed on
// the path.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "StoredZipWriter.h"

// A file beside chapter.xhtml, listed in the manifest.
struct SyntheticFile {
  std::string href;
  std::string mediaType;
  std::string bytes;
};

inline std::shared_ptr<Epub> syntheticBookFromDocument(const std::filesystem::path& work, const int index,
                                                       const std::string& xhtml,
                                                       const std::vector<SyntheticFile>& extra = {}) {
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
          "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
          "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
  std::string manifest = "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
  for (size_t i = 0; i < extra.size(); ++i) {
    manifest += "<item id=\"x" + std::to_string(i) + "\" href=\"" + extra[i].href + "\" media-type=\"" +
                extra[i].mediaType + "\"/>\n";
  }
  zip.add("content.opf",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
          "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
          "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">lut</dc:identifier>"
          "<dc:title>LUT</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n" +
              manifest + "</manifest>\n<spine><itemref idref=\"c\"/></spine>\n</package>\n");
  zip.add("chapter.xhtml", xhtml);
  for (const auto& file : extra) zip.add(file.href, file.bytes);
  // One file per book: the cache directory is keyed on the path, so two books at one path would
  // share a section file and the second build would overwrite the first one's LUT.
  const std::string path = (work / ("book" + std::to_string(index) + ".epub")).string();
  zip.write(path);
  auto epub = std::make_shared<Epub>(path, (work / "cache").string());
  EXPECT_TRUE(epub->load(true));
  epub->setupCacheDir();
  return epub;
}

inline std::shared_ptr<Epub> syntheticBook(const std::filesystem::path& work, const int index, const std::string& body,
                                           const std::vector<SyntheticFile>& extra = {}) {
  return syntheticBookFromDocument(work, index,
                                   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html "
                                   "xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C</title></head><body>\n" +
                                       body + "\n</body></html>\n",
                                   extra);
}

// A PNG's signature and IHDR, all the layout's dimension probe reads (the image is never decoded
// while laying out). 8-bit grayscale.
inline std::string syntheticPngHeader(const uint32_t width, const uint32_t height) {
  const auto putBe32 = [](std::string& out, const uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 0xFF));
  };
  std::string chunk = "IHDR";
  putBe32(chunk, width);
  putBe32(chunk, height);
  chunk += std::string("\x08\x00\x00\x00\x00", 5);  // 8-bit grayscale, deflate, no filter, no interlace
  uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char c : chunk) {
    crc ^= c;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  std::string png("\x89PNG\r\n\x1a\n", 8);
  putBe32(png, 13);
  png += chunk;
  putBe32(png, ~crc);
  return png;
}
