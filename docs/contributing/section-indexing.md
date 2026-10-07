# Section Indexing Workflow

A "section" is one spine item (typically one chapter) laid out into pages for a specific set of rendering parameters. The section cache is the heaviest computation in the reader — parsing EPUB XHTML, applying CSS, measuring glyphs, breaking lines and pages, and writing all of that to SD takes several seconds. Getting it right once and reusing it on every subsequent open is what keeps page turns fast.

## Cache identity: the property hash

The section cache filename is `{cachePath}/spines/{spineIndex / 32}/{spineIndex}_{propertyHash:08x}.bin` (`Epub::spineCacheDir`). The per-spine caches sit in buckets of 32 spine items because a FAT open scans its directory linearly: the single flat `sections/` directory older firmware used held two files per spine item, 3400+ entries on a book with 1700 spine items, all scanned on every section open. The book-keyed inflated XHTML (`html_{spineIndex}.bin`) and the build's anchor spill share the bucket.

The property hash (`Section::calculatePropertyHash`) is 32-bit FNV-1a over a packed buffer of the twelve rendering parameters in `Section::BuildParams` that affect layout:

| Parameter | Type | Effect on layout |
|---|---|---|
| `fontId` | `int` | Glyph metrics, advance widths |
| `lineCompression` | `float` | Line-height multiplier |
| `extraParagraphSpacing` | `bool` | Extra gap between paragraphs |
| `paragraphAlignment` | `u8` | Justify / left / center / right |
| `viewportWidth` | `u16` | Line break positions |
| `viewportHeight` | `u16` | Page break positions |
| `hyphenationEnabled` | `bool` | Hyphenation at line end |
| `fontSizeNormalization` | `bool` | Near-body font sizes (within ±10 %) render as body text |
| `embeddedStyle` | `bool` | Whether book CSS is applied |
| `bionicReadingEnabled` | `bool` | Bold-prefix word transform |
| `inlineFootnotePreviews` | `bool` | Footnote text expanded inline; when on, `INLINE_FOOTNOTE_PREVIEW_LAYOUT_VERSION` is hashed too |
| `imageRendering` | `u8` | Image scaling policy |

Any change to a parameter produces a different hash and a new cache file. Old variants are kept up to `MAX_VARIANTS = 5` per chapter (see Variant eviction below).

## Cache load path

`Section::loadSectionFile` opens the candidate file and validates the version byte (`SECTION_FILE_VERSION`, defined at the top of `lib/Epub/Epub/Section.cpp` — it is bumped on every layout change, so read it there rather than trusting a number quoted here) and the ten render parameters the header stores. `fontSizeNormalization` and `inlineFootnotePreviews` are not in the header; the hash in the filename is what keys them. Then it:

1. Reads the status byte. `kStatusParseComplete` clear means the cache was truncated (a low-memory partial parse), and the reader shows a hint. The other bits record a degraded build (an image dropped to alt text, a table row demoted to paragraphs, skipped CSS lookups) that earns the spine one rebuild per session, or a fixed capacity that changed the output (`kStatusSimplified`, logged only).
2. Reads `pageCount` and the page LUT offset.
3. Loads the LUT into `lut: vector<uint32_t>` — one file offset per page. When no heap block can hold it, pages read their offset from the file instead (`lutFileOffset_`).
4. Builds the TOC boundaries from the anchor map. The page-break labels are read on first use (`Section::ensurePageBreakLabels`) and the paragraph LUT on demand.
5. Leaves the file **open** so `loadPageFromSectionFile()` can seek within the same handle without reopening.

If the exact variant is not found but `embeddedStyle=true`, the loader tries the same hash with `embeddedStyle=false` as a fallback (a cache built without CSS). If that loads, `isEmbeddedStyleFallback()` returns true and the reader immediately schedules a rebuild with CSS enabled.

## Cache creation path

A build is **sliceable**. `Section::stepSectionBuild(params, budgetMs)` advances it by about `budgetMs` of work and returns `More` until it reaches `Done` or `Failed`; the build state lives in the `Section` between calls. It yields between 1 KB feed chunks, so a slice never interrupts a page's SD write. `Section::createSectionFile` is the run-to-completion form: it pumps the same steps with no budget.

Who builds:

| Caller | When | Memory |
|---|---|---|
| Background-B | look-ahead: the next section, while the reader is idle | only in the borrowed secondary buffer |
| Background-C | the current section, in slices, while mid-build pages are drawn | borrows the buffer; on the X4 may keep it resident |
| `EpubReaderActivity::compileSectionCache` | blocking: CSS-fallback rebuild, Background-C failure escalation, no buffer for C | borrows the buffer unless escalated or nothing to lend |

[Background rendering](../background-rendering.md) describes the scheduling, and [Temporary Memory Increase Logic](./temporary-memory-increase.md) the buffer handling.

**Embedded style gate.** Before a heap-backed build with `embeddedStyle=true`, `Section::startBuild` checks `Section::heapAllowsEmbeddedStyle`: free heap against `SCT_EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES` (44 KB), and the largest block against `SCT_EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES` (12 KB) or 8 B per CSS rule plus 8 KB, whichever is larger. A build in the borrowed buffer is exempt, because its ruleset lives in the build arena. If the gate refuses, the build continues with `embeddedStyle=false`, writing the no-CSS variant, rather than failing.

**Two phases.** Phase (a) inflates the spine's XHTML into `html_{spineIndex}.bin`, keyed on the book and spine only, so a settings change or a rebuild skips the inflate. Phase (b) streams that file through `ChapterHtmlSlimParser`. The parser calls `onPageComplete(page)` for each finished page; the callback serialises the page immediately to the section file and records its byte offset. Peak memory is proportional to one page, not the whole chapter.

**Anchors spill to SD.** The parser writes each anchor record to a temp file (`anchors_{spineIndex}.tmp`, `Section::getAnchorSpillPath`) as it finds it, so nothing in memory scales with how many anchors a chapter has. The finalizer copies the spill into the section file's anchor map and deletes it.

**Partial cache.** If the parse stops early (a heap gate in the parser fires, the stream read fails), the build writes the pages it completed, clears `kStatusParseComplete`, and succeeds. The truncated cache is reused on the next open — the reader shows a hint but the chapter remains readable up to the truncation point. A background build that comes out truncated is not kept: Background-B discards it, and Background-C rebuilds the section on the blocking path.

### On-disk file structure

```
[Header: 38 bytes]
  u8   version (= SECTION_FILE_VERSION, see Section.cpp)
  i32  fontId
  f32  lineCompression
  bool extraParagraphSpacing
  u8   paragraphAlignment
  u16  viewportWidth
  u16  viewportHeight
  bool hyphenationEnabled
  bool embeddedStyle
  bool bionicReadingEnabled
  u8   imageRendering
  u8   status              // kStatusParseComplete | ...ImageHeaderDegraded | ...TableRowDegraded
                           //   | ...CssDegraded | ...Simplified
  u16  pageCount
  u32  pageLutOffset
  u32  anchorMapOffset
  u32  pageBreakMapOffset
  u32  paragraphLutOffset

[Page data — serialised Page objects, written during the parse]

[Page LUT — at pageLutOffset]
  u32 offsets[pageCount]   // file offset of each serialised Page

[Anchor map — at anchorMapOffset]
  u16 count
  { u32 idLength, idLength bytes, u16 pageIndex } × count

[Pagebreak label map — at pageBreakMapOffset]
  u16 count
  { u16 pageIndex, u32 labelLength, labelLength bytes } × count   // doc-pagebreak / NCX / page-map

[Paragraph LUT — at paragraphLutOffset]
  u16 count
  { u32 visibleTextOffset, u16 paragraphIndex, u16 listItemIndex } × count
```

The byte offsets of the header fields are in the `header` namespace in `Section.cpp`. The status byte, the page count and the four offsets are written as placeholders when the file is opened and patched in place after the parse completes.

### Paragraph LUT

One 8-byte entry per page. `visibleTextOffset` is the number of visible bytes of the chapter's source text before the page's first element, counted by the layout parser with the rule in `lib/Epub/Epub/VisibleText.h`, the same rule the KOReader XPath mappers count with; it is what a KOReader sync pushes for the page and how a pulled position becomes a page (`Section::getPageForVisibleTextOffset`). `paragraphIndex` is the 1-based `<p>` sibling count; `listItemIndex` is the running `<li>` count. They let incoming KOReader XPath strings (`p[N]`, `li[N]`) snap to a page when no offset could be resolved, and (`paragraphIndex` of a page and of the page before it) tell the sync whether a remote paragraph opens on the current page.

## Images

A build needs only image dimensions. They come from the image headers through the book's image manifest (`EpubImageManifest`); a header that cannot be read for lack of memory is deferred, walked later from the borrowed framebuffer, and the section rebuilt once. Images are decoded when their page is drawn, or ahead of the reader by the image lane, and written to `.pxc` pixel caches on SD. Nothing decodes images right after a build. See [Memory Allocation Strategy §9.3](../memory-allocation-strategy.md#93-image_scratch-and-the-borrowed-region).

## Variant eviction

`Section::evictOldVariants` keeps up to `MAX_VARIANTS = 5` variants per spine index. It walks the spine's bucket directory for files whose names start with `{spineIndex}_`, sorts them by SD modification timestamp (newest first), and deletes any beyond the fifth. Extracted images and their caches are keyed by content, not by layout, and are shared across variants, so they are not deleted with a variant; the function only sweeps up image files in the old layout-keyed naming (`img_{spineIndex}_{hash}_*`) that older firmware left behind.

Eviction runs when a fresh build starts (`createSectionFile` and `stepSectionBuild`), unless the caller passes `skipEviction=true`.
