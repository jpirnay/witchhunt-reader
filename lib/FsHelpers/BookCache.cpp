#include "BookCache.h"

#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>

#include "FsHelpers.h"

namespace BookCache {

bool isReadingState(const std::string_view name) { return name == "progress.bin" || name == "bookmarks.bin"; }

bool isCoverImage(const std::string_view name) {
  // cover.img is the cover image as the book stores it, before it is converted to a BMP.
  return FsHelpers::hasBmpExtension(name) || name == "cover.img";
}

bool clear(const std::string& cacheDir, const Keep keep, const bool keepCovers) {
  if (!Storage.exists(cacheDir.c_str())) return true;

  if (keep == Keep::Nothing && !keepCovers) {
    if (Storage.removeDir(cacheDir.c_str())) return true;
    LOG_ERR("BKC", "Failed to remove %s", cacheDir.c_str());
    return false;
  }

  HalFile dir = Storage.open(cacheDir.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("BKC", "Failed to open %s for a selective clear", cacheDir.c_str());
    if (dir) dir.close();
    return false;
  }

  char name[128];
  bool allRemoved = true;
  bool anyKept = false;
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    HalSystem::feedWatchdog();
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    entry.close();
    if ((keep == Keep::ReadingState && isReadingState(name)) || (keepCovers && isCoverImage(name))) {
      anyKept = true;
      continue;
    }
    // A subdirectory (spines/, img/, fonts/) goes as a whole: a plain file remove refuses one.
    const std::string path = cacheDir + "/" + name;
    if (!(isDir ? Storage.removeDir(path.c_str()) : Storage.remove(path.c_str()))) {
      LOG_ERR("BKC", "Failed to remove %s", path.c_str());
      allRemoved = false;
    }
  }
  dir.close();

  // A book never opened has no reading state, and a folder with nothing in it is no use to keep.
  if (!anyKept && !Storage.removeDir(cacheDir.c_str())) {
    LOG_ERR("BKC", "Failed to remove %s", cacheDir.c_str());
    allRemoved = false;
  }
  return allRemoved;
}

}  // namespace BookCache
