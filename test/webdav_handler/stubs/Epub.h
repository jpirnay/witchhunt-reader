#pragma once

// lib/Epub/Epub.h for WebDAVHandlerTest: clearCache() is all the handler asks of it, and the test asks which book.

#include <string>
#include <utility>

#include "BookCacheLog.h"

class Epub {
 public:
  explicit Epub(std::string filepath, const std::string&) : filepath_(std::move(filepath)) {}
  bool clearCache() const {
    clearedBookCaches().push_back(filepath_);
    return true;
  }

 private:
  std::string filepath_;
};
