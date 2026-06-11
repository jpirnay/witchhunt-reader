#pragma once
#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <string>

class PageListSink;

// Parser for EPUB 2.01 page-map.xml. Each <page name="X" href="...#anchor"/> element
// maps a printed page number to a spine location. Same output shape as TocNcxParser
// and TocNavParser so all three feed the shared pagelist.bin writer.
class PageMapParser final : public Print {
 private:
  const std::string& baseContentPath;
  size_t remainingSize;
  SaxParser saxParser_;
  // Page-list entries are streamed straight to disk via this sink. Owned by
  // the caller (Epub.cpp); may be null when no page-list output is wanted.
  PageListSink* pageListSink;

  static void startElement(void* userData, const char* name, const char** atts);

 public:
  explicit PageMapParser(const std::string& baseContentPath, const size_t xmlSize, PageListSink* pageListSink)
      : baseContentPath(baseContentPath), remainingSize(xmlSize), pageListSink(pageListSink) {}
  ~PageMapParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
