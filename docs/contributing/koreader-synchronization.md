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
   intent, and any future sync result slots in `APP_STATE.koReaderSyncSession`
   (`launchKOReaderSync`). An `AUTO_PULL` on opening a book is handed off by
   `ReaderActivity` before the reader exists, with a zeroed local position.
2. The reader activity stack is replaced with `KOReaderSyncActivity`.
3. `KOReaderSyncActivity` lazily reloads EPUB data only for mapping work and
   releases it again before network-heavy phases.
4. On completion, cancel, or failure, sync persists an outcome and reopens the
   book through the normal `ReaderActivity -> EpubReaderActivity` path.
5. `EpubReaderActivity::applyPendingSyncSession()` consumes that outcome:
   - remote-apply seeds the reader's navigation target from the result, most
     precise anchor first: content offset, list item, paragraph, then the page
     estimate. It also writes the spine and page to `progress.bin`, so a power
     loss before the next save keeps the synced position
   - upload-complete keeps the existing local `progress.bin` unchanged
   - an `AUTO_PULL` that applied nothing leaves `progress.bin` untouched: its
     handoff carried no local position
   - the result's anchors travel in `APP_STATE.koReaderSyncSession` because
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

- [lib/KOReaderSync/LastPushCache.h](../../lib/KOReaderSync/LastPushCache.h)
  - Our own last successful push of a book (`kosync_push.bin` in its cache directory): document
    id, XPath, spine, page and content offset. A server record that is exactly that push needs
    no mapping. Host-tested in `test/progress_comparison/LastPushCacheTest.cpp`.

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
  - Shared stack model and generic SAX callback adapters (`parserStartCb` and friends).
  - Common parser code pattern used by both forward/reverse engines.

- [lib/KOReaderSync/SaxFeedSink.h](../../lib/KOReaderSync/SaxFeedSink.h)
  - Feeds the spine item's inflate output straight into `SaxParser`, and stops the inflate once
    the parser stops.

### Reader and sync screen

- [src/activities/reader/KOReaderSyncActivity.cpp](../../src/activities/reader/KOReaderSyncActivity.cpp)
  - The standalone sync screen.
- [src/activities/reader/EpubReaderSync.cpp](../../src/activities/reader/EpubReaderSync.cpp)
  - The handoff into it (`launchKOReaderSync`), the auto-push on book close
    (`tryAutoPushOnClose`) and `applyPendingSyncSession`.
- [src/activities/reader/EpubReaderAutoSync.cpp](../../src/activities/reader/EpubReaderAutoSync.cpp),
  [KOReaderAutoSync.cpp](../../src/activities/reader/KOReaderAutoSync.cpp),
  [KOReaderSyncWorker.cpp](../../src/activities/reader/KOReaderSyncWorker.cpp)
  - Background auto-sync (see "Background auto-sync" below).

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
   Other matches convert the byte offset to intra-spine progress and snap by the list-item or
   paragraph LUT.

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
2. **Content offset**, within the same spine, when both sides have one. The page's start offset
   and the first later page start beyond it (`visibleOffsetAtPage`, `visibleOffsetAtNextPage`, from
   `Section::getVisibleTextOffsetForPage` and `getVisibleTextOffsetAfterPage`; `UINT32_MAX` on the
   last page) bound the page: a remote offset inside it is Synchronized, before it behind, at or
   past the end ahead. Exact to the character, for any book. The first page's window starts at the
   chapter, not at its first text. The remote has an offset only when its record was mapped to a
   text point (`toCrossPoint`) or is our own last push (`LastPushCache.h`); a peeked record has none.
3. **Paragraph LUT**, within the same spine. Let `K(i)` be the section cache's paragraph index at
   the end of page `i`. A remote `p[K]` opens on the local page `p` exactly when
   `K(p-1) < K <= K(p)`: Synchronized. `K <= K(p-1)`: the remote is behind. Otherwise it is ahead.
   Exact for books whose paragraphs are children of `<body>`; the handoff into the sync screen
   carries `K(p)` and `K(p-1)` (`paragraphIndex`, `paragraphIndexBefore`) and the page's offset
   window (`visibleOffsetAtPage`, `visibleOffsetAtNextPage`), so no section cache is needed there.
4. **Percentages**, with a 0.001 tolerance for rounding. An estimate, bounded to one chapter by
   step 1 when the record names a spine.
5. **Unknown**: neither percentage is usable. The sync screen asks, in smart mode too, and the
   wake pull hands off to it. The auto-push on close uploads: it skips only when the server is
   ahead or already at this page.

The remote side of a comparison is `ProgressMapper::peekRemote()` (string-only) wherever the
chapter has not been inflated yet: the wake pull and the auto-push preflight decide without an
inflate, and only an upload or an apply pays for one. A record that is exactly our own last push
(same document id and XPath) is read from `kosync_push.bin` instead, by the auto-push preflight and
the sync screen, and compares at the offset tier. `selectRemoteRecord()` chooses between the two
document ids' records by spine, then paragraph, then percentage; the alternate wins only when
strictly ahead.

## Background auto-sync

Compiled only for S3 boards with PSRAM (`CROSSPOINT_KOREADER_AUTOSYNC` in `KOReaderAutoSync.h`).
The auto-push on book close is separate: it runs through the sync screen on every board
(`EpubReaderActivity::tryAutoPushOnClose`, intent `AUTO_PUSH`).

The network work runs on `KOReaderSyncWorker`'s task, one job at a time. A job fails as a network
error while a network activity owns the radio; otherwise the worker connects to the saved network
and, if it brought the radio up, takes it down again after the job. The reader side is
`EpubReaderAutoSync.cpp`, driven by `EpubReaderActivity::serviceAutoSync()` from the reader's
`loop()`:

- **Wake pull** (`maybeAutoPullOnWake`, `pollAutoSyncPull`, `evaluateAutoSyncPull`): after a resume,
  fetch the record (in smart mode the alternate document id's too) and compare it through
  `peekRemote()`. Synchronized marks the page synced. In smart mode a local lead is uploaded
  (`silentUploadCurrentPosition`) and a remote lead applied in place (`silentApplyRemote`); any
  other answer hands off to the compare screen (`handOffToInteractiveSync`).
- **Interval push** (`maybeAutoPushInterval`): after the set number of page turns, at most once per
  `MIN_INTERVAL_PUSH_GAP_MS`.
- **Sleep push** (`maybeAutoPushOnSleep`, from `onExit`): maps the page and stashes the job;
  `SleepActivity::onEnter` posts it and waits for it up to `SLEEP_PUSH_JOIN_TIMEOUT_MS`.

Neither push compares with the server first. `AutoSyncState` (`/.crosspoint/autosync.json`) keeps
the last pushed position, the page turns since, whether a push is pending and whether the last one
failed (the status bar's sync indicator).

**Render lock.** `serviceAutoSync()` runs on the loop task, but `section` and `epub`'s metadata
cache belong to the render task. Every entry point takes `RenderLock(RenderLock::Mode::Try)` and
holds it for all of its reads of them (the page, the LUTs, the local XPath mapping of a push); a
busy lock (a render pass in flight) skips the tick. The mutex is not recursive, so the compare
screen is launched only after the lock is released (`pollAutoSyncPull`), and `silentApplyRemote()`
takes no lock of its own but relies on its caller's. The sleep push needs none of its own:
`ActivityManager` runs `onExit()` under the render lock.

**Links.** The sleep push is skipped while a link followed in this session has not been returned
from (`sessionLinkDepth_`, kept by `EpubLinkBackStack::push` and `pop`); the position stays pending
for the next push. It does not read the Back stack's depth (`footnoteDepth`): that stack persists in
`linkstack.bin` and is reloaded on open, but the session count starts at 0 whatever was loaded, so
a reopened book still pushes on sleep.

## Constraints and Non-Goals

- No full DOM materialization for entire books.
- Parse only one spine item on demand.
- Keep memory usage bounded and transient.
- Do not attempt pixel-perfect page parity with KOReader.
