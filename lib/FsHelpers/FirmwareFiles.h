#pragma once

#include <string_view>

#include "ProtectedPaths.h"

// What the firmware keeps at the card root for itself: never a reader's book, though a .txt or .md
// file among it opens in the text reader. A font's OFL.txt or a dictionary's README showed in the
// Library as a book, and the crash report as the newest book of all after a crash.
//
// The Library index, Recent books and the next-book suggestion leave it out, by this one rule.
// Browse Files still lists it all, so the crash report is found where the firmware says it is.
// The folders that hold no books at any depth (.crosspoint, the system folders) are
// ProtectedPaths::holdsNoBooks, which the book listings apply as well.
//
// Root folders after crosspoint-reader PR #4373, the crash report after PR #4471 (both Georg
// Kaindl) and matcha-reader PRs #783 and #802 (Eszter Schuffert).
namespace FirmwareFiles {

// Written after a panic (HalSystem::checkPanic, which spells it out: lib/hal does not include this),
// at the root where a reader looking for it finds it.
inline constexpr const char* CRASH_REPORT_PATH = "/crash_report.txt";

// The root folders the firmware reads its own resources from: fonts and dictionaries (the hidden
// spellings too), plugins, custom sleep images, and the screenshots it writes.
inline constexpr const char* ROOT_FOLDERS[] = {"fonts",    ".fonts", "dictionaries", ".dictionaries", "plugins",
                                               ".plugins", "sleep",  ".sleep",       "screenshots"};

// An entry named `name` directly in the card root that is the firmware's. FAT names are
// case-insensitive, so the match is too.
inline bool isRootEntry(const std::string_view name, const bool isDir) {
  if (!isDir) return ProtectedPaths::equalsIgnoreCase(name, CRASH_REPORT_PATH + 1);  // its name, past the root's '/'
  for (const char* folder : ROOT_FOLDERS) {
    if (ProtectedPaths::equalsIgnoreCase(name, folder)) return true;
  }
  return false;
}

// An entry named `name` in the folder `folderPath` ("/" for the root, "/fonts/Inter" below it) that
// is the firmware's: one of the root's own, or anything inside a root folder of the firmware's.
inline bool isEntry(std::string_view folderPath, const std::string_view name, const bool isDir) {
  while (!folderPath.empty() && folderPath.front() == '/') folderPath.remove_prefix(1);
  if (folderPath.empty()) return isRootEntry(name, isDir);
  return isRootEntry(folderPath.substr(0, folderPath.find('/')), /*isDir=*/true);
}

// The same for a file's absolute path, as the firmware spells it ("/crash_report.txt").
inline bool isFirmwareFile(const std::string_view path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) return false;
  return isEntry(path.substr(0, slash), path.substr(slash + 1), /*isDir=*/false);
}

}  // namespace FirmwareFiles
