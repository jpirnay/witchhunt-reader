#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>

#include "util/FontCatalog.h"

/// Shared utility for font installation (device download + browser upload).
/// Handles directory creation, file validation, deletion, and registry refresh.
class FontInstaller {
 public:
  enum class Error {
    OK,
    INVALID_FAMILY_NAME,
    INVALID_FILE,
    SD_WRITE_ERROR,
    MAX_FAMILIES_REACHED,
  };

  explicit FontInstaller(SdCardFontRegistry& registry);

  // Must fit CrossPointSettings::sdFontFamilyName[32] including NUL.
  static constexpr size_t MAX_FAMILY_NAME_LEN = 31;

  /// Validate a family name: alphanumeric + hyphen + underscore only, no path traversal.
  static bool isValidFamilyName(const char* name);

  /// Validate a font file name as it appears in a manifest entry: non-empty, length-bounded,
  /// no absolute paths, no backslashes, no traversal components. Slashes are allowed for
  /// "<family>/<file>"-style entries.
  static bool isValidFontFileName(const char* name);

  /// Ensure /.crosspoint/fonts/<family>/ directory exists.
  bool ensureFamilyDir(const char* familyName);

  /// Validate a downloaded font file on disk by its kind (FontDownloadRules::kindOf and headOk): a .cpfont's magic, a
  /// .ttf's TrueType outlines, an .otf's CFF outlines. A file of another kind fails.
  bool validateFontFile(const char* path);

  /// Whether the card has room for a download of `bytes` (FontDownloadRules::fits); `freeBytes`, when given, gets
  /// what is free. Scans the FAT once per 20 s (SDCardManager caches it), so ask once per download, not per file.
  static bool hasRoomFor(uint64_t bytes, uint64_t* freeBytes = nullptr);

  /// Compute CRC32 of a file (esp_rom_crc32_le accumulator, matches
  /// zlib.crc32 used by scripts/generate-font-manifest.py). Returns false if
  /// the file cannot be opened. Mirrors the upstream PR #1904 approach so
  /// our manifests stay binary-compatible.
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);

  /// Build the full SD path for a font file.
  /// Writes "/.crosspoint/fonts/<family>/<filename>" to outBuf.
  static void buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize);

  /// Build the staging-dir path "<FONTS_DIR>/<family>__staging".
  static void buildStagingDirPath(const char* family, char* outBuf, size_t outBufSize);
  /// Build the backup-dir path "<FONTS_DIR>/<family>__backup".
  static void buildBackupDirPath(const char* family, char* outBuf, size_t outBufSize);

  /// Delete a family directory and all .cpfont files in it.
  /// If the deleted family is the active reader font, clears the setting.
  Error deleteFamily(const char* familyName);

  /// Re-run registry discovery to pick up new/removed fonts.
  void refreshRegistry();

  /// Check whether a family name already exists in the registry.
  bool isFamilyInstalled(const char* familyName) const;

  /// The family is on the card: the registry lists it, or its folder is there. An outline family the glyph store
  /// cannot hold is installed but not listed; the Font Manager must not offer it again.
  bool isFamilyOnCard(const char* familyName) const;

  /// Whether downloading `family` of `families` would update what the card holds: counts its files that are there at
  /// the manifest's size, at another size, or missing, and asks FontDownloadRules::isUpdate.
  static bool hasUpdate(const FontCatalog& families, size_t family);

  /// An outline family just installed that the registry does not list: the glyph store cannot hold it.
  bool installedButNotListed(const FontCatalog& families, size_t family) const;

 private:
  struct FileCounts {
    size_t same = 0;     // on the card at the manifest's size
    size_t changed = 0;  // on the card at another size
    size_t absent = 0;   // missing
  };
  static FileCounts countFiles(const FontCatalog& families, size_t family);

  SdCardFontRegistry& registry_;
};
