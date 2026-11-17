#include "WebPathGuard.h"

#include <HalStorage.h>
#include <HalSystem.h>  // feedWatchdog()

#include <cstdint>
#include <string>
#include <string_view>

namespace {

// Whether the entry at `prefix` exists under a real name other than `name`, i.e. the request reached it through its
// 8.3 alias.
//
// The handle the alias opens cannot say. SdFat matches an alias against the short entry, after the long name before
// it failed to match, and opens the entry without its long name (FatFile::open, FatFileLFN.cpp:340-352 and :447), so
// getName() on that handle gives the alias back. Taking that for the real name let /CROSSP~1/wifi.json through on
// every board. A handle from a folder walk (openNextFile, FatFile::openNext) does carry the long name, so the parent
// folder is walked for the entry at the same directory index, the way HalStorage::areSameFile tells two spellings of
// one entry apart.
//
// Only asked about alias-shaped segments, which nothing legitimate sends, so it fails closed: a walk that cannot be
// done or does not find the entry, or a name that does not fit, counts as an alias.
bool spelledByAlias(const std::string_view prefix, const std::string_view name) {
  const std::string path(prefix);
  HalFile entry = Storage.open(path.c_str());
  if (!entry) return false;  // nothing there yet: a new entry gets exactly this name
  const uint32_t index = entry.dirIndex();
  entry.close();

  const size_t slash = path.find_last_of('/');
  const std::string parentPath = slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
  HalFile parent = Storage.open(parentPath.c_str());
  if (!parent || !parent.isDirectory()) return true;
  // An alias-shaped segment fits 8.3, at most 12 bytes: a real name too long for this cannot be it.
  char realName[16];
  for (HalFile child = parent.openNextFile(); child; child = parent.openNextFile()) {
    HalSystem::feedWatchdog();
    const bool same = child.dirIndex() == index;
    const size_t len = same ? child.getName(realName, sizeof(realName)) : 0;
    child.close();
    if (same) return len == 0 || !ProtectedPaths::equalsIgnoreCase(std::string_view(realName, len), name);
  }
  return true;
}

}  // namespace

bool isProtectedWebPath(const String& path, const ProtectedPaths::Target target, const bool allowHidden) {
  return ProtectedPaths::isProtectedPath(std::string_view(path.c_str(), path.length()), target, allowHidden,
                                         spelledByAlias);
}

// Judged as a folder walked through with hidden names allowed: that keeps every rule but the two about dot names,
// which a token file such as /.crosspoint/.readest-token.json would otherwise fail.
bool isProtectedPluginFile(const String& path) {
  return path.isEmpty() || path == "/" || path.endsWith("/") ||
         ProtectedPaths::isProtectedPath(std::string_view(path.c_str(), path.length()),
                                         ProtectedPaths::Target::Directory, true, spelledByAlias);
}
