#pragma once
#include <Print.h>
#include <expat.h>

#include <string>

class BookMetadataCache;
class PageListSink;

class TocNcxParser final : public Print {
  enum ParserState {
    START,
    IN_NCX,
    IN_NAV_MAP,
    IN_NAV_POINT,
    IN_NAV_LABEL,
    IN_NAV_LABEL_TEXT,
    IN_CONTENT,
    IN_PAGE_LIST,
    IN_PAGE_TARGET,
    IN_PAGE_TARGET_LABEL,
    IN_PAGE_TARGET_LABEL_TEXT,
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

  std::string currentLabel;
  std::string currentSrc;
  uint8_t currentDepth = 0;

  // <pageList> collection state
  std::string currentPageLabel;
  std::string currentPageSrc;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  explicit TocNcxParser(const std::string& baseContentPath, const size_t xmlSize, BookMetadataCache* cache,
                        PageListSink* pageListSink)
      : baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache), pageListSink(pageListSink) {}
  ~TocNcxParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
