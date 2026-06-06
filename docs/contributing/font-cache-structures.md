# Font Cache Structures

`SdCardFont` is the single class that represents a loaded `.cpfont` file regardless of whether it came from the SD card or from the flash partition. The two load paths (`load` and `loadFromMmap`) produce an identical external interface but different internal ownership arrangements. This document explains the `PerStyle` struct, the ownership flags, and the multi-tier caching lifecycle.

## PerStyle — the per-style metadata record

Each `SdCardFont` holds up to `MAX_STYLES` (currently 4) `PerStyle` entries, one per glyph style (regular, bold, italic, bold-italic). A `PerStyle` has three tiers of data:

**1. Permanent metadata** — loaded once, lives for the font's lifetime:

| Field | Type | Source |
|---|---|---|
| `header` | `CpFontHeader` | Read from TOC entry |
| `fullIntervals` | `EpdUnicodeInterval*` | Heap (always) |
| `kernLeftClasses` | `EpdKernClassEntry*` | Heap (SD) or flash alias (mmap) |
| `kernRightClasses` | `EpdKernClassEntry*` | Heap (SD) or flash alias (mmap) |
| `ligaturePairs` | `EpdLigaturePair*` | Heap (always) |
| `*FileOffset` fields | `uint32_t` | Computed from TOC, always present |

**2. Section-scoped mini prewarm buffers** — rebuilt at the start of each chapter, freed at chapter boundary:

| Field | Type | Notes |
|---|---|---|
| `miniIntervals` | `EpdUnicodeInterval*` | Subset of `fullIntervals` for this chapter's codepoints |
| `miniGlyphs` | `EpdGlyph*` | Metric-only glyph records for layout |
| `miniBitmap` | `uint8_t*` | Glyph bitmaps for render passes |
| `miniMode` | `MiniMode` | NONE → METADATA → FULL |

**3. Page-scoped mini kern matrix** — rebuilt per page render, freed after each page:

| Field | Type | Notes |
|---|---|---|
| `miniKernMatrix` | `int8_t*` | Compressed kern matrix for codepoints on this page |
| `miniKernLeftClasses` / `miniKernRightClasses` | `EpdKernClassEntry*` | Index arrays into the mini matrix |

## Ownership flags

### `metadataOwned_` (bool, always true)

This flag gates `delete[]` calls in the free functions. It is always `true` for both SD and mmap load paths. The name is historical — it was introduced to guard against double-free in edge cases and to make ownership explicit, not to distinguish between SD and mmap.

### `mmapDataBase_` (const uint8_t*)

Non-null only after `loadFromMmap`. Points to the base of the mmap'd flash region. Two things depend on it:

- **`freeStyleKernLigatureData`**: when `mmapDataBase_` is non-null, `kernLeftClasses` and `kernRightClasses` are flash aliases — they must **not** be `delete[]`'d. `ligaturePairs` is always heap-owned and always freed.
- **`buildMiniKernMatrix`**: when `mmapDataBase_` is non-null, the kern matrix is read via direct pointer arithmetic into flash. When null, the SD chunked read path is used.

## Why fullIntervals and ligaturePairs are always heap-copied

Flash on the ESP32-C3 is byte-addressable via the instruction/data cache, but only for naturally-aligned accesses. `EpdUnicodeInterval` and `EpdLigaturePair` both contain `uint32_t` fields. Their layout within the `.cpfont` binary is not guaranteed to fall on a 4-byte boundary relative to the mmap base pointer. Rather than sprinkling `__attribute__((packed))` reads everywhere in the glyph lookup hot path, we copy these arrays to heap once at load time and access them normally.

`EpdKernClassEntry` is explicitly `__attribute__((packed))` throughout the codebase — all its fields are byte-granular — so aliasing it directly from flash is safe.

## Free function hierarchy

```
freeAll()
  └─ freeStyleAll(s)  ×MAX_STYLES
       ├─ freeStyleMiniData(s)
       │    ├─ delete[] miniIntervals, miniGlyphs, miniBitmap
       │    └─ freeStyleMiniKern(s)
       │         └─ delete[] miniKernMatrix, miniKernLeftClasses, miniKernRightClasses
       ├─ if metadataOwned_: delete[] fullIntervals
       └─ freeStyleKernLigatureData(s)
            ├─ if metadataOwned_ && !mmapDataBase_: delete[] kernLeftClasses, kernRightClasses
            └─ if metadataOwned_: delete[] ligaturePairs
```

`freeStyleMiniData` deliberately does **not** reset `reportedMissCount` — that counter accumulates across all paragraphs of a section and is only cleared by `freeStyleAll` at section boundary. This lets the renderer log a single summary of SD misses at the end of a chapter.

## Metadata unload/reload

Font metadata (interval tables, kern/lig tables) can be temporarily released between chapters to make room for `createSectionFile`. This is controlled by `SdCardFontManager` calling `unloadMetadata()` / `reloadMetadata()` around each section build.

**`unloadMetadata()`**:
- No-op for mmap fonts — the data is in flash address space and always accessible.
- For SD fonts: `delete[] fullIntervals`; calls `freeStyleKernLigatureData`. File offsets (`intervalsFileOffset` etc.) are preserved so reload can re-read from the right position.

**`reloadMetadata()`**:
- No-op for mmap fonts.
- For SD fonts: reopens `filePath_`, seeks to `intervalsFileOffset`, re-reads `fullIntervals`. Kern/lig tables are **not** reloaded eagerly — they lazy-load on the next prewarm call via `loadStyleKernLigatureData`, which checks `kernClassesLoaded` and `ligLoaded` flags.

## Mini prewarm lifecycle

The mini prewarm cache exists because the full glyph tables for a section's codepoints are too large to keep resident for every paragraph. Instead, a two-stage process is used:

**METADATA mode** — accumulated across paragraphs during layout (pagination):

Each call to `renderer.ensureFontReady(fontId, text)` (formerly `ensureSdCardFontReady`) merges the text's codepoints into the mini cache if they are not already present. Only metric data (advance widths) is loaded at this stage — no bitmaps. This mode can be entered and extended incrementally.

**FULL mode** — built once per page before rendering:

`prewarm(utf8Text, styleMask=0x0F)` replaces the METADATA-mode mini cache with a complete mini cache including bitmaps for all codepoints on the page. Because glyph indices change when new codepoints are added, FULL mode cannot be extended incrementally; it is rebuilt wholesale for each page. The mini kern matrix is also built at this point (see below).

**Clearing**:
- `clearAccumulation()` — discards METADATA-mode data between sections (called by `SdCardFontManager::dropFontMetadata`).
- `freeStyleMiniData` — called by `freeStyleAll` at font unload.

## Mini kern matrix construction

Building the full kern matrix per page from the compact kern class tables is expensive. `buildMiniKernMatrix` constructs a small `numLeft × numRight` integer matrix covering only the left/right class pairs that appear in the current page's codepoints.

**Sort-then-sweep**: The used left classes are sorted in ascending file-row order. The matrix source is then read in a single forward pass (either via flash pointer arithmetic or a 4 KB SD chunk buffer), extracting only the needed rows and columns. This avoids random-access reads across the full matrix (~28 KB for Literata).

The resulting `miniKernMatrix` is `numLeft × numRight` `int8_t` values in compact row-major order, indexed by the mini left/right class remapping tables. Typical size for a page: 10–20 left classes × 10–20 right classes = 100–400 bytes.

## SdCardFontManager: selecting the load path

`SdCardFontManager` decides whether to load a requested `(familyName, pointSize)` from flash or SD:

1. Check `FlashFontPartition::hasEntry(family, size)`.
2. If yes: call `FlashFontPartition::mmap(...)`, call `font.loadFromMmap(ptr, sz, sdPath)`, call `FlashFontPartition::unmap()`.
3. If no: call `font.load(sdPath)` (SD-based read).

After either path, the font is in the same usable state from the caller's perspective. The mmap handle is released immediately after `loadFromMmap` — the font does not hold a persistent mmap handle.
