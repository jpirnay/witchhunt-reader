#pragma once

#include <ZipFile.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class BuildArena;        // lib/Memory — optional ring storage for the deferred walk
struct ImageDimensions;  // converters/ImageToFramebufferDecoder.h — by reference only, kept out of this header

// Incremental, persisted image-dimension cache. The book is NOT scanned up front: the
// manifest starts from whatever images.bin holds (possibly empty) and grows one image at a
// time via resolve() as section indexing first encounters each image. Each image's header is
// therefore read at most once in the book's lifetime; nothing is held resident for images you
// never reach. (Eagerly building the whole thing cost tens of seconds and shredded the heap on
// image-heavy books — see git history.)
//
// Sized for a large number of images. Everything per image is fixed-size and small, and nothing
// in memory has a cap that would fail silently past it:
//   - a resolved image is a 12-byte record (64-bit FNV-1a of its normalised entry path, width,
//     height) in one sorted array; 1,000 images are 12 KB, not 1,000 path strings on the heap.
//     The array grows nothrow: when the heap cannot grow it, the image is still answered for
//     this build, it just is not remembered.
//   - an image whose header lies beyond the probe window (Photoshop-style exports: 10-29 KB of
//     Exif/IPTC/XMP/ICC before SOF) waits in images.pending on the card -- key + ZIP stat, 22 B
//     each, unbounded -- for the streaming walk. That queue survives a reboot, which is exactly
//     when a fresh heap makes the walk possible. It used to be an in-memory queue of 16; past
//     that, images were laid out as alt text with no retry.
class EpubImageManifest {
 public:
  // v5: fixed-size hashed records (see above) and the on-disk pending queue. Bumping invalidates
  // older caches, which then refill incrementally. (v4 dropped unused per-entry fields.)
  static constexpr uint8_t VERSION = 5;

  // Load images.bin from cachePath into memory. A missing (or stale-version) file is NOT an
  // error: it yields an empty but loaded manifest that resolve() fills incrementally.
  // Always remembers cachePath for later persistIfDirty().
  bool load(const std::string& cachePath);

  bool isLoaded() const { return loaded_; }

  // Outcome of resolve(): Resolved (out set), Unreadable (missing entry, unknown format, corrupt
  // header — alt text for good), or Deferred (a valid JPEG whose SOF lies beyond the header probe
  // window; queued for the streaming walk -- resolveDeferredNow / resolvePending).
  enum class Resolve : uint8_t { Resolved, Unreadable, Deferred };
  // Look up an image's dimensions, resolving + caching them on a miss. On a miss reads just
  // the image header (by central-directory offset) from epubPath. epubEntryPath must be the
  // normalised path (matches find()'s key).
  Resolve resolve(const std::string& epubPath, const std::string& epubEntryPath, ImageDimensions& out);
  // resolve() == Resolved.
  bool ensureResolved(const std::string& epubPath, const std::string& epubEntryPath, ImageDimensions& out);

  // True when at least one image is queued for the walk.
  bool hasPending() const { return pendingLive_ > 0; }

  // The walk of a deferred image runs in stages: first over the entry's leading kWalkStageBytes
  // (a ring of that size -- a deflate back-reference never reaches further back than the bytes
  // produced, so a capped read needs only a capped ring), then, only if the header continues past
  // that, over the whole entry (ring ≤ 32 KB). SOF sits within the first stage for every
  // Photoshop-style export measured (9.7-18.4 KB in, "Strange Pictures"), so the 32 KB ring the
  // walk used to demand outright -- the block an X3 reader heap never holds mid-parse -- is
  // now the exception. A stored entry has no ring at all.
  static constexpr size_t kWalkStageBytes = 16 * 1024;
  // Heap the cheapest stage of a deferred image's walk takes (read chunk + ring). 0 when the image
  // is not pending. Lets the parser gate the walk on what it will actually allocate.
  size_t deferredWalkBytes(const std::string& epubEntryPath) const;
  enum class Walk : uint8_t {
    Resolved,    // recorded like a probe-window hit
    NeedsHeap,   // a stage's ring did not fit `heapBudget` (or its allocation failed): retry later
    Unreadable,  // the entry ends with no SOF: no walk can do better
  };
  // Walk one deferred image now, mid-parse. Ring storage comes from `arena` when it has room for
  // the stage (a borrowed build's lent region, idle by ~25 KB during the layout -- memory audit
  // 2026-09, run 12: with the heap alone, every image of a chapter was deferred to the build's end
  // and the whole chapter laid out twice), else from the heap, where every stage must fit
  // `heapBudget` (contiguous bytes the caller can spare). On Resolved the entry is recorded and
  // no longer pending.
  Walk resolveDeferredNow(const std::string& epubPath, const std::string& epubEntryPath, ImageDimensions& out,
                          size_t heapBudget, BuildArena* arena = nullptr);
  // Walk every queued image through the streaming header reader and record what it finds.
  // Returns how many were resolved. Meant for a build's end, with the build's now-idle arena
  // (the borrowed secondary framebuffer) as ring storage when the caller has one — on the C3 the
  // heap alone rarely holds a full ring while reading. An image whose walk was short of memory
  // stays queued (hasPending) for a caller with a bigger region; an unreadable one is dropped.
  // epubPath: the book the queue belongs to, needed when no resolve() has bound one yet (a
  // manifest reloaded from the card); empty keeps the one already open.
  size_t resolvePending(BuildArena* walkArena = nullptr, const std::string& epubPath = std::string());

  // False when the entry is not (yet) in the manifest.
  bool find(const std::string& epubEntryPath, ImageDimensions& out) const;
  size_t entryCount() const { return count_; }

  // Rewrite images.bin if entries were added since the last persist. Cheap no-op when clean.
  // Also releases the reused resolve handle (see closeResolveHandle): callers invoke this at
  // each build's end, which is also when the run of resolves is finished.
  void persistIfDirty();

  // Persist, then give back every heap block the manifest holds (the record array, the kept
  // resolve handle) and mark it unloaded; Epub::loadImageManifest() rebuilds it from images.bin.
  // The pending queue lives on the card and is untouched. No build may hold the manifest across
  // this call.
  void releaseMemory();

 private:
  struct Record {
    uint32_t keyLo;
    uint32_t keyHi;
    int16_t width;
    int16_t height;
  };
  static uint64_t keyFor(const std::string& epubEntryPath);
  static uint64_t keyOf(const Record& r) { return (static_cast<uint64_t>(r.keyHi) << 32) | r.keyLo; }

  // What images.pending holds per image: the key and the central-directory stat, so the walk
  // skips the rescan.
  struct PendingImage {
    uint64_t key;
    ZipFile::FileStatSlim stat;
  };
  static constexpr size_t kPendingRecordBytes = 8 + 2 + 4 + 4 + 4;

  // Close the SD descriptor that resolve() keeps open across a build's images. Run from
  // persistIfDirty() regardless of dirty state, so a build that resolved nothing new (or only
  // failed lookups) still releases the handle.
  void closeResolveHandle();

  // Sorted by key. Grown nothrow in steps; see insertEntry.
  std::unique_ptr<Record[]> records_;
  uint16_t count_ = 0;
  uint16_t capacity_ = 0;
  std::string cachePath_;
  bool loaded_ = false;
  bool dirty_ = false;
  // Queued images not yet resolved. A record resolved inline (resolveDeferredNow) stays in the
  // file as a dead entry until resolvePending() next compacts it; this count excludes it.
  uint16_t pendingLive_ = 0;

  std::string pendingPath() const;
  bool reserveRecords(uint16_t wanted);
  // Records the dimensions under the key; false when the record array could not grow (the caller
  // still has the dimensions -- the image is answered, just not remembered).
  bool insertEntry(uint64_t key, const ImageDimensions& dims);
  const Record* findRecord(uint64_t key) const;

  bool openResolveZip(const std::string& epubPath);
  // Queue the image for the walk (no-op if already queued) and report Deferred.
  Resolve deferFor(uint64_t key, const ZipFile::FileStatSlim& stat);
  bool findPending(uint64_t key, ZipFile::FileStatSlim* stat) const;
  // Read chunk + ring for one stage: `outputCap` bytes of the entry, 0 = all of it.
  static size_t walkBytesFor(const ZipFile::FileStatSlim& stat, size_t outputCap);
  // Stream the entry's header through an inflate ring until SOF, stage by stage (see
  // kWalkStageBytes). Each stage's ring is held only for that stage: a scoped block of `arena`
  // when one is given and can host it, else the heap when the stage fits `heapBudget` (0 = no
  // limit). NeedsHeap when neither could host a stage the walk still needed.
  Walk walkEntry(const ZipFile::FileStatSlim& stat, ImageDimensions& dims, BuildArena* arena, size_t heapBudget);

  // One ZipFile reused across resolve() misses (see the .cpp). ZipFile caches the EOCD
  // details and a sequential central-directory cursor in its members, so consecutive image
  // lookups resume the scan instead of re-reading the whole central directory each time.
  std::unique_ptr<ZipFile> resolveZip_;
  std::string resolveEpubPath_;
};
