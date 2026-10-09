#include "WebPathGuard.h"

#include <HalStorage.h>

#include <string>
#include <string_view>

namespace {

// Whether the entry at `prefix` exists under a real name other than `name`, i.e. the
// request reached it through its 8.3 alias. Only asked about alias-shaped segments,
// which nothing legitimate sends, so a name that cannot be read back (longer than the
// buffer) counts as an alias rather than as a reason to let the request through.
bool spelledByAlias(const std::string_view prefix, const std::string_view name) {
  const std::string path(prefix);
  HalFile entry = Storage.open(path.c_str());
  if (!entry) return false;  // nothing there yet: a new entry gets exactly this name
  char realName[64];
  const size_t len = entry.getName(realName, sizeof(realName));
  return len == 0 || !ProtectedPaths::equalsIgnoreCase(std::string_view(realName, len), name);
}

}  // namespace

bool isProtectedWebPath(const String& path, const ProtectedPaths::Target target, const bool allowHidden) {
  return ProtectedPaths::isProtectedPath(std::string_view(path.c_str(), path.length()), target, allowHidden,
                                         spelledByAlias);
}
