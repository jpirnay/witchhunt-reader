#include "EpubImageManifest.h"

#include <Arduino.h>
#include <BuildArena.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>

#include "HashUtils.h"
#include "ImageFormatDetector.h"
#include "converters/GifToFramebufferConverter.h"
#include "converters/JpegToFramebufferConverter.h"
#include "converters/PngToFramebufferConverter.h"

namespace {
constexpr const char* kImagesBinFile = "/images.bin";
constexpr const char* kPendingFile = "/images.pending";
constexpr const char* kPendingTempFile = "/images.pending.tmp";
// Enough to find any PNG IHDR chunk or GIF logical-screen header, and the SOF of a JPEG that
// carries no more than a thumbnail's worth of metadata. Anything larger is deferred to a
// streaming walk (see resolve()).
constexpr size_t kHeaderBufSize = 4 * 1024;
// Compressed-side read chunk of the streaming walk; the walk's other cost is the inflate ring.
constexpr size_t kWalkChunkBytes = 512;
// Largest-free-block readings land a few bytes under the round number (allocator bookkeeping),
// so the walk gate does not sit exactly on the ring size.
constexpr size_t kWalkHeapSlack = 64;
// The record array grows by at least this many records at a time (12 B each).
constexpr uint16_t kRecordGrowth = 32;
constexpr size_t kRecordBytes = 12;

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

// Pending-file record I/O. Fixed layout, little-endian pods, no strings.
void writePending(FsFile& f, const uint64_t key, const ZipFile::FileStatSlim& stat) {
  serialization::writePod(f, key);
  serialization::writePod(f, stat.method);
  serialization::writePod(f, stat.compressedSize);
  serialization::writePod(f, stat.uncompressedSize);
  serialization::writePod(f, stat.localHeaderOffset);
}

bool readPending(FsFile& f, uint64_t& key, ZipFile::FileStatSlim& stat) {
  if (f.available() < static_cast<int>(8 + 2 + 4 + 4 + 4)) return false;
  serialization::readPod(f, key);
  serialization::readPod(f, stat.method);
  serialization::readPod(f, stat.compressedSize);
  serialization::readPod(f, stat.uncompressedSize);
  serialization::readPod(f, stat.localHeaderOffset);
  return true;
}
}  // namespace

uint64_t EpubImageManifest::keyFor(const std::string& epubEntryPath) { return HashUtils::fnvHash64(epubEntryPath); }

std::string EpubImageManifest::pendingPath() const { return cachePath_ + kPendingFile; }

bool EpubImageManifest::load(const std::string& cachePath) {
  loaded_ = false;
  dirty_ = false;
  records_.reset();
  count_ = 0;
  capacity_ = 0;
  pendingLive_ = 0;
  cachePath_ = cachePath;
  // Drop any ZipFile bound to a previous book; resolve() lazily recreates it.
  resolveZip_.reset();
  resolveEpubPath_.clear();

  const std::string path = cachePath + kImagesBinFile;
  FsFile f;
  if (Storage.openFileForRead("IMF", path, f)) {
    uint8_t version = 0;
    serialization::readPod(f, version);
    uint16_t count = 0;
    if (version == VERSION) serialization::readPod(f, count);
    // A corrupt/truncated images.bin must not steer the array into a huge allocation: a count
    // that cannot fit in the bytes left in the file is garbage, and a bad header means the rest
    // is untrustworthy too. Drop the whole cache and start empty; the next persist overwrites it.
    const int remaining = f.available();
    const uint32_t maxPlausible = remaining > 0 ? static_cast<uint32_t>(remaining) / kRecordBytes : 0u;
    if (version != VERSION) {
      count = 0;  // stale format: start empty, first persist overwrites it
    } else if (count > maxPlausible) {
      LOG_ERR("IMF", "images.bin count %u exceeds file capacity %u; ignoring cache", count, maxPlausible);
      count = 0;
    }
    if (count > 0 && reserveRecords(count)) {
      for (uint16_t i = 0; i < count; ++i) {
        Record& r = records_[i];
        serialization::readPod(f, r.keyLo);
        serialization::readPod(f, r.keyHi);
        serialization::readPod(f, r.width);
        serialization::readPod(f, r.height);
      }
      count_ = count;
    } else if (count > 0) {
      LOG_ERR("IMF", "images.bin: no heap for %u records; starting empty", count);
    }
    f.close();
  }

  // Live queue length: records whose image is not already known (a record resolved inline stays
  // in the file until the next compaction).
  FsFile pf;
  if (Storage.openFileForRead("IMF", pendingPath(), pf)) {
    uint64_t key = 0;
    ZipFile::FileStatSlim stat = {};
    while (readPending(pf, key, stat)) {
      if (!findRecord(key)) ++pendingLive_;
    }
    pf.close();
  }
  loaded_ = true;
  LOG_DBG("IMF", "Loaded image manifest: %u entries, %u pending", static_cast<unsigned>(count_),
          static_cast<unsigned>(pendingLive_));
  return true;
}

bool EpubImageManifest::reserveRecords(const uint16_t wanted) {
  if (wanted <= capacity_) return true;
  auto grown = makeUniqueNoThrow<Record[]>(wanted);
  if (!grown) return false;
  if (count_ > 0) memcpy(grown.get(), records_.get(), static_cast<size_t>(count_) * sizeof(Record));
  records_ = std::move(grown);
  capacity_ = wanted;
  return true;
}

const EpubImageManifest::Record* EpubImageManifest::findRecord(const uint64_t key) const {
  // records_ is sorted by key (guaranteed by load() order and insertEntry).
  const Record* first = records_.get();
  const Record* last = first + count_;
  const Record* it = std::lower_bound(first, last, key, [](const Record& r, const uint64_t k) { return keyOf(r) < k; });
  if (it != last && keyOf(*it) == key) return it;
  return nullptr;
}

bool EpubImageManifest::find(const std::string& epubEntryPath, ImageDimensions& out) const {
  const Record* r = findRecord(keyFor(epubEntryPath));
  if (!r) return false;
  out.width = r->width;
  out.height = r->height;
  return true;
}

bool EpubImageManifest::insertEntry(const uint64_t key, const ImageDimensions& dims) {
  if (count_ == capacity_) {
    if (capacity_ == 0xFFFF) return false;
    const uint32_t grown = std::min<uint32_t>(0xFFFF, capacity_ + std::max<uint32_t>(kRecordGrowth, capacity_ / 2));
    if (!reserveRecords(static_cast<uint16_t>(grown))) {
      LOG_ERR("IMF", "No heap to remember image %u of this book (%u records); resolved for this build only",
              static_cast<unsigned>(count_ + 1), static_cast<unsigned>(capacity_));
      return false;
    }
  }
  // Insert keeping records_ sorted so findRecord()'s binary search stays valid.
  Record* first = records_.get();
  Record* pos =
      std::lower_bound(first, first + count_, key, [](const Record& r, const uint64_t k) { return keyOf(r) < k; });
  if (pos != first + count_ && keyOf(*pos) == key) {
    pos->width = dims.width;  // re-resolved: keep one record per image
    pos->height = dims.height;
    dirty_ = true;
    return true;
  }
  memmove(pos + 1, pos, static_cast<size_t>((first + count_) - pos) * sizeof(Record));
  pos->keyLo = static_cast<uint32_t>(key);
  pos->keyHi = static_cast<uint32_t>(key >> 32);
  pos->width = dims.width;
  pos->height = dims.height;
  ++count_;
  dirty_ = true;
  return true;
}

bool EpubImageManifest::ensureResolved(const std::string& epubPath, const std::string& epubEntryPath,
                                       ImageDimensions& out) {
  return resolve(epubPath, epubEntryPath, out) == Resolve::Resolved;
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
                                                      ImageDimensions& out) {
  const uint64_t key = keyFor(epubEntryPath);
  if (const Record* r = findRecord(key)) {
    out.width = r->width;
    out.height = r->height;
    return Resolve::Resolved;
  }

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
    return deferFor(key, stat);
  }
  const size_t bytesRead = zf.readBytesFromStat(stat, headerBuf, kHeaderBufSize);
  ImageDimensions dims = {0, 0};
  bool needMore = false;
  const bool ok = bytesRead > 0 && parseImageDimensions(headerBuf, bytesRead, dims, &needMore);
  free(headerBuf);

  if (ok && dims.width > 0 && dims.height > 0) {
    insertEntry(key, dims);
    LOG_DBG("IMF", "Resolved %s -> %dx%d (%u cached)", epubEntryPath.c_str(), dims.width, dims.height,
            static_cast<unsigned>(count_));
    out = dims;
    return Resolve::Resolved;
  }
  // The whole entry fitted in the window and still showed no SOF: a truncated file, which no
  // walk can do better on. Only a header that really continues past the window is deferred.
  if (needMore && bytesRead >= stat.uncompressedSize) needMore = false;
  if (needMore || bytesRead == 0) {
    // needMore: a valid JPEG whose SOF lies beyond the window (Exif/IPTC/XMP/ICC ahead of it —
    // 10-29 KB per image in the books this was measured on). bytesRead == 0: the bounded read
    // itself found no room or no bytes. Neither says anything about the image; both are answered
    // by the streaming walk (resolveDeferredNow mid-parse, resolvePending at a build's end).
    LOG_DBG("IMF", "resolve: header beyond %u B window, deferred: %s", static_cast<unsigned>(kHeaderBufSize),
            epubEntryPath.c_str());
    return deferFor(key, stat);
  }
  LOG_DBG("IMF", "resolve: no dimensions for %s", epubEntryPath.c_str());
  return Resolve::Unreadable;
}

EpubImageManifest::Resolve EpubImageManifest::deferFor(const uint64_t key, const ZipFile::FileStatSlim& stat) {
  if (findPending(key, nullptr)) return Resolve::Deferred;
  // Append to the queue on the card: no per-image memory, no cap, and it outlives the session.
  FsFile f;
  const std::string path = pendingPath();
  bool opened = Storage.exists(path.c_str()) && Storage.openFileForUpdate("IMF", path, f);
  if (opened && !f.seek(f.size())) {
    f.close();
    opened = false;
  }
  if (!opened && !Storage.openFileForWrite("IMF", path, f)) {
    LOG_ERR("IMF", "defer: cannot open %s; image stays unqueued for this build", path.c_str());
    return Resolve::Deferred;  // still deferred for the caller; the next build's miss retries
  }
  writePending(f, key, stat);
  f.flush();
  f.close();
  ++pendingLive_;
  return Resolve::Deferred;
}

bool EpubImageManifest::findPending(const uint64_t key, ZipFile::FileStatSlim* stat) const {
  if (pendingLive_ == 0) return false;
  FsFile f;
  if (!Storage.openFileForRead("IMF", pendingPath(), f)) return false;
  uint64_t k = 0;
  ZipFile::FileStatSlim s = {};
  bool found = false;
  while (readPending(f, k, s)) {
    if (k == key) {
      found = true;
      if (stat) *stat = s;
      break;
    }
  }
  f.close();
  return found;
}

size_t EpubImageManifest::walkBytesFor(const ZipFile::FileStatSlim& stat, const size_t outputCap) {
  // EntryReader::open(stat, cap): the read chunk, plus — for a deflated entry — a ring sized to
  // what will be read. A stored entry needs no ring; its walk is a seek.
  constexpr uint16_t kZipMethodStored = 0;
  const size_t expected = outputCap == 0 ? stat.uncompressedSize : std::min<size_t>(stat.uncompressedSize, outputCap);
  const size_t ring = stat.method == kZipMethodStored ? 0 : InflateReader::ringSizeFor(expected);
  return kWalkChunkBytes + ring;
}

size_t EpubImageManifest::deferredWalkBytes(const std::string& epubEntryPath) const {
  ZipFile::FileStatSlim stat = {};
  return findPending(keyFor(epubEntryPath), &stat) ? walkBytesFor(stat, kWalkStageBytes) : 0;
}

EpubImageManifest::Walk EpubImageManifest::walkEntry(const ZipFile::FileStatSlim& stat, ImageDimensions& dims,
                                                     BuildArena* arena, const size_t heapBudget) {
  if (!resolveZip_ || (!resolveZip_->isOpen() && !resolveZip_->open())) return Walk::NeedsHeap;
  // Stage caps: the leading kWalkStageBytes, then the whole entry. The second stage is only
  // reached when the first ended with the marker chain still consistent (needMore), and is
  // skipped when the first already covered the entry.
  const size_t caps[2] = {kWalkStageBytes, 0};
  for (const size_t cap : caps) {
    if (cap != 0 && cap >= stat.uncompressedSize) {
      // The stage would read the whole entry: identical to the final stage, so run that one.
      continue;
    }
    const size_t need = walkBytesFor(stat, cap);
    // Prefer the caller's arena: the borrowed secondary framebuffer, idle once its build is
    // finished, is the only region of full-ring size a C3 reader session ever has. EntryReader
    // reserves a block and releases it on close, so the arena is left as found. Without an arena
    // (or one too small) the heap must cover it, within the caller's budget.
    BuildArena* stageArena = nullptr;
    if (arena && arena->valid() && arena->capacity() - arena->used() >= need + 2 * alignof(std::max_align_t)) {
      stageArena = arena;
    }
    if (!stageArena && heapBudget != 0 && need > heapBudget) {
      LOG_DBG("IMF", "walk stage %u: needs %u contiguous, budget %u", static_cast<unsigned>(cap),
              static_cast<unsigned>(need), static_cast<unsigned>(heapBudget));
      return Walk::NeedsHeap;
    }
    // Its own file handle, so the resolve zip's central-directory cursor is untouched.
    // EntryReader::open allocates nothrow, logs the exact shortfall and returns false.
    ZipFile::EntryReader reader(*resolveZip_, kWalkChunkBytes, stageArena);
    if (!reader.open(stat, cap)) return Walk::NeedsHeap;
    bool needMore = false;
    if (JpegToFramebufferConverter::getDimensionsFromEntryReader(reader, dims, nullptr, &needMore) && dims.width > 0 &&
        dims.height > 0) {
      return Walk::Resolved;
    }
    if (!needMore || cap == 0) return Walk::Unreadable;
  }
  return Walk::Unreadable;
}

EpubImageManifest::Walk EpubImageManifest::resolveDeferredNow(const std::string& epubPath,
                                                              const std::string& epubEntryPath, ImageDimensions& out,
                                                              const size_t heapBudget, BuildArena* arena) {
  const uint64_t key = keyFor(epubEntryPath);
  ZipFile::FileStatSlim stat = {};
  if (!findPending(key, &stat)) return Walk::Unreadable;
  if (!openResolveZip(epubPath)) return Walk::NeedsHeap;
  ImageDimensions dims = {0, 0};
  const Walk walk = walkEntry(stat, dims, arena, heapBudget);
  if (walk != Walk::Resolved) return walk;
  insertEntry(key, dims);
  LOG_DBG("IMF", "Resolved %s -> %dx%d by walk (%u cached)", epubEntryPath.c_str(), dims.width, dims.height,
          static_cast<unsigned>(count_));
  out = dims;
  // Its record stays in images.pending as a dead entry until the next compaction.
  if (pendingLive_ > 0) --pendingLive_;
  return Walk::Resolved;
}

size_t EpubImageManifest::resolvePending(BuildArena* walkArena, const std::string& epubPath) {
  const std::string path = pendingPath();
  if (!Storage.exists(path.c_str())) {
    pendingLive_ = 0;
    return 0;
  }
  const std::string& bookPath = epubPath.empty() ? resolveEpubPath_ : epubPath;
  if (bookPath.empty() || !openResolveZip(bookPath)) {
    LOG_ERR("IMF", "resolvePending: no book to walk %u queued image(s) in", static_cast<unsigned>(pendingLive_));
    return 0;
  }
  FsFile in;
  if (!Storage.openFileForRead("IMF", path, in)) return 0;
  // Walks short of memory are kept -- written to a temp file as we go, so the working set is one
  // record whatever the queue's length -- and copied back over the queue afterwards.
  const std::string tempPath = cachePath_ + kPendingTempFile;
  FsFile keep;
  bool keeping = Storage.openFileForWrite("IMF", tempPath, keep);
  size_t resolved = 0;
  uint16_t kept = 0;
  uint64_t key = 0;
  ZipFile::FileStatSlim stat = {};
  while (readPending(in, key, stat)) {
    if (findRecord(key)) continue;  // resolved inline earlier: dead record
    // Largest-free-block readings land a few bytes under the round number (allocator
    // bookkeeping), so the heap budget sits just under what the allocator reports.
    const uint32_t maxAlloc = ESP.getMaxAllocHeap();
    const size_t heapBudget = maxAlloc > kWalkHeapSlack ? maxAlloc - kWalkHeapSlack : 1;
    ImageDimensions dims = {0, 0};
    switch (walkEntry(stat, dims, walkArena, heapBudget)) {
      case Walk::Resolved:
        insertEntry(key, dims);
        ++resolved;
        break;
      case Walk::NeedsHeap:
        LOG_DBG("IMF", "resolvePending: no room for a walk (have %u contiguous); still pending",
                static_cast<unsigned>(maxAlloc));
        // An image only memory stood in the way of stays queued: the reader retries it from the
        // borrowed framebuffer (the one region of full-ring size it can always get), so a walk is
        // never quietly dropped between builds.
        if (keeping) {
          writePending(keep, key, stat);
          ++kept;
        }
        break;
      case Walk::Unreadable:
        // Dropped; the next build's miss re-probes it and decides again.
        LOG_DBG("IMF", "resolvePending: no dimensions for a queued image");
        break;
    }
  }
  in.close();
  if (keeping) {
    keep.flush();
    keep.close();
  }

  // Compact: the queue becomes exactly the kept records. Without a rename on this storage the
  // temp is streamed back over a truncated queue file.
  if (!keeping) {
    // Could not write the temp: leave the queue as it was (dead and unresolved records both stay,
    // and the count still excludes the dead ones).
    LOG_ERR("IMF", "resolvePending: cannot write %s; queue left uncompacted", tempPath.c_str());
    return resolved;
  }
  if (kept == 0) {
    Storage.remove(path.c_str());
  } else {
    FsFile src;
    FsFile dst;
    if (Storage.openFileForRead("IMF", tempPath, src) && Storage.openFileForWrite("IMF", path, dst)) {
      uint8_t buf[128];
      int n = 0;
      while ((n = src.read(buf, sizeof(buf))) > 0) dst.write(buf, static_cast<size_t>(n));
      dst.flush();
      dst.close();
      src.close();
    } else {
      LOG_ERR("IMF", "resolvePending: cannot rewrite %s", path.c_str());
      if (src) src.close();
    }
  }
  Storage.remove(tempPath.c_str());
  pendingLive_ = kept;
  return resolved;
}

void EpubImageManifest::closeResolveHandle() {
  // Release the SD descriptor the resolve loop held open across this build's images. The
  // ZipFile object itself is kept (cheap) and reused for the next build, which reopens lazily.
  if (resolveZip_) resolveZip_->close();
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
  serialization::writePod(f, count_);
  for (uint16_t i = 0; i < count_; ++i) {
    const Record& r = records_[i];
    serialization::writePod(f, r.keyLo);
    serialization::writePod(f, r.keyHi);
    serialization::writePod(f, r.width);
    serialization::writePod(f, r.height);
  }
  f.flush();
  f.close();

  dirty_ = false;
  LOG_DBG("IMF", "Persisted image manifest: %u entries", static_cast<unsigned>(count_));
}

void EpubImageManifest::releaseMemory() {
  persistIfDirty();
  records_.reset();
  count_ = 0;
  capacity_ = 0;
  resolveZip_.reset();
  std::string().swap(resolveEpubPath_);
  loaded_ = false;
}
