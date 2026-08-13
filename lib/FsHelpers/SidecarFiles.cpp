#include "SidecarFiles.h"

#include <HalStorage.h>

#include <cctype>

namespace SidecarFiles {
namespace {

// Local rather than strcasecmp/_stricmp, which differ by platform and would
// need a portability shim for a four-character comparison.
bool sameExtensionIgnoringCase(const char* a, const char* b) {
  while (*a && *b) {
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

// First extension in `exts` for which "<base><ext>" exists on the card.
std::string firstExisting(const std::string& bookPath, const char* const* exts, size_t count) {
  const std::string base = basePath(bookPath);
  if (base.empty()) return "";
  for (size_t i = 0; i < count; i++) {
    std::string candidate = base + exts[i];
    if (Storage.exists(candidate.c_str())) return candidate;
  }
  return "";
}

}  // namespace

std::string basePath(const std::string& bookPath) {
  const auto sep = bookPath.find_last_of("/\\");
  const auto dot = bookPath.rfind('.');
  // No dot at all, or the only dot belongs to a parent directory.
  if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return "";
  return bookPath.substr(0, dot);
}

std::string coverPath(const std::string& bookPath) {
  return firstExisting(bookPath, kCoverExtensions, sizeof(kCoverExtensions) / sizeof(kCoverExtensions[0]));
}

std::string metadataPath(const std::string& bookPath) {
  return firstExisting(bookPath, kMetadataExtensions, sizeof(kMetadataExtensions) / sizeof(kMetadataExtensions[0]));
}

std::vector<const char*> existingExtensions(const std::string& bookPath) {
  std::vector<const char*> found;
  const std::string base = basePath(bookPath);
  if (base.empty()) return found;

  // Small and bounded by the tables above; reserve once rather than grow.
  found.reserve(sizeof(kCoverExtensions) / sizeof(kCoverExtensions[0]) +
                sizeof(kMetadataExtensions) / sizeof(kMetadataExtensions[0]));

  // The tables list each extension in both cases, but the SD card is FAT/exFAT
  // and answers to either - so "book.jpg" matches ".jpg" AND ".JPG", and
  // reporting both would have a caller that moves them rename the same file
  // twice, the second failing because the first already moved it. Report the
  // first case that matched and skip its variants. On a case-sensitive
  // filesystem nothing is skipped that did not genuinely match first.
  const auto alreadyMatched = [&found](const char* ext) {
    for (const char* seen : found) {
      if (sameExtensionIgnoringCase(seen, ext)) return true;
    }
    return false;
  };

  for (const char* const* table : {kCoverExtensions, kMetadataExtensions}) {
    const size_t count = (table == kCoverExtensions) ? sizeof(kCoverExtensions) / sizeof(kCoverExtensions[0])
                                                     : sizeof(kMetadataExtensions) / sizeof(kMetadataExtensions[0]);
    for (size_t i = 0; i < count; i++) {
      const char* ext = table[i];
      if (alreadyMatched(ext)) continue;
      if (Storage.exists((base + ext).c_str())) found.push_back(ext);
    }
  }
  return found;
}

}  // namespace SidecarFiles
