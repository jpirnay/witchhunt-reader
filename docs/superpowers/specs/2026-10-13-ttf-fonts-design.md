# On-device TTF fonts with a persistent glyph store: design

Status (2026-10-13): approved. A step-back review replaced the earlier `SdCardFont`-based integration
with the direct integration in section 3.

- **Step 0 (device benchmark): done.** Its numbers are in Appendix A; the decisions they drove are in
  section 2.
- **Stage 1: done, in review as PR #468** (branch `refactor/glyph-provider`, plan
  `docs/superpowers/plans/2026-10-13-ttf-stage1-glyph-provider.md`).
- **Stage 2: not planned yet.** The next step is the Stage 2a plan.
- **Where everything lives:** section 8, "Branches and files".

It is working material. When the work merges, lasting parts move to reference docs, open items move
to `docs/future_work/`, and this file is deleted (see the docs layout convention).

## 1. Goal

A reader copies a `.ttf` family to the SD card and reads with it. There is no PC conversion step. The
target is the ESP32-C3 boards (X3/X4: no PSRAM, about 25-39 KB of largest free heap block while
reading). The same code runs on S3 boards.

Success means:

- Once a glyph is stored, a page turn with a TTF font is as fast as with a flash-mapped `.cpfont`.
- Rendered glyphs survive power cycles, so a new session does not render them again.
- Heap use is no worse than today's SD-font path.
- Latin families and CJK fonts both work. CJK fonts are 5-20 MB per weight.
- Headings and other CSS sizes snap to the reader's size ladder (10-26 pt, `FONT_SIZE_RUNGS`) and
  render natively at that rung, as built-in fonts do. Only the small remainder is resampled.

Out of scope, by decision: OTF/CFF outlines and any other format. The aim is TTF, done properly and fast.

## 2. Decisions and their evidence

All numbers were measured on 2026-10-13. The flash figures come from RISC-V cross-compiles at -Os
after garbage collection. The speed figures come from a host build and only rank the engines; device
numbers come from step 0.

| Decision | Why |
|---|---|
| No FreeType | 98 KB of flash, or 149 KB with the auto-hinter, against about 280 KB of headroom. |
| No stb_truetype, libschrift or bb_truetype | **stb:** a 56,004 B allocation per glyph on RV32 (`stbtt__hheap_alloc`), and edges are dropped silently when it fails. Its float maths is emulated. **libschrift:** a 262,880 B stack frame in `render_outline`, and `double` maths throughout. **bb_truetype:** 1-bit output, no bounds checks, kerning that does not compile. |
| Engine: our own integer TrueType loader plus FreeType's `ftgrays.c` built standalone | 6.4 KB of flash and **no soft-float routine linked**. Across 12,868 glyphs (all of Bookerly, Noto Sans, Inter and DejaVu Sans at 14 pt), compared with FreeType unhinted: no load failures, no cmap or advance differences, 5 box differences, and the 2-bit output differs on 2.1-2.8% of pixels. Host speed 4-9 us/glyph; FreeType took 6-40. |
| Render lazily and persist the result | Chosen over converting the whole font once at import, which would mean a long wait for CJK. |
| Glyph store in the `spiffs` partition (raw flash, memory-mapped) | Fast random reads with no heap; it survives power-off; it is already used for the `.cpfont` cache. |
| Wipe the store on family change | One family at a time, as with `.cpfont` today. |
| **Direct integration through the glyph-miss hook**, not through `SdCardFont` | `SdCardFont`'s per-page machinery works around SD reads: per-page lookup tables, a bitmap arena with a retry ladder, metrics-only layout, a per-page kern matrix, a heap miss ring. The mapped store makes all of it unnecessary, because a lookup returns a pointer into flash. Reusing that machinery would have meant two large refactors of a 2,000-line class on the page-turn path. |
| Render a glyph at its first lookup, in layout or in a draw | New glyphs are rendered during section builds, which already run in the background or behind the "Indexing" popup. Page turns then rarely render anything. There is no metrics-only mode to maintain. |
| Native ladder rungs, created lazily | Sharp headings are a main benefit of TTF. Rungs are the absolute `FONT_SIZE_RUNGS` (at most 9), so the store holds a bounded set of sizes. A rung costs about 0.5 KB of RAM. |
| A resident scratch block per loaded family, holding a 4 KB cell pool (8 KB for SD-mode families) | Section builds already borrow the lent framebuffer. With the pool on the stack, rendering took 4.1 / 6.1 / 10.2 KB of the loop task's 16 KB stack for a 2 / 4 / 8 KB pool (X3, step 0). The pool size is a speed dial: against 2 KB, 4 KB is 9-18% faster (Bookerly 29 px: 469 → 429 µs), and 8 KB makes CJK about 37% faster (1,327 → 833 µs). Output is byte-identical across pool sizes. |
| Kerning: pairs stored lazily in the store's pair index; a miss is resolved by a GPOS reader that pre-resolves the kern subtables once | Measured on the X3 over a real page (736 pairs) and 30,000 characters of prose (19,379 pairs), every option exact. FreeInkFont's `Gpos.cpp` re-parses GPOS on every call: 137 µs/pair. The pre-resolved reader: 37.7 µs. Tables compiled at install: 3.0 µs, but 1.4 s compile, 71 KB flash and a 114 KB heap peak per style. A RAM pair cache: 2.0 µs over a chapter, 5 KB RAM, cold every session. The lazy store: **0.45 µs warm**, no RAM, about 230 slot writes on a first page and about 400 more over a first chapter. The user chose the lazy store. |
| Misses are rendered in glyph-ID order, batched per text run | On a real 19.7 MB CJK font on the SD card, reading each glyph's outline costs 4.76 ms in text order and 0.73 ms in file order, because SdFat walks the FAT chain on every backward seek. Glyph IDs follow file order. |

## 3. Architecture

```
GfxRenderer ── fontMap ── EpdFontFamily ── EpdFont ── EpdFontData (one per size and style)
                                                         │ provider (new)
                                                         ▼
                                   GlyphProvider ── SdCardFont (existing contexts, unchanged behaviour)
                                                 └─ TtfFont (new, one per loaded TTF family)
                                                      ├─ FontBytes        reads font bytes: mapped flash, or SD through a page cache
                                                      ├─ TtfRasterizer    integer TrueType loader + standalone ftgrays
                                                      ├─ FlashGlyphStore  (lib/hal) write-once index + append-only log in `spiffs`
                                                      └─ GPOS reader, Gsub  kerning (pre-resolved subtables) and ligatures (no FreeType)
```

### 3.1 `GlyphProvider` (replaces `glyphMissHandler` and `glyphMissCtx` in `EpdFontData`)

Today `EpdFontData` carries a miss callback plus a `void*` context. `GfxRenderer::getGlyphBitmap`
treats any non-null context as an `SdCardFont` (`SdCardFont::fromMissCtx`) to recognise its ring
glyphs, so a second provider would be misread. Those two fields are replaced by one interface pointer:

```cpp
class GlyphProvider {
 public:
  // A glyph the font's own tables do not have (all of them, for a TTF font).
  // The record lives at least until the next call on this provider.
  virtual const EpdGlyph* missGlyph(uint32_t codepoint) = 0;
  // True when the provider keeps this record's bitmap itself (ring glyphs); `bitmap` then gets it,
  // which is nullptr for a glyph with no pixels. False: the bitmap is at EpdFontData::bitmap +
  // dataOffset. A plain pointer return could not tell "mine, no pixels" from "not mine".
  virtual bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const { return false; }
  // 4.4 fixed-point pixels, like the kern tables. Asked only when the font has no kern tables.
  virtual int8_t kerning(uint32_t leftCp, uint32_t rightCp) { return 0; }
  virtual ~GlyphProvider() = default;
};
```

- **`EpdFont::findGlyph`** calls `provider->missGlyph` where it called the callback.
- **`EpdFont::getKerning`** asks the provider when no kern tables are set.
- **`GfxRenderer::getGlyphBitmap`** asks `provider->ownsBitmap` instead of the cast.
- **`SdCardFont`** implements the interface with the per-style contexts it already has. Its ring
  glyphs report their heap bitmap through `ownsBitmap`; its behaviour does not change.
- **Built-in fonts** leave the pointer null, so the generated font headers keep their initialisers.

### 3.2 `TtfFont` (lib/EpdFont/ttf/)

`TtfFont` is one object per loaded TTF family, and it implements `GlyphProvider`. It owns:

- `FontBytes`, the rasterizer, the scratch block, the store handle, and the ligature tables;
- **a face per size**. A face is just registration data: one `EpdFontData` per present style, and four
  `EpdFont` objects for the `EpdFontFamily` registered in `fontMap`.

Each face's `EpdFontData` holds:

- no intervals and no glyph table, so every lookup goes to the provider;
- `bitmap` = the mapped log base;
- `advanceY`, `ascender` and `descender` from that style's `hhea` at that size;
- `is2Bit = true`;
- the style's ligature table;
- `provider` = a small per-(face, style) adapter that forwards to the `TtfFont` with its size and
  style.

A face costs about 0.5 KB: four `EpdFont`, four 116 B `EpdFontData`, and the adapters.

**A glyph lookup.** `missGlyph(cp)` turns the code point into a store key (code point, style, the
face's size index) and probes the mapped index.

- **Hit:** it returns a pointer to the `EpdGlyph` inside the log record, in flash. Its `dataOffset`
  is relative to the log base, so the renderer's normal bitmap path reads the bitmap in place, with no
  copy and no heap.
- **Miss:** it renders the glyph, stores it (4.5), and returns the stored record.
- **Ring mode:** used when the store is full, after a write error, or in previews (6, 7.6). The glyph
  is rendered into an 8-slot heap ring instead, and `ownsBitmap` reports the ring bitmap.

`TtfFont` is not registered in `sdCardFonts_` or `sdCardFontAliases_`. Those maps drive `SdCardFont`'s
own maintenance loops: metadata drop and restore, cache clear, prewarm, stats.

**Batching.** `GfxRenderer::ensureFontReady` (before layout measures a text run) and
`FontCacheManager::prewarmCache` (before a page is drawn) each gain a TTF branch, which calls
`TtfFont::prefetch(text, style)`.
- `prefetch` collects the codepoints of the text that are not stored yet, sorts them by glyph ID and
  renders and stores them in that order.
- Glyph IDs follow file order, so in SD mode the outline reads become forward reads (5.1).
- The per-glyph `missGlyph` path then finds them stored. It still renders anything the prefetch did not
  cover, one glyph at a time.

### 3.3 `GfxRenderer`: one lookup helper and a resolver

`GfxRenderer` looks fonts up with direct `fontMap.find` calls at more than 8 sites. Some draw paths
record text during the scan pass before any lookup; some measure paths look up first and return 0 on
a miss. A rung that does not exist yet would therefore be silently skipped by some paths. So:

- **One private lookup helper** replaces the direct calls.
- **On a miss, the helper calls an optional resolver** installed by `SdCardFontManager`. The resolver
  creates the face for a dormant TTF rung (5.5) and RETURNS the family; the helper registers it in
  `fontMap` (a plain emplace, not `insertFont()`). The resolver must not register it itself, and must
  not look fonts up through the renderer, which would recurse. (As built in Stage 1:
  `lib/GfxRenderer/FontLookup.h`, `GfxRenderer::setFontResolver`.)
- **A declined ID is asked again on every lookup**, so declining must be cheap.
- `fontMap` becomes `mutable`, the same compromise `sdCardFonts_` already makes.

With no resolver installed, or for an ID the resolver does not know, the behaviour is today's.

### 3.4 Concurrency

Layout runs on the loop task (background section builds) and drawing runs on the render task.
Both hold `RenderLock` around their work, and `SdCardFont`'s unprotected per-page state already
relies on that.

`TtfFont` relies on the same contract. Its miss path, prefetch, kerning store writes, scratch block, ring
and SD page cache are touched only under `RenderLock`. A debug-build assert on the miss path checks that the
calling task holds the rendering mutex.

Store hits read immutable flash. The write order (record first, slot last) means a probe never sees a
slot whose record is incomplete.

## 4. The glyph store (`FlashGlyphStore`, lib/hal)

### 4.1 Partition layout in TTF mode

The `spiffs` partition holds either a `.cpfont` family (`FlashFontPartition`, magic `CPFC`) or one TTF
family (`FlashGlyphStore`, magic `CPTT`). `SdCardFontManager` decides which one to install. Both stay
below `FlashFontPartition::fontUsableSize()`; the last 64 KB is the language slot and is never touched.

```
sector 0 (4 KB)  header (4.2)
style data       flash mode: each style's whole TTF, every style 64 KB-aligned so it maps alone
                 SD mode: every table except glyf, per style (cmap loca hmtx head hhea maxp GPOS GSUB)
index            open-addressing hash table of 8-byte slots, erased (0xFF) at install
pair index       open-addressing table of 8-byte kerning slots (4.3), 64 KB, erased at install
log              append-only glyph records, erased at install
```

**Mode choice at install.** Flash mode applies if every style, plus the index and a minimum log of
256 KB, fits the usable partition and the mapping budget. Otherwise the font uses SD mode.

**Region sizes come from the mapping budget, not from the partition size.** The C3 has 128 MMU pages
of 64 KB shared by the app's code, its rodata and every mapping. About 31 are free at runtime, a
figure step 0 measures with `spi_flash_mmap_get_free_pages`. The index and the log stay mapped while
the font is loaded, so glyph records and bitmaps are read in place.

Rule at install:

```
log pages = free pages − (largest style, or the copied tables) − index pages − 1 (pair index) − 2 spare pages
```

The largest style is ≤10 pages in flash mode; the copied tables are about 7 pages for CJK in SD mode.
Examples at 31 free pages:

| Mode | Also mapped | Index | Log (roughly) |
|---|---|---|---|
| Flash (Latin family) | the style being rendered, one at a time | 64 KB (8,192 slots) | 18 pages ≈ 1.1 MB ≈ 11,000 glyphs at ~100 B |
| SD (CJK, large families) | the copied tables | 256 KB (32,768 slots) | 18 pages ≈ 1.1 MB ≈ 4,500 glyphs at ~250 B |

The geometry is fixed at install from the budget measured then. If a later firmware update shrinks the
budget so that a mapping fails, the store reinstalls with smaller regions (6.2).

### 4.2 Header (sector 0, written at install; some fields are write-once afterwards)

- magic `CPTT` and a format version;
- mode (flash or SD) and the style count;
- **family identity**: FNV-1a over each style file's name, byte size and FAT modification time, plus a
  render-version byte covering the engine, the 150 DPI sizing and the quantization thresholds;
- geometry: index offset, slot count, log offset, log size;
- per style: in flash mode, the TTF's offset and size; in SD mode, the SD path hash, the file size, and
  a table map (tag, offset in the file, offset in the partition, length) for each copied table;
- **size table**: 16 write-once entries of 8 bytes (pixels-per-em in 26.6, a flags byte). An erased
  entry is unused;
- **wipe-pending word**: write-once, from 0xFFFFFFFF to 0.

### 4.3 Index slot: `{u32 key, u32 logOffset}`

- `key = cp (bits 0-20) | style << 21 | sizeIndex << 23`. Bits 27-31 are zero, so a key can never equal
  the erased value 0xFFFFFFFF.
- `logOffset = 0xFFFFFFFE` means "the font has no glyph for this codepoint", or it can never load
  (6.1). Such slots have no log record, and the font is not asked again.
- Slot position: `(key * 2654435761) >> (32 - log2(slots))`, then linear probing up to 32 slots. A
  longer chain counts as full (4.6).
- **Pair slots** (the separate 64 KB pair index, 8,192 slots) are `{u32 leftCp << 16 | rightCp,
  u32 style << 16 | u16 value}`.
  - The value is the kerning in font units (int16), independent of size; it is scaled to 4.4 pixels
    per face at lookup.
  - A probe matches key and style.
  - Only BMP pairs are stored. Rarer pairs are resolved by the GPOS reader every time.

### 4.4 Log record

```
[u32 key][EpdGlyph, 16 B][2-bit bitmap, MSB-first stream], padded to 4 bytes
```

- **`EpdGlyph` sits at offset 4 of a 4-byte-aligned record,** so a pointer to it is a valid
  `const EpdGlyph*`, read from flash.
- **`dataOffset`** is relative to the log base.
- **The bitmap format matches `.cpfont` exactly:** 4 pixels per byte, 3 = full ink.
- **`dataLength` is capped at 8 KB.** A larger glyph cannot be stored, and the cap also bounds the
  recovery scan.

### 4.5 Writing, and surviving a power cut

NOR flash bits can only change from 1 to 0 without an erase, so:

1. program the record (key, `EpdGlyph`, bitmap);
2. program the index slot (8 bytes).

- **Reads trust a slot only if** the record at `logOffset` lies inside the log, has the same key, and
  has `dataLength` ≤ 8 KB. A record left behind by a power cut is never referenced, so it only wastes
  space.
- **At load the log is scanned record by record** to find where writing resumes. This takes a few
  thousand hops through mapped flash, well under 100 ms; checkpoints are deferred (section 11).
  - The scan stops at an erased key.
  - A header with an impossible length (over 8 KB, or past the log's end) is a torn write. Writing
    resumes at the next 4 KB boundary that is fully erased, so no byte is ever programmed twice.
- **A slot cut short is skipped.** Insertion carries on probing past it.
- **ESP-IDF keeps the mapped view current.** It invalidates the mapped cache lines after every write
  (`spi_flash_check_and_flush_cache` in `memspi_host_driver.c`), so a record can be read back
  immediately.
- **Flash encryption is assumed off** for this partition, as it is now. Encrypted writes come in 16-byte
  blocks and would break write-once slots.

### 4.6 When the store fills

The store counts as full when any of these holds:

- the log is more than 95% used;
- the index is more than 70% loaded, or a probe chain passes 32 slots;
- all 16 size entries are taken.

It then sets the wipe-pending word, and `TtfFont` switches to ring mode (3.2): glyphs are still drawn
but not stored. **The wipe itself runs only at a point where no `EpdGlyph` pointer into the log can be
live: at book open, or before a section build starts.** It erases the index and the log behind a
"Preparing font" popup. That is about 1.2-1.4 MB of erase, roughly 3-8 s depending on the chip.

### 4.7 Wear

Sectors are erased only at install (family change), on a wipe, or on a reinstall after an update.
Everything else programs erased bytes once. Typical SPI NOR flash allows about 100,000 erase cycles per
sector, so even one family change a day lasts centuries. The partition is separate from the app
partitions, so firmware updates do not wear it. Write time is the real cost: while flash is being
programmed, the CPU cannot fetch code, so it stalls. That is about 0.7 ms per 256-byte page programmed
and about 45 ms per 4 KB sector erased, from typical datasheets; step 0 measures the real chip.

## 5. Font source and engine

### 5.1 `FontBytes`

`FontBytes` is the engine's only way to read a font. It offers `u8/u16/u32(offset)` and
`read(offset, len, dst)`, and every access is bounds-checked. Two error flags are kept apart, because
6.1 treats them differently:

- **out of range:** the font itself is broken, which is permanent;
- **I/O error:** an SD read failed, which is transient.

Modes:

- **Flash mode:** a pointer and a length into the style's mapping.
- **SD mode:** offsets inside a copied table are read from the mapping. Everything else (the glyph
  outlines) comes from an open `HalFile` through an LRU page cache of 8 × 512 B. That 4 KB of heap is
  held while the font is loaded, in SD mode only.
- **Outline reads in SD mode must move forward through the file.** A backward seek makes SdFat walk the
  FAT chain from the start of the file. On a 19.7 MB font that costs 4.5 ms per random 512 B read, against
  0.35 ms sorted. Prefetch therefore renders misses in glyph-ID order (3.2).

### 5.2 `TtfRasterizer`

`render(FontBytes&, glyphId, pxPerEm26_6, Scratch&) -> 2-bit bitmap + metrics`.

- **Integer TrueType loader:**
  - cmap formats 4 and 12; loca, both short and long; simple glyphs;
  - compound glyphs with xy offsets and 2.14 scale or matrix transforms (point matching deferred);
  - hmtx advances, and hhea ascender/descender.
  - Font units are scaled to 26.6 with integer multiply and divide. No floating point is allowed
    anywhere.
  - A build check enforces this. It reads the "archive member included to satisfy reference" section
    of `firmware.map` and fails if any `lib/EpdFont/ttf` object pulls in a libgcc soft-float member
    (`*sf*.o` or `*df*.o`).
- **`ftgrays.c`** (FreeType 2.13.3, the version the SDK vendors) is built with `STANDALONE_`:
  - It is vendored under `lib/EpdFont/ttf/third_party/freetype/`, with `FTL.TXT` and a PATCHES note.
  - Shims: `FALL_THROUGH`, `FT_BEGIN_HEADER`, `FT_STATIC_BYTE_CAST`.
  - `ft_grays_raster` is renamed so it cannot clash with the SDK's FreeType.
  - Patched to take its cell pool from the scratch block instead of the stack. Unpatched,
    `gray_convert_glyph` has a 4,400 B frame.
  - It renders through the direct span callback (`FT_RASTER_FLAG_DIRECT`). Spans are packed straight
    into the 2-bit output with the PC converter's thresholds (coverage nibble ≥12 → 3, ≥8 → 2, ≥4 → 1),
    so there is no 8-bit coverage buffer.
- **Sizes match the PC converter:**
  - pixels per em = pt × 150 / 72;
  - advances in 12.4 fixed point from the unscaled `hmtx` value;
  - ascender, descender and line height from `hhea`, rounded like `fontconvert_sdcard.py`.

  So a TTF at 14 pt lays out like a `.cpfont` at 14 pt.

### 5.3 Scratch memory (resident while a TTF family is loaded)

One block, allocated at family load and freed at unload. Its size is computed from the font, with no
allocation per glyph:

- **cell pool:** 4 KB, or 8 KB for an SD-mode (CJK) family. The pool size never changes the output
  (host check: identical bytes), only the speed; see the scratch row in section 2;
- **outline points:** 9 B per point, sized from `maxp` (the larger of maxPoints and
  maxCompositePoints), capped at 512 points (4.6 KB);
- **contours:** 2 B each, from `maxp`, capped at 256;
- **2-bit output:** sized from the head bbox at the largest rung, capped at 4 KB.

That comes to about 6-10 KB for text fonts and about 14 KB for a CJK family. The block is not borrowed from the lent framebuffer,
because section builds already use that region for their build arena. A glyph with more points than
the cap can never load (6.1); text glyphs stay well under it.

### 5.4 Kerning and ligatures

- **Code:**
  - `Gsub.{h,cpp}` is copied from FreeInkFont at a pinned SDK commit, with attribution.
  - `Gpos.cpp` is NOT used for lookups: it re-parses the script, feature and lookup lists on every call
    and scans format-2 ranges linearly (137 µs/pair on the X3).
  - Our GPOS reader resolves, once per style at load, the ordered list of `kern` PairPos subtables
    (required feature first, then the language system's features, lookups and subtables; Extension
    lookups unwrapped). Per pair it applies them first-found-wins, with binary search in format-2
    coverage and class tables, which OpenType keeps sorted.
  - Host check: identical to `Gpos.cpp` on every repertoire pair of six fonts. On the X3: 37.7 µs/pair
    for Bookerly, 23 µs for Noto Sans.
- **Kerning:** `TtfFont::kerning(l, r)`:
  1. probes the mapped pair index (4.3);
  2. on a hit, scales the stored font units to 4.4 pixels at the face's size, clamped to int8 like the
     `.cpfont` matrix;
  3. on a miss, asks the GPOS reader and writes the pair's slot (font units, a zero included).

  Measured on the X3: 0.45 µs/pair warm. About 230 slots are written on a first page and about 400 more
  over a first chapter, each slot write costing 124 µs.
  - There is no RAM pair cache: the warm store is already faster than one (2.0 µs).
  - Layout and draw use the same values, as built-in fonts do. SD fonts today kern only when drawing.
- **Ligatures:**
  - fi, fl, ff, ffi and ffl are resolved once at load through GSUB and become an `EpdLigaturePair`
    table per style. It is shared by all of that style's faces and is a few dozen bytes.
  - A ligature glyph with no codepoint of its own gets the private-use key `U+F0000 + glyphId`, which
    `TtfFont` maps back to the glyph ID.

### 5.5 Sizes: the body face and the ladder rungs

- **The body face** is the reader size the user has chosen, at any point size the settings allow,
  rendered natively. A TTF family never uses the scaled alias (`ensureSizeAlias`).
- **Rungs.** `buildReaderFontSizeLadder` gets a TTF branch. For a TTF body font it adds one rung per
  `FONT_SIZE_RUNGS` entry:
  - font ID `computeFontId(identity, family, rungPt)`;
  - `sizePct = rungPt * 100 / bodyPt`.

  This is the same shape as the built-in ladders, so `FontSizeLadder::resolve` and its 3% dead zone
  are unchanged. A rung equal to the body size is the body face.
- **Dormant until needed.** At family load, `SdCardFontManager` keeps a dormant table (font ID → rung
  point size). Nothing is allocated and nothing is entered in `fontMap`. The lookup helper's resolver
  (3.3) creates the face on first use. That covers every lookup inside `GfxRenderer`: layout, scan
  passes and draws, including pages laid out in an earlier session. It does NOT cover code that reads
  `fontMap` directly: `FontCacheManager::prewarmCache` (its own `fontMap_` reference), FreeInkUI
  (`getFontMap().find`) and `SdCardFontManager`. A cached page drawn in a fresh session therefore
  reaches `prewarmCache` while its rung is still dormant, so the TTF branch of `prewarmCache` must
  resolve through `TtfFont`'s dormant table, not `fontMap_`; otherwise that page loses the batched
  prefetch and renders its glyphs one at a time (correct output, but about 4.76 against 0.73 ms per CJK
  glyph in SD mode).
- **Lifetime.** Faces live until the family is unloaded. There is no eviction in v1: at most 9 rungs,
  about 0.5 KB each.
- **Store use.** Each face has its own size index. Rung sizes are absolute, so a body-size change adds
  at most one new entry (the new body size, if it is not a rung). The 16 entries cover all rungs plus
  several body sizes.

### 5.6 Not supported in the first version

- compound glyphs positioned by point matching: the glyph is logged and drawn as missing;
- glyphs with more than 512 points;
- font collections (`.ttc`);
- variable fonts beyond their default instance: they register as Regular, and missing styles fall back
  as an incomplete `.cpfont` family does;
- hinting.

See section 11 for the full deferred list.

## 6. Errors and limits

### 6.1 Glyph failures

| Situation | Behaviour |
|---|---|
| The font has no glyph for the codepoint | A slot with the "missing" marker; the replacement glyph is drawn as today. |
| A permanent load failure (point matching, over the cap, an out-of-range read in the outline) | A slot with the "missing" marker; logged once. |
| A transient failure (an SD I/O error) | Drawn as missing this time, **not** marked, retried at the next lookup. |

### 6.2 Store and mapping failures

| Situation | Behaviour |
|---|---|
| A flash write fails | Set wipe pending, switch to ring mode, log it. |
| The index or log cannot be mapped | Reinstall with smaller regions. If that also fails, use ring mode for the session; log it. |
| The TTF file was changed or removed | The identity check at load reinstalls, or the family drops out of the registry and the reader falls back as it does today. |

### 6.3 Budgets

- **Resident heap per loaded TTF family:**
  - scratch, 6-10 KB (about 14 KB for a CJK family, with the 8 KB pool);
  - the GPOS reader's subtable list, about 100-200 B per style;
  - the SD page cache, 4 KB, in SD mode only;
  - the ligature tables, a few dozen bytes;
  - about 0.5 KB per face in use: the body face plus typically 2-4 rungs.

  Every font table and every stored bitmap is mapped, not copied, and there is no per-page arena. That
  comes to about 7-14 KB for a Latin family (about 19 KB for CJK in SD mode), against roughly 9-14 KB for an SD `.cpfont` (about 6 KB of kern classes plus
  a 3-8 KB arena per page).
- **Ring mode only:** the 8-slot heap ring, allocated when entering ring mode and freed at the wipe or
  at unload.
- **Flash for the code:** about 20-25 KB. The engine is 6.4 KB and `Gpos`/`Gsub` about 4 KB; the rest is
  `TtfFont`, the store, `FontBytes`, the provider and lookup changes, and the registry. Measured in the
  build and quoted in the PR.

## 7. Data flow

### 7.1 Discovery and install

1. `SdCardFontRegistry` also scans `.ttf` files. Each file's family comes from the `name` table (ID 16,
   or ID 1 if there is no 16), and its style from `head.macStyle` (bold and italic bits). A registry
   entry records its kind, TTF or `.cpfont`.
2. For a TTF family, `SdCardFontManager::loadFamily` creates a `TtfFont` instead of an `SdCardFont`.
   `TtfFont` computes the family identity.
3. If the store header matches, the store is reused. Otherwise the family is installed behind a
   "Preparing font…" popup:
   - flash mode: erase, then copy every style. About 1.8 MB, roughly 10 s;
   - SD mode: erase, then copy the tables. About 3-5 s for CJK.
4. The index, the log and (in SD mode) the tables are mapped, the log is scanned to find where writing
   resumes, and the scratch block is allocated.
5. The body face is registered in `fontMap`, the rungs are recorded as dormant, and the resolver is
   installed.

### 7.2 Layout and drawing

Layout and drawing both go through `EpdFont`. Before a text run is measured (`ensureFontReady`) and
before a page is drawn (`prewarmCache`), `TtfFont::prefetch` renders that text's missing glyphs in
glyph-ID order (3.2). A glyph lookup:

1. probes the store and returns the record pointer on a hit;
2. on a miss, renders the glyph, writes it through to the log and the index (4.5), and returns the
   stored record.

- **Most rendering happens during section builds,** so page turns mostly hit the store.
- **Kerning comes from the provider** (the pair index, then the GPOS reader), both in layout and when
  drawing (5.4).

### 7.3 Ring mode

Ring mode applies when wipe is pending, after a write error, or for previews. It is the same flow,
except that a glyph that is not stored is rendered into the 8-slot heap ring and is not written. Glyphs
already in the store are still read from it.

### 7.4 Size and family changes

- A new body size gets a new body face and, unless it is a rung, the next size-table entry. Its glyphs
  build up alongside the others.
- Rung faces do not change, because rungs are absolute point sizes.
- A new family means a reinstall.
- The family identity feeds `computeFontId`, so changing the TTF files gives new font IDs and the
  sections are laid out again.

### 7.5 Wipe

The wipe runs when wipe is pending, at book open or before a section build starts, when no record
pointer into the log can be live (4.6).

### 7.6 Previews (font selector)

A TTF family that is not installed is previewed through a temporary `TtfFont` in ring mode, reading
from the SD card. **Nothing is written to the store,** so a preview never wipes the installed family;
`.cpfont` previews follow the same rule with `FlashCachePolicy::ReadOnly`. A preview sentence has about
18 distinct glyphs, more than the ring's 8 slots, so some are rendered again across the measure, wrap
and draw passes. At about 1 ms per glyph that is acceptable; step 0 confirms it. The rendered previews
are cached as `.fpv` files, keyed by the family identity.

## 8. Delivery stages

| Stage | Content | Merges? | Status (2026-10-13) |
|---|---|---|---|
| **Step 0** | Throwaway device benchmark on the X3 (section 9.1). | No | Done; Appendix A |
| **Stage 1** | Two changes, neither altering behaviour: (a) `GlyphProvider` in place of `glyphMissHandler` and its context; `SdCardFont` implements it and `getGlyphBitmap` loses the cast. (b) `GfxRenderer`'s single font-lookup helper plus the optional resolver hook. | One PR (the plan's Task 5) | PR #468 |
| **Stage 2a** | `FlashGlyphStore` and its host tests (simulated NOR flash, power-cut injection). | Own PR | Not planned |
| **Stage 2b** | `TtfRasterizer`: integer loader, vendored and patched `ftgrays`, span packing to 2-bit, and the regression test against FreeType. | Own PR | Not planned |
| **Stage 2c** | `FontBytes`, `TtfFont` (with `prefetch` and its two hooks), the pre-resolved GPOS reader and the kerning pair index, `Gsub`, registry and manager wiring, dormant rungs and the TTF ladder branch, previews. | Own PR | Not planned |
| **Stage 2d** | User-guide section on TTF fonts; FreeType credit (required by the FTL licence). | With 2c or own PR | Not planned |

Each stage builds on the previous one. Stacked PRs are retargeted to master as their base merges.

**Planning.** Each stage gets its own implementation plan, written when the previous stage's results
are in. Step 0 and Stage 1 come first and have no dependency on each other. Stage 2's plans use step
0's numbers.

**Branches and files** (all pushed to origin, 2026-10-14):

| Branch | What it holds | Fate |
| --- | --- | --- |
| `docs/ttf-fonts-design` | This spec, the step 0 plan and the Stage 1 plan. | Working material: merges with or before the code, then is folded away (docs layout convention). |
| `bench/ttf-step0` | The step 0 bench, `bench/ttf/` and `bench/ttf_main.cpp`, with a README on regenerating its data, running it and rebuilding its host checks. It also holds the prototype: the integer loader (`ittf.c`), the standalone `ftgrays` builds, the pre-resolved GPOS reader (`fastkern.h`) and the kerning options (`kernopts.h`). | Never merges. Stage 2b and 2c start from its prototype code; delete the branch once Stage 2 no longer needs to re-measure. |
| `refactor/glyph-provider` | Stage 1. | PR #468 to master. |

The CJK font used for measuring (Microsoft YaHei) is a Windows system font: use it locally only, and
never commit it or data generated from it.

### 8.1 Carried into Stage 2 from the Stage 1 review

The Stage 1 final review (2026-10-13) found nothing to fix in Stage 1 itself. These points bind the
Stage 2 plans:

- **Resolver writes and concurrency (2c).** Once a resolver is installed, any lookup can insert into
  `fontMap`. On the S3 the loop and render tasks run in parallel, so an unlocked reader would race an
  insert that rebalances the map. Either assert `RenderLock` in the resolve branch (as 3.4 does for the
  miss path) or create rung faces at a safe point.
- **The resolve path allocates in the middle of a draw** (a map node of about 40 B plus a face of about
  0.5 KB), and `std::map::emplace` aborts on OOM because the firmware has no exceptions. Prefer creating
  rung faces during section builds or at family load; if they are created lazily, create them with
  `nothrow` allocation before the emplace.
- **`prewarmCache` and the dormant table:** see 5.5.
- **`TtfFont::ownsBitmap` should be O(1)**, an address-range check against its ring, because every draw
  from the store pays that call.
- **Record lifetime.** `GlyphProvider::missGlyph` promises a record only until the next miss on ANY
  provider of the same font (SdCardFont's styles share one ring; `TtfFont`'s per-face adapters over one
  ring will too). `EpdGlyphRef` copies the metrics by value, so layout may hold any number of refs; only
  `sdRecord` (bitmap offset and ownership identity) ages, and draws consume it straight after the lookup.
  Rename `sdRecord` in 2c, since it will carry flash-store records too.
- **Optional:** a `prefetch(text)` virtual on `GlyphProvider` (no-op by default) would let
  `ensureFontReady` and `prewarmCache` reach `TtfFont` through `family->getData(style)->provider`,
  without a font-ID-to-TtfFont registry.

## 9. Testing

### 9.1 Step 0: device benchmark (throwaway, X3), DONE 2026-10-13

Its numbers are in Appendix A; the decisions they drove are in section 2. Gate verdicts:
- **Latin all-in:** about 1.3 ms per new glyph, with the cache emptied and the writes measured in place:
  PASS.
- **CJK all-in:** 2.5-2.9 ms per new glyph with file-order reads: over 2 ms, so REVISIT. The first-chapter
  gate passes (3.8-4.3 s for 1,500 glyphs) only with file-order reads (about 10.4 s in text order). The
  remedy taken is batching misses in glyph-ID order (3.2, 5.1). Metrics-only layout stays deferred.
- **Free mapping pages:** 32: PASS.

What was measured:

- µs per glyph for a Latin font from mapped flash, and a CJK font with its tables mapped and outlines
  read from SD;
- a store hit as a hash probe through mapped flash, against today's mini-table lookup;
- a GPOS kerning lookup, with and without the pair cache;
- the stack high-water mark of the loop task and the render task while rendering;
- flash program time per stored record, and the erase time for 64 KB and 1 MB;
- free mapping pages at runtime.

Gates:

- **If a glyph costs more than about 2 ms all-in** (render plus write), or **a CJK first chapter would
  add more than about 10 s to its build,** pull the deferred metrics-only layout mode (section 11) into
  Stage 2c.
- **If fewer than about 25 mapping pages are free,** shrink the store regions in 4.1.

### 9.2 Stage 1

Stage 1 must not change behaviour:

- All existing host tests pass, and page-dump goldens are byte-identical.
- A device smoke test with a `.cpfont` family and with a built-in family: page turns, chapter builds,
  previews, the dictionary view, and ring glyphs (force a prewarm miss). The heap low-water mark
  (`logStats`) does not get worse.

### 9.3 Stage 2: host tests

- **`FlashGlyphStore` on a simulated NOR flash.** Programming can only clear bits, erase sets 0xFF, and
  the test fails on any write that needs 0 → 1. A power cut is injected at every write step, then the
  store is reopened. The index must be consistent, no torn record may be referenced, and the write
  position must be correct. Also covered: the full and wipe-pending transitions, and the size-table
  limit.
- **The integer loader against FreeType unhinted** (the host build has FreeType through FreeInkFont),
  over every glyph of the bundled TTF sources: no load failures, ≤5 box differences per font, and ≤3% of
  pixels different at 2-bit. These are today's prototype numbers kept as a regression gate. Span
  packing must match packing the bitmap afterwards.
- **`TtfFont` cold against warm:** the same layout metrics and the same page dumps with an empty store
  and with a populated one. Ring mode must give the same dumps as stored mode.
- **`FontBytes`:** bounds checks, SD page-cache eviction, and routing between copied tables and SD.
- **Corrupted fonts** under AddressSanitizer and UBSan: truncated files and randomly mutated tables. No
  crash and no read out of bounds.

### 9.4 Stage 2: device (X3 and X4)

- a Latin family in flash mode and a CJK font in SD mode;
- headings render from rung faces, including on pages laid out in an earlier session;
- after a reboot nothing is rendered again (the logs show store hits);
- a family switch wipes the store, the store-full path enters ring mode and wipes at the next book
  open;
- first-page latency, first-chapter build time, and the heap low-water mark against the `.cpfont`
  baseline;
- a preview never touches the store.

## 10. Prototype

The prototype is on branch `bench/ttf-step0` under `bench/ttf/` (see its README): `ittf.c` (the integer
loader), the standalone `ftgrays` builds, `fastkern.h` (the pre-resolved GPOS reader) and host checks.
`bench/ttf/host/fullcmp.cpp` is the comparison against FreeType that produced the accuracy figures in
section 2. All of it is throwaway code: Stage 2b reimplements the loader on top of `FontBytes`, keeping
the measured behaviour as its test gate.

## 11. Deferred (pick up after the first version)

1. Compound glyphs positioned by point matching (ARGS_ARE_XY_VALUES clear).
2. Glyphs with more than 512 points (scratch cap).
3. Font collections (`.ttc`).
4. Variable fonts beyond the default instance (real bold from the `wght` axis).
5. Hinting: glyphs are unhinted, so stem weight at small sizes can vary (compare issue #149). A light
   integer stem darkening or x-height snap could follow.
6. FreeType's 4×4 oversampling of overlapping outlines. The cause of the 2-3% difference at 2-bit;
   revisit only if artefacts show on the panel.
7. Ligatures beyond fi/fl/ff/ffi/ffl, and other GSUB features.
8. Native rendering at exact CSS sizes beyond the ladder rungs, and evicting faces before the family
   unloads.
9. Several families in the store, for example archiving a family's store to SD on switch.
10. Smarter handling of a full store (generations, LRU) instead of a wipe.
11. Checkpoints for finding the log's write position, if the scan measures slow.
12. A metrics-only layout mode, so a CJK first chapter build need not render every new glyph (step 0
    gate).

## 12. Side findings (separate from this work)

- **Possible null dereference (unverified, code reading only).** For a `.cpfont` loaded from SD (not
  mmap), `unloadMetadata()` sets `fullIntervals` to null before the section build, but
  `findGlobalGlyphIndex` indexes it with no null check during layout prewarm.
- **Stale comments:** the `SdCardFont::loadFromMmap` header comment says "no heap allocation", and the
  `prewarmStyle` mmap comment says bitmaps are not in the mapping. Both are wrong.
- **Performance:** a full prewarm reads glyph records from SD even for mmap fonts, and `onGlyphMiss` has
  no mmap branch.
- **SD fonts kern only when drawing,** so layout widths and drawn widths differ by the kerning.
- **The loop task's stack is 16 KB, not 8 KB.** FreeInkUI overrides `getArduinoLoopTaskStackSize`.
  The comment in `src/main.cpp` above the loop-task high-water log still says 8 KB.
- **SDK:**
  - The `TtfFont.cpp` comment says stb allocates nothing; in fact it allocates a 56 KB block per glyph
    on RV32 and drops edges silently when that fails.
  - stb's GPOS kerning skips Extension (type 9) lookups.

## Appendix A. Step 0 measurements (X3, 160 MHz, 2026-10-13)

Measured with the `bench_ttf` sketch on branch `bench/ttf-step0` (`bench/ttf/README.md` says how to
re-run it). Three runs agreed within 1%. Renderer: the integer loader plus standalone `ftgrays`,
rendering through the span callback straight to 2-bit.

**Render, µs per glyph** (29 px/em = 14 pt / 54 px/em = 26 pt). The cold pass, which keeps the render
code cached, is about equal to the warm one. The last row empties the 16 KB flash cache before each
glyph.

| Font | 2 KB pool | 4 KB pool | 8 KB pool |
| --- | --- | --- | --- |
| Bookerly | 469 / 769 | 429 / 691 | 424 / 628 |
| Noto Sans | 369 / 643 | 335 / 559 | 335 / 512 |
| CJK (Microsoft YaHei subset) | 1,327 / 2,297 | 1,185 / 1,896 | 833 / 1,713 |
| Flash cache emptied, 29 px, Bookerly / CJK | 738 / 1,585 | 696 / 1,442 | 690 / 1,087 |

- The output is byte-identical across pool sizes (host span check).
- An emptied flash cache adds about 0.25-0.27 ms per glyph.
- Stack used while rendering with the pool on the stack: 4,064 / 6,128 / 10,224 B for the 2 / 4 / 8 KB
  pool. The loop task's stack is 16 KB (FreeInkUI's `getArduinoLoopTaskStackSize`). Production puts the
  pool in the scratch block (5.3).

**Flash (`spiffs` partition):**
- Erase: 64 KB of written flash 138 ms; 1 MB of mostly blank flash 1.08-1.58 s; a full wipe of a written
  1.2-1.4 MB store about 2.6-3.1 s.
- Program: a 100 B record 297 µs, a 252 B record 601 µs, an 8 B index slot 124 µs.
- Each `esp_partition_write` call has a fixed cost: writing a record's header and bitmap in one call saves
  about 120 µs per glyph.

**Store hit:** 1.5 µs per hash probe through mapped flash, against 1.08 µs for a built-in
`glyph_lookup`.

**Mapping budget:** the reader image leaves **32** free MMU pages (a device boot probe; the 6.27 MB image
uses 96 of 128). The largest mapping of the `spiffs` partition is 32 pages (2 MB). An estimate from the
ELF must take the union of the IROM and DROM page indices, because both buses index the same table;
summing the segments over-counts.

**One page** (1,199 characters, 42 distinct; no kerning on any path):

| Font | built-in `.cpfont` | TTF, first contact | TTF, warm store |
| --- | --- | --- | --- |
| Bookerly | 41.9 ms (16.3 prewarm + 25.6 draw) | 66.8 ms (18.3 render + 25.5 writes) | **21.2 ms** |
| Noto Sans | 34.7 ms | 58.3 ms | **18.4 ms** |

A warm store is about twice as fast as a built-in font, because nothing is decompressed. On first contact
the flash writes cost more than the rendering.

**Kerning, µs per pair** (Bookerly). "Page" is a real page with 736 kernable pairs; "chapter" is 30,000
characters of Moby-Dick with 19,379 pairs. Times include about 0.15-0.19 µs of loop overhead. Every
option returned exactly what the reference reader returns.

| Option | Page | Chapter | Cost |
| --- | --- | --- | --- |
| FreeInkFont `Gpos.cpp` | 137 | 136 | none |
| Pre-resolved GPOS reader (`fastkern.h`) | 37.8 | 37.7 | none |
| Reader + per-left-glyph subtable mask + RAM pair cache | 5.4 cold, 1.4 again | 2.0 (83% hits) | 5 KB RAM, cold every session |
| Class tables compiled at install, read from flash | 3.2 | 3.0 | 1.37 s compile + 0.28 s write, 71 KB flash and a 114 KB transient heap peak, per style |
| **Pairs stored lazily in the flash store (chosen)** | 58.7 first, 0.9 warm | 5.5 first, **0.45 warm** | 229 slot writes for the first page, 397 more over the chapter |

Noto Sans: `Gpos.cpp` 90 µs, the reader 23.0 µs, compiled tables 2.4-2.6 µs, the RAM cache 2.0 µs over
the chapter, the lazy store 0.45 µs warm.

**SD card** (the full 19.7 MB YaHei font as `/bench/cjk.ttf`):
- A random 512 B read costs 4.5 ms; a sorted one 0.35 ms. A backward seek makes SdFat walk the FAT chain
  from the start of the file, so the cost grows with the file size (21.5 ms in a 167 MB file).
- Reading whole glyph records (666 B, 2.3 sectors on average): **4.76 ms per glyph in text order, 0.73 ms
  in file order.**

**Gates** (9.1), from the cache-emptied render figures and the measured writes (about 0.61 ms per Latin
glyph, 0.73 ms per CJK glyph):
- Latin all-in: 0.70 ms render (4 KB pool) + 0.61 ms writes = **1.3 ms**: PASS.
- CJK all-in: 1.44 ms render (4 KB pool) + 0.73 ms SD read in file order + 0.73 ms writes = **2.9 ms**
  (2.5 ms with the 8 KB pool): REVISIT. In text order it is 6.9 ms.
- CJK first chapter, assuming 1,500 new glyphs: 4.3 s (4 KB pool) or 3.8 s (8 KB pool) in file order:
  PASS. In text order about 10.4 s: FAIL. The remedy taken is batching misses in glyph-ID order (3.2,
  5.1), not the metrics-only layout mode.
- Free mapping pages: 32: PASS.
