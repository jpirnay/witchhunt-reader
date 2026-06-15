#include "EpubImageManifest.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

#include "converters/GifToFramebufferConverter.h"
#include "converters/JpegToFramebufferConverter.h"
#include "converters/PngToFramebufferConverter.h"

namespace {
constexpr const char* kImagesBinFile = "/images.bin";
// Enough to find any JPEG SOF marker, PNG IHDR chunk, or GIF logical-screen header.
constexpr size_t kHeaderBufSize = 4 * 1024;

std::string extractedPathFor(const std::string& cachePath, const std::string& epubEntryPath) {
  // Use only the basename so the path stays short and is spine-agnostic.
  const size_t slash = epubEntryPath.rfind('/');
  const std::string basename = (slash != std::string::npos) ? epubEntryPath.substr(slash + 1) : epubEntryPath;
  return cachePath + "/img/" + basename;
}

// Mirror the formats the parser's render-time probe (ImageDecoderFactory) supports, by sniffing
// the magic bytes — so any image the parser would lay out can be resolved here.
bool parseImageDimensions(const uint8_t* buf, size_t n, ImageDimensions& dims) {
  if (n >= 2 && buf[0] == 0xFF && buf[1] == 0xD8)
    return JpegToFramebufferConverter::getDimensionsFromBuffer(buf, n, dims);
  if (n >= 8 && buf[0] == 0x89 && buf[1] == 0x50)
    return PngToFramebufferConverter::getDimensionsFromBuffer(buf, n, dims);
  if (n >= 10 && buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F')
    return GifToFramebufferConverter::getDimensionsFromBuffer(buf, n, dims);
  return false;
}
}  // namespace

bool EpubImageManifest::load(const std::string& cachePath) {
  loaded_ = false;
  dirty_ = false;
  entries_.clear();
  cachePath_ = cachePath;
  // Drop any ZipFile bound to a previous book; ensureResolved() lazily recreates it.
  resolveZip_.reset();
  resolveEpubPath_.clear();

  const std::string path = cachePath + kImagesBinFile;
  FsFile f;
  if (!Storage.openFileForRead("IMF", path, f)) {
    // No cache yet — start empty; ensureResolved() fills it incrementally.
    loaded_ = true;
    return true;
  }

  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != VERSION) {
    // Stale format — ignore its contents and start empty; first persist overwrites it.
    f.close();
    entries_.clear();
    loaded_ = true;
    return true;
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

const ImageManifestEntry* EpubImageManifest::ensureResolved(const std::string& epubPath,
                                                            const std::string& epubEntryPath) {
  if (const ImageManifestEntry* hit = find(epubEntryPath)) return hit;

  // Miss: read just this one image's header by its central-dir offset. One image, on demand —
  // not the whole book. Reuse a single ZipFile across misses: it caches the EOCD details and
  // a sequential central-directory cursor in its members, so consecutive image lookups resume
  // the scan instead of re-reading the whole central directory each time. A fresh ZipFile per
  // call made this O(images × entries) of SD I/O, and because it all runs inside one parser
  // write() — which the section build's time budget cannot interrupt mid-call — an image-heavy
  // section froze input for the duration of the (background) build.
  if (!resolveZip_ || resolveEpubPath_ != epubPath) {
    resolveEpubPath_ = epubPath;
    resolveZip_.reset(new (std::nothrow) ZipFile(resolveEpubPath_));
    if (!resolveZip_) {
      LOG_ERR("IMF", "ensureResolved: ZipFile alloc failed");
      return nullptr;
    }
  }
  ZipFile& zf = *resolveZip_;

  // Hold the file open across both reads so loadFileStatSlim and readBytesFromStat share one
  // SD open instead of opening/closing the file twice per image.
  if (!zf.open()) {
    LOG_DBG("IMF", "ensureResolved: failed to open zip for %s", epubEntryPath.c_str());
    return nullptr;
  }

  const ImageManifestEntry* result = nullptr;
  ZipFile::FileStatSlim stat = {};
  if (!zf.loadFileStatSlim(epubEntryPath.c_str(), &stat)) {
    LOG_DBG("IMF", "ensureResolved: entry not found: %s", epubEntryPath.c_str());
  } else if (auto* headerBuf = static_cast<uint8_t*>(malloc(kHeaderBufSize))) {
    const size_t bytesRead = zf.readBytesFromStat(stat, headerBuf, kHeaderBufSize);
    ImageDimensions dims = {0, 0};
    const bool ok = bytesRead > 0 && parseImageDimensions(headerBuf, bytesRead, dims);
    free(headerBuf);
    if (!ok || dims.width <= 0 || dims.height <= 0) {
      LOG_DBG("IMF", "ensureResolved: no dimensions for %s", epubEntryPath.c_str());
    } else {
      ImageManifestEntry e;
      e.epubEntryPath = epubEntryPath;  // caller passes the normalised key
      e.width = dims.width;
      e.height = dims.height;
      e.method = stat.method;
      e.compressedSize = stat.compressedSize;
      e.uncompressedSize = stat.uncompressedSize;
      e.localHeaderOffset = stat.localHeaderOffset;
      e.extractedPath = extractedPathFor(cachePath_, epubEntryPath);

      // Insert keeping entries_ sorted so find()'s binary search stays valid.
      auto it = std::lower_bound(entries_.begin(), entries_.end(), epubEntryPath,
                                 [](const ImageManifestEntry& a, const std::string& k) { return a.epubEntryPath < k; });
      result = &*entries_.insert(it, std::move(e));
      dirty_ = true;
      LOG_DBG("IMF", "Resolved %s -> %dx%d (%u cached)", epubEntryPath.c_str(), result->width, result->height,
              static_cast<unsigned>(entries_.size()));
    }
  } else {
    LOG_ERR("IMF", "ensureResolved: header buffer alloc failed");
  }

  zf.close();
  return result;
}

const ImageManifestEntry* EpubImageManifest::find(const std::string& epubEntryPath) const {
  // entries_ is sorted by epubEntryPath (guaranteed by load() order and ensureResolved insert).
  auto it = std::lower_bound(entries_.begin(), entries_.end(), epubEntryPath,
                             [](const ImageManifestEntry& e, const std::string& key) { return e.epubEntryPath < key; });
  if (it != entries_.end() && it->epubEntryPath == epubEntryPath) return &*it;
  return nullptr;
}

void EpubImageManifest::persistIfDirty() {
  if (!dirty_) return;

  // Ensure img/ subdir exists (extracted images land there at render time).
  const std::string imgDir = cachePath_ + "/img";
  if (!Storage.exists(imgDir.c_str())) {
    Storage.mkdir(imgDir.c_str(), true);
  }

  const std::string binPath = cachePath_ + kImagesBinFile;
  FsFile f;
  if (!Storage.openFileForWrite("IMF", binPath, f)) {
    LOG_ERR("IMF", "persist: failed to open %s for write", binPath.c_str());
    return;  // keep dirty_ so a later build retries the write
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

  dirty_ = false;
  LOG_DBG("IMF", "Persisted image manifest: %u entries", static_cast<unsigned>(entries_.size()));
}
