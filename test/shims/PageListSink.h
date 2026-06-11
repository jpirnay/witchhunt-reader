#pragma once

#include <string>

// Host-build stub: the benchmark passes nullptr for pageListSink so no methods
// are ever called; we only need the class to exist so TocNcxParser.cpp compiles.
class PageListSink {
 public:
  void addEntry(const std::string&, const std::string&, const std::string&) {}
};
