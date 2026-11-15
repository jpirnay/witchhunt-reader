#pragma once

// lib/Txt/Txt.h for WebDAVHandlerTest. The handler removes the folder getCachePath() names itself; this one names a
// folder per book that a test can put on the card.

#include <functional>
#include <string>
#include <utility>

class Txt {
 public:
  explicit Txt(std::string path, std::string cacheBasePath)
      : cachePath_(std::move(cacheBasePath) + "/txt_" + std::to_string(std::hash<std::string>{}(path))) {}
  const std::string& getCachePath() const { return cachePath_; }

 private:
  std::string cachePath_;
};
