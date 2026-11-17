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

uint32_t metadataStamp(const std::string& bookPath) {
  const std::string path = metadataPath(bookPath);
  if (path.empty()) return 0;

  HalFile file;
  if (!Storage.openFileForRead("SIDECAR", path, file)) return 0;
  const size_t size = file.fileSize();

  // FNV-1a over the size, then the contents up to the cap.
  uint32_t hash = 2166136261u;
  const auto mix = [&hash](uint8_t byte) {
    hash ^= byte;
    hash *= 16777619u;
  };
  for (size_t i = 0; i < sizeof(size); i++) mix(static_cast<uint8_t>(size >> (8 * i)));

  uint8_t buf[128];
  size_t remaining = size < kMetadataStampBytes ? size : kMetadataStampBytes;
  while (remaining > 0) {
    const int got = file.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (got <= 0) break;
    for (int i = 0; i < got; i++) mix(buf[i]);
    remaining -= static_cast<size_t>(got);
  }
  file.close();
  return hash != 0 ? hash : 1;
}

std::vector<const char*> existingExtensions(const std::string& bookPath) {
  std::vector<const char*> found;
  const std::string base = basePath(bookPath);
  if (base.empty()) return found;

  struct Table {
    const char* const* extensions;
    size_t count;
  };
  constexpr Table tables[] = {
      {kCoverExtensions, sizeof(kCoverExtensions) / sizeof(kCoverExtensions[0])},
      {kMetadataExtensions, sizeof(kMetadataExtensions) / sizeof(kMetadataExtensions[0])},
      {kKeyExtensions, sizeof(kKeyExtensions) / sizeof(kKeyExtensions[0])},
  };
  // Small and bounded by the tables above; reserve once rather than grow.
  size_t total = 0;
  for (const Table& table : tables) total += table.count;
  found.reserve(total);

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

  for (const Table& table : tables) {
    for (size_t i = 0; i < table.count; i++) {
      const char* ext = table.extensions[i];
      if (alreadyMatched(ext)) continue;
      if (Storage.exists((base + ext).c_str())) found.push_back(ext);
    }
  }
  return found;
}

bool anyTargetTaken(const std::string& bookPath, const std::string& targetBookPath) {
  const std::string dstBase = basePath(targetBookPath);
  if (dstBase.empty()) return false;
  for (const char* ext : existingExtensions(bookPath)) {
    if (Storage.exists((dstBase + ext).c_str())) return true;
  }
  return false;
}

bool moveAll(const std::string& bookPath, const std::string& targetBookPath) {
  const std::string srcBase = basePath(bookPath);
  const std::string dstBase = basePath(targetBookPath);
  if (srcBase.empty() || dstBase.empty()) return false;
  bool success = true;
  for (const char* ext : existingExtensions(bookPath)) {
    if (!Storage.rename((srcBase + ext).c_str(), (dstBase + ext).c_str())) success = false;
  }
  return success;
}

bool removeAll(const std::string& bookPath) {
  const std::string base = basePath(bookPath);
  if (base.empty()) return true;  // no extension, so nothing can share its name
  bool success = true;
  for (const char* ext : existingExtensions(bookPath)) {
    if (!Storage.remove((base + ext).c_str())) success = false;
  }
  return success;
}

}  // namespace SidecarFiles
