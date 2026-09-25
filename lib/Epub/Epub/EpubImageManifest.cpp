#include "EpubImageManifest.h"

#include <Arduino.h>
#include <BuildArena.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

#include "ImageFormatDetector.h"
#include "converters/GifToFramebufferConverter.h"
#include "converters/JpegToFramebufferConverter.h"
#include "converters/PngToFramebufferConverter.h"

namespace {
constexpr const char* kImagesBinFile = "/images.bin";
// Enough to find any PNG IHDR chunk or GIF logical-screen header, and the SOF of a JPEG that
// carries no more than a thumbnail's worth of metadata. Anything larger is deferred to a
// streaming walk (see resolve()).
constexpr size_t kHeaderBufSize = 4 * 1024;
// Compressed-side read chunk of the streaming walk; the walk's other cost is the inflate ring.
constexpr size_t kWalkChunkBytes = 512;
// Largest-free-block readings land a few bytes under the round number (allocator bookkeeping),
// so the walk gate does not sit exactly on the ring size.
constexpr size_t kWalkHeapSlack = 64;

// Parse image dimensions by detecting format and delegating to the appropriate converter.
// needMore (JPEG only) reports a header that continues past the buffer, as distinct from one
// that cannot be read at all.
bool parseImageDimensions(const uint8_t* buf, size_t n, ImageDimensions& dims, bool* needMore) {
  *needMore = false;
  const auto fmt = ImageFormatDetector::detect(buf, n);
  switch (fmt) {
    case ImageFormatDetector::Format::Jpeg:
      return JpegToFramebufferConverter::getDimensionsFromBuffer(buf, n, dims, nullptr, needMore);
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
  pending_.clear();
  cachePath_ = cachePath;
  // Drop any ZipFile bound to a previous book; resolve() lazily recreates it.
  resolveZip_.reset();
  resolveEpubPath_.clear();

  const std::string path = cachePath + kImagesBinFile;
  FsFile f;
  if (!Storage.openFileForRead("IMF", path, f)) {
    // No cache yet — start empty; resolve() fills it incrementally.
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
  // drop the whole cache and start empty — resolve() refills it and the next persist
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
  const ImageManifestEntry* entry = nullptr;
  resolve(epubPath, epubEntryPath, entry);
  return entry;
}

bool EpubImageManifest::openResolveZip(const std::string& epubPath) {
  // Reuse a single ZipFile across misses: it caches the EOCD details and a sequential
  // central-directory cursor in its members, so consecutive image lookups resume the scan
  // instead of re-reading the whole central directory each time. A fresh ZipFile per call made
  // this O(images × entries) of SD I/O, and because it all runs inside one parser write() —
  // which the section build's time budget cannot interrupt mid-call — an image-heavy section
  // froze input for the duration of the (background) build.
  if (!resolveZip_ || resolveEpubPath_ != epubPath) {
    resolveEpubPath_ = epubPath;
    resolveZip_.reset(new (std::nothrow) ZipFile(resolveEpubPath_));
    if (!resolveZip_) {
      LOG_ERR("IMF", "resolve: ZipFile alloc failed");
      return false;
    }
  }
  // Open once and keep it open across all the resolves in this build. ZipFile::close() wipes
  // the cursor — so closing per image would re-scan the central directory from the start every
  // time, which is most of the per-image cost on a large book. persistIfDirty() (run once at
  // each build's end, under RenderLock) releases the handle. While open, loadFileStatSlim and
  // readBytesFromStat also share this one SD open instead of opening/closing per call.
  if (!resolveZip_->isOpen() && !resolveZip_->open()) {
    LOG_DBG("IMF", "resolve: failed to open zip %s", epubPath.c_str());
    return false;
  }
  return true;
}

EpubImageManifest::Resolve EpubImageManifest::resolve(const std::string& epubPath, const std::string& epubEntryPath,
                                                      const ImageManifestEntry*& out) {
  out = find(epubEntryPath);
  if (out) return Resolve::Resolved;

  // Miss: read just this one image's header by its central-dir offset. One image, on demand —
  // not the whole book.
  if (!openResolveZip(epubPath)) return Resolve::Unreadable;
  ZipFile& zf = *resolveZip_;

  ZipFile::FileStatSlim stat = {};
  if (!zf.loadFileStatSlim(epubEntryPath.c_str(), &stat)) {
    LOG_DBG("IMF", "resolve: entry not found: %s", epubEntryPath.c_str());
    return Resolve::Unreadable;
  }

  // Probe-window read: kHeaderBufSize on the heap (the ~8 KB task stack cannot hold it) for the
  // duration of this call only, plus the bounded ring readBytesFromStat sizes to the same amount.
  auto* headerBuf = static_cast<uint8_t*>(malloc(kHeaderBufSize));
  if (!headerBuf) {
    LOG_ERR("IMF", "resolve: header buffer alloc failed (%u bytes)", static_cast<unsigned>(kHeaderBufSize));
    // A moment's heap, not a verdict on the image: same treatment as a header that outruns the
    // window, so the build's end (or a later build) gets to answer it.
    return deferFor(epubEntryPath, stat);
  }
  const size_t bytesRead = zf.readBytesFromStat(stat, headerBuf, kHeaderBufSize);
  ImageDimensions dims = {0, 0};
  bool needMore = false;
  const bool ok = bytesRead > 0 && parseImageDimensions(headerBuf, bytesRead, dims, &needMore);
  free(headerBuf);

  if (ok && dims.width > 0 && dims.height > 0) {
    out = insertEntry(epubEntryPath, dims);
    return Resolve::Resolved;
  }
  // The whole entry fitted in the window and still showed no SOF: a truncated file, which no
  // walk can do better on. Only a header that really continues past the window is deferred.
  if (needMore && bytesRead >= stat.uncompressedSize) needMore = false;
  if (needMore || bytesRead == 0) {
    // needMore: a valid JPEG whose SOF lies beyond the window (Exif/IPTC/XMP/ICC ahead of it —
    // ~29 KB per image in the #249 book). bytesRead == 0: the bounded read itself found no room
    // or no bytes. Neither says anything about the image; both are answered by the streaming
    // walk, whose inflate ring the parser gates against the heap (deferredWalkBytes) and which
    // resolvePending() retries at the build's end.
    return deferFor(epubEntryPath, stat);
  }
  LOG_DBG("IMF", "resolve: no dimensions for %s", epubEntryPath.c_str());
  return Resolve::Unreadable;
}

EpubImageManifest::Resolve EpubImageManifest::deferFor(const std::string& epubEntryPath,
                                                       const ZipFile::FileStatSlim& stat) {
  if (!findPending(epubEntryPath)) {
    if (pending_.size() >= kMaxPending) {
      // Still Deferred for the caller's purposes (an inline walk may resolve it); only the
      // build-end retry is forgone for this one.
      LOG_DBG("IMF", "resolve: pending queue full; %s left to the parser", epubEntryPath.c_str());
      return Resolve::Deferred;
    }
    if (pending_.capacity() == 0) pending_.reserve(kMaxPending);  // once, and only for a book that needs it
    pending_.push_back({epubEntryPath, stat});
    LOG_DBG("IMF", "resolve: header beyond %u B window, deferred: %s (%u pending)",
            static_cast<unsigned>(kHeaderBufSize), epubEntryPath.c_str(), static_cast<unsigned>(pending_.size()));
  }
  return Resolve::Deferred;
}

const EpubImageManifest::PendingImage* EpubImageManifest::findPending(const std::string& epubEntryPath) const {
  for (const auto& p : pending_) {
    if (p.epubEntryPath == epubEntryPath) return &p;
  }
  return nullptr;
}

bool EpubImageManifest::hasPending() const { return !pending_.empty(); }

size_t EpubImageManifest::walkBytesFor(const ZipFile::FileStatSlim& stat) {
  // EntryReader::open: the read chunk, plus — for a deflated entry — a ring sized to the entry.
  // A stored entry needs no ring; its walk is a seek.
  constexpr uint16_t kZipMethodStored = 0;
  const size_t ring = stat.method == kZipMethodStored ? 0 : InflateReader::ringSizeFor(stat.uncompressedSize);
  return kWalkChunkBytes + ring;
}

size_t EpubImageManifest::deferredWalkBytes(const std::string& epubEntryPath) const {
  const PendingImage* p = findPending(epubEntryPath);
  return p ? walkBytesFor(p->stat) : 0;
}

bool EpubImageManifest::walkEntry(const ZipFile::FileStatSlim& stat, ImageDimensions& dims, BuildArena* arena) {
  if (!resolveZip_ || (!resolveZip_->isOpen() && !resolveZip_->open())) return false;
  // walkBytesFor(stat) for this call only — carved from `arena` as a scoped block when given,
  // else on the heap. EntryReader::open allocates nothrow, logs the exact shortfall and returns
  // false. Its own file handle, so the resolve zip's central-directory cursor is untouched.
  ZipFile::EntryReader reader(*resolveZip_, kWalkChunkBytes, arena);
  if (!reader.open(stat)) return false;
  return JpegToFramebufferConverter::getDimensionsFromEntryReader(reader, dims) && dims.width > 0 && dims.height > 0;
}

bool EpubImageManifest::resolveDeferredNow(const std::string& epubPath, const std::string& epubEntryPath,
                                           const ImageManifestEntry*& out) {
  out = nullptr;
  const PendingImage* p = findPending(epubEntryPath);
  if (!p) return false;
  if (!openResolveZip(epubPath)) return false;
  ImageDimensions dims = {0, 0};
  if (!walkEntry(p->stat, dims, nullptr)) return false;
  out = insertEntry(epubEntryPath, dims);
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [&](const PendingImage& q) { return q.epubEntryPath == epubEntryPath; }),
                 pending_.end());
  return true;
}

size_t EpubImageManifest::resolvePending(BuildArena* walkArena) {
  size_t resolved = 0;
  for (const auto& p : pending_) {
    const size_t need = walkBytesFor(p.stat);
    // Prefer the caller's arena: the borrowed secondary framebuffer, idle once its build is
    // finished, is the only region of ring size a C3 reader session ever has — device-measured
    // (#249), reader-time contiguous heap tops out around 31.7 KB against a 33.3 KB walk, at
    // every moment including this one. EntryReader reserves a block and releases it on close, so
    // the arena is left as found. Without an arena (or one too small) the heap must cover it.
    BuildArena* arena = nullptr;
    if (walkArena && walkArena->valid() &&
        walkArena->capacity() - walkArena->used() >= need + 2 * alignof(std::max_align_t)) {
      arena = walkArena;
    }
    if (!arena && ESP.getMaxAllocHeap() < need + kWalkHeapSlack) {
      LOG_DBG("IMF", "resolvePending: %s needs %u contiguous, have %u; left for a later build", p.epubEntryPath.c_str(),
              static_cast<unsigned>(need), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      continue;
    }
    ImageDimensions dims = {0, 0};
    if (walkEntry(p.stat, dims, arena)) {
      insertEntry(p.epubEntryPath, dims);
      ++resolved;
    } else {
      LOG_DBG("IMF", "resolvePending: no dimensions for %s", p.epubEntryPath.c_str());
    }
  }
  // Unresolved ones are re-queued by the next build's miss; holding them here would only keep
  // their keys resident between builds.
  pending_.clear();
  return resolved;
}

const ImageManifestEntry* EpubImageManifest::insertEntry(const std::string& epubEntryPath,
                                                         const ImageDimensions& dims) {
  ImageManifestEntry e;
  e.epubEntryPath = epubEntryPath;  // caller passes the normalised key
  e.width = dims.width;
  e.height = dims.height;

  // Insert keeping entries_ sorted so find()'s binary search stays valid.
  auto it = std::lower_bound(entries_.begin(), entries_.end(), epubEntryPath,
                             [](const ImageManifestEntry& a, const std::string& k) { return a.epubEntryPath < k; });
  const ImageManifestEntry* result = &*entries_.insert(it, std::move(e));
  dirty_ = true;
  LOG_DBG("IMF", "Resolved %s -> %dx%d (%u cached)", epubEntryPath.c_str(), result->width, result->height,
          static_cast<unsigned>(entries_.size()));
  return result;
}

void EpubImageManifest::closeResolveHandle() {
  // Release the SD descriptor the resolve loop held open across this build's images. The
  // ZipFile object itself is kept (cheap) and reused for the next build, which reopens lazily.
  if (resolveZip_) resolveZip_->close();
}

void EpubImageManifest::releaseMemory() {
  persistIfDirty();
  std::vector<ImageManifestEntry>().swap(entries_);
  std::vector<PendingImage>().swap(pending_);
  resolveZip_.reset();
  std::string().swap(resolveEpubPath_);
  loaded_ = false;
}

const ImageManifestEntry* EpubImageManifest::find(const std::string& epubEntryPath) const {
  // entries_ is sorted by epubEntryPath (guaranteed by load() order and insertEntry).
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
