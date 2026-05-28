#pragma once

#include <cstdint>
#include <string>

// Host-build stub: the benchmark passes nullptr for cache so no methods are
// ever called; we only need the class to exist so TocNcxParser.cpp compiles.
class BookMetadataCache {
 public:
  void createTocEntry(const std::string&, const std::string&, const std::string&, uint8_t) {}
};
