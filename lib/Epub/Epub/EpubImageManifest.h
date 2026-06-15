#pragma once

#include <ZipFile.h>

#include <string>
#include <vector>

// Per-image entry: dimensions (needed at section-build pagination time) plus the ZIP
// location, cached so the parser never re-reads a header it has seen before.
struct ImageManifestEntry {
  std::string epubEntryPath;  // normalised key, e.g. "OEBPS/images/foo.jpg"
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

// Incremental, persisted image-dimension cache. The book is NOT scanned up front: the
// manifest starts from whatever images.bin holds (possibly empty) and grows one image at a
// time via ensureResolved() as section indexing first encounters each image. Each image's
// header is therefore read at most once in the book's lifetime; nothing is held resident for
// images you never reach. (Eagerly building the whole thing cost tens of seconds and shredded
// the heap on image-heavy books — see git history.)
class EpubImageManifest {
 public:
  // v2: normalised keys. Bump invalidates v1 caches (raw keys), which then refill incrementally.
  static constexpr uint8_t VERSION = 2;

  // Load images.bin from cachePath into memory. A missing (or stale-version) file is NOT an
  // error: it yields an empty but loaded manifest that ensureResolved() fills incrementally.
  // Always remembers cachePath for later persistIfDirty().
  bool load(const std::string& cachePath);

  bool isLoaded() const { return loaded_; }

  // Look up an image's dimensions, resolving + caching them on a miss. On a miss reads just
  // the image header (by central-directory offset) from epubPath, appends the entry in sorted
  // order, and marks the manifest dirty for the next persistIfDirty(). epubEntryPath must be
  // the normalised path (matches find()'s key). Returns nullptr if the header can't be read.
  const ImageManifestEntry* ensureResolved(const std::string& epubPath, const std::string& epubEntryPath);

  // Returns nullptr when the entry is not (yet) in the manifest.
  const ImageManifestEntry* find(const std::string& epubEntryPath) const;

  // Rewrite images.bin if entries were added since the last persist. Cheap no-op when clean.
  void persistIfDirty();

 private:
  std::vector<ImageManifestEntry> entries_;
  std::string cachePath_;
  bool loaded_ = false;
  bool dirty_ = false;
};
