#pragma once

// src/BookRelocation.h for WebDAVHandlerTest: the moves it is told about, and a cache folder per book, named as the
// Epub, Xtc and Txt stubs' caches are, that a test can put on the card.

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace BookRelocation {

inline std::vector<std::pair<std::string, std::string>>& movesSeen() {
  static std::vector<std::pair<std::string, std::string>> moves;
  return moves;
}

inline std::string cacheDirFor(const std::string& path) {
  const auto endsWith = [&path](const char* ext) {
    const std::string e(ext);
    return path.size() >= e.size() && path.compare(path.size() - e.size(), e.size(), e) == 0;
  };
  const char* kind = endsWith(".epub") ? "epub" : endsWith(".xtc") ? "xtc" : endsWith(".txt") ? "txt" : nullptr;
  return kind ? std::string("/.crosspoint/") + kind + "_" + std::to_string(std::hash<std::string>{}(path)) : "";
}

inline void moved(const std::string& oldPath, const std::string& newPath) {
  movesSeen().emplace_back(oldPath, newPath);
}

}  // namespace BookRelocation
