#pragma once

#include <cstddef>
#include <cstdint>

// Whether a section build may run in place (the X4's resident build: the secondary framebuffer stays where it is, and
// the build runs on the owned 10 KB heap arena) or borrows the secondary buffer as its arena instead.
// EpubReaderActivity::heapAllowsInPlaceBuild asks it, for Background-C (chooseSectionBuildMode) and for the blocking
// build (compileSectionCache), after its CSS-index check (Section::heapAllowsEmbeddedStyle).
//
// Foreground in-place section build (the "keep the secondary buffer" path). When heap is
// ample we build the new section WITHOUT releasing the secondary framebuffer, so the chapter's
// first page keeps a valid fast-refresh baseline and avoids the baseline-resetting half-
// refresh. Conservative floors: the foreground build runs at page-turn time with the secondary
// buffer (and possibly a pre-rendered page) resident, so pin above Background-B's idle gate.
// Failure is recoverable — the build retries with the buffer released — so these gate "try in
// place" rather than guaranteeing success; tune from the "Index start mem" serial logs.
//
// SHAPE (fixed 2026-08-15, device-measured — see admitsResident): the free floor is the
// MAXIMUM of the two phase peaks, not their sum. runBuildParse releases the inflate ring before
// the parse begins, so the ring and the layout working set are never live together; adding them
// asked for memory no build has ever needed. The bases below are the PARSE-phase floors and are
// unchanged by that fix — only the arithmetic combining them with the ring changed, so an A/B
// against the old behaviour reads cleanly.
#ifndef IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES
#define IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES (60 * 1024)
#endif
#ifndef IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES
#define IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES (28 * 1024)
#endif
// Extract-phase free floor, to which the entry's ring is added (the ring IS live in this phase,
// so here the sum is correct). Its own name: this floor is reached from a page turn, not from
// Background-B's borrow-first path (whose heap-backed twin was deleted in the 2026-09 audit).
//
// 30 -> 50 KB (2026-09-01), device-measured on Small Gods spine 1 — the whole book in one
// 583991-byte entry — which the 30 KB version admitted to a resident build that could not
// possibly finish. The trace, from the gate to the abort ~170 ms later:
//
//   gate      free=71592  (floor was 67584: the CSS base, since 30720 + 32768 = 63488 lost the max)
//   start     free=60396  -11196  activity/section/popup before the build begins
//   setup     free=56648   -3748  CSS index load
//   abort     free=19924  -36724  ZipFile + read buffer + the 32768 inflate ring
//
// So the ring is only two thirds of what the extract phase costs from here; the other ~18.9 KB is
// fixed overhead the base is meant to cover, and on top of that the build has to stay above
// RESIDENT_BUILD_ABORT_FREE_HEAP_BYTES (30 KB) or it aborts anyway. 18.9 + 30 = 48.9 KB, so 50
// with a little margin. Note what this does NOT change: with the ring capped at 32 KB the base
// only binds for large entries — a few-KB chapter still floors at the flat 60/66 KB above and
// still builds in place. What it changes is that a whole-book-in-one-spine entry now goes
// straight to the released path instead of paying ~1.4 s for setup, abort, cache clear and a
// second setup to get there.
#ifndef IN_PLACE_BUILD_EXTRACT_BASE_HEAP_BYTES
#define IN_PLACE_BUILD_EXTRACT_BASE_HEAP_BYTES (50 * 1024)
#endif
// CSS books need more margin to build in place: the parse resolves embedded styles, and the lean
// resolver skips lookups below its floor, which yields a css-degraded cache and (since audit R3
// step 4) one released rebuild -- seconds of resident work thrown away. Since every build is
// two-phase (the inflate ring is released BEFORE the CSS-resolving parse), the resolve runs with
// the ring gone; contig stays pinned at the ring size (<=32 KB) for the extraction phase.
//
// Re-derived 2026-09-26 (memory audit R3). The old 66 KB was "parse working set ~25-28 KB + the
// 40 KB resolver floor"; the resolver has run lean on every build since Section::runBuildSetup
// set it unconditionally, so the floor it protects is CSS_LEAN_MIN_FREE_HEAP_FOR_CSS:
//   parse working set of a heap-backed CSS build (SAX ~10 KB, index, page/paragraph heap)  28 KB
//   resolver lean floor                                                                   24 KB
//   margin                                                                                 4 KB
//                                                                                       = 56 KB
// The residentAbort guard (RESIDENT_BUILD_ABORT_*) still catches a build that dips further. X4
// device confirmation of the new admission band (56-66 KB free) is pending.
#ifndef IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES
#define IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES (56 * 1024)
#endif
#ifndef IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES
#define IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES (32 * 1024)
#endif

namespace InPlaceBuildGate {

// The allocator reports its largest free block a few bytes under the round number (the block header), so the contig
// floor subtracts this, as EpubReaderActivity's LARGEST_FREE_BLOCK_SLACK does: a refusal here changes the build path.
constexpr uint32_t kContigSlackBytes = 16;
// The inflate ring the extraction phase holds: sized to the entry, at most 32 KB (InflateReader::ringSizeFor).
constexpr uint32_t kMaxRingBytes = 32768;

struct Request {
  bool embeddedStyle = false;
  size_t inflatedSize = 0;     // the spine's uncompressed size; 0 when unknown
  bool cjkRepertoire = false;  // the book's language draws on the CJK repertoire (TypographyProfile::cjkRepertoire)
  bool trueTypeSet = false;    // a TrueType or CFF set is loaded (SdCardFontSystem::ttfLoaded)
  uint32_t freeHeap = 0;
  uint32_t contigHeap = 0;
};

struct Admission {
  bool resident = false;
  bool cjkTrueType = false;  // refused for what the book and its fonts are, whatever the heap
  uint32_t ringBytes = 0;
  uint32_t freeFloor = 0;
  uint32_t contigFloor = 0;
};

// A CJK book drawn in a TrueType set never builds in place. Its chapters bring glyphs the glyph store lacks (thousands
// of characters), and a miss takes at least the set's scratch block, and in SD mode its page cache, from the heap: a
// resident build lends the misses no region (Section::stepSectionBuild), and they stay until the slice gives
// RenderLock back (docs/memory-allocation-strategy.md §9.7). Its layout working set is also larger than the bases
// above assume. Measured on the X4 (2026-10-23, chapters of 1.5-9.6 KB admitted at about 67.7 KB free): with the store
// empty every build was thrown away for a borrowed rebuild, once or twice, at 8-20 KB; with every glyph stored, the
// Japanese one, which missed nothing, still fell to 28.9 KB at the between-slice guard. What they would have needed at
// the start, about 79 KB with a family in flash mode and 89 KB in SD mode, is more than the reading heap reaches, so
// no floor would admit them safely. Borrowing from the start costs a chapter that would have finished in place the
// indexing popup a borrowed build draws first (about 0.54 s, a 1.5 KB chapter with its glyphs stored).
//
// Every other book keeps the floors defined above: built-in and .cpfont fonts make no miss memory, and a Latin book's
// single-slice chapters in two TrueType families built in place and finished, with the store empty and full.
//
// The extraction phase holds an inflate ring sized to the entry (≤32 KB) — a per-spine cost the
// static floors were never tuned for (they fit the common few-KB chapter, where the ring is
// noise). Add it to the floors the same way Background-B's WaitHeap gate does (ring on top of
// the base, contig raised to ring + scratch), so a whole-book-in-one-spine entry is sent
// straight to the released path instead of starting a resident build that is doomed to the
// low-heap abort (~1.4 s of popup/setup/abort/re-setup waste, observed heap dip to <8 KB free).
// max(), not sum: the build's two phases have disjoint peaks (runBuildParse releases the
// inflate ring before the CSS-resolving parse starts), so the ring and the layout working set
// are never live at the same time. This is the same shape Background-B's heap-backed gate has
// always used; only this one summed them.
//
// Device-measured on X4 (2026-08-15, HEAP_GATE_TRACE, book cache deleted to force builds):
//   spine 14  ring=27875  free=62468  old floor=95459   arena high-water 39268/48000, failedAlloc=0
//   spine 16  ring=32768  free=61580  old floor=100352  arena high-water 44148/48000, failedAlloc=0
// Total heap on that device is 265120 and free never exceeds ~69 KB at reader time, so the old
// floor could not be met by any book on any page — it was a constant false, not a tuning. That
// also made RESIDENT_BUILD_ABORT_* dead code, since reaching it requires IncrementalResident.
//
// NOTE the CSS base still (correctly) rejects both of those builds at ~62 KB free: a heap-backed
// parse there would dip under the runtime CSS-resolve floor (~40 KB) and come out degraded. The
// fix is not "let more builds go resident" — it is "let this gate be answerable at all". What it
// changes in practice is the NON-CSS case, where the floor drops from 60 KB + ring (up to 92 KB,
// unreachable) to a flat 60 KB, which the ~62-69 KB reading heap can actually clear.
constexpr Admission admitsResident(const Request& r) {
  Admission a;
  a.ringBytes = r.inflatedSize < kMaxRingBytes ? static_cast<uint32_t>(r.inflatedSize) : kMaxRingBytes;
  const uint32_t parseFree =
      r.embeddedStyle ? IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES : IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES;
  const uint32_t extractFree = IN_PLACE_BUILD_EXTRACT_BASE_HEAP_BYTES + a.ringBytes;
  a.freeFloor = parseFree > extractFree ? parseFree : extractFree;
  const uint32_t parseContig =
      r.embeddedStyle ? IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES : IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES;
  const uint32_t extractContig = a.ringBytes + 8 * 1024;
  a.contigFloor = (parseContig > extractContig ? parseContig : extractContig) - kContigSlackBytes;
  a.cjkTrueType = r.cjkRepertoire && r.trueTypeSet;
  a.resident = !a.cjkTrueType && r.freeHeap >= a.freeFloor && r.contigHeap >= a.contigFloor;
  return a;
}

}  // namespace InPlaceBuildGate
