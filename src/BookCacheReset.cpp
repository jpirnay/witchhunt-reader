#include "BookCacheReset.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string_view>
#include <vector>

#include "BookRelocation.h"

namespace {

// A cover sidecar was written or removed: the book's thumbnails were drawn from the old cover. They
// are rebuilt only when the sidecar is newer than them, which a reader without a set clock cannot
// tell, and the single-height thumb_<H>.bmp some Home themes draw is never compared at all
// (docs/sidecar-files.md). Deleting them makes every view draw the new cover.
void dropThumbnailsForCover(const std::string& imagePath) {
  static constexpr const char* kCoverExtensions[] = {".jpg", ".jpeg", ".png", ".bmp"};
  static constexpr const char* kBookExtensions[] = {".epub", ".xtc", ".xtch", ".txt", ".md"};
  const char* coverExt = nullptr;
  for (const char* ext : kCoverExtensions) {
    if (FsHelpers::checkFileExtension(std::string_view(imagePath), ext)) coverExt = ext;
  }
  if (coverExt == nullptr) return;
  const std::string base = imagePath.substr(0, imagePath.size() - strlen(coverExt));

  for (const char* bookExt : kBookExtensions) {
    const std::string bookPath = base + bookExt;
    if (!Storage.exists(bookPath.c_str())) continue;
    const std::string cacheDir = BookRelocation::cacheDirFor(bookPath);
    FsFile dir = Storage.open(cacheDir.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    std::vector<std::string> stale;
    char name[64];
    for (FsFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(name, sizeof(name));
      entry.close();
      const size_t len = strlen(name);
      const bool thumb = strncmp(name, "thumb_", 6) == 0 && len > 4 && strcmp(name + len - 4, ".bmp") == 0;
      if (thumb || strcmp(name, "cover.bmp") == 0) stale.emplace_back(cacheDir + "/" + name);
    }
    dir.close();
    for (const std::string& file : stale) Storage.remove(file.c_str());
    LOG_DBG("BCR", "New cover for %s: dropped %u cached cover images", bookPath.c_str(),
            static_cast<unsigned>(stale.size()));
  }
}

}  // namespace

void clearBookCacheIfNeeded(const std::string& path, const BookCache::Keep keep) {
  dropThumbnailsForCover(path);
  // Empty for a file that is not a book. The cache folder as each reader names it, and the clear
  // every cache clear goes through (BookCache): what Epub::clearCache() and Xtc::clearCache() do,
  // and for TXT and Markdown the reading state is kept when asked, as for the others.
  const std::string cacheDir = BookRelocation::cacheDirFor(path);
  if (cacheDir.empty()) return;
  if (BookCache::clear(cacheDir, keep)) {
    LOG_DBG("BCR", "Cleared the cache of %s", path.c_str());
  } else {
    LOG_ERR("BCR", "Could not clear the cache of %s", path.c_str());
  }
}
