#include "WebBookCache.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "BookRelocation.h"

namespace {

// A cover sidecar ("book.jpg" beside "book.epub") was written: the book's thumbnails were drawn from
// the old cover. They are rebuilt only when the sidecar is newer than them, which a reader without a
// set clock cannot tell, and the single-height thumb_<H>.bmp some Home themes draw is never compared
// at all (docs/sidecar-files.md). Deleting them makes every view draw the new cover.
void dropThumbnailsForCover(const String& imagePath) {
  static constexpr const char* kCoverExtensions[] = {".jpg", ".jpeg", ".png", ".bmp"};
  static constexpr const char* kBookExtensions[] = {".epub", ".xtc", ".xtch", ".txt", ".md"};
  String lower = imagePath;
  lower.toLowerCase();
  const char* const* coverExt = std::find_if(std::begin(kCoverExtensions), std::end(kCoverExtensions),
                                             [&lower](const char* ext) { return lower.endsWith(ext); });
  if (coverExt == std::end(kCoverExtensions)) return;
  const std::string base(imagePath.c_str(), imagePath.length() - strlen(*coverExt));

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
    LOG_DBG("WEB", "New cover for %s: dropped %u cached cover images", bookPath.c_str(),
            static_cast<unsigned>(stale.size()));
  }
}

}  // namespace

void clearBookCacheIfNeeded(const String& filePath) {
  dropThumbnailsForCover(filePath);
  if (FsHelpers::hasEpubExtension(filePath)) {
    Epub(filePath.c_str(), "/.crosspoint").clearCache();
    LOG_DBG("WEB", "Cleared epub cache for: %s", filePath.c_str());
  } else if (FsHelpers::hasXtcExtension(filePath)) {
    Xtc(filePath.c_str(), "/.crosspoint").clearCache();
    LOG_DBG("WEB", "Cleared xtc cache for: %s", filePath.c_str());
  } else if (FsHelpers::hasTxtExtension(filePath) || FsHelpers::hasMarkdownExtension(filePath)) {
    const Txt txt(filePath.c_str(), "/.crosspoint");
    const String cachePath = txt.getCachePath().c_str();
    if (Storage.exists(cachePath.c_str())) {
      Storage.removeDir(cachePath.c_str());
      LOG_DBG("WEB", "Cleared txt cache for: %s", filePath.c_str());
    }
  }
}
