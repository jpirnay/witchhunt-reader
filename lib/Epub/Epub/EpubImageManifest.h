#pragma once

#include <ZipFile.h>

#include <memory>
#include <string>
#include <vector>

class BuildArena;        // lib/Memory — optional ring storage for the deferred walk
struct ImageDimensions;  // converters/ImageToFramebufferDecoder.h — by reference only, kept out of this header

// Per-image entry: just the normalised key and its pixel dimensions. Dimensions are the only
// thing any consumer reads (pagination needs them; the parser reads width/height and nothing
// else). Render-time extraction re-resolves the ZIP entry by path via ImageBlock::ensureExtracted,
// so no ZIP-stat fields are cached here.
struct ImageManifestEntry {
  std::string epubEntryPath;  // normalised key, e.g. "OEBPS/images/foo.jpg"
  int16_t width = 0;
  int16_t height = 0;
};

// Incremental, persisted image-dimension cache. The book is NOT scanned up front: the
// manifest starts from whatever images.bin holds (possibly empty) and grows one image at a
// time via ensureResolved() as section indexing first encounters each image. Each image's
// header is therefore read at most once in the book's lifetime; nothing is held resident for
// images you never reach. (Eagerly building the whole thing cost tens of seconds and shredded
// the heap on image-heavy books — see git history.)
class EpubImageManifest {
 public:
  // v4: entries are now just {key, width, height}. Dropped the unused per-entry extractedPath and
  // the unused ZIP-stat fields (method/compressed/uncompressed/localHeaderOffset) — all were
  // written and persisted but never read. Bumping invalidates older caches, which then refill
  // incrementally. (v2 added normalised keys; v3 dropped extractedPath.)
  static constexpr uint8_t VERSION = 4;

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

  // Outcome of resolve(): Resolved (out set), Unreadable (missing entry, unknown format, corrupt
  // header — alt text for good), or Deferred (a valid JPEG whose SOF lies beyond the header probe
  // window; queued for resolvePending(), which walks the entry through an inflate ring).
  enum class Resolve : uint8_t { Resolved, Unreadable, Deferred };
  Resolve resolve(const std::string& epubPath, const std::string& epubEntryPath, const ImageManifestEntry*& out);

  // True when resolve() deferred at least one image since the last resolvePending().
  bool hasPending() const;
  // Heap the streaming walk of a deferred image takes: its read chunk plus an inflate ring sized
  // to the entry (InflateReader::ringSizeFor, ≤32 KB; none for a stored entry). 0 when the image
  // is not pending. Lets the parser gate the walk on what it will actually allocate.
  size_t deferredWalkBytes(const std::string& epubEntryPath) const;
  // Walk one deferred image now — mid-parse, for a caller that has checked deferredWalkBytes()
  // against the heap. On success the entry is recorded (and dequeued) exactly as a probe-window
  // hit would be; false means the ring could not be had or the walk found no SOF.
  bool resolveDeferredNow(const std::string& epubPath, const std::string& epubEntryPath,
                          const ImageManifestEntry*& out);
  // Walk every deferred entry through the streaming header reader and record what it finds.
  // Returns how many were resolved. Meant for a build's end, with the build's now-idle arena
  // (the borrowed secondary framebuffer) as ring storage when the caller has one — on the C3 the
  // heap alone never holds a 32 KB ring while reading. Clears the queue either way: an image
  // that still could not be walked is re-queued by the next build's miss.
  size_t resolvePending(BuildArena* walkArena = nullptr);

  // Returns nullptr when the entry is not (yet) in the manifest.
  const ImageManifestEntry* find(const std::string& epubEntryPath) const;

  // Rewrite images.bin if entries were added since the last persist. Cheap no-op when clean.
  // Also releases the reused resolve handle (see closeResolveHandle): callers invoke this at
  // each build's end, which is also when the run of ensureResolved() resolves is finished.
  void persistIfDirty();

  // Persist, then give back every heap block the manifest holds (entries, pending queue, the
  // kept resolve handle) and mark it unloaded; Epub::loadImageManifest() rebuilds it from
  // images.bin. For a caller that needs contiguous heap: the entries are one path string per
  // image, created as builds first meet each image, so a released build strews them through
  // the secondary framebuffer's hole. No build may hold the manifest across this call.
  void releaseMemory();

 private:
  // Close the SD descriptor that ensureResolved() keeps open across a build's images. Run from
  // persistIfDirty() regardless of dirty state, so a build that resolved nothing new (or only
  // failed lookups) still releases the handle.
  void closeResolveHandle();

  std::vector<ImageManifestEntry> entries_;
  std::string cachePath_;
  bool loaded_ = false;
  bool dirty_ = false;

  // Images the probe window could not answer (a valid JPEG whose SOF lies beyond it), kept with
  // the central-directory stat so the walk skips the rescan. Bounded, build-scoped, and only
  // ever allocated for a book that has such images: kMaxPending × (key string + 16 B stat).
  // Past the cap the parser's own fallback handles the rest exactly as it did before.
  static constexpr size_t kMaxPending = 16;
  struct PendingImage {
    std::string epubEntryPath;
    ZipFile::FileStatSlim stat;
  };
  std::vector<PendingImage> pending_;

  bool openResolveZip(const std::string& epubPath);
  // Queue the image for the walk (no-op if already queued or the queue is full) and report Deferred.
  Resolve deferFor(const std::string& epubEntryPath, const ZipFile::FileStatSlim& stat);
  const PendingImage* findPending(const std::string& epubEntryPath) const;
  static size_t walkBytesFor(const ZipFile::FileStatSlim& stat);
  // Stream the entry's header through an inflate ring until SOF. walkBytesFor(stat), held only
  // for the call: a scoped block of `arena` when one is given, else heap.
  bool walkEntry(const ZipFile::FileStatSlim& stat, ImageDimensions& dims, BuildArena* arena);
  const ImageManifestEntry* insertEntry(const std::string& epubEntryPath, const ImageDimensions& dims);

  // One ZipFile reused across ensureResolved() misses (see the .cpp). ZipFile caches the
  // EOCD details and a sequential central-directory cursor in its members; reusing the
  // instance keeps both alive between images instead of re-scanning the whole central
  // directory per image. resolveEpubPath_ backs the reference ZipFile holds, so it must
  // outlive resolveZip_ — they are always (re)assigned together.
  std::string resolveEpubPath_;
  std::unique_ptr<ZipFile> resolveZip_;
};
