# The flash area — design

> **Status.** Step 1 (one interface, the partition behind it) and step 2 (the PSRAM image, built but enabled for no
> board) are implemented. The SD-only case is design only. Code: `lib/hal/FlashArea.*` (the choice),
> `lib/hal/EspPartitionFlash.*` (backend a), `lib/hal/PsramImageFlash.*` and `lib/hal/CardImageFile.*` (backend b),
> `lib/hal/FlashAreaRegion.*` (a window for the glyph and hyphenation stores). Tests: `test/flash_area/` (every
> consumer over a simulated area, the PSRAM backend with power cuts), `test/flash_area_region/` and
> `test/flash_lang_partition/` (the partition backend over an in-memory partition).

## Problem

Every board we ship flashes the same `partitions.csv`, whose `spiffs` partition (3,456 KB at 0xc90000) nothing mounts:
the firmware keeps four caches in it as raw NOR flash and reads three of them in place through `esp_partition_mmap`.
Each cache found the partition by its name and called ESP-IDF itself. The Onyx BOOX Picco (ESP32-S3, 16 MB flash, 8 MB
octal PSRAM, 4-bit SDMMC card, per freeink-sdk's `picco` profile and env) runs a retail app of ~9 MB, so its stock
layout cannot be ours; the SDK does not record that layout. A firmware that keeps it has no `spiffs` partition, and
none of the four caches would have anywhere to live.

## Consumers

The area's layout, by offset from its start (C3 figures; the S3 boards have no hyphenation region):

| Range | Size (C3 / S3) | Owner |
|---|---|---|
| 0 .. fontUsable | 2,944 KB / 3,328 KB | the font area: the .cpfont cache **or** the TTF glyph store, never both |
| fontUsable .. +384 KB | 384 KB / none | the hyphenation store (C3 only; the S3 boards load tries into PSRAM) |
| last 128 KB | 128 KB | the language slot (the format-1 slot of 2.40 was its upper 64 KB) |

How each one uses it:

| Consumer | Reads | Mappings, and how long they are held | Program / erase | How often it writes |
|---|---|---|---|---|
| `FlashFontPartition` (.cpfont cache) | the 776 B index on each query | one entry (one size of the family) while that font is loaded: a reading session | erases the whole font area, then 4 KB programs, index last | a .cpfont family switch |
| `FlashGlyphStore` (TTF families) via `FlashAreaRegion` | headers, journal, face table | the index + pairs + log region, the SD-mode style blocks, one flash-mode style; while the families are loaded | install: erase, then the styles' bytes (MB); reading: a glyph batch per pass, slots, a segment erase when the log's head moves | install on a family set change; small writes while reading |
| `HyphenationStore` via `FlashAreaRegion` (C3) | two header copies | a language's trie (German: 4 pages), at most 2, while a book is open | a payload's sectors, then a header copy | the first book in a language |
| `FlashLangPartition` (UI language) | the 40 B header | the slot's strings (1 page; 2 with a CJK language's UI faces) for the whole run: `I18n::get()` hands out `const char*` into it | erases what the pack needs, 512 B programs, header last | a language or pack change, a new firmware |
| `SystemStatus` | the 776 B .cpfont index and the area's size | none | none | never |

Every consumer is a **cache of an original on the SD card** (the .cpfont files, the TTF files, the .cphyph packs, the
.cplang packs), and each one already survives a power cut at any instant: headers are written last, the language slot
and the hyphenation store check CRCs, the glyph store has its torn-write rules (`NorFlash.h`).

## The interface

`NorFlash` (`lib/hal/NorFlash.h`), which the glyph and hyphenation stores already used, is the one interface:
`size / read / program / erase / map / unmap / freeMapPages`, with NOR semantics (a program only clears bits, an erase
sets whole 4 KB sectors to 0xFF), plus `sync()`: "make every program and erase so far survive a power cut", a no-op
on flash.

| Piece | What it is |
|---|---|
| `FlashArea::get()` | the board's area over its whole range, chosen once; `nullptr` when the board has none |
| `EspPartitionFlash` | backend (a): the `spiffs` partition. The only code that calls `esp_partition_*` for the area; keeps the mapping handles and the PSRAM-source bounce buffer |
| `PsramImageFlash` | backend (b): the area's image in PSRAM, persisted as a file on the card through `AreaImageFile` (`CardImageFile` on the device) |
| `FlashAreaRegion` | a window of the area for one store: the font area (glyph store) or the hyphenation region, with the rule that keeps the .cpfont cache and the glyph store off each other |

`FlashFontPartition` and `FlashLangPartition` use `FlashArea::get()` with the offsets they always used; the two stores
use a `FlashAreaRegion`; `SystemStatus` asks `FlashFontPartition::cachedBytes()` and the area's size.

## Backends

| | (a) partition | (b) PSRAM image | (c) none (SD only) |
|---|---|---|---|
| .cpfont cache | yes | yes | no: `SdCardFont` reads the card (the existing fallback, its tables on the heap) |
| TTF glyph store | yes | yes | **no**: the store needs a mapping; TTF families cannot load |
| hyphenation | C3 region | not used (tries go to PSRAM already) | not used |
| UI language | yes | yes | **no**: the slot must hand out pointers; the UI stays English |
| Mapping budget | the MMU's free pages (C3 ~31) | the whole image | none |
| Internal RAM | the handle table (C3 +48 B static); on S3 also the 512 B bounce buffer, static now, which the TTF family set used to hold on the heap | the image in PSRAM; +64 B static, 112 B of bitmaps | none |

### (b) The PSRAM image

- **Image.** One PSRAM block of the area's size (`FLASH_AREA_PSRAM_BYTES`, by default the partition's 3,456 KB, so
  every layout constant holds). `program` ANDs bytes, `erase` writes 0xFF: the consumers keep their NOR rules. `map`
  returns a pointer into the block and costs no MMU page, so `freeMapPages()` reports the whole image.
- **On the card or not.** The board's SDK profile says (`BoardProfile::flashAreaOnCard`, read through
  `HalCapabilities::flashAreaOnCard()`). Its default, for a board that has not decided, is PSRAM only: the image starts
  erased at every boot, `sync()` does nothing, the card is never written, and every cache is built again after a boot.
- **File.** `/.crosspoint/flash-area.img`: a 4 KB header block (magic, version, state, size, CRC), then the image.
  `CardImageFile` opens it for each call and closes it again, so nothing holds it open (a cleared cache may delete
  it, USB mass storage may take the card) and each write is on the card when it returns.
- **Load, on first use.** Pages (64 KB) are read from the file when something first reads, programs or maps them, so
  a boot that maps one language slot and one font size reads only those pages. A file that is missing, of another
  size, has a bad header, or says *dirty* gives an erased image instead, and every consumer rebuilds from the card.
- **Commit (`sync()`).** Header *dirty*, sync; the dirty sectors, in runs; sync; header *clean*, sync. Called at the end
  of a .cpfont family copy, a language build and a glyph store install, and before deep sleep. A refused write (no
  card) keeps the sectors pending for the next one. A file that went away during the run is written again whole if
  every page is in PSRAM; otherwise the run's writes stay in PSRAM and the next boot starts erased.
- **Locking.** One mutex around every call: a lazy page load or a dirty bit must not interleave with a sync, which
  flash never needed.
- **Power cut.** The image in PSRAM is gone. Cut before the *dirty* header lands: the file keeps the previous commit.
  Cut after it and before *clean*: the next boot starts erased. Either is a state a NOR power cut could have left
  (any instant between two operations is), and every consumer already handles both; what differs is the time to
  rebuild (a TTF install takes seconds). Writes since the last commit (the glyph log while reading) are rendered again.
- **Cost.** PSRAM: the image (3,456 KB by default, of 8 MB). Internal RAM: the object, its mutex and the two bitmaps
  (112 B). Card writes: the first commit writes the whole file once (3.4 MB), later ones only the sectors that changed
  and 32 B of header. Boot: the header, then each page (64 KB) the first time something touches it.
- **Measured** on the X4 Pro (1-bit SDMMC, 2026-11-11, image forced with `FLASH_AREA_PSRAM_IMAGE=2`, card on):

  | What | Bytes | Time |
  |---|---|---|
  | First save, a new file | 3,456 KB | 2.41 s |
  | Boot: the header | 16 B | 46 ms |
  | A page loaded on first use | 64 KB | 36-37 ms (a .cpfont size at book open: 10 pages, 0.37 s) |
  | TTF install into an empty area | 256 KB | 0.22 s |
  | TTF install over a .cpfont family | 2,544 KB | 1.25 s (3 runs) |
  | .cpfont family switch: the whole font area | 3,328 KB | 1.55-1.62 s (3 runs); once 18.2 s |
  | Language slot (German) | 24 KB | 0.10 s |
  | The pre-sleep sync after 10 TTF pages (called by hand, no sleep) | 372 KB | 0.44 s |
  | Page turn | none | no sync |
  | PSRAM left beside the image | 4,522 KB | |

  The 18.2 s was the first in-place rewrite of the new file. The two runs after it took 1.55-1.62 s. A cause on the
  card's side (its own housekeeping) is likely but not proven.
- **Smaller images.** `FLASH_AREA_PSRAM_BYTES` may be smaller: the font area shrinks with it, larger .cpfont families
  fall back to the card (their existing path), and the TTF planner puts more styles in SD mode. The language slot
  needs 128 KB at the end in any case.

### (c) SD only

What a board without the partition and without PSRAM (or whose image cannot be allocated) gets: `FlashArea::get()` is
`nullptr`. The .cpfont cache is skipped and fonts load from the card, which works today. The language slot and the
glyph store would need a different design (strings in heap RAM, ~30 KB, rejected for the C3 in `FlashLangPartition.h`;
a glyph store reading its index and log through the card), so on such a board the UI stays English and TTF families do
not load. No board we know of is in that case: the Picco has 8 MB of PSRAM.

## How a board chooses

One place, `FlashArea.cpp`, at the first `FlashArea::get()`:

1. `FLASH_AREA_PSRAM_IMAGE=2` (a test build): the PSRAM image, whatever the partition table holds.
2. The `spiffs` partition, when the partition table has one. Every board today: the C3 boards stay byte for byte as
   they were.
3. `FLASH_AREA_PSRAM_IMAGE=1` and PSRAM to hold the image: the PSRAM image (the Picco's env will set this).
4. None.

The flag only decides whether the PSRAM backend is in the build and whether it beats the partition; the partition's
presence and the chip's PSRAM are asked at runtime (`esp_partition_find_first`, `heap_caps_malloc(MALLOC_CAP_SPIRAM)`).
No board sets the flag yet. `FLASH_AREA_PSRAM_BYTES` sets the image's size (default `0x360000`, the partition's).

To try the backend on an S3 board without touching its partition:
`PLATFORMIO_BUILD_FLAGS="-DFLASH_AREA_PSRAM_IMAGE=2" pio run -e x4pro -t upload`. The partition keeps what it held,
and a normal build finds it as it was.
