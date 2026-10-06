#pragma once
// A one-chapter EPUB built from a <body> fragment: STORED zip at work/book<index>.epub, loaded,
// cache directory set up. One file per book: the cache is keyed on the path.
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "Epub.h"
#include "StoredZipWriter.h"

inline std::shared_ptr<Epub> syntheticBook(const std::filesystem::path& work, const int index,
                                           const std::string& body) {
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
          "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
          "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
  zip.add("content.opf",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
          "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
          "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">lut</dc:identifier>"
          "<dc:title>LUT</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n"
          "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
          "</manifest>\n<spine><itemref idref=\"c\"/></spine>\n</package>\n");
  zip.add("chapter.xhtml",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
          "<head><title>C</title></head><body>\n" +
              body + "\n</body></html>\n");
  // One file per book: the cache directory is keyed on the path, so two books at one path would
  // share a section file and the second build would overwrite the first one's LUT.
  const std::string path = (work / ("book" + std::to_string(index) + ".epub")).string();
  zip.write(path);
  auto epub = std::make_shared<Epub>(path, (work / "cache").string());
  EXPECT_TRUE(epub->load(true));
  epub->setupCacheDir();
  return epub;
}
