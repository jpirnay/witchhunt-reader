# KOReader Synchronization Architecture

This document explains the intent and internal structure of the KOReader synchronization code in CrossPoint.

Scope:
- Synchronization logic that maps between CrossPoint reading position and KOReader sync payloads.
- Module boundaries and responsibilities.
- Matching rules, fallback strategy, and expected behavior.

For XPath-specific details and examples, see [koreader-sync-xpath-mapping.md](koreader-sync-xpath-mapping.md).

## Goals

The synchronization layer is designed to:
- Be robust on constrained devices (ESP32-C3 memory constraints).
- Be deterministic and debuggable when mapping positions.
- Keep transport/client logic separated from parsing/mapping logic.
- Prefer precise anchors when available, but degrade gracefully.

## Standalone Sync Lifecycle

KOReader sync no longer runs as a child activity on top of a live EPUB reader.
Instead, the reader persists a compact handoff record in `APP_STATE`, then the
activity stack is replaced with `KOReaderSyncActivity`.

Why this exists:
- HTTPS/TLS setup on ESP32-C3 is sensitive to heap fragmentation.
- Keeping the full reader activity alive underneath sync reclaimed too little
  memory and made failures harder to reason about.
- A standalone sync screen makes memory snapshots and failure modes easier to
  interpret.

Current lifecycle:
1. `EpubReaderActivity` stores the EPUB path, current local position, sync
   intent, and any future sync result slots in `APP_STATE.koReaderSyncSession`.
2. The reader activity stack is replaced with `KOReaderSyncActivity`.
3. `KOReaderSyncActivity` lazily reloads EPUB data only for mapping work and
   releases it again before network-heavy phases.
4. On completion, cancel, or failure, sync persists an outcome and reopens the
   book through the normal `ReaderActivity -> EpubReaderActivity` path.
5. `EpubReaderActivity::applyPendingSyncSession()` consumes that outcome:
   - remote-apply writes the reopen position into `progress.bin` before normal
     reader startup loads it
   - upload-complete keeps the existing local `progress.bin` unchanged
   - paragraph-level correction metadata is still carried separately because
     `progress.bin` stores only spine/page/pageCount

Memory notes:
- Reader-owned state is reclaimed by fully exiting the reader before sync.
- Sync still trims its own renderer/font caches right before TLS because sync UI
  rendering can repopulate those caches after the reader is gone.

## Data Model Mismatch

CrossPoint stores position as chapter/page-centric state.
KOReader sync payload stores position as XPath-like anchor plus percentage.

Because layout engines differ, page equality cannot be guaranteed across devices.
The synchronization strategy therefore combines:
- Structural anchor mapping (XPath).
- Percent-based fallback.
- Paragraph LUT refinement when available.

## Module Responsibilities

### Client / orchestration

- [lib/KOReaderSync/KOReaderSyncClient.cpp](../../lib/KOReaderSync/KOReaderSyncClient.cpp)
  - HTTP calls and payload exchange.

- [lib/KOReaderSync/ProgressMapper.cpp](../../lib/KOReaderSync/ProgressMapper.cpp)
  - High-level mapping from app state to KOReader payload and back.
  - Chooses XPath path or percentage fallback.
  - `peekRemote()`: what a record says from its XPath string alone (spine, body-child
    paragraph), for comparisons that must not inflate a chapter.

- [lib/KOReaderSync/ProgressComparison.cpp](../../lib/KOReaderSync/ProgressComparison.cpp)
  - Which side of a sync is further along (see "Which side is further" below).
  - Pure functions over positions; host-tested in `test/progress_comparison/`.

### XPath indexing facade

- [lib/KOReaderSync/ChapterXPathIndexer.h](../../lib/KOReaderSync/ChapterXPathIndexer.h)
- [lib/KOReaderSync/ChapterXPathIndexer.cpp](../../lib/KOReaderSync/ChapterXPathIndexer.cpp)
  - Public API consumed by ProgressMapper.
  - Thin facade over forward/reverse mapper internals.
  - Utility extraction helpers (DocFragment index, paragraph index).

### Forward mapping engine

- [lib/KOReaderSync/ChapterXPathForwardMapper.cpp](../../lib/KOReaderSync/ChapterXPathForwardMapper.cpp)
  - Maps a page's content offset (or, without one, intra-spine progress) to XPath.
  - Emits /text()[N].M for the text at the offset, in a block element or inside inline elements
    (N and M follow crengine's DOM).

### Reverse mapping engine

- [lib/KOReaderSync/ChapterXPathReverseMapper.cpp](../../lib/KOReaderSync/ChapterXPathReverseMapper.cpp)
  - Maps XPath to intra-spine progress.
  - Supports exact and tolerant matching tiers.
  - Handles /text()[N].M codepoint offsets.

### Shared parser/state/utilities

- [lib/KOReaderSync/ChapterXPathIndexerInternal.cpp](../../lib/KOReaderSync/ChapterXPathIndexerInternal.cpp)
- [lib/KOReaderSync/ChapterXPathIndexerInternal.h](../../lib/KOReaderSync/ChapterXPathIndexerInternal.h)
  - UTF-8 helpers, XPath normalization, parse runner, and chapter text-byte counting.

- [lib/KOReaderSync/ChapterXPathIndexerState.h](../../lib/KOReaderSync/ChapterXPathIndexerState.h)
  - Shared stack model and generic Expat callback adapters.
  - Common parser code pattern used by both forward/reverse engines.

## Core Logic

### Forward (CrossPoint -> KOReader)

1. Take the page's content offset from the section LUT (`Section::getVisibleTextOffsetForPage`).
2. Stream the spine XHTML into the SAX parser and stop at the offset.
3. Emit `/text()[N].M` for the text at the offset, inside inline elements too
   (`/p[K]/span[1]/text()[N].M`). The push never names an image: a page that starts with an image
   names the first text after it, and a page that starts at the chapter's total (a last image or
   spacing page) names the end of the last text node. The element path remains only for stray text
   in table rows.
4. Without an offset, count the chapter's text and scan to the byte fraction (also streamed).

### Reverse (KOReader -> CrossPoint)

1. Stream the spine XHTML into the SAX parser while evaluating candidate matches.
2. Resolve best tier in this order:
   - exact
   - exact-no-index
   - ancestor
   - ancestor-no-index
3. An exact text-point match yields the content offset; the reader resolves it to a page through the
   section LUT (`Section::getPageForVisibleTextOffset`: the last page whose start is <= the offset,
   so an image page and the text page after it, which share a start, resolve to the text page).
   Other matches convert the byte offset to intra-spine progress and snap by paragraph LUT.

For text-node anchors /text()[N].M:
- N is treated as 1-based text node index.
- M is treated as 0-based codepoint offset.

## Fallback Strategy

When XPath mapping fails or is ambiguous:
- Fall back to percentage-driven chapter/page estimation.
- Use paragraph LUT refinement where available.

This guarantees user progress continuity even for malformed or sparse content.

## Which side is further

Smart sync, the auto-push on book close, the wake pull and the choice between the two document
ids (filename and binary hash) all have to decide whether the local or the remote position is
further along. The two percentages cannot decide that on their own: KOReader's is its rendered
page over its page count, ours is XHTML bytes, and they routinely differ by a percent or two of
the book, which is more than a chapter boundary. Comparing them put a chapter start behind the
end of the previous chapter (#268's family).

`compareProgress()` in `ProgressComparison.cpp` ranks the evidence instead:

1. **Spine**, when the record names one (`DocFragment`). Exact.
2. **Content offset**, within the same spine. The page's start offset and the next page's start
   (`visibleOffsetAtPage`, `visibleOffsetAtNextPage`, from `Section::getVisibleTextOffsetForPage` and
   `getVisibleTextOffsetAfterPage`) bound the page: a remote text point inside it is Synchronized,
   before it behind, at or past the next page's start ahead. Exact to the character,
   for any book. The first page's window starts at the chapter, not at its first text.
3. **Paragraph LUT**, within the same spine. Let `K(i)` be the section cache's paragraph index at
   the end of page `i`. A remote `p[K]` opens on the local page `p` exactly when
   `K(p-1) < K <= K(p)`: Synchronized. `K <= K(p-1)`: the remote is behind. Otherwise it is ahead.
   Exact for books whose paragraphs are children of `<body>`; the handoff into the sync screen
   carries `K(p)` and `K(p-1)` (`paragraphIndex`, `paragraphIndexBefore`) so no section cache is
   needed there.
4. **Percentages**, with a 0.001 tolerance for rounding. An estimate, now bounded to one chapter
   by step 1.
5. **Unknown**: neither percentage is usable. The sync screen asks; the auto paths hand off to
   the sync screen rather than guess.

The remote side of a comparison is `ProgressMapper::peekRemote()` (string-only) wherever the
chapter has not been inflated yet: the wake pull and the auto-push preflight decide without an
inflate, and only an upload or an apply pays for one. `selectRemoteRecord()` applies the same
order to the two document ids' records; the alternate wins only when strictly ahead.

## Constraints and Non-Goals

- No full DOM materialization for entire books.
- Parse only one spine item on demand.
- Keep memory usage bounded and transient.
- Do not attempt pixel-perfect page parity with KOReader.
