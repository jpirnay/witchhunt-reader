#pragma once

#include <ZipFile.h>

#include <string>
#include <vector>

// Per-image entry cached at epub-open time so the parser can look up dimensions
// and ZIP location without a nested inflate during XHTML streaming.
struct ImageManifestEntry {
  std::string epubEntryPath;  // e.g. "OEBPS/images/foo.jpg"
  int16_t width = 0;
  int16_t height = 0;
  // ZipFile stat — avoids re-scanning the central directory at render time
  uint16_t method = 0;
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint32_t localHeaderOffset = 0;
  // Stable extracted path shared across all spines and render configs:
  // {cachePath}/img/{basename}  e.g. "/.crosspoint/epub_HASH/img/foo.jpg"
  std::string extractedPath;
};

class EpubImageManifest {
 public:
  static constexpr uint8_t VERSION = 1;

  // Load images.bin from cachePath. Returns true when loaded successfully.
  bool load(const std::string& cachePath);

  // True when images.bin already exists for this cachePath, i.e. loadImageManifest()
  // won't have to scan the ZIP to rebuild it. Cheap (a single Storage.exists).
  static bool cacheExists(const std::string& cachePath);

  // Build images.bin by scanning zf (must have loadAllFileStatSlims() already called)
  // and reading each image header from the zip. epubPath is the on-disk epub file path.
  bool build(const std::string& cachePath, const std::string& epubPath, ZipFile& zf);

  bool isLoaded() const { return loaded_; }

  // Returns nullptr when the entry is not in the manifest.
  const ImageManifestEntry* find(const std::string& epubEntryPath) const;

 private:
  std::vector<ImageManifestEntry> entries_;
  bool loaded_ = false;
};
