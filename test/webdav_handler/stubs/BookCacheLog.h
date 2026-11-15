#pragma once

// The books whose caches the Epub, Xtc and Txt stubs were told to clear, in order, for WebDAVHandlerTest.

#include <string>
#include <vector>

inline std::vector<std::string>& clearedBookCaches() {
  static std::vector<std::string> paths;
  return paths;
}
