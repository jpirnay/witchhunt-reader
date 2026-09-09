#include "FootnotePreviews.h"

#include <BuildArena.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <SaxParser/SaxParser.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <type_traits>

#include "../Epub.h"
#include "FootnoteShape.h"
#include "Section.h"

namespace {

constexpr uint32_t CACHE_MAGIC = 0x31504E46;  // "FNP1"
// 2: added the resolved-spine bitmap. A v1 store is discarded rather than migrated — it
// rebuilds a spine at a time as the reader moves, so there is nothing to preserve.
constexpr uint16_t CACHE_VERSION = 2;
constexpr size_t STREAM_CHUNK_BYTES = 1024;
constexpr size_t MAX_HREF_BYTES = 191;
constexpr uint32_t HEADER_BYTES = 12;    // magic + version + count + indexOffset
constexpr uint32_t INDEX_ROW_BYTES = 8;  // keyHash + blobOffset
// Bitmap of spines whose links have been scanned and whose every target is stored. Sits between
// the header and the blob, one bit per spine, so the answer costs a 76-byte read. Fixed size: a
// book with more spines than this simply has no bits and behaves as it did before, re-scanning
// on each build. 512 covers every book in the corpus by a wide margin (the largest is 30).
constexpr uint32_t RESOLVED_BITMAP_BYTES = 64;
constexpr int RESOLVED_BITMAP_SPINES = static_cast<int>(RESOLVED_BITMAP_BYTES) * 8;
constexpr uint32_t BLOB_START = HEADER_BYTES + RESOLVED_BITMAP_BYTES;
constexpr size_t MAX_MARKER_BYTES = 15;

uint32_t fnv1a32(const char* s) {
  uint32_t h = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

// Key = "<targetSpineIndex>#<fragment>": identical construction at gather and lookup
// time, so resolution differences can never desynchronise the two sides.
uint32_t makeKeyHash(const int spineIndex, const char* fragment) {
  char buf[16 + MAX_HREF_BYTES + 1];
  snprintf(buf, sizeof(buf), "%d#%s", spineIndex, fragment);
  return fnv1a32(buf);
}

bool isSpaceChar(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

const char* getAttribute(const char** atts, const char* name) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i] && atts[i + 1]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

// Growable array with NO-THROW allocation, for the four collections the gather builds.
//
// std::vector cannot be used here: the firmware compiles -fno-exceptions, so a growth the heap
// cannot satisfy throws bad_alloc straight into std::terminate -> abort(). Device-observed on a
// 30-spine book with 155 note targets: at spine 29 the target list tried to grow from 2048 to
// 4096 bytes, failed, and took the firmware down mid-gather (the "Gathering footnotes" popup was
// on screen at the time).
//
// push() returns false instead, and a pass that cannot complete leaves the store exactly as it
// was rather than persisting a truncated answer — the reader gets plain markers for that spine
// and the next build of it tries again.
template <typename T>
class NoThrowArray {
  static_assert(std::is_trivially_copyable<T>::value, "grows by memcpy");

  std::unique_ptr<T[]> data_;
  size_t size_ = 0;
  size_t capacity_ = 0;

 public:
  bool reserve(const size_t want) {
    if (want <= capacity_) return true;
    auto grown = makeUniqueNoThrow<T[]>(want);
    if (!grown) return false;
    if (size_ > 0) memcpy(grown.get(), data_.get(), size_ * sizeof(T));
    data_ = std::move(grown);
    capacity_ = want;
    return true;
  }
  bool push(const T& value) {
    if (size_ == capacity_ && !reserve(capacity_ > 0 ? capacity_ * 2 : 16)) return false;
    data_[size_++] = value;
    return true;
  }
  void clear() { size_ = 0; }
  void pop() {
    if (size_ > 0) --size_;
  }
  size_t size() const { return size_; }
  const T& operator[](const size_t i) const { return data_[i]; }
  T* begin() { return data_.get(); }
  T* end() { return data_.get() + size_; }
  const T* begin() const { return data_.get(); }
  const T* end() const { return data_.get() + size_; }
};

// 8 bytes, and deliberately no fragment string: Pass B matches an element's id by hashing it the
// same way (makeKeyHash) rather than by comparing text, so the fragments never have to be held in
// RAM at all. At 155 targets that is 1.2 KB instead of the ~5 KB a vector of std::string cost.
// It does mean a 32-bit collision between a note anchor and ANY other id in the same document
// shows the wrong preview text — the format already accepts that risk between note anchors, and
// the odds do not meaningfully change by widening the pool.
struct Target {
  uint32_t keyHash;
  uint16_t spineIndex;
};

// One row of the on-disk index, sorted by key so Lookup can binary-search it.
struct IndexRow {
  uint32_t keyHash;
  uint32_t blobOffset;
  bool operator<(const IndexRow& other) const { return keyHash < other.keyHash; }
};

// Binary search of the on-disk index, shared by the two sides that ask the same question of the
// same bytes: Lookup::find ("what is this note's text?") and Store::contains ("do I already have
// this note?"). They used to answer it differently -- Lookup searched the file, the Store held
// every row in RAM and scanned them linearly -- which is how the store's index became the thing
// that bounded MAX_ENTRIES. One implementation, one format assumption, no rows resident.
//
// `file` must be open for reading and stays open; `count` rows sit at `indexOffset`, sorted by
// keyHash. Returns false when the key is absent or the file cannot be read.
bool findIndexRow(FsFile& file, const uint32_t indexOffset, const uint16_t count, const uint32_t keyHash,
                  uint32_t& outBlobOffset) {
  int lo = 0;
  int hi = static_cast<int>(count) - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    uint32_t row[2] = {0, 0};
    if (!file.seekSet(indexOffset + static_cast<uint32_t>(mid) * INDEX_ROW_BYTES)) return false;
    if (file.read(reinterpret_cast<uint8_t*>(row), sizeof(row)) != static_cast<int>(sizeof(row))) return false;
    if (row[0] == keyHash) {
      outBlobOffset = row[1];
      return true;
    }
    if (row[0] < keyHash) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return false;
}

// Pass A: SAX scan of one spine document collecting footnote-shaped link targets.
class LinkScanner {
  SaxParser parser_;
  const Epub& epub_;
  const int spineIndex_;
  NoThrowArray<Target>& targets_;
  bool oom_ = false;
  int depth_ = 0;
  int linkDepth_ = -1;
  bool linkIsNoteref_ = false;
  char href_[MAX_HREF_BYTES + 1] = {};
  char text_[MAX_MARKER_BYTES + 1] = {};
  size_t textLen_ = 0;
  bool textOverflow_ = false;

  static bool isInternalHref(const char* href) {
    if (!href || *href == '\0') return false;
    return strstr(href, "://") == nullptr && strncmp(href, "mailto:", 7) != 0 && strncmp(href, "javascript:", 11) != 0;
  }

  static void startElement(void* ctx, const char* name, const char** atts) {
    auto* self = static_cast<LinkScanner*>(ctx);
    if (self->linkDepth_ < 0 && strcmp(name, "a") == 0) {
      const char* href = getAttribute(atts, "href");
      if (isInternalHref(href) && strchr(href, '#') != nullptr && strlen(href) <= MAX_HREF_BYTES) {
        self->linkDepth_ = self->depth_;
        strncpy(self->href_, href, MAX_HREF_BYTES);
        self->href_[MAX_HREF_BYTES] = '\0';
        self->linkIsNoteref_ =
            FootnoteShape::isNoterefTagged(getAttribute(atts, "epub:type"), getAttribute(atts, "role"));
        self->textLen_ = 0;
        self->textOverflow_ = false;
      }
    }
    ++self->depth_;
  }

  static void characterData(void* ctx, const char* text, const int length) {
    auto* self = static_cast<LinkScanner*>(ctx);
    if (self->linkDepth_ < 0) return;
    for (int i = 0; i < length; ++i) {
      // Leading whitespace does not consume the marker budget. Pretty-printed XHTML writes
      // <a href="#n1">\n        1\n      </a>, whose indentation alone exceeds
      // MAX_MARKER_BYTES — the link would overflow before its marker was ever seen, and a
      // real footnote would be dropped from the cache. isMarkerText trims the rest.
      if (self->textLen_ == 0 && isSpaceChar(text[i])) continue;
      if (self->textLen_ >= MAX_MARKER_BYTES) {
        self->textOverflow_ = true;  // too long to be a marker; noteref tagging may still qualify
        return;
      }
      self->text_[self->textLen_++] = text[i];
    }
  }

  static void endElement(void* ctx, const char*) {
    auto* self = static_cast<LinkScanner*>(ctx);
    --self->depth_;
    if (self->linkDepth_ != self->depth_) return;
    self->text_[self->textLen_] = '\0';
    const bool qualifies =
        self->linkIsNoteref_ || (!self->textOverflow_ && FootnoteShape::isMarkerText(self->text_, self->textLen_));
    if (qualifies && self->targets_.size() < FootnotePreviews::MAX_TARGETS_PER_SPINE) {
      const char* hash = strchr(self->href_, '#');
      const char* fragment = hash + 1;  // '#' presence checked at startElement
      if (*fragment != '\0') {
        const int targetSpine =
            self->href_[0] == '#' ? self->spineIndex_ : self->epub_.resolveHrefToSpineIndex(self->href_);
        if (targetSpine >= 0) {
          const uint32_t keyHash = makeKeyHash(targetSpine, fragment);
          const bool seen = std::any_of(self->targets_.begin(), self->targets_.end(),
                                        [&](const Target& t) { return t.keyHash == keyHash; });
          if (!seen && !self->targets_.push({keyHash, static_cast<uint16_t>(targetSpine)})) {
            self->oom_ = true;  // caller abandons the gather; see NoThrowArray
          }
        }
      }
    }
    self->linkDepth_ = -1;
  }

 public:
  LinkScanner(const Epub& epub, const int spineIndex, NoThrowArray<Target>& targets)
      : epub_(epub), spineIndex_(spineIndex), targets_(targets) {}
  bool outOfMemory() const { return oom_; }
  // Scans chapter XHTML (HTML-flavored): enable bare-void-tag repair.
  bool setup() { return parser_.init(this, startElement, endElement, characterData, nullptr, true); }
  bool feed(const uint8_t* data, const size_t size) { return parser_.feed(data, size); }
  void finalize() { parser_.finalize(); }
};

// Pass B: SAX scan of one target spine document capturing note text at wanted anchors.
// Handles both real-world shapes:
//  - container id (<p id="n3">text</p>, <aside epub:type="rearnote" id=...>): subtree text.
//  - empty inline anchor (<a id="filepos123"/>text…</p>, Calibre/MOBI): the subtree
//    yields nothing, so capture continues past the anchor until its parent block closes
//    or the NEXT wanted anchor starts (sequential rearnote lists).
class NoteCapturer {
 public:
  using EmitFn = std::function<void(uint32_t keyHash, const char* text, size_t len)>;

 private:
  SaxParser parser_;
  const int spineIndex_;                  // the document being scanned; anchors the id hashes
  const NoThrowArray<uint32_t>& wanted_;  // key hashes of the targets that live in this spine
  EmitFn emit_;
  int depth_ = 0;
  int captureDepth_ = -1;  // depth of the id'd element while capturing, -1 = idle
  int skipDepth_ = -1;     // depth of a chrome element being skipped over, -1 = not skipping
  bool tailMode_ = false;  // id element closed short; capturing following siblings
  size_t activeIdx_ = 0;   // wanted_ index being captured
  char text_[FootnotePreviews::MAX_TEXT_BYTES + 1] = {};
  size_t textLen_ = 0;
  bool truncated_ = false;

  // Hash the id exactly as Pass A hashed the href fragment that points at it, so the two sides
  // cannot drift — the same rule the key comment states. Costs one snprintf per id'd element,
  // and saves holding every fragment string in RAM for the whole gather.
  int findWanted(const char* id) const {
    if (!id || *id == '\0') return -1;
    const uint32_t key = makeKeyHash(spineIndex_, id);
    for (size_t i = 0; i < wanted_.size(); ++i) {
      if (wanted_[i] == key) return static_cast<int>(i);
    }
    return -1;
  }

  // Chrome inside a note container that is not part of the note's prose: the number repeated as
  // a heading, and the "back to the text" link most converters append. Both are noise in a
  // preview — the reader is looking at the marker already, and a preview reading
  // "1 Or, if you are a believer in Omnianism, the Pole. note_1" (Small Gods, device-observed)
  // spends a third of its width saying nothing. The capture root itself is never skipped: in the
  // Calibre pattern the anchor IS an <a>.
  static bool isChrome(const char* name) {
    if (strcmp(name, "a") == 0) return true;
    return name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0';
  }

  void beginCapture(const size_t targetIdx, const int idDepth) {
    activeIdx_ = targetIdx;
    captureDepth_ = idDepth;
    skipDepth_ = -1;
    tailMode_ = false;
    textLen_ = 0;
    truncated_ = false;
  }

  void finishCapture() {
    while (textLen_ > 0 && text_[textLen_ - 1] == ' ') --textLen_;
    if (truncated_ && textLen_ >= 3) {
      text_[textLen_ - 3] = '.';
      text_[textLen_ - 2] = '.';
      text_[textLen_ - 1] = '.';
    }
    text_[textLen_] = '\0';
    if (textLen_ > 0) emit_(wanted_[activeIdx_], text_, textLen_);
    captureDepth_ = -1;
    skipDepth_ = -1;
    tailMode_ = false;
  }

  static void startElement(void* ctx, const char* name, const char** atts) {
    auto* self = static_cast<NoteCapturer*>(ctx);
    const int wantedIdx = self->findWanted(getAttribute(atts, "id"));
    if (wantedIdx >= 0) {
      // A new wanted anchor always starts its own capture — in sequential rearnote
      // lists it is also what terminates the previous note's tail capture.
      if (self->captureDepth_ >= 0) self->finishCapture();
      self->beginCapture(static_cast<size_t>(wantedIdx), self->depth_);
    } else if (self->captureDepth_ >= 0 && self->skipDepth_ < 0 && isChrome(name)) {
      self->skipDepth_ = self->depth_;
    }
    ++self->depth_;
  }

  static void characterData(void* ctx, const char* text, const int length) {
    auto* self = static_cast<NoteCapturer*>(ctx);
    if (self->captureDepth_ < 0 || self->skipDepth_ >= 0) return;
    for (int i = 0; i < length; ++i) {
      const bool space = isSpaceChar(text[i]);
      if (space && (self->textLen_ == 0 || self->text_[self->textLen_ - 1] == ' ')) continue;
      if (self->textLen_ >= FootnotePreviews::MAX_TEXT_BYTES) {
        self->truncated_ = true;
        return;
      }
      self->text_[self->textLen_++] = space ? ' ' : text[i];
    }
  }

  static void endElement(void* ctx, const char*) {
    auto* self = static_cast<NoteCapturer*>(ctx);
    --self->depth_;
    if (self->captureDepth_ < 0) return;
    if (self->skipDepth_ >= 0) {
      if (self->depth_ == self->skipDepth_) self->skipDepth_ = -1;
      return;  // chrome closes before anything else is decided
    }
    if (!self->tailMode_ && self->depth_ == self->captureDepth_) {
      // The id'd element itself closed. Enough subtree text = container pattern, done;
      // near-empty = inline anchor pattern, keep capturing the following siblings.
      if (self->textLen_ >= FootnotePreviews::MIN_SUBTREE_BYTES || self->truncated_) {
        self->finishCapture();
      } else {
        self->tailMode_ = true;
      }
    } else if (self->depth_ < self->captureDepth_) {
      // Parent of the anchor closed — the enclosing block is over either way.
      self->finishCapture();
    }
  }

 public:
  NoteCapturer(const int spineIndex, const NoThrowArray<uint32_t>& wanted, EmitFn emit)
      : spineIndex_(spineIndex), wanted_(wanted), emit_(std::move(emit)) {}
  // Scans chapter XHTML (HTML-flavored): enable bare-void-tag repair.
  bool setup() { return parser_.init(this, startElement, endElement, characterData, nullptr, true); }
  bool feed(const uint8_t* data, const size_t size) { return parser_.feed(data, size); }
  void finalize() {
    parser_.finalize();
    if (captureDepth_ >= 0) finishCapture();  // note ran to end-of-document
  }
};

// One spine document, read a chunk at a time, from wherever it is cheapest.
//
// Prefers the inflated XHTML the section builder already left on SD (sections/html_<spine>.bin,
// keyed on the spine alone) over re-opening and re-inflating the ZIP entry. Every spine the
// reader has actually built is sitting there in plain text: inflating it again repeats work a
// build just did, and needs the ZIP's 4 KB EOCD window plus a ~32 KB inflate ring at exactly the
// moment — mid-read, framebuffer resident — the heap can least afford them. Same staleness test
// as the builder: a size mismatch means partial or out-of-date, so fall back to the ZIP (and
// leave the file alone; the next build re-validates it).
//
// next() yields AT MOST one chunk per call and keeps its position, which is what lets the
// resolver spread a document across as many slices as it takes. The one-shot helpers below are
// written on top of it so the two paths cannot drift.
class DocStream {
 public:
  ~DocStream() { close(); }

  // bankIt: write the inflated bytes to the spine's HTML cache as they stream past, so the next
  // chapter pointing into this document reads SD instead of inflating. Ignored when the document
  // is already being read from that cache.
  bool open(Epub& epub, ZipFile& zip, const int spineIndex, const std::string& bankedHtmlPath, const bool bankIt,
            BuildArena* arena = nullptr) {
    close();
    size_t inflatedSize = 0;
    const std::string htmlPath =
        bankedHtmlPath.empty() ? Section::sectionHtmlCachePath(epub.getCachePath(), spineIndex) : bankedHtmlPath;
    if (epub.getSpineItemInflatedSize(spineIndex, &inflatedSize) && inflatedSize > 0) {
      if (Storage.openFileForRead("FNP", htmlPath, file_)) {
        if (file_.size() == inflatedSize) {
          return true;
        }
        file_.close();
      }
    }
    // Only the ZIP path allocates anything worth placing: a read buffer plus an inflate ring
    // sized to the entry (up to 32 KB). Put both in the build's arena when there is room — a
    // resolve runs in slices, so a heap-backed ring would sit on the reading heap across every
    // page render in between. EntryReader does NOT fall back if an arena alloc fails, so the
    // capacity is checked here (mirrors Section::runBuildParse's extract).
    const size_t arenaWanted =
        STREAM_CHUNK_BYTES + InflateReader::ringSizeFor(inflatedSize) + 2 * alignof(std::max_align_t);
    BuildArena* useArena =
        (arena && arena->valid() && arena->capacity() - arena->used() >= arenaWanted) ? arena : nullptr;
    reader_ = makeUniqueNoThrow<ZipFile::EntryReader>(zip, STREAM_CHUNK_BYTES, useArena);
    if (!reader_ || !reader_->open(FsHelpers::normalisePath(epub.getSpineItem(spineIndex).href).c_str())) {
      reader_.reset();
      return false;
    }
    banking_ = bankIt && Storage.openFileForWrite("FNP", htmlPath, bank_);
    return true;
  }

  // Fills `out` with up to `cap` bytes. Returns the byte count and sets *done when the document
  // is exhausted; -1 on a read/inflate error.
  int next(uint8_t* out, const size_t cap, bool* done) {
    *done = false;
    if (file_) {
      const int n = file_.read(out, cap);
      if (n <= 0) {
        *done = true;
        return n < 0 ? -1 : 0;
      }
      return n;
    }
    if (!reader_) {
      *done = true;
      return 0;
    }
    size_t produced = 0;
    if (!reader_->step(out, cap, &produced, done)) {
      return -1;
    }
    // Bank the inflated bytes as they pass. A banked file is only usable if it is complete: the
    // builder's staleness test is a size match, so a stream abandoned part-way simply fails that
    // test later and is re-inflated — nothing to undo here.
    if (banking_ && produced > 0) bank_.write(out, produced);
    return static_cast<int>(produced);
  }

  void close() {
    if (file_) file_.close();
    reader_.reset();
    if (banking_) {
      bank_.close();
      banking_ = false;
    }
  }

 private:
  FsFile file_;                                   // banked XHTML (preferred), unset on the ZIP path
  std::unique_ptr<ZipFile::EntryReader> reader_;  // ZIP path; heap because EntryReader is not movable-in-place
  FsFile bank_;
  bool banking_ = false;
};

// Feeds one whole spine document to a SAX consumer in one call. Returns false on ZIP/read
// errors; SAX-level errors are tolerated (the consumer keeps whatever it saw — same policy as
// the section parser, which survives loose real-world HTML).
template <typename Consumer>
bool streamSpineDocument(Epub& epub, ZipFile& zip, const int spineIndex, const std::string& bankedHtmlPath,
                         uint8_t* chunk, Consumer& consumer, const bool bankIt = false) {
  DocStream doc;
  if (!doc.open(epub, zip, spineIndex, bankedHtmlPath, bankIt)) {
    return false;
  }
  bool done = false;
  while (!done) {
    const int n = doc.next(chunk, STREAM_CHUNK_BYTES, &done);
    if (n < 0) {
      return false;
    }
    if (n > 0 && !consumer.feed(chunk, static_cast<size_t>(n))) {
      break;  // malformed markup mid-stream: keep partial results
    }
  }
  consumer.finalize();
  return true;
}

// Phases of one spine's resolve. Linear: each falls through to the next, and every one of them
// does a BOUNDED amount of work per step() so the caller keeps control of the loop task.
enum class ResolvePhase : uint8_t {
  ScanSpine,    // pass A: SAX-scan this spine's document for footnote-shaped links
  OpenStore,    // read the store, work out which of those targets are missing
  PlanNotes,    // group the missing targets by the document they live in, open the append
  OpenNote,     // start (or finish) one note document
  CaptureNote,  // pass B: SAX-scan that document, capturing text at the wanted anchors
  Commit,       // write the merged index, set the resolved bit
  Finished,
  Failed,
};

}  // namespace

namespace FootnotePreviews {

bool cacheExists(const std::string& bookCachePath) { return Storage.exists((bookCachePath + CACHE_FILENAME).c_str()); }

bool spineResolved(const std::string& bookCachePath, const int spineIndex) {
  if (spineIndex < 0 || spineIndex >= RESOLVED_BITMAP_SPINES) {
    return false;  // outside the bitmap: never claim resolved, so the build still scans
  }
  FsFile file;
  if (!Storage.openFileForRead("FNP", bookCachePath + CACHE_FILENAME, file)) {
    return false;
  }
  uint32_t magic = 0;
  uint16_t version = 0;
  serialization::readPod(file, magic);
  serialization::readPod(file, version);
  uint8_t byte = 0;
  const bool ok = magic == CACHE_MAGIC && version == CACHE_VERSION &&
                  file.seekSet(HEADER_BYTES + static_cast<uint32_t>(spineIndex / 8)) && file.read(&byte, 1) == 1;
  file.close();
  return ok && (byte & (1u << (spineIndex % 8))) != 0;
}

namespace {

// The store's on-disk shape: a header, the resolved-spine bitmap, a blob of length-prefixed
// texts, then the hash index sorted by key. It GROWS -- each resolve pass writes its blobs over
// the old index and then writes a merged index after them.
//
// NOTHING about it is resident in proportion to how much it holds. The index used to be: open()
// read all of it into a NoThrowArray, contains() scanned that linearly, and commit() sorted and
// rewrote it. At 8 bytes a row that made MAX_ENTRIES a RAM budget -- which is why it was 512, and
// why a book with more notes than that silently lost the feature partway through (an 1,100-note
// history stopped expanding notes five chapters in, measured).
//
// Now the index stays on disk end to end. contains() binary-searches it in place; a pass spills
// it aside before its blobs overwrite it, and commit() merges that spill with the pass's own rows
// -- at most MAX_TARGETS_PER_SPINE of them, ~1 KB -- straight back out to the file. The cap is
// now a question about SD space and lookup depth, both cheap (8 bytes a row, log2 reads a
// lookup), rather than about heap. Same shape as BookMetadataCache's spine.bin.tmp pass and the
// section cache's anchor spill.
class Store {
  std::string path_;
  FsFile file_;
  uint8_t resolved_[RESOLVED_BITMAP_BYTES] = {};
  uint32_t blobEnd_ = BLOB_START;      // where the next blob record goes
  uint32_t indexOffset_ = BLOB_START;  // where the index sat when this pass began
  uint16_t existing_ = 0;              // rows present before this pass
  // Only THIS pass's rows. A pass resolves at most one spine's outstanding targets, so this is
  // bounded by MAX_TARGETS_PER_SPINE (~1 KB) whatever MAX_ENTRIES is.
  NoThrowArray<IndexRow> newRows_;
  bool spilled_ = false;

  // Where the pre-pass index is parked while the pass writes over it. Keyed on the store rather
  // than the spine: only one resolve runs at a time, stepped from the build that owns it.
  std::string spillPath() const { return path_ + ".idx"; }

  // HalFile::close() on a handle that was never opened aborts on device, and this class has
  // several paths that reach a close without knowing whether the last one opened anything.
  void closeIfOpen() {
    if (file_) file_.close();
  }

  // Writes header + bitmap at the current position. The caller positions and closes.
  void writeHeader(const uint16_t count, const uint32_t indexOffset) {
    serialization::writePod(file_, CACHE_MAGIC);
    serialization::writePod(file_, CACHE_VERSION);
    serialization::writePod(file_, count);
    serialization::writePod(file_, indexOffset);
    file_.write(resolved_, RESOLVED_BITMAP_BYTES);
  }

 public:
  ~Store() { closeIfOpen(); }

  // Reads an existing store's header, or starts an empty one. False means "cannot work with this
  // store" -- the caller leaves the file alone. The index is NOT read: contains() searches it
  // where it lies, which is why the handle is left open here.
  bool open(const std::string& path) {
    path_ = path;
    newRows_.clear();
    spilled_ = false;
    if (!Storage.exists(path.c_str())) {
      indexOffset_ = blobEnd_ = BLOB_START;
      existing_ = 0;
      return true;
    }
    if (!Storage.openFileForRead("FNP", path, file_)) {
      return false;
    }
    uint32_t magic = 0, indexOffset = 0;
    uint16_t version = 0, count = 0;
    serialization::readPod(file_, magic);
    serialization::readPod(file_, version);
    serialization::readPod(file_, count);
    serialization::readPod(file_, indexOffset);
    const uint32_t expectedEnd = indexOffset + static_cast<uint32_t>(count) * INDEX_ROW_BYTES;
    if (magic != CACHE_MAGIC || version != CACHE_VERSION || count > FootnotePreviews::MAX_ENTRIES ||
        expectedEnd != file_.size() || indexOffset < BLOB_START) {
      LOG_ERR("FNP", "Store is not usable (magic=%08lx version=%u count=%u); starting a new one",
              static_cast<unsigned long>(magic), version, count);
      file_.close();
      Storage.remove(path.c_str());
      indexOffset_ = blobEnd_ = BLOB_START;
      existing_ = 0;
      return true;
    }
    if (file_.read(resolved_, RESOLVED_BITMAP_BYTES) != static_cast<int>(RESOLVED_BITMAP_BYTES)) {
      file_.close();
      return false;
    }
    existing_ = count;
    indexOffset_ = indexOffset;
    blobEnd_ = indexOffset;
    return true;
  }

  // Do we already hold this note? Searched on disk against the handle open() left open, so it
  // costs ~log2(count) eight-byte reads and nothing resident.
  bool contains(const uint32_t keyHash) {
    for (const IndexRow& row : newRows_) {
      if (row.keyHash == keyHash) return true;
    }
    if (existing_ == 0 || !file_) return false;
    uint32_t blobOffset = 0;
    return findIndexRow(file_, indexOffset_, existing_, keyHash, blobOffset);
  }

  bool full() const { return static_cast<size_t>(existing_) + newRows_.size() >= FootnotePreviews::MAX_ENTRIES; }
  size_t added() const { return newRows_.size(); }

  static bool inBitmap(const int spineIndex) { return spineIndex >= 0 && spineIndex < RESOLVED_BITMAP_SPINES; }
  bool isResolved(const int spineIndex) const {
    return inBitmap(spineIndex) && (resolved_[spineIndex / 8] & (1u << (spineIndex % 8))) != 0;
  }
  void markResolved(const int spineIndex) {
    if (inBitmap(spineIndex)) resolved_[spineIndex / 8] |= static_cast<uint8_t>(1u << (spineIndex % 8));
  }

  // Patches the bitmap where it lies, touching neither blob nor index. This is the path for a
  // chapter with nothing to add -- no notes, or none outstanding -- which is most chapters in
  // most books (20 of The Anarchy's 40 spines). It used to go the long way through
  // beginAppend()/commit(), rewriting the entire index to flip one bit.
  bool writeBitmapInPlace() {
    if (!Storage.exists(path_.c_str())) return false;
    closeIfOpen();
    if (!Storage.openFileForUpdate("FNP", path_, file_)) return false;
    const bool ok =
        file_.seekSet(HEADER_BYTES) && file_.write(resolved_, RESOLVED_BITMAP_BYTES) == RESOLVED_BITMAP_BYTES;
    file_.close();
    return ok;
  }

  // Parks the existing index in the spill file, then opens the store positioned where that index
  // started: the first blob written here lands on top of it, which is safe precisely because the
  // copy that matters is now the spill.
  bool beginAppend() {
    closeIfOpen();
    if (existing_ > 0) {
      FsFile in, out;
      if (!Storage.openFileForRead("FNP", path_, in)) return false;
      if (!Storage.openFileForWrite("FNP", spillPath(), out)) {
        in.close();
        return false;
      }
      const bool ok = in.seekSet(indexOffset_) &&
                      serialization::copyBytes(in, out, static_cast<uint32_t>(existing_) * INDEX_ROW_BYTES);
      out.flush();
      out.close();
      in.close();
      if (!ok) {
        LOG_ERR("FNP", "Could not park the index before appending; leaving the store alone");
        Storage.remove(spillPath().c_str());
        return false;
      }
      spilled_ = true;
    }
    const bool opened = Storage.exists(path_.c_str()) ? Storage.openFileForUpdate("FNP", path_, file_)
                                                      : Storage.openFileForWrite("FNP", path_, file_);
    if (!opened) {
      if (spilled_) Storage.remove(spillPath().c_str());
      spilled_ = false;
      return false;
    }
    if (existing_ == 0) {
      // Fresh store: lay down a placeholder header and an empty bitmap, patched by commit().
      writeHeader(0, 0);
    }
    return file_.seekSet(blobEnd_);
  }

  void addNote(const uint32_t keyHash, const char* text, const size_t len) {
    if (full()) return;
    // Remember it BEFORE writing: a row we cannot record is a blob nothing could ever find, and
    // writing it anyway would push blobEnd_ past bytes the index does not describe.
    if (!newRows_.push({keyHash, blobEnd_})) return;
    serialization::writePod(file_, static_cast<uint16_t>(len));
    file_.write(reinterpret_cast<const uint8_t*>(text), len);
    blobEnd_ += static_cast<uint32_t>(sizeof(uint16_t) + len);
  }

  // Merges the parked index with this pass's rows and patches the header. Both sides are sorted,
  // so this is one streaming pass over the spill against one walk of newRows_: the merged index
  // is written straight out, never assembled in memory.
  bool commit() {
    std::sort(newRows_.begin(), newRows_.end());
    if (!file_.seekSet(blobEnd_)) return false;

    FsFile spill;
    if (spilled_ && !Storage.openFileForRead("FNP", spillPath(), spill)) {
      LOG_ERR("FNP", "Lost the parked index; the store keeps what it had");
      return false;
    }

    uint16_t written = 0;
    size_t next = 0;  // cursor into newRows_
    uint16_t remaining = spilled_ ? existing_ : 0;
    IndexRow old{0, 0};
    bool haveOld = false;
    while (true) {
      if (!haveOld && remaining > 0) {
        uint32_t row[2] = {0, 0};
        if (spill.read(reinterpret_cast<uint8_t*>(row), sizeof(row)) != static_cast<int>(sizeof(row))) {
          spill.close();
          return false;
        }
        old = {row[0], row[1]};
        haveOld = true;
        --remaining;
      }
      const bool haveNew = next < newRows_.size();
      if (!haveOld && !haveNew) break;
      // On a tie the stored row wins. Belt and braces rather than load-bearing: OpenStore filters
      // this pass's targets through contains() before any of this runs, so a key already on disk
      // never reaches newRows_ and the tie cannot currently occur. It costs one comparison and
      // keeps the merge correct on its own terms if that filtering ever moves.
      const bool takeOld = haveOld && (!haveNew || old.keyHash <= newRows_[next].keyHash);
      const IndexRow& row = takeOld ? old : newRows_[next];
      serialization::writePod(file_, row.keyHash);
      serialization::writePod(file_, row.blobOffset);
      ++written;
      if (takeOld) {
        haveOld = false;
      } else {
        ++next;
      }
    }
    if (spilled_) spill.close();

    if (!file_.seekSet(0)) return false;
    writeHeader(written, blobEnd_);
    file_.close();
    if (spilled_) {
      Storage.remove(spillPath().c_str());
      spilled_ = false;
    }
    // What this pass wrote is now the store's state, so a second commit -- or an abandon after
    // one -- is a no-op rather than merging the same rows a second time.
    existing_ = written;
    indexOffset_ = blobEnd_;
    newRows_.clear();
    return true;
  }

  // Drops what THIS pass appended and puts the parked index back, so the store stays exactly as
  // complete as it was. The abandoned blobs are left as an unreferenced gap ahead of the restored
  // index -- deliberately: the file's length has to stay exactly indexOffset + count*8 for open()
  // and Lookup to accept it, and there is no truncate.
  void abandon() {
    newRows_.clear();
    if (!file_) {
      if (spilled_) Storage.remove(spillPath().c_str());
      spilled_ = false;
      return;
    }
    bool ok = file_.seekSet(blobEnd_);
    if (ok && spilled_) {
      FsFile spill;
      if (Storage.openFileForRead("FNP", spillPath(), spill)) {
        ok = serialization::copyBytes(spill, file_, static_cast<uint32_t>(existing_) * INDEX_ROW_BYTES);
        spill.close();
      } else {
        ok = false;
      }
    }
    if (ok && file_.seekSet(0)) {
      writeHeader(existing_, blobEnd_);
      indexOffset_ = blobEnd_;
    } else {
      LOG_ERR("FNP", "Could not restore the parked index; the store will be rebuilt on next open");
    }
    file_.close();
    if (spilled_) {
      Storage.remove(spillPath().c_str());
      spilled_ = false;
    }
  }
};

// Sets a spine's bit and writes the store back. Used on the paths that have nothing to append —
// a chapter with no notes, or one whose notes are all stored already — where commit() is just the
// bitmap write plus an unchanged index.
bool markSpineResolved(Store& store, const int spineIndex) {
  if (store.isResolved(spineIndex) || !Store::inBitmap(spineIndex)) {
    return true;
  }
  store.markResolved(spineIndex);
  // The bitmap sits at a fixed offset, so an existing store takes the bit in place -- no index
  // work at all. Only a store that does not exist yet has to be laid down first.
  if (store.writeBitmapInPlace()) {
    return true;
  }
  if (!store.beginAppend()) {
    return false;
  }
  return store.commit();
}

}  // namespace

struct Resolver::State {
  Epub* epub = nullptr;
  int spineIndex = -1;
  std::string banked;
  BuildArena* arena = nullptr;
  uint32_t startMs = 0;
  ResolvePhase phase = ResolvePhase::ScanSpine;

  std::unique_ptr<uint8_t[]> chunk;
  // Declared before `doc`: members destroy in reverse order and the reader inside DocStream
  // holds a handle (and possibly an arena block) that must go first.
  std::unique_ptr<ZipFile> zip;
  DocStream doc;
  bool docOpen = false;

  NoThrowArray<Target> targets;
  std::unique_ptr<LinkScanner> scanner;

  Store store;
  bool appending = false;  // beginAppend() succeeded; teardown must roll the store back
  NoThrowArray<Target> missing;
  NoThrowArray<uint16_t> noteSpines;
  size_t noteCursor = 0;
  NoThrowArray<uint32_t> wanted;
  std::unique_ptr<NoteCapturer> capturer;

  // Rolls back a half-finished append so the store stays exactly as complete as it was. Safe to
  // call repeatedly, and called from the destructor: a resolve preempted mid-way (the reader
  // turned a page and Background-B handed its buffer back) must leave nothing behind.
  void rollBack() {
    if (appending) {
      appending = false;
      store.abandon();
    }
  }
};

Resolver::Resolver() = default;
Resolver::~Resolver() {
  if (state_) state_->rollBack();
}

bool Resolver::begin(Epub& epub, const int spineIndex, const std::string& bankedHtmlPath, BuildArena* arena) {
  state_ = makeUniqueNoThrow<State>();
  if (!state_) {
    LOG_ERR("FNP", "OOM: cannot allocate the resolver");
    return false;
  }
  State& st = *state_;
  st.epub = &epub;
  st.spineIndex = spineIndex;
  st.banked = bankedHtmlPath;
  st.arena = arena;
  st.startMs = millis();

  // Already done, in this session or an earlier one: no scan, no parser, no file reads beyond
  // one 13-byte peek. Without this every rebuild of a spine — a font change, a variant miss —
  // re-scanned the whole document with a 9.2 KB parser only to learn that nothing was missing.
  if (spineResolved(epub.getCachePath(), spineIndex)) {
    st.phase = ResolvePhase::Finished;
    return true;
  }

  st.chunk = makeUniqueNoThrow<uint8_t[]>(STREAM_CHUNK_BYTES);
  if (!st.chunk || !st.targets.reserve(16)) {
    LOG_ERR("FNP", "OOM: cannot allocate the resolver's scan buffers");
    return false;
  }
  // Reuse the book's cached central-directory details: without this every pass re-scans for the
  // EOCD record, a 4 KB contiguous allocation before a single byte is read. Spines served from
  // banked XHTML never touch the ZIP at all.
  st.zip = makeUniqueNoThrow<ZipFile>(epub.getPath());
  if (!st.zip) {
    LOG_ERR("FNP", "OOM: cannot allocate the archive reader");
    return false;
  }
  epub.primeZip(*st.zip);
  return true;
}

Resolver::Step Resolver::step() {
  if (!state_) {
    return Step::Failed;
  }
  State& st = *state_;
  const auto fail = [&st](const char* what) {
    LOG_ERR("FNP", "%s (spine=%d)", what, st.spineIndex);
    st.doc.close();
    st.rollBack();
    st.phase = ResolvePhase::Failed;
    return Step::Failed;
  };

  switch (st.phase) {
    case ResolvePhase::Finished:
      return Step::Done;
    case ResolvePhase::Failed:
      return Step::Failed;

    case ResolvePhase::ScanSpine: {
      if (!st.docOpen) {
        st.scanner = makeUniqueNoThrow<LinkScanner>(*st.epub, st.spineIndex, st.targets);
        if (!st.scanner || !st.scanner->setup()) {
          return fail("Link scanner setup failed");
        }
        if (!st.doc.open(*st.epub, *st.zip, st.spineIndex, st.banked, /*bankIt=*/false, st.arena)) {
          return fail("Could not read the spine for its link scan");
        }
        st.docOpen = true;
        return Step::More;
      }
      bool done = false;
      const int n = st.doc.next(st.chunk.get(), STREAM_CHUNK_BYTES, &done);
      if (n < 0) {
        return fail("Read error during the link scan");
      }
      // A short feed means malformed markup: keep the partial results, exactly as the section
      // parser does, and stop reading this document.
      if (n > 0 && !st.scanner->feed(st.chunk.get(), static_cast<size_t>(n))) {
        done = true;
      }
      if (!done) {
        return Step::More;
      }
      st.scanner->finalize();
      const bool oom = st.scanner->outOfMemory();
      st.scanner.reset();
      st.doc.close();
      st.docOpen = false;
      if (oom) {
        return fail("OOM growing the target list");
      }
      st.phase = ResolvePhase::OpenStore;
      return Step::More;
    }

    case ResolvePhase::OpenStore: {
      if (!st.store.open(st.epub->getCachePath() + CACHE_FILENAME)) {
        return fail("Could not open the preview store");
      }
      // A chapter with no notes still gets its bit, and that matters: the bit means "scanned,
      // nothing outstanding", not "has notes". Background-B reads it to size the gates it puts
      // in front of a build, and a book without footnotes must not look permanently unresolved.
      // The same is true of a chapter whose every target another chapter already stored.
      if (!st.missing.reserve(st.targets.size() > 0 ? st.targets.size() : 1)) {
        return fail("OOM sizing the missing-target list");
      }
      for (const Target& t : st.targets) {
        if (!st.store.contains(t.keyHash)) st.missing.push(t);
      }
      if (st.missing.size() == 0) {
        LOG_DBG("FNP", "Spine %d: %u note targets, all already resolved", st.spineIndex,
                static_cast<uint32_t>(st.targets.size()));
        st.phase = ResolvePhase::Commit;
        return Step::More;
      }
      if (st.store.full()) {
        // Not a failure: the book is simply larger than the budget. No bit either — the targets
        // really are outstanding, so a later pass with room should still try.
        LOG_ERR("FNP", "Preview store is at its %u-entry cap; spine %d keeps plain markers",
                static_cast<uint32_t>(MAX_ENTRIES), st.spineIndex);
        st.phase = ResolvePhase::Finished;
        return Step::Done;
      }
      st.phase = ResolvePhase::PlanNotes;
      return Step::More;
    }

    case ResolvePhase::PlanNotes: {
      if (!st.noteSpines.reserve(st.missing.size())) {
        return fail("OOM planning the note documents");
      }
      for (const Target& t : st.missing) {
        if (std::find(st.noteSpines.begin(), st.noteSpines.end(), t.spineIndex) == st.noteSpines.end()) {
          st.noteSpines.push(t.spineIndex);
        }
      }
      if (!st.store.beginAppend()) {
        return fail("Could not open the preview store for append");
      }
      st.appending = true;
      st.phase = ResolvePhase::OpenNote;
      return Step::More;
    }

    case ResolvePhase::OpenNote: {
      if (st.noteCursor >= st.noteSpines.size()) {
        st.phase = ResolvePhase::Commit;
        return Step::More;
      }
      const uint16_t noteSpine = st.noteSpines[st.noteCursor];
      st.wanted.clear();
      if (!st.wanted.reserve(st.missing.size())) {
        return fail("OOM listing the anchors wanted from one note document");
      }
      for (const Target& t : st.missing) {
        if (t.spineIndex == noteSpine) st.wanted.push(t.keyHash);
      }
      st.capturer = makeUniqueNoThrow<NoteCapturer>(
          noteSpine, st.wanted,
          [&st](const uint32_t keyHash, const char* text, const size_t len) { st.store.addNote(keyHash, text, len); });
      if (!st.capturer || !st.capturer->setup()) {
        return fail("Note capturer setup failed");
      }
      // A note document the reader has already visited is banked like any other spine; one that
      // has not is streamed from the ZIP and banked on the way past, so the next chapter that
      // points into it pays an SD read instead of an inflate.
      if (!st.doc.open(*st.epub, *st.zip, noteSpine, {}, /*bankIt=*/true, st.arena)) {
        return fail("Could not read a note document");
      }
      st.docOpen = true;
      st.phase = ResolvePhase::CaptureNote;
      return Step::More;
    }

    case ResolvePhase::CaptureNote: {
      bool done = false;
      const int n = st.doc.next(st.chunk.get(), STREAM_CHUNK_BYTES, &done);
      if (n < 0) {
        return fail("Read error inside a note document");
      }
      if (n > 0 && !st.capturer->feed(st.chunk.get(), static_cast<size_t>(n))) {
        done = true;
      }
      if (!done) {
        return Step::More;
      }
      st.capturer->finalize();
      st.capturer.reset();
      st.doc.close();
      st.docOpen = false;
      st.noteCursor++;
      st.phase = ResolvePhase::OpenNote;
      return Step::More;
    }

    case ResolvePhase::Commit: {
      const size_t added = st.store.added();
      if (st.appending) {
        st.appending = false;  // commit() consumes the append; nothing left to roll back
        st.store.markResolved(st.spineIndex);
        if (!st.store.commit()) {
          return fail("Could not write the preview store");
        }
      } else if (!markSpineResolved(st.store, st.spineIndex)) {
        // Nothing was appended, so the store is not open: markSpineResolved does its own
        // beginAppend/commit, and it must see the bit still unset — hence no markResolved here.
        return fail("Could not write the preview store");
      }
      st.epub->adoptZipDetails(*st.zip);
      if (added > 0) {
        LOG_INF("FNP", "Spine %d: resolved %u of %u note targets from %u document(s) in %lums", st.spineIndex,
                static_cast<uint32_t>(added), static_cast<uint32_t>(st.missing.size()),
                static_cast<uint32_t>(st.noteSpines.size()), millis() - st.startMs);
      }
      st.phase = ResolvePhase::Finished;
      return Step::Done;
    }
  }
  return Step::Failed;
}

bool resolveSpine(Epub& epub, const int spineIndex, const std::string& bankedHtmlPath) {
  Resolver resolver;
  if (!resolver.begin(epub, spineIndex, bankedHtmlPath)) {
    return false;
  }
  Resolver::Step step = Resolver::Step::More;
  while (step == Resolver::Step::More) {
    step = resolver.step();
  }
  return step == Resolver::Step::Done;
}

bool Lookup::open(const std::string& bookCachePath, const Epub* epub, const int currentSpineIndex) {
  entryCount_ = 0;
  epub_ = epub;
  currentSpineIndex_ = currentSpineIndex;
  if (!Storage.openFileForRead("FNP", bookCachePath + CACHE_FILENAME, file_)) {
    return false;
  }
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t count = 0;
  uint32_t indexOffset = 0;
  serialization::readPod(file_, magic);
  serialization::readPod(file_, version);
  serialization::readPod(file_, count);
  serialization::readPod(file_, indexOffset);
  const uint32_t expectedEnd = indexOffset + static_cast<uint32_t>(count) * INDEX_ENTRY_BYTES;
  if (magic != CACHE_MAGIC || version != CACHE_VERSION || count > MAX_ENTRIES || expectedEnd != file_.size()) {
    LOG_ERR("FNP", "Invalid footnotes.bin (magic=%08lx version=%u count=%u); previews skipped",
            static_cast<unsigned long>(magic), version, count);
    file_.close();
    return false;
  }
  if (count == 0) {
    file_.close();
    return false;  // valid-but-empty: nothing to look up, keep the parser fast path off
  }
  // No allocation: find() searches the index where it lies.
  indexOffset_ = indexOffset;
  entryCount_ = count;
  lastPathLen_ = 0;  // the memo below is keyed within one open store
  lastPathSpine_ = -1;
  return true;
}

int Lookup::resolveTargetSpine(const char* href, const size_t pathLen) {
  // Every caller in a chapter points at the same one or two note documents -- the whole point of
  // a rearnotes file -- so the same path is resolved over and over. Epub::resolveHrefToSpineIndex
  // answers it by walking the ENTIRE spine and reading each entry out of book.bin (two seeks, two
  // reads and a std::string per item), which is why the repetition is worth avoiding rather than
  // merely inelegant: a chapter of a heavily annotated history with 162 callers over a 40-item
  // spine spent ~6,500 book.bin entry reads and ~13,000 short-lived string allocations on one
  // question with one answer. That runs inside the section build, where the heap is at its
  // tightest, and again on the loop task every time the footnote list is opened.
  //
  // One slot is enough for that access pattern, and it is a fixed buffer rather than a
  // std::string so the saving does not itself land on the heap. A path too long to memo (or an
  // empty one, which cannot occur for a href with a '#' after position 0) simply resolves the
  // long way, as before.
  const bool memoable = pathLen > 0 && pathLen < sizeof(lastPath_);
  if (memoable && lastPathLen_ == pathLen && memcmp(lastPath_, href, pathLen) == 0) {
    return lastPathSpine_;
  }
  const int spine = epub_->resolveHrefToSpineIndex(href);
  if (memoable) {
    memcpy(lastPath_, href, pathLen);
    lastPathLen_ = pathLen;
    lastPathSpine_ = spine;
  }
  return spine;
}

bool Lookup::find(const char* href, std::string& outText) {
  if (entryCount_ == 0 || !href) return false;
  const char* hash = strchr(href, '#');
  if (!hash || hash[1] == '\0') return false;
  int targetSpine = currentSpineIndex_;
  if (href[0] != '#') {
    if (!epub_) return false;
    targetSpine = resolveTargetSpine(href, static_cast<size_t>(hash - href));
    if (targetSpine < 0) return false;
  }
  const uint32_t keyHash = makeKeyHash(targetSpine, hash + 1);

  // Searched where it lies, by the same helper the Store uses to ask whether it already holds a
  // note -- one implementation of the format for both sides.
  uint32_t blobOffset = 0;
  if (!findIndexRow(file_, indexOffset_, entryCount_, keyHash, blobOffset)) return false;

  uint16_t len = 0;
  if (!file_.seekSet(blobOffset)) return false;
  serialization::readPod(file_, len);
  if (len == 0 || len > MAX_TEXT_BYTES) return false;
  outText.resize(len);
  return file_.read(reinterpret_cast<uint8_t*>(&outText[0]), len) == static_cast<int>(len);
}

}  // namespace FootnotePreviews
