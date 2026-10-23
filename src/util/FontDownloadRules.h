#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "FontCatalog.h"

// What the Font Manager and the web font API check of a font download, apart from the card so the host can test it:
// which kind of file a manifest names, what a downloaded file of that kind starts with, whether the card has room for
// the download, and whether a family already on the card is being updated.
namespace FontDownloadRules {

enum class FileKind : uint8_t { Unknown, Cpfont, TrueType, OpenTypeCff };

// By the name's extension, in any case.
inline FileKind kindOf(const char* name) {
  const size_t len = name ? strlen(name) : 0;
  const auto endsWith = [&](const char* ext) {
    const size_t n = strlen(ext);
    if (len < n) return false;
    for (size_t i = 0; i < n; ++i) {
      char c = name[len - n + i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != ext[i]) return false;
    }
    return true;
  };
  if (endsWith(".cpfont")) return FileKind::Cpfont;
  if (endsWith(".ttf")) return FileKind::TrueType;
  if (endsWith(".otf")) return FileKind::OpenTypeCff;
  return FileKind::Unknown;
}

// By a manifest's `format` value: "cpfont", "ttf" or "otf", exactly.
inline FileKind kindOfFormat(const char* value, const size_t len) {
  if (!value) return FileKind::Unknown;
  const auto is = [&](const char* word) { return strlen(word) == len && memcmp(value, word, len) == 0; };
  if (is("cpfont")) return FileKind::Cpfont;
  if (is("ttf")) return FileKind::TrueType;
  if (is("otf")) return FileKind::OpenTypeCff;
  return FileKind::Unknown;
}

// The bytes headOk() reads.
constexpr size_t kHeadBytes = 8;

// Whether a file of `kind` starts as it must: a .cpfont with its magic, a .ttf with TrueType outlines (sfnt version
// 0x00010000 or 'true'), an .otf with CFF outlines ('OTTO'). A collection ('ttcf') and WOFF are files of no kind the
// reader draws.
inline bool headOk(const FileKind kind, const uint8_t* head, const size_t len) {
  if (len < kHeadBytes) return false;
  if (kind == FileKind::Cpfont) return memcmp(head, "CPFONT\0\0", kHeadBytes) == 0;
  if (kind == FileKind::TrueType) {
    return memcmp(head, "\x00\x01\x00\x00", 4) == 0 || memcmp(head, "true", 4) == 0;
  }
  if (kind == FileKind::OpenTypeCff) return memcmp(head, "OTTO", 4) == 0;
  return false;
}

// Room beyond the files themselves: the manifest and the font list's stash, written beside them, and the card's
// cluster slack on a family's files.
constexpr uint64_t kSpaceMarginBytes = 1024 * 1024;

// Whether a card with `freeBytes` free can take a download of `downloadBytes`. Checked before the first file, so a
// family the card cannot hold fails at once rather than at 90%.
constexpr bool fits(const uint64_t freeBytes, const uint64_t downloadBytes) {
  return freeBytes >= downloadBytes + kSpaceMarginBytes;
}

// Whether downloading a family is an update of what the card holds, from the counts of the manifest's files: `same`
// are on the card at their manifest size, `changed` at another size, `absent` missing. A folder holding none of them
// is someone else's family of that name, not an update.
// Two accepted limits: a reader's own file with a manifest file name but other bytes counts as an update and is
// replaced; a hosted family whose files are all renamed is never offered an update (never rename hosted files: ship a
// renamed set as a new family).
constexpr bool isUpdate(const size_t same, const size_t changed, const size_t absent) {
  return changed > 0 || (absent > 0 && same > 0);
}

// "Download all" on the device, and the web's "all" with updates: a family that is not on the card and says it
// belongs. An update to an installed family is the caller's `|| catalog.hasUpdate(family)`.
inline bool downloadAllTakes(const FontCatalog& catalog, const size_t family) {
  return !catalog.installed(family) && catalog.inDownloadAll(family);
}

}  // namespace FontDownloadRules
