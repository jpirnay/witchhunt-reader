#pragma once
#include <Print.h>
#include <expat.h>

#include <string>

class BookMetadataCache;
class PageListSink;

// Parser for EPUB 3 nav.xhtml navigation documents
// Parses HTML5 nav elements with epub:type="toc" (table of contents) and
// epub:type="page-list" (printed page list, EPUB 3 equivalent of NCX <pageList>).
class TocNavParser final : public Print {
  enum ParserState {
    START,
    IN_HTML,
    IN_BODY,
    IN_NAV_TOC,        // Inside <nav epub:type="toc">
    IN_OL,             // Inside <ol> (within toc nav)
    IN_LI,             // Inside <li> (within toc nav)
    IN_ANCHOR,         // Inside <a> (within toc nav)
    IN_NAV_PAGE_LIST,  // Inside <nav epub:type="page-list">
    IN_PL_OL,          // Inside <ol> (within page-list nav)
    IN_PL_LI,          // Inside <li> (within page-list nav)
    IN_PL_ANCHOR,      // Inside <a> (within page-list nav)
  };

 private:
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  // Page-list entries are streamed straight to disk via this sink. Owned by
  // the caller (Epub.cpp); may be null when no page-list output is wanted.
  PageListSink* pageListSink;

  // Track nesting depth for <ol> elements to determine TOC depth
  uint8_t olDepth = 0;
  // Current entry data being collected
  std::string currentLabel;
  std::string currentHref;

  // Page-list collection state (independent of TOC; structurally identical handlers but
  // a separate label/href pair so a malformed nav with overlapping navs cannot mix them).
  uint8_t plOlDepth = 0;
  std::string currentPageLabel;
  std::string currentPageHref;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  explicit TocNavParser(const std::string& baseContentPath, const size_t xmlSize, BookMetadataCache* cache,
                        PageListSink* pageListSink)
      : baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache), pageListSink(pageListSink) {}
  ~TocNavParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
