// Out-of-line stubs for BookMetadataCache and PageListSink methods referenced
// by TocNcxParser. The benchmark passes nullptr for both so these never execute.
#include "../../lib/Epub/Epub/BookMetadataCache.h"
#include "../../lib/Epub/Epub/parsers/PageListSink.h"

void BookMetadataCache::createTocEntry(const std::string&, const std::string&, const std::string&, uint8_t) {}

PageListSink::PageListSink(const std::string&) {}
PageListSink::~PageListSink() {}
void PageListSink::addEntry(const std::string&, const std::string&, const std::string&) {}
void PageListSink::finalize() {}
