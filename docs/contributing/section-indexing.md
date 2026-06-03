# Section Indexing Workflow

A "section" is one spine item (typically one chapter) laid out into pages for a specific set of rendering parameters. The section cache is the heaviest computation in the reader — parsing EPUB XHTML, applying CSS, measuring glyphs, breaking lines and pages, and writing all of that to SD takes several seconds. Getting it right once and reusing it on every subsequent open is what keeps page turns fast.

## Cache identity: the property hash

The section cache filename is `{cachePath}/sections/{spineIndex}_{propertyHash:08x}.bin`. The property hash is FNV-1a over a packed 64-byte buffer of the ten rendering parameters that affect layout:

| Parameter | Type | Effect on layout |
|---|---|---|
| `fontId` | `int` | Glyph metrics, advance widths |
| `lineCompression` | `float` | Line-height multiplier |
| `extraParagraphSpacing` | `bool` | Extra gap between paragraphs |
| `paragraphAlignment` | `u8` | Justify / left / center / right |
| `viewportWidth` | `u16` | Line break positions |
| `viewportHeight` | `u16` | Page break positions |
| `hyphenationEnabled` | `bool` | Hyphenation at line end |
| `embeddedStyle` | `bool` | Whether book CSS is applied |
| `bionicReadingEnabled` | `bool` | Bold-prefix word transform |
| `imageRendering` | `u8` | Image scaling policy |

Any change to a parameter produces a different hash and a new cache file. Old variants are kept up to `MAX_VARIANTS = 5` per chapter; the oldest are evicted along with their associated image files (see Variant Eviction below).

## Cache load path

`Section::loadSectionFile` opens the candidate file, validates the version byte (`SECTION_FILE_VERSION = 31`) and all ten render parameters, then:

1. Reads `parseComplete` (bool) — false means the cache was truncated (low-memory partial parse); the reader shows a hint to the user.
2. Reads `pageCount` and the page LUT offset.
3. Seeks to the LUT and loads it into `lut: vector<uint32_t>` — one file offset per page.
4. Reads the TOC boundary array, pagebreak label map, and paragraph LUT from their offsets.
5. Leaves the file **open** so `loadPageFromSectionFile()` can seek within the same handle without reopening.

If the exact variant is not found but `embeddedStyle=true`, the loader tries the same hash with `embeddedStyle=false` as a fallback (a cache built without CSS). If that loads, `isEmbeddedStyleFallback()` returns true and the reader immediately schedules a rebuild with CSS enabled.

## Cache creation path

`Section::createSectionFile` runs when no usable cache file exists. The caller must release the secondary display buffer first (see [Temporary Memory Increase Logic](./temporary-memory-increase.md)).

**Embedded style gate.** Before starting, if `embeddedStyle=true` the function checks:

- `esp_get_free_heap_size() >= EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES` (96 KB)
- `heap_caps_get_largest_free_block(...) >= EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES` (36 KB)

If either check fails the function calls itself recursively with `embeddedStyle=false, skipEviction=true`. This allows the chapter to be indexed without CSS rather than failing entirely.

**Stream parse.** The EPUB XHTML spine item is inflated and streamed directly into `ChapterHtmlSlimParser`. No intermediate temporary file is written. The parser calls `onPageComplete(page)` for each finished page; the callback serialises the page immediately to the section file and records its byte offset. This keeps peak memory proportional to one page, not the whole chapter.

**Partial cache.** If the stream is interrupted (heap gate in the parser fires, stream read fails), the function writes whatever pages were completed, sets `parseComplete=false` in the header, and returns true. The truncated cache is reused on next open — the reader shows a hint but the chapter remains readable up to the truncation point.

### On-disk file structure

```
[Header: 56 bytes]
  u8  version (= SECTION_FILE_VERSION = 31)
  int fontId
  float lineCompression
  bool extraParagraphSpacing
  u8  paragraphAlignment
  u16 viewportWidth
  u16 viewportHeight
  bool hyphenationEnabled
  bool embeddedStyle
  bool bionicReadingEnabled
  u8  imageRendering
  bool parseComplete
  u16 pageCount
  u32 pageLutOffset
  u32 anchorMapOffset
  u32 pageBreakMapOffset
  u32 paragraphLutOffset

[Page data — serialised Page objects, written during parse]

[Page LUT — at pageLutOffset]
  u32 offsets[pageCount]   // file offset of each serialised Page

[Anchor map — at anchorMapOffset]
  u16 count
  { string anchorId, u16 pageIndex } × count

[Pagebreak label map — at pageBreakMapOffset]
  u16 count
  { u16 pageIndex, string label } × count   // from doc-pagebreak / NCX / page-map

[Paragraph LUT — at paragraphLutOffset]
  u16 count
  { u32 xhtmlByteOffset, u16 paragraphIndex, u16 listItemIndex } × count
```

The header offsets are written as placeholder zeros at file-open time and patched in-place after parse completes.

### Paragraph LUT

One 8-byte entry per page. `xhtmlByteOffset` is the Expat byte position within the inflated XHTML at the moment the page break fired — used as a seek hint so KOReader XPath resolution doesn't have to scan from byte 0. `paragraphIndex` is the 1-based `<p>` sibling count; `listItemIndex` is the running `<li>` count. Together they let incoming KOReader XPath strings (`p[N]`, `li[N]`) snap to the correct page.

## Image warm pass

Immediately after `createSectionFile` returns — and **before** `reallocSecondaryBuffer` is called — the caller runs `section->warmAllImageCaches(0, 0, forceLoad, true)`. This iterates every page, loads it from the section file, and calls `Page::warmImageCaches` on pages that contain images.

The warm pass decodes each image (PNG/JPEG) into a `.pxc` pixel cache file on SD. It can do this because the secondary display buffer is still released at this point, giving the PNG decoder (~59 KB contiguous requirement) a clear run. The decode writes pixels into the framebuffer as a side effect; the caller calls `renderer.clearScreen()` afterward to discard them.

Once the `.pxc` files are on SD, every subsequent render finds them cached and never needs the decoder. See [Temporary Memory Increase Logic](./temporary-memory-increase.md) for the full heap sequence.

## Two indexing call sites

| Call site | Trigger | Notes |
|---|---|---|
| Initial load | Cache miss on chapter open | Shows "Indexing..." popup first |
| CSS fallback rebuild | `isEmbeddedStyleFallback()` true after `loadSectionFile` | Immediate rebuild with CSS enabled |

## Variant eviction

`Section::evictOldVariants` keeps up to `MAX_VARIANTS = 5` variants per spine index. It lists all files in the `sections/` directory whose names start with `{spineIndex}_`, sorts by SD modification timestamp (newest first), and deletes any beyond the fifth. For each deleted section variant it also deletes the associated image files (`img_{spineIndex}_{hash}_*`).

Eviction runs at the end of `createSectionFile` unless `skipEviction=true` (used during the CSS-fallback recursive call to avoid evicting the just-created no-CSS variant).
