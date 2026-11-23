// lib/BookProtection/BookSession.cpp for WebDAVHandlerTest, whose real one reaches the ZIP reader's protection
// registry. Records each book closed, and whether its cache folder was still there at the time.

#include "BookSession.h"

#include <string>
#include <utility>
#include <vector>

#include "BookRelocation.h"
#include "HalStorage.h"

namespace book_protection_stub {
std::vector<std::pair<std::string, bool>> closed;
}  // namespace book_protection_stub

namespace book_protection {

void closeBook(const std::string& epubPath) {
  const std::string dir = BookRelocation::cacheDirFor(epubPath);
  book_protection_stub::closed.emplace_back(epubPath, !dir.empty() && Storage.exists(dir.c_str()));
}

}  // namespace book_protection
