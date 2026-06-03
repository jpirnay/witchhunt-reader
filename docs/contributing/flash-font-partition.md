# Flash Font Partition (FlashFontPartition)

The device's `spiffs` partition (3.47 MB at flash address `0xc90000`) is repurposed as a raw binary font store. It is not a SPIFFS filesystem — nothing mounts it. `FlashFontPartition` writes `.cpfont` files directly into this region and memory-maps them for zero-SRAM-cost access at runtime.

## Why flash storage for fonts

SD card font loading allocates heap for every glyph metric table read. For a full Literata family (5 point sizes × regular/bold/italic/bold-italic = 20 styles), the kern and interval tables alone can exceed 100 KB. Flash mmap eliminates that cost: the data lives in flash address space, the CPU reads it via cache, and no SRAM is consumed beyond the metadata pointers.

Fonts written to the flash partition are written **once** at install time (via the web server update flow) and read-only thereafter — an ideal write-once, read-many pattern for NOR flash.

## Partition layout

```
[4 B]  Magic: "CPFC"
[1 B]  Entry count (0..MAX_ENTRIES=16)
[3 B]  Reserved (zero)
[16 × 48 B = 768 B]  Index entries (unused entries zeroed)
--- HEADER_BYTES = 776 bytes total ---

[variable, 4-byte aligned]  Font data blobs
```

Each 48-byte index entry:

```
[32 B]  familyName — null-padded C string (e.g. "Literata")
[1 B]   pointSize — u8 (e.g. 16)
[3 B]   Padding
[4 B]   dataOffset — u32 LE, byte offset from partition start
[4 B]   dataSize — u32 LE
[4 B]   Reserved
```

The `(familyName, pointSize)` pair is the lookup key. `MAX_ENTRIES = 16` is enough for three font families × five sizes plus margin.

## Write API

Font files are written once per install session using three calls:

```cpp
FlashFontPartition::beginWrite("Literata");
for (uint8_t ptSize : {10, 12, 14, 16, 18}) {
    FlashFontPartition::appendFile(sdPath, "Literata", ptSize);
}
FlashFontPartition::finaliseWrite();
```

**`beginWrite(familyName)`** erases the entire partition upfront (`esp_partition_erase_range` on the full 3.47 MB), then initialises the write session. Erase is sector-granular (4 KB sectors); erasing the whole partition once is cheaper than per-sector erase management.

**`appendFile(sdPath, familyName, pointSize)`** reads the `.cpfont` file from SD in 4 KB chunks and writes it to flash via `esp_partition_write`. Data offsets are 4-byte aligned. The function records the entry in the write session but does not write the index yet.

**`finaliseWrite()`** writes the 8-byte header (magic + count) and the packed entry table to the start of the partition. Index entries are written in order of insertion.

The full partition is erased on `beginWrite`, so installing a new font family replaces all previous contents. There is no incremental update path — if you need to add a family, reinstall all families.

## Mmap read API

```cpp
const uint8_t* ptr;
size_t sz;
if (FlashFontPartition::mmap("Literata", 16, &ptr, &sz)) {
    SdCardFont font;
    font.loadFromMmap(ptr, sz, sdPath);
    FlashFontPartition::unmap();
}
```

**`mmap(familyName, pointSize, outPtr, outSize)`** reads the index from flash, finds the matching entry, and calls `esp_partition_mmap`. The mapped region covers from partition start to `dataOffset + dataSize`, rounded up to 64 KB alignment (the minimum mmap granularity on ESP32). `outPtr` points to the start of the font data within the mapped region; `outSize` is the raw `.cpfont` file size.

Only one mmap handle is active at a time. `SdCardFont::loadFromMmap` copies the metadata it needs to heap-aligned buffers (see below) before the caller unmaps.

**`unmap()`** releases the mmap handle. After this call the pointer returned by `mmap` is invalid.

## How `loadFromMmap` differs from `load`

Both paths produce an `SdCardFont` in the same usable state. The difference is where data lives.

`SdCardFont::load(sdPath)` opens the file, reads every table into heap-allocated arrays, and closes the file. All metadata is heap-owned (`metadataOwned_ = true`, `mmapDataBase_ = nullptr`).

`SdCardFont::loadFromMmap(base, size, sdPath)` reads the same binary format from the mmap pointer. For each style it:

- **Copies** `fullIntervals` to heap — `EpdUnicodeInterval` contains `uint32_t` fields that need natural alignment, which flash cannot guarantee.
- **Aliased directly** the kern class tables — `EpdKernClassEntry` is `__attribute__((packed))`, so byte-granular reads are safe. The pointers point into flash address space.
- **Copies** `ligaturePairs` to heap — `EpdLigaturePair` contains `uint32_t` fields; alignment required.
- Sets `mmapDataBase_ = base` so the kern matrix fast path works (see below).
- Sets `metadataOwned_ = true` so the destructor knows to `delete[]` the heap copies.

The key benefit: the kern **matrix** (the large int8_t grid, up to ~28 KB for Literata) stays in flash and is read via direct pointer arithmetic — no heap allocation, no SD I/O.

## Kern matrix access: mmap vs SD

When building the per-page mini kern matrix (`buildMiniKernMatrix`), the code branches on `mmapDataBase_`:

**Mmap fast path** — zero heap, zero SD I/O:
```cpp
const int8_t* matrixBase = reinterpret_cast<const int8_t*>(mmapDataBase_ + s.kernMatrixFileOffset);
const int8_t* srcRow = matrixBase + (oldL - 1) * rowBytes;
```

**SD chunked path** — 4 KB chunk buffer, forward sweep through file:
```cpp
std::unique_ptr<int8_t[]> chunkBuf(new (std::nothrow) int8_t[KERN_CHUNK_BYTES]);  // 4096 bytes
// seeks only when the needed row falls outside the current chunk
```
Falls back to per-row reads if the chunk buffer cannot be allocated.

## Query API

```cpp
FlashFontPartition::hasValidIndex()          // partition has been written at least once
FlashFontPartition::hasEntry("Literata", 16) // specific entry present
FlashFontPartition::hasFamilyComplete("Literata", sizes, count) // all sizes present
FlashFontPartition::isMapped()               // mmap currently active
```

These are read-only and do not require a write session or active mmap.

## Lifetime and mmap validity

The mmap pointer is valid from `mmap()` to `unmap()`. `SdCardFont::loadFromMmap` must finish all reads before the caller unmaps. The aliased kern class pointers in `PerStyle` point into flash and remain valid as long as `isMapped()` — but in practice the font is fully loaded (aliased pointers and heap copies set) before unmapping, so the kern class pointers work correctly after unmap via the aliased flash address (flash address space is always accessible; only the mmap *handle* is released, not the flash itself).

`sdPath` is stored in `filePath_` for the `reloadMetadata` path: if metadata is unloaded between chapters and needs to reload, the SD path is the fallback for fonts whose mmap is no longer active.
