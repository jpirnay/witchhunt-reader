#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class BuildArena;  // lib/Memory — optional scratch for a note document's inflate ring
class Epub;

// Book-level inline-footnote preview cache ("footnotes.bin" in the book's cache dir).
//
// Real-world books rarely carry the EPUB 3 semantic markup the original per-chapter
// prepass relied on: notes are centralised in a separate rearnotes spine file (classic
// French/academic layout), or scattered across tiny Calibre split files addressed by
// bare `filepos` anchors with untagged `*` reference links (MOBI conversions). Both
// are invisible to a same-file, epub:type-gated scan.
//
// This module resolves notes ONE SPINE AT A TIME, from inside that spine's build:
//   Pass A: scan this spine's document for footnote-shaped links — short marker text
//           (*, †, digits …) or noteref-tagged — and collect their resolved targets
//           (spine index + fragment). Reads the inflated XHTML the build just banked,
//           so it costs an SD read and a SAX walk, never a second inflate.
//   Pass B: for the targets not already in the store, stream each target document once,
//           capturing the note text at every wanted anchor id (subtree text, or the text
//           FOLLOWING an empty inline anchor until its parent block closes — the Calibre
//           filepos pattern), and append the results.
// Results accumulate in footnotes.bin as the reader moves through the book; section builds
// read them back through Lookup, which holds nothing in RAM.
//
// Doing this INSIDE the build, before its layout parse, is what makes inline expansion safe:
// preview text is a layout input, so it must be present before the spine is laid out or the
// resulting page cache would be keyed "previews on" while showing bare markers. The old design
// scanned the whole book on first sighting of a footnote and then had to throw away and rebuild
// the chapter the reader was sitting in.
//
// File format v1 (little-endian):
//   u32 magic 'FNP1' | u16 version | u16 count | u32 indexOffset
//   blob:  count x { u16 textLen, char text[textLen] }   (gather order)
//   index: count x { u32 keyHash, u32 blobOffset }        (sorted by keyHash)
// Key string: "<targetSpineIndex>#<fragment>", FNV-1a 32-bit. A 32-bit collision
// inside one book shows a wrong preview — cosmetically harmless, astronomically rare.
namespace FootnotePreviews {

constexpr const char* CACHE_FILENAME = "/footnotes.bin";
constexpr size_t MAX_ENTRIES = 512;      // gather cap; index cap in Lookup matches
constexpr size_t MAX_TEXT_BYTES = 240;   // per-preview text cap (matches old table)
constexpr size_t MIN_SUBTREE_BYTES = 8;  // below this, an id'd element is treated as an
                                         // empty anchor and capture continues past it

// True when the book's preview store exists (cheap existence probe).
bool cacheExists(const std::string& bookCachePath);

// True when this spine's links have been scanned and every note they point at is stored — i.e.
// building it needs no resolve work at all. Costs one small read. Note the question is "is there
// anything outstanding", NOT "does it have notes": a chapter without footnotes answers true.
//
// Background-B asks it to size its gates, NOT to decide whether to build: it must never be used
// to skip a spine, because the bit is only ever set BY a build, which makes skipping
// self-perpetuating — that was issue #211, where look-ahead died for whole books.
bool spineResolved(const std::string& bookCachePath, int spineIndex);

// Most footnote-shaped links one spine may contribute in a single pass. 8 bytes each, so the
// scan's own footprint is bounded at 1 KB however many notes a chapter carries; links past the
// cap keep their plain marker and stay navigable. A chapter with more than this many notes does
// not exist in practice — Feet of Clay's heaviest has 5.
constexpr size_t MAX_TARGETS_PER_SPINE = 128;

// Resolves every footnote-shaped link in `spineIndex`'s document whose note text is not already
// in the store, and appends what it finds. Idempotent: a spine that has been resolved before
// re-scans its links (cheap, from banked XHTML) and finds every target present, appending
// nothing. `bankedHtmlPath`, when non-empty and matching the spine's inflated size, is read
// instead of the ZIP entry.
//
// Returns false when the pass could not complete — the store is left exactly as it was, never
// half-updated, and the caller may retry later. Note text that no note file yields (a target
// whose anchor does not exist) is simply absent; there is no negative caching, because "not
// found this time" and "this book has no such note" are not the same statement.
bool resolveSpine(Epub& epub, int spineIndex, const std::string& bankedHtmlPath = {});

// The same work, resumable. resolveSpine() above is this class run to completion in one call.
//
// The resolve runs on the loop task from inside a section build, so it cannot be allowed to take
// however long the book feels like: a 200 KB chapter and a chapter with a hundred callers into a
// fat rearnotes document must both stay responsive. step() therefore does AT MOST one
// STREAM_CHUNK_BYTES chunk of one document, or one bookkeeping transition, and returns — the
// caller decides how many to run before yielding (Section spends its slice budget on them, the
// same way it does on the layout parse). Nothing here measures time, which also makes the
// slicing deterministic to test.
//
// Preemption is safe at any point. The store is only ever written between beginAppend() and
// commit(), and the destructor rolls a half-finished append back, so a resolver torn down
// mid-document (the reader turned a page, Background-B handed its buffer back) leaves the store
// exactly as complete as it found it. The spine's resolved bit is set only by a pass that ran
// to Done, so an abandoned one simply starts again next time.
class Resolver {
 public:
  enum class Step : uint8_t { More, Done, Failed };

  Resolver();
  ~Resolver();
  Resolver(const Resolver&) = delete;
  Resolver& operator=(const Resolver&) = delete;

  // Prepares the pass. `arena`, when it has room, backs the inflate ring and read buffer of any
  // note document that has to come out of the ZIP — worth passing from a build that owns one,
  // since a sliced resolve would otherwise hold that ring on the heap across page renders.
  // False means the pass could not even start (OOM); the store is untouched.
  bool begin(Epub& epub, int spineIndex, const std::string& bankedHtmlPath = {}, BuildArena* arena = nullptr);

  // One bounded unit of work. More = call again, Done = the spine is resolved and its bit set,
  // Failed = the pass gave up and rolled back (the caller may retry later).
  Step step();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Disk-backed lookup used during a section build. Holds NOTHING but an open read handle: the
// sorted index is binary-searched in the file itself, and the text is read on a hit.
class Lookup {
 public:
  // Opens and validates footnotes.bin. currentSpineIndex anchors same-file ("#frag")
  // references. Returns false when the file is missing or invalid (previews skipped).
  bool open(const std::string& bookCachePath, const Epub* epub, int currentSpineIndex);

  // Resolves an href from the chapter being parsed ("#frag", "notes.xhtml#frag",
  // "../Text/notes.xhtml#frag") against the index. On a hit fills outText and
  // returns true.
  bool find(const char* href, std::string& outText);

  bool isOpen() const { return entryCount_ > 0; }

 private:
  // The index used to be read into a heap array of entryCount_ entries — up to 4 KB contiguous,
  // allocated at build SETUP and held for the whole section build, which is the tightest the heap
  // ever gets. Searching it in place costs ~log2(count) eight-byte reads per lookup (nine at the
  // 512-entry cap), nearly all landing in the same one or two filesystem blocks, against a build
  // that runs for seconds. It also removes the allocation that could fail: Section falls back to
  // building WITHOUT previews when the lookup will not open, yet still caches that chapter under
  // the previews-ON hash, so one unlucky allocation used to cost a chapter its previews for good.
  static constexpr uint32_t INDEX_ENTRY_BYTES = 8;  // u32 keyHash + u32 blobOffset
  uint32_t indexOffset_ = 0;
  uint16_t entryCount_ = 0;
  FsFile file_;
  const Epub* epub_ = nullptr;
  int currentSpineIndex_ = -1;
};

}  // namespace FootnotePreviews
