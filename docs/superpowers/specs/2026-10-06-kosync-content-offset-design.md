# KOSync content offsets: exact positions in both directions

Date: 2026-10-06. Builds on PR #396 (#268 fix) and PR #397 (ProgressComparison). Sync-only: the
reader's own position restore keeps using the paragraph LUT.

## Problem

A KOSync record is an XPath plus a percentage. We map our page to and from it by **byte fraction
of the chapter**: the push names the paragraph containing `page/(pages-1)` of the chapter's text,
the pull turns a resolved text offset into a page by the same fraction. Both are estimates, and the
corpus round-trip test (`test/epub_pipeline/ProgressMapperRoundTripTest.cpp`) measures them: on
heading-rich pages the push names the paragraph on the *next* page and the pull lands one page
ahead (the reader skips text); on a dense appendix it lands two behind. Books whose paragraphs are
not children of `<body>` (Calibre's `<body><div><p>`) have no paragraph LUT and cannot be snapped
at all. The push also costs an inflate of the chapter to an SD temp file plus two full parses.

crosspoint-reader solved this in #2805/#3111/#3174 by recording a visible-text offset per page at
layout time. We take the same idea with two differences: our layout parser and our mappers share
one `SaxParser`, so the count is identical by construction instead of hand-mirrored (their push
counts only `<p>`/`<li>` text and is late by every heading); and we replace a dead field in the
existing LUT entry instead of adding a table.

## Goal and success criteria

Positions round-trip exactly, in both directions, for every book. Success:

- the corpus round trip asserts **the same page** for every page of all 30 books, no allowances;
- a push is one streamed inflate stopped at the page, no temp file, no counting pass;
- goldens stay byte-identical;
- the X3's minimum free heap during a long chapter's build is not materially lower (device check).

Non-goals: restoring the reader's position by offset after a font change (follow-up); 16-bit
offset deltas (only if the X3 build gate trips); the crosspoint-sync server's rich positions.

## The invariant

For every page `p` of a chapter, the section cache stores `start(p)`: the number of **visible
bytes** of the chapter's source text before the page's first element.

A visible byte is every non-whitespace byte of SAX-delivered character data that lies inside
`<body>` and outside `head`, `script` and `style`. Entity references count as their expansion.
Text the layout hides or drops (display:none, nested tables, SVG, skipped depths) counts all the
same: the rule is about the source, not the layout. One helper, `lib/Epub/Epub/VisibleText.h`,
defines it (`visibleBytes(const char*, int)`, `isNonVisibleTag(tag)`), and the layout parser, the
forward mapper and the reverse mapper all use it. All three run the same yxml `SaxParser` over the
same inflated bytes, so their counts agree by construction; `VisibleTextOffsetLutTest` checks it on
every corpus chapter anyway.

## Components

### Layout parser (`ChapterHtmlSlimParser`)

- `uint32_t visibleTextOffset_`, advanced by the SAX `characterData` entry for the whole chunk
  **before any skip logic** (nested-table, `skipUntilDepth`, SVG, footnote-link and spacer paths
  all return early today; the count must precede them). `nonVisibleDepth_` tracks head/script/style
  through start/end element; `xpathBodyDepth` already says whether we are inside `<body>`.
- `characterData` becomes a thin SAX entry: count, then `consumeText(s, len, baseOffset,
  source)`. The four re-entrant callers move to `consumeText`: image alt text and footnote
  previews with `source=false` (synthetic: words take the current offset, the running offset does
  not advance); the drop-cap re-dispatch (captured text, then the remainder of the chunk) and the
  entity expansion in `defaultHandlerExpand` with `source=true` and their real base offsets. The
  drop-cap capture records the offset of its first captured byte for this.
- Inside the consume loop a running offset gives each word its start at the byte that opens it;
  `flushPartWordBuffer` passes it to `addWord`. Synthetic words (list markers, drop caps, the
  `<pre>` blank-line space) take the current offset.
- A page's start is the offset of its first element. `addLineToPage` (after its own fit test, which
  may emit the page) and the eight `currentPage->elements.push_back` sites call
  `noteElementOnPage(offset)`, which records only the first call per page: a text line passes the
  line's first-word offset, an image/HR/drop-cap line passes `visibleTextOffset_`. Grid-table lines
  pass the smallest word offset on the line; the cell layout lambdas collect per-line offsets
  alongside the lines, as the row is assembled at `</tr>`. `emitPage` writes the recorded start into
  the LUT entry and clears it.

### `ParsedText`

- `std::vector<uint32_t> wordVisibleOffsets`, parallel to `words`, maintained at the eight places
  `wordStyles` is: reserve, push_back, the two erases (consume, split), clear, resize and the two
  inserts (bionic suffix, hyphenation split: the inserted suffix takes the prefix's offset plus the
  prefix's visible bytes). `preserveSource` snapshots and restores it. The heap gate in `addWord`
  adds `newCapacity * sizeof(uint32_t)` to `needed`.
- `addWord(..., uint32_t visibleOffset)`.
- `extractLine` sets `lastLineVisibleOffset_ = wordVisibleOffsets[lastBreakAt]` before
  `processLine`; the parser's lambdas read it. The callback signature is unchanged.

### Section cache

- `ParagraphLutEntry` becomes `{u32 visibleTextOffset, u16 paragraphIndex, u16 listItemIndex}`.
  The replaced `xhtmlByteOffset` has had no reader since #397. Entry size, header and the build's
  per-page RAM are unchanged. `SECTION_FILE_VERSION` 79 → 80.
- `std::optional<uint32_t> Section::getVisibleTextOffsetForPage(uint16_t page)`.
- `std::optional<uint16_t> Section::getPageForVisibleTextOffset(uint32_t offset)`: the last page
  whose start is `<= offset`, a linear scan with early exit like `getPageForParagraphIndex`
  (starts are non-decreasing in document order; if a deferred element ever breaks that, "the page
  before the first start above the offset" is still a sane answer). `nullopt` when the LUT is
  missing or empty.
- `docs/file-formats.md` and `docs/contributing/section-indexing.md` describe the new entry.

### Streaming (`lib/KOReaderSync`, `Epub`, `ZipFile`)

- `decompressToTempFile`, `runParse` and the `.tmp_kox_N.html` files go. `SaxFeedSink : Print`
  hands inflate output to `SaxParser::feed()` and sets a stop flag when the parser stops (target
  reached) or fails. `streamSpine(epub, spine, saxParser)` wraps `readItemContentsToStream`.
- `Epub::readItemContentsToStream(href, out, chunkSize, const bool* stop = nullptr)` and
  `ZipFile::readFileToStream` get the flag; both loops (heap path and arena path) return `true` as
  soon as it is set. A parse error is reported through the sink's own flag, not as a short write.
- `countTotalTextBytes` stays for the fraction fallback, streamed.

### Push (`ProgressMapper::toKOReader`)

- `CrossPointPosition` gains `uint32_t visibleTextOffset` and `bool hasVisibleTextOffset`. The
  reader fills them from `section->getVisibleTextOffsetForPage(page)` in `currentKoPosition()`
  and the sync handoff.
- With an offset: `ChapterXPathIndexer::findXPathForVisibleOffset(epub, spine, offset)`. The
  existing `ForwardState` scan with the target given directly: no counting pass.
- Emission: `StackNode` gains per-element text-node bookkeeping (`textNodeCount`,
  `codepointsInTextNode`, reset at every child start/end, exactly as the reverse mapper's
  `inParentTextNode` counting). When the cursor's text is a direct child of a **block** element
  (`p`, `li`, `h1`–`h6`, `div`, `td`, `th`, `blockquote`, `dd`, `dt`, `figcaption`, `pre`,
  `section`, `article`, `body`) the result is `…/block[K]/text()[N].M`, `M` in codepoints within
  that text node (KOReader's unit). Inside an inline element (`em`, `span`, `a`, `b`, `i`, …) the
  element path is emitted, as today. This is the shape KOReader itself produces and the shape our
  reverse mapper resolves at the text-node-exact tier.
- Without an offset (the finished-book sentinel, a cache without the LUT) the fraction path
  `findXPathForProgress` stays, streamed.

### Pull (`ProgressMapper::toCrossPoint`)

- `findProgressForXPath` also returns the matched offset in visible bytes (`bestOffset`) and the
  chapter total.
- `toCrossPoint` sets `visibleTextOffset/hasVisibleTextOffset` **only for exact matches**
  (text-node-exact or exact element). Ancestor and index-insensitive matches behave as today
  (intra from bytes, the percentage override, the paragraph LUT snap in the reader).
- The page estimate from the fraction is still computed, as the fallback page.
- The INF mapping line gains `off=`.

### Reader

- `NavigationTarget::Kind::VisibleOffset` with `uint32_t visibleOffset` and `fallbackPage`;
  `makeVisibleOffset(offset, fallback)`. `resolveInto`: LUT hit → that page, exact; miss →
  `fallbackPage`, an estimate (rescaled like the other estimate kinds).
- `applyPendingSyncSession` prefers `resultHasVisibleOffset` over the list-item and paragraph
  kinds; `silentApplyRemote` likewise.

### Sync handoff and screen

- `KOReaderSyncSessionState` gains `visibleOffsetAtPage`, `visibleOffsetAtNextPage`
  (`UINT32_MAX` on the last page), `hasVisibleOffset`, `resultVisibleOffset`,
  `resultHasVisibleOffset`; `JsonSettingsIO` serialises them; `launchKOReaderSync` and the
  AUTO_PULL handoff in `ReaderActivity` fill or zero them.
- `KOReaderSyncActivity`'s positional position parameters (ten today, thirteen with the offsets)
  become one `SyncLocalPosition` struct (spine, page, totalPages, paragraphIndex, hasParagraphIndex,
  paragraphIndexBefore, visibleOffsetAtPage, visibleOffsetAtNextPage, hasVisibleOffset) that
  `ActivityManager` builds from the session.

### Comparator (`ProgressComparison`)

- `LocalReadingPosition` gains the two offsets and `hasVisibleOffset`.
- `compareProgress`, same spine, both sides with offsets: remote `< start(p)` → LocalAhead;
  `< start(p+1)` → Synchronized; else RemoteAhead. Otherwise the existing paragraph → percentage
  → Unknown order. `selectRemoteRecord` is unchanged: its inputs are string-only peeks.
- The wake pull and the auto-push preflight still compare without an inflate, so they never have
  a remote offset and keep the paragraph tier. Only the sync screen (which maps the remote anyway)
  and `silentApplyRemote` use offsets.

## Data flow

Push: page `p` → `start(p)` from the LUT → one streamed inflate, stopped at the offset →
`…/p[K]/text()[N].M` → server. Pull: server → XPath → one streamed inflate → matched offset →
`getPageForVisibleTextOffset` in the reader → exact page. Compare, in the sync screen: the
remote's matched offset against `[start(p), start(p+1))`.

## Error handling

- No LUT entry (cache built before v80 cannot exist after the bump; a truncated build has entries
  for the pages it wrote): `getVisibleTextOffsetForPage` → `nullopt` → push falls back to the
  fraction path; pull falls back to the fallback page.
- Resolver failure on pull (parse error, missing entry): as today, string-derived spine and
  paragraph, percentage page.
- `visibleTextOffset_` is `uint32_t`; a chapter with more than 4 GB of text is not a concern.
- The build's heap gate already aborts to a partial cache when the word vectors cannot grow; the
  fifth vector joins that arithmetic.

## Testing

Every "after" is a comparison, so the baseline is recorded first:

0. **Baseline, before any change**: the host suite's count and result (1464/1464 on #397), the
   corpus round-trip drift as the `allowedDrift()` table states it (two books: 2 behind, 1
   ahead), the goldens as they are, and on the X3 the minimum free heap and largest free block
   during the build of a long chapter (the existing `[MEM]` watermark lines), for a book with
   body-child paragraphs and for a Calibre-wrapped one. The same measurements are taken after
   each of the delivery commits that can move them (1 and 2 for heap, 6 for drift).

Host, test-first wherever behaviour changes:

1. `VisibleTextOffsetLutTest` (epub_pipeline): every corpus chapter has non-decreasing starts,
   `start(0) == 0`, `getPageForVisibleTextOffset(start(p)) == p` for every page, and the last start
   is at most the mapper's `countTotalTextBytes` of the chapter.
2. `ProgressMapperRoundTripTest`: exactly the same page for every page of every book; the
   `allowedDrift()` table is deleted; the last-page test asserts the last page.
3. `ProgressMapperTest`: a mid-paragraph page start pushes as `…/text()[N].M` and pulls back to the
   same offset; an inline-element start keeps the element path; an inexact match yields no offset;
   the finished-book sentinel still yields an XPath; the push stops the inflate early (bytes fed
   less than the chapter).
4. `ProgressComparisonTest`: the offset tier's three outcomes, the last-page sentinel, one side
   without an offset falling through.
5. Goldens: byte-identical (the suite).

Device, before merge: the X3 build-heap measurement above, after against before, on the same two
books; a sync on the trimmed heap (the `[KOSync] Sync mem[...]` snapshots around the push and the
apply) after against before; PR #397's four-step checklist plus "push from a page that starts
mid-paragraph, pull on the other device, same page".

## Memory and flash

The device has ~380 KB with no PSRAM, and fragmentation, not total use, is what kills it. The
accounting below is per stage, because the three stages run on very different heaps.

**Layout time** (the section build, the one stage that gains heap use): `wordVisibleOffsets`
costs 4 B per word of the paragraph being laid out, so +12% on the per-word vectors; a 2 KB
paragraph is ~350 words, +1.4 KB. It is reserved with the other four word vectors in the same
capacity-doubling step (one allocation per growth, not a separate growth loop), it is counted in
`addWord`'s heap gate (`needed += newCapacity * 4`), and `releaseLayoutScratch` and `reset` treat
it as they treat `wordStyles`, so a yield between build slices holds no more than before. The
parser gains ~24 B of counters. Grid-table cell lambdas collect per-line offsets in a vector that
is `reserve`d to the row's line count before the loop. The LUT entry keeps its 8 bytes, so the
in-memory LUT during the build is unchanged. If the X3's minimum free heap during a long chapter's
build drops measurably (see Testing, device), the fallback is 16-bit deltas from a per-block base,
halving the cost; it is not built speculatively.

**Sync time** (the heap is already trimmed for Wi-Fi/TLS): no new allocation. The streaming sink
is a stack object holding a pointer to the parser and a flag; the `FsFile` write buffer and the SD
temp file of the old path go away; the `ForwardState`/`ReverseState` stacks are the existing ones;
the inflate ring is the same 32 KB block the old path needed; `getPageForVisibleTextOffset` reads
4 bytes per entry through the LUT, no buffer. The result XPath is one `std::string` built once at
the match, as today.

**Steady state**: `APP_STATE` grows by 12 B for the handoff fields; `CrossPointPosition` and
`NavigationTarget` by 8 B each where they live on the stack or in an activity.

**Flash**: about +1.5 KB (upstream measured +1.4 KB for the equivalent).

Every allocation the implementation adds carries the one-line size and why-not-stack note the
heap-discipline rules ask for, and the plan's device task measures the layout-time number before
and after rather than estimating it.

## Migration

The version bump rebuilds every chapter on first open, as every bump does. `progress.bin` is
untouched. The handoff's new fields default to "no offset", so a session saved by older firmware
degrades to the paragraph tier.

## Delivery

One PR, stacked on #397, commits in this order so each builds and passes alone:

1. shared helper, parser counting, `ParsedText` offsets, LUT entry, `Section` accessors, version
   bump, `VisibleTextOffsetLutTest`, docs for the format;
2. streaming with early stop;
3. push by offset with text points;
4. pull by offset, reader target;
5. handoff struct, comparator tier;
6. round-trip tightening, docs, release note.
