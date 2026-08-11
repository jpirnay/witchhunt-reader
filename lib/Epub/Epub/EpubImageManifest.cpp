#include "EpubImageManifest.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>

#include "ImageFormatDetector.h"
#include "converters/GifToFramebufferConverter.h"
#include "converters/JpegToFramebufferConverter.h"
#include "converters/PngToFramebufferConverter.h"

// --- Resolve-path retention probe (temporary; answers docs/memory-allocation-strategy.md §8.4) --
//
// The per-page build trace shows contig HALVING in a single page (X3 2026-08-11, spine 1:
// 45044 -> 23540 between page 2 and page 3) with allocBytes stepping +15776 at the same
// boundary. That is not the gradual small-object churn the free-block count also shows
// (freeBlk 16 -> 35 across the parse) — it is one retention event, and it lands immediately
// after the first image resolve of the build:
//
//   [ZIP] EOCD found at offset 1473764 in file
//   [IMF] Resolved OEBPS/..._endpapers.jpg -> 600x401 (1 cached)
//
// ensureResolved() deliberately constructs a ZipFile on the first miss and leaves it OPEN for
// the rest of the build (closeResolveHandle() releases at persist). If that is the +15776, it
// is a large block taken MID-PARSE and held to the end — the shape eliminated #8 of the heap
// handover proved is the worst kind on a no-compaction heap, recurring somewhere nobody looked.
//
// The correlation is across a page boundary, so it names a suspect, not a cause. This probe
// brackets the three allocations separately (ZipFile object / open() + its EOCD scan / the
// per-resolve header buffer) and the release, so one device trace attributes the step exactly.
//
// IMF_RETAIN_TRACE=0 compiles it out. Delete once the question is answered.
#ifndef IMF_RETAIN_TRACE
#define IMF_RETAIN_TRACE 1
#endif

#if IMF_RETAIN_TRACE
namespace {
struct ImfHeapSample {
  uint32_t free = 0;
  uint32_t contig = 0;
  uint32_t allocBytes = 0;
  uint32_t allocBlk = 0;
};

ImfHeapSample imfSampleHeap() {
  multi_heap_info_t info{};
  heap_caps_get_info(&info, MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  return {static_cast<uint32_t>(esp_get_free_heap_size()),
          static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)),
          static_cast<uint32_t>(info.total_allocated_bytes), static_cast<uint32_t>(info.allocated_blocks)};
}

// Signed deltas: a retention shows as allocBytes UP and contig DOWN across the same step.
void imfLogStep(const char* what, const ImfHeapSample& before, const ImfHeapSample& after) {
  LOG_INF("IMF", "retain[%-12s] allocBytes%+ld blk%+ld free%+ld contig %lu->%lu", what,
          static_cast<long>(after.allocBytes) - static_cast<long>(before.allocBytes),
          static_cast<long>(after.allocBlk) - static_cast<long>(before.allocBlk),
          static_cast<long>(after.free) - static_cast<long>(before.free), static_cast<unsigned long>(before.contig),
          static_cast<unsigned long>(after.contig));
}
}  // namespace
#define IMF_SAMPLE(name) const ImfHeapSample name = imfSampleHeap()
#define IMF_STEP(what, before, after) imfLogStep((what), (before), (after))
#else
#define IMF_SAMPLE(name) ((void)0)
#define IMF_STEP(what, before, after) ((void)0)
#endif

namespace {
constexpr const char* kImagesBinFile = "/images.bin";
// Enough to find any JPEG SOF marker, PNG IHDR chunk, or GIF logical-screen header.
constexpr size_t kHeaderBufSize = 4 * 1024;

// Parse image dimensions by detecting format and delegating to the appropriate converter.
bool parseImageDimensions(const uint8_t* buf, size_t n, ImageDimensions& dims) {
  const auto fmt = ImageFormatDetector::detect(buf, n);
  switch (fmt) {
    case ImageFormatDetector::Format::Jpeg:
      return JpegToFramebufferConverter::getDimensionsFromBuffer(buf, n, dims);
    case ImageFormatDetector::Format::Png:
      return PngToFramebufferConverter::getDimensionsFromBuffer(buf, n, dims);
    case ImageFormatDetector::Format::Gif:
      return GifToFramebufferConverter::getDimensionsFromBuffer(buf, n, dims);
    case ImageFormatDetector::Format::Unknown:
      return false;
  }
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
  // A corrupt/truncated images.bin must not steer reserve() into a multi-MB allocation that aborts
  // the firmware (uncaught bad_alloc, -fno-exceptions). The smallest possible on-disk entry is one
  // u32 string-length prefix (empty key) + width + height (two int16) = 8 B, so a count that can't
  // fit in the bytes left in the file is garbage. A bad header means the rest is untrustworthy too:
  // drop the whole cache and start empty — ensureResolved() refills it and the next persist
  // overwrites the corrupt file.
  const int remaining = f.available();
  const uint32_t maxPlausible = remaining > 0 ? static_cast<uint32_t>(remaining) / 8u : 0u;
  if (count > maxPlausible) {
    LOG_ERR("IMF", "images.bin count %u exceeds file capacity %u; ignoring cache", count, maxPlausible);
    f.close();
    entries_.clear();
    loaded_ = true;
    return true;
  }
  entries_.reserve(count);

  for (uint16_t i = 0; i < count; ++i) {
    ImageManifestEntry e;
    if (!serialization::readString(f, e.epubEntryPath)) break;
    serialization::readPod(f, e.width);
    serialization::readPod(f, e.height);
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
  IMF_SAMPLE(atEntry);
  if (!resolveZip_ || resolveEpubPath_ != epubPath) {
    resolveEpubPath_ = epubPath;
    resolveZip_.reset(new (std::nothrow) ZipFile(resolveEpubPath_));
    if (!resolveZip_) {
      LOG_ERR("IMF", "ensureResolved: ZipFile alloc failed");
      return nullptr;
    }
    IMF_SAMPLE(afterCtor);
    IMF_STEP("ZipFile ctor", atEntry, afterCtor);
  }
  ZipFile& zf = *resolveZip_;

  // Open once and keep it open across all the resolves in this build. ZipFile caches both the
  // EOCD details and a sequential central-dir cursor, but ZipFile::close() wipes the cursor —
  // so closing per image would re-scan the central directory from the start every time, which
  // is most of the per-image cost on a large book. persistIfDirty() (run once at each build's
  // end, under RenderLock) releases the handle. While open, loadFileStatSlim and
  // readBytesFromStat also share this one SD open instead of opening/closing per call.
  const bool openedThisCall = !zf.isOpen();
  IMF_SAMPLE(beforeOpen);
  if (openedThisCall && !zf.open()) {
    LOG_DBG("IMF", "ensureResolved: failed to open zip for %s", epubEntryPath.c_str());
    return nullptr;
  }
  if (openedThisCall) {
    IMF_SAMPLE(afterOpen);
    // The suspected culprit: open() runs the EOCD scan and leaves the handle live for the whole
    // build. A large positive allocBytes here, with contig dropping and NOT recovering until
    // closeResolveHandle(), confirms the page-3 step.
    IMF_STEP("zip open", beforeOpen, afterOpen);
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

  // Note: the handle is intentionally left open; closeResolveHandle() releases it at persist.
  //
  // The whole-call net is what the per-page build trace actually attributes to the page the
  // resolve happened on — compare it against the +15776 step seen at spine_page=3.
  IMF_SAMPLE(atExit);
  IMF_STEP("resolve net", atEntry, atExit);
  return result;
}

void EpubImageManifest::closeResolveHandle() {
  // Release the SD descriptor the resolve loop held open across this build's images. The
  // ZipFile object itself is kept (cheap) and reused for the next build, which reopens lazily.
  if (!resolveZip_) return;
  // Does the retention come back, and does contig come back WITH it? Bytes returning while
  // contig stays low is the signature that matters: it means the damage was the placement of
  // the block, not its size, and closing it later cannot undo that.
  IMF_SAMPLE(beforeClose);
  resolveZip_->close();
  IMF_SAMPLE(afterClose);
  IMF_STEP("zip close", beforeClose, afterClose);
}

const ImageManifestEntry* EpubImageManifest::find(const std::string& epubEntryPath) const {
  // entries_ is sorted by epubEntryPath (guaranteed by load() order and ensureResolved insert).
  auto it = std::lower_bound(entries_.begin(), entries_.end(), epubEntryPath,
                             [](const ImageManifestEntry& e, const std::string& key) { return e.epubEntryPath < key; });
  if (it != entries_.end() && it->epubEntryPath == epubEntryPath) return &*it;
  return nullptr;
}

void EpubImageManifest::persistIfDirty() {
  // The build whose resolves just ran is finished — release the handle they kept open.
  closeResolveHandle();
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
  }
  f.flush();
  f.close();

  dirty_ = false;
  LOG_DBG("IMF", "Persisted image manifest: %u entries", static_cast<unsigned>(entries_.size()));
}
