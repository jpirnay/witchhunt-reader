// src/BookRelocation.cpp for WebDAVHandlerTest, whose real one reaches the recent list, the bookmark index and the
// reading stats. A cache folder per book, named as the readers name theirs; a move changes nothing else here.

#include "BookRelocation.h"

#include <functional>
#include <string>

namespace BookRelocation {

std::string cacheDirFor(const std::string& path) {
  const auto endsWith = [&path](const char* ext) {
    const std::string e(ext);
    return path.size() >= e.size() && path.compare(path.size() - e.size(), e.size(), e) == 0;
  };
  const char* kind = endsWith(".epub")                       ? "epub"
                     : endsWith(".xtc") || endsWith(".xtch") ? "xtc"
                     : endsWith(".txt") || endsWith(".md")   ? "txt"
                                                             : nullptr;
  return kind ? std::string("/.crosspoint/") + kind + "_" + std::to_string(std::hash<std::string>{}(path)) : "";
}

void moved(const std::string&, const std::string&) {}

}  // namespace BookRelocation
