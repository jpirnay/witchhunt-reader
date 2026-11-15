#pragma once

// lib/Xtc/Xtc.h for WebDAVHandlerTest; see Epub.h here.

#include <string>
#include <utility>

#include "BookCacheLog.h"

class Xtc {
 public:
  explicit Xtc(std::string filepath, const std::string&) : filepath_(std::move(filepath)) {}
  bool clearCache() const {
    clearedBookCaches().push_back(filepath_);
    return true;
  }

 private:
  std::string filepath_;
};
