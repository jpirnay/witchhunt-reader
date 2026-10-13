# On-device TTF fonts with a persistent glyph store: design

Status: design approved in conversation on 2026-10-13. This file is the written spec for review.
It is working material. When the work merges, lasting parts move to reference docs, open items move
to `docs/future_work/`, and this file is deleted (see the docs layout convention).

## 1. Goal

A reader copies a `.ttf` family to the SD card and reads with it. There is no PC conversion step. The
target is the ESP32-C3 boards (X3/X4: no PSRAM, about 25-39 KB of largest free heap block while
reading). The same code runs on S3 boards.

Success means:

- Once a glyph is cached, a page turn with a TTF font is as fast as with a flash-mapped `.cpfont`.
- Rendered glyphs survive power cycles, so a new session does not render them again.
- Heap use is no worse than today's SD-font path.
- Latin families and CJK fonts both work. CJK fonts are 5-20 MB per weight.

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
| Integration in stages: a `GlyphSource` interface under `SdCardFont` | `SdCardFont`'s page machinery is exactly what a TTF needs. Only the data source differs. |
| Write glyphs to the store during page warm-up | It is simpler than deferring the writes, which would need 10-15 KB of heap to hold new bitmaps. The next page is pre-rendered in the background, so the cost is mostly hidden. |

## 3. Architecture

```
SdCardFont (page engine; its API toward the eight dependents is unchanged)
   │ owns one
   ▼
GlyphSource ── CpFontSource    today's .cpfont reading code, moved, behaviour identical
            └─ TtfSource       new
                 ├─ FontBytes        reads font bytes: mapped flash, or SD through a page cache
                 ├─ TtfRasterizer    integer TrueType loader + standalone ftgrays
                 ├─ FlashGlyphStore  (lib/hal) write-once index + append-only log in `spiffs`
                 └─ Gpos / Gsub      kerning and ligature lookups (no FreeType)
```

The code that depends on `SdCardFont` today keeps talking to `SdCardFont` and to the `EpdFont*`
addresses it hands out:

- GfxRenderer and FontCacheManager;
- SdCardFontManager;
- FontSelectionActivity and DictionaryDefinitionActivity;
- EpdFont and EpdFontData, through the miss-handler context.
`SdCardFont` keeps everything per page:

- prewarm into the mini `EpdFontData` tables;
- the 512-glyph accumulation limit;
- the `PREWARM_ARENA_TOO_LARGE` retry ladder;
- the 8-slot miss ring;
- the per-page kern matrix layout;
- `clearCache` and `clearAccumulation`.

### 3.1 `GlyphSource` (lib/EpdFont/GlyphSource.h)

The interface works on whole pages, not single glyphs. That keeps virtual calls out of the inner loops
and lets `CpFontSource` keep its current code paths unchanged. Below is a sketch of the contract; the
implementation plan fixes the exact signatures.

```cpp
class GlyphSource {
 public:
  struct StyleMetrics { uint8_t advanceY; int16_t ascender; int16_t descender; bool is2Bit; };
  virtual ~GlyphSource() = default;

  virtual bool hasStyle(uint8_t style) const = 0;
  virtual StyleMetrics metrics(uint8_t style) const = 0;
  virtual uint32_t contentHash() const = 0;  // feeds computeFontId -> section cache keys

  // Metrics for a sorted, de-duplicated codepoint list. found[i] == false: not in the font.
  virtual int resolve(uint8_t style, const uint32_t* cps, uint16_t n, EpdGlyph* out, bool* found) = 0;

  // Make the bitmaps of `n` resolved records drawable. Returns the base pointer that
  // EpdGlyph::dataOffset is relative to: either a mapped region (no copy) or `arena` after
  // filling it (dataOffset rewritten). Reports "arena too large" so the page engine can retry.
  virtual BitmapResult bitmaps(uint8_t style, const uint32_t* cps, EpdGlyph* recs, uint16_t n,
                               BitmapArena& arena) = 0;

  virtual bool buildPageKern(uint8_t style, const uint32_t* cps, uint16_t n, PageKern& out) = 0;
  virtual const EpdLigaturePair* ligatures(uint8_t style, uint8_t& count) = 0;

  // A glyph the page prewarm did not cover. bitmap points into a mapping or the miss ring.
  virtual bool missGlyph(uint8_t style, uint32_t cp, EpdGlyph& out, const uint8_t*& bitmap) = 0;

  virtual void dropMetadata() {}
  virtual bool restoreMetadata() { return true; }
};
```

The helper types:

- `BitmapArena` wraps the page engine's existing per-page bitmap allocation, either the heap or a
  `BuildArena` set with `useArena`.
- `BitmapResult` holds the base pointer, or "arena too large" together with the measured bytes per
  glyph.
- `PageKern` holds the mini kern class tables and matrix the page engine already builds.

`CpFontSource` keeps two things exactly as they are: the existing split between mmap and SD (glyph
records read from SD, bitmaps from the mapping or into the arena), and the sharing of identical
interval tables between styles.

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
log              append-only glyph records, erased at install
```

**Mode choice at install.** Flash mode applies if every style, plus the index and a minimum log of
256 KB, fits the usable partition and the mapping budget. Otherwise the font uses SD mode.

**Region sizes come from the mapping budget, not from the partition size.** The C3 has 128 MMU pages
of 64 KB shared by the app's code, its rodata and every mapping. About 31 are free at runtime, a
figure step 0 measures with `spi_flash_mmap_get_free_pages`. While a font is loaded, the index and the
log stay mapped, so bitmaps are read in place.

Rule at install:

```
log pages = free pages − (largest style, or the copied tables) − index pages − 2 spare pages
```

The largest style is ≤10 pages in flash mode; the copied tables are about 7 pages for CJK in SD mode.
Examples at 31 free pages:

| Mode | Mapped while reading | Index | Log (roughly) |
|---|---|---|---|
| Flash (Latin family) | the style being warmed, one at a time | 64 KB (8,192 slots) | 18 pages ≈ 1.1 MB ≈ 11,000 glyphs at ~100 B |
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
- `logOffset = 0xFFFFFFFE` means "the font has no glyph for this codepoint". Such slots have no log
  record, and the font is not asked again.
- Slot position: `(key * 2654435761) >> (32 - log2(slots))`, then linear probing up to 32 slots. A
  longer chain counts as full (4.6).

### 4.4 Log record

```
[u32 key][EpdGlyph, 16 B][2-bit bitmap, MSB-first stream][u32 commit = CRC-32/IEEE (esp_rom_crc32_le) over the preceding bytes]
padded to 4 bytes
```

`EpdGlyph.dataOffset` is relative to the start of the log, so `bitmaps()` returns the log's base
pointer. The bitmap format matches `.cpfont` exactly: 4 pixels per byte, 3 = full ink.

### 4.5 Writing, and surviving a power cut

NOR flash bits can only change from 1 to 0 without an erase, so:

1. program the record's key and `EpdGlyph`;
2. program the bitmap;
3. program the commit word;
4. program the index slot (8 bytes).

- **Reads trust a slot only if** the record at `logOffset` has the same key and a valid commit. A
  record left behind by a power cut is never referenced, so it only wastes space.
- **At load the log is scanned record by record** to find where writing resumes. This takes a few
  thousand hops through mapped flash, well under 100 ms; checkpoints are deferred (section 11).
- **A header cut short cannot cause a double write.** Whatever length it holds, the next offset lands
  in erased space after it, so no byte is ever programmed twice.
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

It then sets the wipe-pending word. Until the next book open, pages fall back to the arena path
(7.3): glyphs are still drawn but not stored. At the next book open, the index and the log are erased
behind a "Preparing font" popup. That is about 1.2-1.4 MB of erase, roughly 3-8 s depending on the
chip.

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
`read(offset, len, dst)`, and every access is bounds-checked: an out-of-range read returns 0 and sets
an error flag, so a broken font produces missing glyphs, never a crash.

Two error flags are kept apart, because 6.1 treats them differently:

- **out of range:** the font itself is broken, which is permanent;
- **I/O error:** an SD read failed, which is transient.

- **Flash mode:** a pointer and a length into the style's mapping.
- **SD mode:** offsets inside a copied table are read from the mapping. Everything else (the glyph
  outlines) comes from an open `HalFile` through an LRU page cache of 8 × 512 B. That 4 KB of heap is
  held while the font is loaded, in SD mode only.
- **Misses are rendered in glyph-ID order,** so outline reads from SD are mostly sequential.

### 5.2 `TtfRasterizer`

`render(FontBytes&, glyphId, pxPerEm26_6, Scratch&) -> 8-bit coverage + metrics`. The coverage is then
packed to 2-bit with the PC converter's thresholds: nibble ≥12 → 3, ≥8 → 2, ≥4 → 1.

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
  - The 4 KB cell pool moves off the stack into the scratch block. Unpatched, `gray_convert_glyph` has
    a 4,400 B frame.
- **Sizes match the PC converter:**
  - pixels per em = pt × 150 / 72;
  - advances in 12.4 fixed point from the unscaled `hmtx` value;
  - ascender, descender and line height from `hhea`, rounded like `fontconvert_sdcard.py`.

  So a TTF at 14 pt lays out like a `.cpfont` at 14 pt.
- **Limits:** buffers are sized from `maxp` (maxPoints, maxContours), with hard caps of 1,024 points
  and 256 contours. The largest glyph measured is 852 points, in DejaVu Sans.

### 5.3 Scratch memory (class C, phase-scoped)

During prewarm, rendering borrows the lent framebuffer through the same scoped-arena pattern the image
decoder uses. When nothing is lent, it falls back to an owned 16 KB heap block, freed when the prewarm
ends. The scratch holds:

- the cell pool, 4 KB;
- the outline points, 9 B per point;
- the coverage buffer: at most 70×70 px at the largest reader size, 4.9 KB.

There is no allocation per glyph. A glyph that does not fit the owned block (more than about 700
points) is a transient failure (6.1).

### 5.4 Kerning and ligatures

- **Code:** `Gpos.{h,cpp}` and `Gsub.{h,cpp}` are copied from FreeInkFont at a pinned SDK commit, with
  attribution. They need no FreeType, and `Gpos.cpp` handles Extension (type 9) lookups, which
  Bookerly uses for all of its kerning.
- **Kerning:**
  - `buildPageKern` looks pairs up in the GPOS table in place: the style's mapping in flash mode, the
    copied table in SD mode.
  - It only queries codepoints listed in a kern lookup's coverage, and fills the same mini class tables
    and matrix the page engine already builds.
  - Kerning stays 0 during layout, as it is today for every SD font.
- **Ligatures:**
  - fi, fl, ff, ffi and ffl are resolved once at load through GSUB and become an `EpdLigaturePair`
    table on the heap (a few dozen bytes).
  - A ligature glyph with no codepoint of its own gets the private-use key `U+F0000 + glyphId`, which
    `TtfSource` maps back to the glyph ID.

### 5.5 Not supported in the first version

- compound glyphs positioned by point matching: the glyph is logged and drawn as missing;
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
| A transient failure (no scratch memory, an SD I/O error) | Drawn as missing on this page, **not** marked, retried on the next page. |

### 6.2 Store and mapping failures

| Situation | Behaviour |
|---|---|
| A flash write fails | Set wipe pending, use the arena path for the page, log it. |
| The index or log cannot be mapped | Reinstall with smaller regions. If that also fails, no glyphs are stored this session (arena path); log it. |
| The TTF file was changed or removed | The identity check at load reinstalls, or the family drops out of the registry and the reader falls back as it does today. |

### 6.3 Budgets

- **Resident heap per loaded TTF family:**
  - the SD page cache, 4 KB, in SD mode only;
  - the ligature table, a few dozen bytes;
  - the per-page structures `SdCardFont` already allocates.

  Every table is mapped, not copied.
- **Flash for the code:** about 20-25 KB. The engine is 6.4 KB and `Gpos`/`Gsub` about 4 KB; the rest is
  the source, the store, `FontBytes` and the registry. Measured in the build and quoted in the PR.

## 7. Data flow

### 7.1 Discovery and install

1. `SdCardFontRegistry` also scans `.ttf` files. Each file's family comes from the `name` table (ID 16,
   or ID 1 if there is no 16), and its style from `head.macStyle` (bold and italic bits). A TTF family
   offers every reader size natively, so the alias path (`ensureSizeAlias`) is not used for it.
2. `SdCardFontManager::loadFamily` creates a `TtfSource`, which computes the family identity.
3. If the store header matches, the store is reused. Otherwise the family is installed behind a
   "Preparing font…" popup:
   - flash mode: erase, then copy every style. About 1.8 MB, roughly 10 s;
   - SD mode: erase, then copy the tables. About 3-5 s for CJK.
4. The index, the log and (in SD mode) the tables are mapped, and the log is scanned to find where
   writing resumes.

### 7.2 Layout: section build (`ensureFontReady` → prewarm with metrics only)

- A codepoint found in the store returns its record.
- One that is not gets metrics **without rendering**: the glyph ID from cmap, the advance from hmtx,
  and the ink box from the glyph header. Nothing is written during layout.
- `dropMetadata` and `restoreMetadata` do nothing.

### 7.3 Page warm-up (`FontCacheManager::prewarmCache` → full prewarm, one style at a time)

1. Store hits return records whose bitmaps point into the mapped log, with no copy.
2. Misses are collected and sorted by glyph ID. The style is mapped (flash mode) and scratch is
   borrowed.
3. Each miss is rendered, packed to 2-bit and written through to the log and the index (4.5). It then
   draws from the log like a hit.
4. **Arena path** (wipe pending, or a write failed): every bitmap on that page, both from the log and
   newly rendered, is copied into the per-page arena. The existing allocation and retry ladder handle
   it, as for an SD `.cpfont`.

### 7.4 Draw-time misses

`missGlyph` takes the same route (a store hit, or render and write) and returns a pointer into the log.
The heap miss ring is used only on the arena path.

### 7.5 Size and family changes

- A new size takes the next size-table entry, and its glyphs build up alongside the others.
- A new family means a reinstall.
- `contentHash()` is the family identity, so changing the TTF files gives new font IDs and the
  sections are laid out again.

### 7.6 Previews (font selector)

A TTF family that is not installed is previewed straight from the SD card. It is read through the page
cache and rendered into the existing preview arena (the lent framebuffer). **Nothing is written to the
store,** so a preview never wipes the installed family; `.cpfont` previews follow the same rule with
`FlashCachePolicy::ReadOnly`. The rendered previews are cached as `.fpv` files, as today.

## 8. Delivery stages

| Stage | Content | Merges? |
|---|---|---|
| **Step 0** | Throwaway device benchmark on the X3 (section 9.1). | No |
| **Stage 1** | Extract `CpFontSource` behind `GlyphSource`. A pure refactor with identical behaviour. | Own PR |
| **Stage 2a** | `FlashGlyphStore` and its host tests (simulated NOR flash, power-cut injection). | Own PR |
| **Stage 2b** | `TtfRasterizer`: integer loader, vendored `ftgrays`, and the regression test against FreeType. | Own PR |
| **Stage 2c** | `FontBytes`, `TtfSource`, `Gpos`/`Gsub`, registry and manager wiring, previews. | Own PR |
| **Stage 2d** | User-guide section on TTF fonts; FreeType credit (required by the FTL licence). | With 2c or own PR |

Each stage builds on the previous one. Stacked PRs are retargeted to master as their base merges.

**Planning.** Each stage gets its own implementation plan, written when the previous stage's results
are in. Step 0 and Stage 1 come first and have no dependency on each other. Stage 2's plans use step
0's numbers.

## 9. Testing

### 9.1 Step 0: device benchmark (throwaway, X3)

Measure:

- µs per glyph for a Latin font from mapped flash, and a CJK font with its tables mapped and outlines
  read from SD;
- the scratch peak and the stack high-water mark;
- flash program time per stored record, and the erase time for 64 KB and 1 MB;
- free mapping pages at runtime.

Gates:

- **If a glyph costs more than about 2 ms all-in** (render plus write), revisit writing during warm-up
  (2, 7.3) before Stage 2c.
- **If fewer than about 25 mapping pages are free,** shrink the store regions in 4.1.

### 9.2 Stage 1

- All existing host tests pass, and page-dump goldens are byte-identical.
- A device smoke test with a `.cpfont` family: page turns, chapter builds, previews, and the
  dictionary view. The heap low-water mark (`logStats`) does not move.

### 9.3 Stage 2: host tests

- **`FlashGlyphStore` on a simulated NOR flash.** Programming can only clear bits, erase sets 0xFF, and
  the test fails on any write that needs 0 → 1. A power cut is injected at every write step, then the
  store is reopened. The index must be consistent, no torn record may be referenced, and the write
  position must be correct. Also covered: the full and wipe-pending transitions, and the size-table
  limit.
- **The integer loader against FreeType unhinted** (the host build has FreeType through FreeInkFont),
  over every glyph of the bundled TTF sources: no load failures, ≤5 box differences per font, and ≤3% of
  pixels different at 2-bit. These are today's prototype numbers kept as a regression gate.
- **`TtfSource` cold against warm:** the same layout metrics and the same page dumps with and without a
  populated store.
- **`FontBytes`:** bounds checks, SD page-cache eviction, and routing between copied tables and SD.
- **Corrupted fonts** under AddressSanitizer and UBSan: truncated files and randomly mutated tables. No
  crash and no read out of bounds.

### 9.4 Stage 2: device (X3 and X4)

- a Latin family in flash mode and a CJK font in SD mode;
- after a reboot nothing is rendered again (the logs show store hits);
- a family switch wipes the store, and the store-full path works;
- first-page latency, and the heap low-water mark against the `.cpfont` baseline;
- a preview never touches the store.

## 10. Prototype

`ittf.c` (the integer loader) and its benchmarks live in the session scratchpad, not in the repo. They
are throwaway code; Stage 2b reimplements the loader on top of `FontBytes`, keeping the measured
behaviour as its test gate.

## 11. Deferred (pick up after the first version)

1. Compound glyphs positioned by point matching (ARGS_ARE_XY_VALUES clear).
2. Font collections (`.ttc`).
3. Variable fonts beyond the default instance (real bold from the `wght` axis).
4. Hinting: glyphs are unhinted, so stem weight at small sizes can vary (compare issue #149). A light
   integer stem darkening or x-height snap could follow.
5. FreeType's 4×4 oversampling of overlapping outlines. The cause of the 2-3% difference at 2-bit;
   revisit only if artefacts show on the panel.
6. Ligatures beyond fi/fl/ff/ffi/ffl, and other GSUB features.
7. Heading sizes rendered natively instead of through the scaled-glyph path.
8. Several families in the store, for example archiving a family's store to SD on switch.
9. Smarter handling of a full store (generations, LRU) instead of a wipe.
10. Checkpoints for finding the log's write position, if the scan measures slow.

## 12. Side findings (separate from this work)

- **Possible null dereference (unverified, code reading only).** For a `.cpfont` loaded from SD (not
  mmap), `unloadMetadata()` sets `fullIntervals` to null before the section build, but
  `findGlobalGlyphIndex` indexes it with no null check during layout prewarm.
- **Stale comments:** the `SdCardFont::loadFromMmap` header comment says "no heap allocation", and the
  `prewarmStyle` mmap comment says bitmaps are not in the mapping. Both are wrong.
- **Performance:** a full prewarm reads glyph records from SD even for mmap fonts, and `onGlyphMiss` has
  no mmap branch.
- **SDK:**
  - The `TtfFont.cpp` comment says stb allocates nothing; in fact it allocates a 56 KB block per glyph
    on RV32 and drops edges silently when that fails.
  - stb's GPOS kerning skips Extension (type 9) lookups.
