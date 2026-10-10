# KOReader Sync XPath Mapping

This note documents how CrossPoint maps reading positions to and from KOReader sync payloads.

Related architecture overview: [koreader-synchronization.md](koreader-synchronization.md)

## Problem

CrossPoint internally stores position as:

- `spineIndex` (chapter index, 0-based)
- `pageNumber` + `totalPages`

KOReader sync payload stores:

- `progress` (XPath-like location)
- `percentage` (overall progress)

A direct 1:1 mapping is not guaranteed because page layout differs between engines/devices.

## DocFragment Index Convention

KOReader uses **1-based** XPath predicates throughout, following standard XPath conventions.
The first EPUB spine item is `DocFragment[1]`, the second is `DocFragment[2]`, and so on.

CrossPoint stores spine items as 0-based indices internally. The conversion is:

- **Generating XPath (to KOReader):** `DocFragment[spineIndex + 1]`
- **Parsing XPath (from KOReader):** `spineIndex = DocFragment[N] - 1`

Reference: [koreader/koreader#11585](https://github.com/koreader/koreader/issues/11585) confirms this
via a KOReader contributor mapping spine items to DocFragment numbers.

## Current Strategy

### CrossPoint -> KOReader

Implemented in `ProgressMapper::toKOReader`.

1. Compute overall `percentage` from chapter/page (`Epub::calculateProgress`, with intra-spine
   progress `page / (pageCount - 1)`).
2. Take the page's content offset from the section cache (`Section::getVisibleTextOffsetForPage`:
   where the page's first text starts, in visible bytes, in the chapter's text) and name that text with
   `ChapterXPathIndexer::findXPathForVisibleOffset`: one streamed pass over the spine XHTML (a second, inclusive pass only when the page starts at the chapter's total, to name the end of the last text),
   stopped at the offset. The result is `.../block[K]/text()[N].M` when the text is a direct child of
   a block element (paragraph, heading, list item, table cell, div), and
   `.../p[K]/span[1]/text()[N].M` when it sits inside an inline element (span, em, a). The push never
   names an image: a page that starts with an image names the first text after it, which pulls back
   onto the page holding that text (the page after the image, when the image fills a page of its
   own). A page that starts at the chapter's total (a last page holding only an image or spacing)
   names the end of the chapter's last text node. The element path remains only for stray text in
   table rows, which crengine keeps no text node for.
3. Without an offset (a section cache without one, a page past the LUT), the byte-fraction scan
   (`ChapterXPathIndexer::findXPathForProgress`) names the paragraph proportional to intra-spine progress.
4. If XPath extraction fails, fall back to the synthetic chapter path:
   - `/body/DocFragment[spineIndex + 1]/body`

The paragraph LUT is not used for upload: snapping to the start of `p[N]` when the user is
mid-paragraph lands pulled positions at the start of the paragraph (and at the start of the chapter
when an opening paragraph spans many pages). The content offset names the page's own first
character instead.

### KOReader -> CrossPoint

Implemented in `ProgressMapper::toCrossPoint`.

1. Attempt to parse `DocFragment[N]` from incoming XPath; convert N to 0-based `spineIndex = N - 1`.
2. If valid, attempt XPath-to-offset mapping via `ChapterXPathIndexer::findProgressForXPath`.
   (2b) An exact text-point match carries the resolved offset (`CrossPointPosition::visibleTextOffset`);
   the reader lands on the page by `Section::getPageForVisibleTextOffset`: the last page whose start
   is <= the offset, so pages sharing one start (an image or rule page before the text at that
   offset) resolve to the last of them, the text page. The paragraph LUT snap below is the fallback
   for inexact matches and failed resolves.
3. If the match is **not exact** and disagrees with KOReader's `percentage` by more than 1% of
   the book inside the same spine, use the percentage-derived intra-spine progress instead.
   An **exact** match is never overridden. KOReader's percentage is its rendered page over its
   page count, while ours is XHTML bytes, so a gap of over 1% between them is normal.
   Overriding an exact chapter-start match moved the reader a dozen pages into the chapter (#268).
4. Extract paragraph index from XPath via `ChapterXPathIndexer::tryExtractParagraphIndexFromXPath`
   (e.g. `/body/DocFragment[7]/body/p[685]/text().96` -> `paragraphIndex = 685`).
5. Convert resolved intra-spine progress to page estimate.
6. If the XPath does not resolve but names a spine, keep that spine and take the page from the
   percentage within it; a chapter-start XPath (no `p`/`li` predicate, ending in `.0`) pins the
   first page instead. Without a usable `DocFragment`, estimate spine and page from the percentage.

The reader lands through the most precise anchor the result carries (`NavigationTarget`): the
content offset (`Section::getPageForVisibleTextOffset`), else a list item
(`Section::getPageForListItemIndex`, for an XPath ending in `/li[N]`), else a paragraph, else the
page estimate. For a paragraph index, `EpubReaderActivity` refines the page estimate using the
section cache's per-page paragraph LUT (`Section::getPageForParagraphIndex`). This finds the first
page whose recorded paragraph index is >= the target, giving a more accurate landing position than
byte-offset-based estimation alone.

## ChapterXPathIndexer Design

The module streams one spine XHTML from the inflate straight into the SAX parser (`SaxFeedSink`);
nothing touches the SD card, and the forward pass stops the inflate at its target.

Source-of-truth note: XPath anchors are built from the original EPUB spine XHTML bytes (zip item contents), not from CrossPoint's distilled section render cache. This is intentional to preserve KOReader XPath compatibility.

Each lookup is one streamed SAX pass over the chapter. `StackState` keeps the element stack, the
visible-byte count (whitespace is not counted) and crengine's text-node counter (see "crengine's text
nodes"). The forward pass stops the parse at the target offset and names the text there; the reverse
pass compares the incoming path against the paths it meets, and for a `text()[N].M` point resolves the
text-node-exact tier by collapsed codepoints, returning the visible-byte offset of that codepoint.
No anchor list is kept.

Matching for reverse lookup:

1. exact path match — reported as `exact=yes`
2. index-insensitive path match (`div[2]` vs `div[3]` tolerated) — reported as `exact=no`
3. ancestor fallback — reported as `exact=no`

If no match is found, caller must fallback to percentage.

An element with no text of its own (a wrapper such as `section` or `div`) is matched at its
**end** tag, so an ancestor-only match on a wrapper lands at the wrapper's end, often the
chapter's last page. This is why inexact matches still defer to the percentage.

## crengine's text nodes

`N` and `M` in `text()[N].M` count crengine's DOM, not our SAX stream, because KOReader resolves
the point in crengine. The rules (R1-R7 in the header comment of `ChapterXPathForwardMapper.cpp`,
read from koreader/crengine `lvtinydom.cpp` and `lvxml.cpp`) that both mappers share through
`StackState::onTextChunk`:

- `ldomElementWriter::onText`: the first whitespace-only text run of a non-`pre` block (no child
  yet) is dropped, so `<p>\n  <em>x</em> rest</p>` has one text node.
- `PreProcessXmlString`: CR, LF and TAB become spaces and a run of spaces is stored as one space;
  `M` indexes that stored text in codepoints. `<pre>` keeps its text raw.
- `ldomNode::removeStandaloneWhitespaceTextChildrenInMixedContent`: in an element with block
  children and inline content, a whitespace-only text node is deleted unless it sits between two
  inline-ish siblings.
- `createXPointerV1/V2`: a `text()[N]` that does not exist is a null XPointer, which
  `LVDocView::getBookmarkPage` treats as page 0. `table`, `thead`, `tbody`, `tfoot` and `tr` keep
  no text, so stray text there is pushed as the element path.
- Accepted gap (R6): a comment splits a text node in crengine; our parser does not see comments.
- Accepted gap (R7): crengine decides block-ness and `pre` from computed CSS (`isBlockNode`,
  `white_space >= pre-line`); we decide from tag names. `span{display:block}`, `div{display:inline}`
  or `white-space: pre-wrap` over pretty-printed whitespace can shift `N` or `M`.

## Device evidence

Three probes pushed under these rules to a live KOReader (through `kosync.rustysoft.de`):

- R2, from KOReader's own upload `/body/DocFragment[3]/body/p[31]/text()[1].851`: our collapsed
  codepoint count lands on a word boundary, the raw count lands mid-word.
- R1, `/body/DocFragment[7]/body/p[40]/text()[1].55` landed on "mighty Job!"; numbering that ignores rule R1
  names a text node that does not exist, a null XPointer, which KOReader opens at the book's first page.
- Deep points, the Chapter 40 probe `/body/DocFragment[10]/body/p[18]/i[1]/text()[1].39`: a text point
  inside an inline element landed on the page holding its target. crengine returns a null XPointer
  (page 0) on any unresolved step, so the whole path resolved. This is why
  `kTextPointsInsideInlineElements` is on.

## Memory / Safety Constraints (ESP32-C3)

The implementation intentionally avoids full DOM storage.

- Parse one chapter only.
- Keep only the element stack and counters for the duration of the call.
- Free the XML parser and the inflate state on all success/failure paths.
- No persistent cache structures are introduced by this module.
- The parser parses like the layout parser: bare HTML void tags (`<br>`) are repaired
  (`htmlVoidTagRepair`), so both sides see the same tree.

The streamed pass trades peak for I/O: the SAX parser's state lives alongside the inflate ring (up
to 32 KB) instead of after it, while the temp file and its SD write are gone. No chapter is mapped
with WiFi up, except the two retry fallbacks (`performSync`, `performUpload`) when the pre-WiFi
mapping failed:
- The mappers parse with `SaxParser::Profile::Lean`, which leaves out the attribute table they never
  read (4,672 B of state on the C3 instead of 9,712 B).
- **The push position** is mapped in `KOReaderSyncActivity::onEnter`, after the secondary framebuffer
  is released and before WiFi comes up, and reused for the session.
- **A pull** (and the reader's auto-pull) maps the fetched record with the radio fully down: after the
  GET, `HalClock::wifiOff` stops it. Nothing is sent after a pull; the reader reopens on the applied
  position.
- **A compare** first asks whether the record is our own last push (`LastPushCache.h`): the same
  document id and the same XPath as the last successful upload, which `kosync_push.bin` in the book's
  cache directory keeps with that page's spine, page and content offset.
  - If it is, the remote position comes from that file: no mapping, and WiFi stays up for a PUT on
    the warm session.
  - Any other record is mapped with the radio down, as a pull is. If an upload then follows (smart
    mode finds us ahead, or the user picks it), WiFi is brought up again through
    `WifiSelectionActivity`. A failed or cancelled reconnect ends in "WiFi connection failed",
    never a silent skip.
- **The auto-push on close** never maps the remote: it compares our own last push from the cache,
  and any other record from its XPath string.
- **The wake pull** maps after the background worker has already turned the radio off.

Device runs on the X3 established the rule behind this ordering. Mapping a 174 KB chapter
after WiFi came up, with the full parser state, drove Min Free down to about 10.9 KB, against about
19 KB for the old temp-file path. Mapping the push position before WiFi, and pulls and compares after
`HalClock::wifiOff`, keeps the mapper out of the WiFi peak. Each stage logs a `[KOSync] Sync mem[...]`
line (`after_local_mapping`, `after_wifi_down_before_remote_mapping`, `after_remote_mapping`,
`after_reconnect_for_upload`) so a regression shows in the serial log.

## Paragraph Index LUT

The section cache stores a per-page paragraph index LUT built during page layout
(`ChapterHtmlSlimParser`). Each entry records the 1-based `<p>` sibling index
(direct children of `<body>`, matching XPath convention) at the time each page was completed.
The same entry holds the page's content offset and the running `<li>` count: per page, a `u32`
visible-text offset, a `u16` paragraph index and a `u16` list-item index.

This enables these lookups without reparsing:

- **XPath → page** (`Section::getPageForParagraphIndex`): finds the first page where the
  recorded paragraph index >= target. Used when applying remote KOReader progress.
- **Page → paragraph** (`Section::getParagraphIndexForPage`): returns the paragraph index for
  a given page. Used by the sync comparison; not used for upload (the content offset is).
- **List item → page** (`Section::getPageForListItemIndex`), for an XPath ending in `/li[N]`.
- **Page ↔ content offset** (`Section::getVisibleTextOffsetForPage`, `getVisibleTextOffsetAfterPage`,
  `getPageForVisibleTextOffset`): what a push names, a pull lands on, and the comparison's page
  window.

The paragraph counter in `ChapterHtmlSlimParser` counts **all** `<p>` elements at body-child
level, including `display:none` elements. This matches `ChapterXPathIndexer` and crengine's
standard XPath same-name sibling counting.

## Known Limitations

- On reverse mapping, the page is an estimate where the position carries no text point (renderer
  differences). The paragraph LUT refines this but cannot guarantee an exact page.
- XPath mapping uses the original spine XHTML while pagination comes from the distilled renderer
  output, so page drift is possible, but only for pages that push no text point of their own.
- A page that pushes a text point, inside inline elements too, comes back on itself.
- A page that starts with an image pushes the text after it, so a push from a full-page image comes
  back one page late, on the text page.
- Image-only/low-text chapters may yield coarse anchors.
- Extremely malformed XHTML can force fallback behavior.

## Operational Logging

`ProgressMapper` logs the reverse mapping at **INF**, so it appears in release-build logs:

```
[ProgressMapper] KOReader -> CrossPoint: 26.72% at /body/DocFragment[13]/... -> spine=12, page=0/40 off=0/1 (xpath, exact=yes)
```

`off=` is the resolved content offset and whether the result carries it (`1` only for a text point
matched to the codepoint). The source in parentheses is one of:

- `xpath`: the XPath resolved and was used as is
- `xpath+percentage`: an inexact XPath match was replaced by the percentage
- `xpath-spine+chapter-start`: the XPath did not resolve; spine from the XPath, first page
- `xpath-spine+percentage`: the XPath did not resolve; spine from the XPath, page from the percentage
- `percentage`: no usable XPath at all

It also logs exactness (`exact=yes/no`) for XPath matches. Note that `exact=yes` is only set for
a full path match with correct indices; index-insensitive and ancestor matches always log `exact=no`.

Host coverage, in `test/epub_pipeline/`:

- `ProgressMapperTest.cpp` builds a three-chapter book (the third malformed, so the resolver fails
  as it does when the inflate cannot be allocated) and maps XPath/percentage pairs through
  `ProgressMapper::toCrossPoint`.
- `ProgressMapperRoundTripTest.cpp` pushes every page of every corpus chapter, pulls it back and
  snaps it as the reader does; it must land on the page it left.
- `TextNodeRulesTest.cpp` (crengine's text-node rules), `XPathExtractionTest.cpp` (the string side:
  spine, paragraph, normalisation), `StreamedParseTest.cpp` (the streamed parse) and
  `VisibleTextOffsetLutTest.cpp` (the content-offset LUT).
