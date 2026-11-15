#pragma once

// src/BookRelocation.h for WebDAVHandlerTest: the moves it is told about.

#include <string>
#include <utility>
#include <vector>

namespace BookRelocation {

inline std::vector<std::pair<std::string, std::string>>& movesSeen() {
  static std::vector<std::pair<std::string, std::string>> moves;
  return moves;
}

inline void moved(const std::string& oldPath, const std::string& newPath) {
  movesSeen().emplace_back(oldPath, newPath);
}

}  // namespace BookRelocation
