# The flash area — design

> **Status.** Step 1 (one interface, the partition behind it) and step 2 (the PSRAM image) are implemented. Every
> build with PSRAM (the S3 boards) takes the image, in PSRAM only, when its partition table has no usable `spiffs`
> partition (issue #733: an X4 Pro that kept its stock table). The SD-only case is design only. Code:
> `lib/hal/FlashArea.*` (the choice), `lib/hal/EspPartitionFlash.*` (backend a), `lib/hal/PsramImageFlash.*` and
> `lib/hal/CardImageFile.*` (backend b), `lib/hal/FlashAreaRegion.*` (a window for the glyph and hyphenation stores).
> Tests: `test/flash_area/` (every consumer over a simulated area, the PSRAM backend with power cuts),
> `test/flash_area_select/` (the choice, per board), `test/i18n_lang_pack/LangSlotPsramImageTest.cpp` (a card pack
> into the image), `test/flash_area_region/` and `test/flash_lang_partition/` (the partition backend over an
> in-memory partition).

## Problem

Every board we ship builds for the same `partitions.csv`, whose `spiffs` partition (3,456 KB at 0xc90000) nothing mounts:
the firmware keeps four caches in it as raw NOR flash and reads three of them in place through `esp_partition_mmap`.
Each cache found the partition by its name and called ESP-IDF itself. The Onyx BOOX Picco (ESP32-S3, 16 MB flash, 8 MB
octal PSRAM, 4-bit SDMMC card, per freeink-sdk's `picco` profile and env) runs a retail app of ~9 MB, so its stock
layout cannot be ours; the SDK does not record that layout. A firmware that keeps it has no `spiffs` partition, and
none of the four caches would have anywhere to live.

The X4 Pro is in that case already (issue #733). Its stock table, read from a retail unit's flash, is app0 0x10000 and
app1 0x7f0000 (0x7e0000 each), an 80 KB `littlefs` partition at 0xfd0000 and a coredump: no `spiffs`. Every way onto
an X4 Pro from its stock firmware writes the app alone and keeps that table: the stock SD and OTA updaters, our SD
Card Firmware Update and OTA, and CrossPoint's web flasher (crosspointreader.com/install reads the device's table and
writes the app into its idle OTA slot; its repair page writes, for the X4 Pro, a table whose `spiffs` partition is the
same 80 KB at 0xfd0000). Only a USB flash of a PlatformIO build writes `partitions.csv`. Before the PSRAM image was
the fallback, such an X4 Pro had no area at all: the interface stayed English with its pack on the card, TTF families
did not load, .cpfont fonts were read from the card.

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
- **In PSRAM only (the default).** No file: the image starts erased at every boot, and each 64 KB page is set to 0xFF
  the first time something reads, programs or maps it, so a boot pays for the pages it uses rather than a memset of
  3.4 MB. `sync()` has nothing to do. Every consumer rebuilds from the card after each start, as after a cache clear:
  the language slot at boot (I18n finds no slot in an erased area), a .cpfont family or the TTF families at the first
  book. `FLASH_AREA_PSRAM_ON_CARD=1` keeps the image as the file below instead.
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
  (112 B). In PSRAM only: no card writes; each start rebuilds what it uses. Measured on the X4 Pro with the image on
  the card (2026-11-11, the times without their sync): a German slot built in under 0.1 s, one TTF style (Literata)
  installed in ~1.0 s, a five-size .cpfont family (Bitter, 2.6 MB) copied in ~1.2 s. On the card: the first commit
  writes the whole file once (3.4 MB, 2.4 s), later ones only the sectors that changed and 32 B of header; a boot
  reads the header (46 ms), then each page (64 KB, ~37 ms) the first time something touches it.
- **Smaller images.** `FLASH_AREA_PSRAM_BYTES` may be smaller: the font area shrinks with it, larger .cpfont families
  fall back to the card (their existing path), and the TTF planner puts more styles in SD mode. The language slot
  needs 128 KB at the end in any case.

### (c) SD only

What a board without the partition and without PSRAM (or whose image cannot be allocated) gets: `FlashArea::get()` is
`nullptr`. The .cpfont cache is skipped and fonts load from the card, which works today. The language slot and the
glyph store would need a different design (strings in heap RAM, ~30 KB, rejected for the C3 in `FlashLangPartition.h`;
a glyph store reading its index and log through the card), so on such a board the UI stays English and TTF families do
not load. No board we know of is in that case: the S3 boards have 8 MB of PSRAM, and the C3 boards' tables have a
`spiffs` partition. The boot log says so (`[FLA] no flash area: ...`), and System Information shows "No spiffs
partition" in its Font cache row.

## How a board chooses

One place, `FlashArea.cpp`, at the first `FlashArea::get()`:

1. `FLASH_AREA_PSRAM_IMAGE=2` (a test build): the PSRAM image, whatever the partition table holds.
2. The `spiffs` partition, when the partition table has one larger than the language slot (128 KB). Every board
   flashed with `partitions.csv`: as before. A smaller one (the 80 KB of CrossPoint's X4 Pro repair) is left alone:
   the language slot could not live in it.
3. `FLASH_AREA_PSRAM_IMAGE=1` and PSRAM to hold the image: the PSRAM image. The default on every build with
   `CONFIG_SPIRAM`, so the X4 Pro, T5S3 and Murphy builds take it without a flag of their own.
4. None.

`main.cpp` asks for the area before I18n loads its language, so the choice is made, and logged, at every boot: no line
for the partition; `[FLA] the partition table has no `spiffs` partition ...` (or `... is N KB: too small ...`),
then `[FLA] the flash area is a PSRAM image instead ...` or `[FLA] no flash area: ...`. The image is then also taken
while the PSRAM is still one free block.

The flag only decides whether the PSRAM backend is in the build and whether it beats the partition; the partition's
presence and the chip's PSRAM are asked at runtime (`esp_partition_find_first`, `heap_caps_malloc(MALLOC_CAP_SPIRAM)`).
`FLASH_AREA_PSRAM_BYTES` sets the image's size (default `0x360000`, the partition's).

To try the backend on an S3 board without touching its partition:
`PLATFORMIO_BUILD_FLAGS="-DFLASH_AREA_PSRAM_IMAGE=2" pio run -e x4pro -t upload`. The partition keeps what it held,
and a normal build finds it as it was.
