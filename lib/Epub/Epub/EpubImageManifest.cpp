#include "EpubImageManifest.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

#include "converters/JpegToFramebufferConverter.h"
#include "converters/PngToFramebufferConverter.h"

namespace {
constexpr const char* kImagesBinFile = "/images.bin";
// Enough to find any JPEG SOF marker or PNG IHDR chunk.
constexpr size_t kHeaderBufSize = 4 * 1024;

bool isImagePath(std::string_view path) { return FsHelpers::hasJpgExtension(path) || FsHelpers::hasPngExtension(path); }

std::string extractedPathFor(const std::string& cachePath, const std::string& epubEntryPath) {
  // Use only the basename so the path stays short and is spine-agnostic.
  const size_t slash = epubEntryPath.rfind('/');
  const std::string basename = (slash != std::string::npos) ? epubEntryPath.substr(slash + 1) : epubEntryPath;
  return cachePath + "/img/" + basename;
}
}  // namespace

bool EpubImageManifest::cacheExists(const std::string& cachePath) {
  return Storage.exists((cachePath + kImagesBinFile).c_str());
}

bool EpubImageManifest::load(const std::string& cachePath) {
  loaded_ = false;
  entries_.clear();

  const std::string path = cachePath + kImagesBinFile;
  FsFile f;
  if (!Storage.openFileForRead("IMF", path, f)) return false;

  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != VERSION) {
    f.close();
    return false;
  }

  uint16_t count = 0;
  serialization::readPod(f, count);
  entries_.reserve(count);

  for (uint16_t i = 0; i < count; ++i) {
    ImageManifestEntry e;
    if (!serialization::readString(f, e.epubEntryPath)) break;
    serialization::readPod(f, e.width);
    serialization::readPod(f, e.height);
    serialization::readPod(f, e.method);
    serialization::readPod(f, e.compressedSize);
    serialization::readPod(f, e.uncompressedSize);
    serialization::readPod(f, e.localHeaderOffset);
    if (!serialization::readString(f, e.extractedPath)) break;
    entries_.push_back(std::move(e));
  }

  f.close();
  loaded_ = true;
  LOG_DBG("IMF", "Loaded image manifest: %u entries", static_cast<unsigned>(entries_.size()));
  return true;
}

bool EpubImageManifest::build(const std::string& cachePath, const std::string& epubPath, ZipFile& zf) {
  loaded_ = false;
  entries_.clear();

  // First pass: count images via streaming scan (O(1) heap — no stat cache required).
  // Safe for large EPUBs (3000+ entries) that would OOM with loadAllFileStatSlims.
  uint16_t imageCount = 0;
  zf.streamCentralDirectoryNames([&](std::string_view p) {
    if (isImagePath(p)) ++imageCount;
  });

  if (imageCount == 0) {
    LOG_DBG("IMF", "No images found in ZIP");
    loaded_ = true;  // valid empty manifest
    return true;
  }

  std::vector<std::string> imagePaths;
  imagePaths.reserve(imageCount);
  entries_.reserve(imageCount);
  // Second pass: collect image paths (still streaming, no heap growth per entry).
  zf.streamCentralDirectoryNames([&](std::string_view p) {
    if (isImagePath(p)) imagePaths.emplace_back(p);
  });

  // Ensure img/ subdir exists for future extracted images.
  const std::string imgDir = cachePath + "/img";
  if (!Storage.exists(imgDir.c_str())) {
    Storage.mkdir(imgDir.c_str(), true);
  }

  auto* headerBuf = static_cast<uint8_t*>(malloc(kHeaderBufSize));
  if (!headerBuf) {
    LOG_ERR("IMF", "Failed to alloc header buffer");
    return false;
  }

  for (const auto& entryPath : imagePaths) {
    const size_t bytesRead = zf.readBytesFromEntry(entryPath.c_str(), headerBuf, kHeaderBufSize);
    if (bytesRead == 0) {
      LOG_ERR("IMF", "Failed to read header for %s", entryPath.c_str());
      continue;
    }

    ImageDimensions dims = {0, 0};
    bool ok = false;
    if (bytesRead >= 2 && headerBuf[0] == 0xFF && headerBuf[1] == 0xD8) {
      ok = JpegToFramebufferConverter::getDimensionsFromBuffer(headerBuf, bytesRead, dims);
    } else if (bytesRead >= 8 && headerBuf[0] == 0x89 && headerBuf[1] == 0x50) {
      ok = PngToFramebufferConverter::getDimensionsFromBuffer(headerBuf, bytesRead, dims);
    }

    if (!ok || dims.width <= 0 || dims.height <= 0) {
      LOG_DBG("IMF", "Could not read dimensions for %s", entryPath.c_str());
      continue;
    }

    ZipFile::FileStatSlim stat = {};
    if (!zf.loadFileStatSlim(entryPath.c_str(), &stat)) {
      LOG_ERR("IMF", "Missing stat for %s", entryPath.c_str());
      continue;
    }

    ImageManifestEntry e;
    e.epubEntryPath = entryPath;
    e.width = dims.width;
    e.height = dims.height;
    e.method = stat.method;
    e.compressedSize = stat.compressedSize;
    e.uncompressedSize = stat.uncompressedSize;
    e.localHeaderOffset = stat.localHeaderOffset;
    e.extractedPath = extractedPathFor(cachePath, entryPath);
    entries_.push_back(std::move(e));
  }

  free(headerBuf);

  std::sort(entries_.begin(), entries_.end(),
            [](const ImageManifestEntry& a, const ImageManifestEntry& b) { return a.epubEntryPath < b.epubEntryPath; });

  // Write images.bin
  const std::string binPath = cachePath + kImagesBinFile;
  FsFile f;
  if (!Storage.openFileForWrite("IMF", binPath, f)) {
    LOG_ERR("IMF", "Failed to open %s for write", binPath.c_str());
    return false;
  }

  serialization::writePod(f, VERSION);
  const uint16_t count = static_cast<uint16_t>(entries_.size());
  serialization::writePod(f, count);
  for (const auto& e : entries_) {
    serialization::writeString(f, e.epubEntryPath);
    serialization::writePod(f, e.width);
    serialization::writePod(f, e.height);
    serialization::writePod(f, e.method);
    serialization::writePod(f, e.compressedSize);
    serialization::writePod(f, e.uncompressedSize);
    serialization::writePod(f, e.localHeaderOffset);
    serialization::writeString(f, e.extractedPath);
  }
  f.flush();
  f.close();

  loaded_ = true;
  LOG_DBG("IMF", "Built image manifest: %u images in %s", static_cast<unsigned>(entries_.size()), binPath.c_str());
  return true;
}

const ImageManifestEntry* EpubImageManifest::find(const std::string& epubEntryPath) const {
  // entries_ is sorted by epubEntryPath (guaranteed by build() and load() order).
  auto it = std::lower_bound(entries_.begin(), entries_.end(), epubEntryPath,
                             [](const ImageManifestEntry& e, const std::string& key) { return e.epubEntryPath < key; });
  if (it != entries_.end() && it->epubEntryPath == epubEntryPath) return &*it;
  return nullptr;
}
