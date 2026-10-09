#include "FolderSearch.h"

#include <HalStorage.h>

#include <vector>

namespace FolderSearch {

bool anyBelow(const std::string& dirPath, const Rules& rules) {
  const auto knownOf = [&rules](const std::string& path) {
    return rules.known != nullptr ? rules.known(rules.user, path) : -1;
  };
  if (const int known = knownOf(dirPath); known >= 0) return known > 0;

  struct Level {
    HalFile dir;
    std::string path;
    bool complete = true;  // nothing below was cut off by maxDepth or left unopened
  };
  std::vector<Level> levels;
  levels.reserve(static_cast<size_t>(rules.maxDepth) + 1);  // references into it stay valid while it grows
  {
    auto root = Storage.open(dirPath.c_str());
    if (!root || !root.isDirectory()) return false;
    root.rewindDirectory();
    levels.push_back(Level{std::move(root), dirPath});
  }
  char name[500];
  while (true) {
    if (rules.stop != nullptr && rules.stop(rules.user)) return true;  // the levels close their directories
    Level& level = levels.back();
    auto entry = level.dir.openNextFile();
    if (!entry) {
      level.dir.close();
      const bool complete = level.complete;
      if (complete && rules.foundNone != nullptr) rules.foundNone(rules.user, level.path);
      levels.pop_back();
      if (levels.empty()) return false;
      levels.back().complete = levels.back().complete && complete;
      continue;
    }
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    entry.close();
    if (rules.listable != nullptr && !rules.listable(name)) continue;
    if (!isDir) {
      if (rules.wanted != nullptr && rules.wanted(name)) return true;
      continue;
    }
    std::string path = level.path;
    if (path.empty() || path.back() != '/') path += '/';
    path += name;
    if (const int known = knownOf(path); known >= 0) {
      if (known > 0) return true;
      continue;
    }
    if (static_cast<int>(levels.size()) > rules.maxDepth) {
      level.complete = false;
      continue;
    }
    auto sub = Storage.open(path.c_str());
    if (!sub || !sub.isDirectory()) {
      level.complete = false;
      continue;
    }
    sub.rewindDirectory();
    levels.push_back(Level{std::move(sub), std::move(path)});
  }
}

}  // namespace FolderSearch
