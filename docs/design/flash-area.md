# The flash area — design

> **Status.** Step 1 (one interface, the partition behind it) and step 2 (the PSRAM image) are implemented. Every
> build with PSRAM (the S3 boards) takes the image, saved to the card, when its partition table has no usable
> `spiffs` partition (issue #733: an X4 Pro that kept its stock table). The SD-only case is design only. Code:
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

The X4 Pro is in that case already (issue #733). Its stock tables have app0 0x10000 and app1 0x7f0000 (0x7e0000 each)
and an 80 KB data partition at 0xfd0000: `littlefs` on a retail unit on Licorice OS (read from its flash), `spiffs` in
what CrossPoint's web flasher calls the X4 Pro's factory-compatible layout. Every way onto an X4 Pro from its stock
firmware writes the app alone and keeps that table: the stock SD and OTA updaters, our SD Card Firmware Update and
OTA, and CrossPoint's web flasher (crosspointreader.com/install reads the device's table and writes the app into its
idle OTA slot; its repair page writes the factory-compatible table). Only a USB flash of a PlatformIO build writes
`partitions.csv`. An 80 KB `spiffs` partition held 2.40's 64 KB language slot; the slot grew to 128 KB in 2.41RC (for
the CJK UI faces, 42350ddd5), and from then on such an X4 Pro had no room for it: the interface stayed English with
its pack on the card (the reporter of #733 had Czech on 2.40 and English on 2.41RC and 2.50 after SD updates), and
with no `spiffs` partition at all there was no area for anything, TTF families included.

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
- **File.** `/.crosspoint/flash-area.img`: a 4 KB header block (magic, version, state, size, CRC, and right after it
  a bit per 64 KB page: the page is all 0xFF), then the image.
  `CardImageFile` opens it for each call and closes it again, so nothing holds it open (a cleared cache may delete
  it, USB mass storage may take the card) and each write is on the card when it returns.
- **Load, on first use.** Pages (64 KB) are read from the file when something first reads, programs or maps them, so
  a boot that maps one language slot and one font size reads only those pages. A page the header's bitmap calls erased
  is filled with 0xFF in PSRAM instead of read: the glyph store maps its whole log region (41 pages on the X4 Pro), of
  which a short session fills a page or two. A file that is missing, of another size or version (version 1, from
  before the bitmap, included), has a bad header, or says *dirty* gives an erased image instead, and every consumer
  rebuilds from the card.
- **Commit (`sync()`).** Header *dirty*, sync; the dirty sectors, in runs; sync; the bitmap, recomputed for the pages
  with a changed sector (a scan of each in PSRAM), and header *clean* over it, sync. With nothing changed since the
  last commit it writes nothing, not even the header. The bitmap shares the header's sector and CRC, and is only
  trusted under a clean header, so a cut anywhere leaves the previous commit or an erased image, as before. A refused write (no card) keeps the sectors
  pending for the next one. A file that went away during the run is written again whole if every page is in PSRAM;
  otherwise the run's writes stay in PSRAM and the next boot starts erased.
- **When it commits.** The consumers commit at the end of a .cpfont family copy, a language build and a glyph store
  install. `FlashArea::sync()` commits on every planned loss of PSRAM: before deep sleep (`enterDeepSleep()`), before
  every planned restart (`restartKeepingFlashArea()` in main.cpp, which every `silentRestart*()`, the heap-recovery
  restart, the restart into sleep, the SD and OTA firmware updates and the USB-drive switch end in), before the card
  goes to a USB host (`UsbDriveActivity`), and at book close (`unloadSdFontIfLoaded()`: the glyphs the book rendered).
  Not after USB Drive (the card was the host's; the restart after it does not sync) and not in the boot-time heap check
  (the area is not chosen yet). `scripts/check_restart_flash_sync.py`, run by the host tests, fails on an
  `ESP.restart()` or `esp_restart()` in src/ that does neither. There is no low-battery power-off in this firmware; a
  battery that runs flat, a crash or the reset button is a cut, below.
- **Locking.** One mutex around every call: a lazy page load or a dirty bit must not interleave with a sync, which
  flash never needed.
- **Power cut.** The image in PSRAM is gone. Cut before the *dirty* header lands: the file keeps the previous commit.
  Cut after it and before *clean*: the next boot starts erased. Either is a state a NOR power cut could have left
  (any instant between two operations is), and every consumer already handles both; what differs is the time to
  rebuild (a TTF install takes seconds). Writes since the last commit (the glyph log while reading) are rendered again.
- **Cost.** PSRAM: the image (3,456 KB by default, of 8 MB). Internal RAM: the object, its mutex and the two bitmaps
  (112 B). Measured on the X4 Pro (2026-11-11, test build forcing the image over our table):
  - first boot without a file: a 117 ms erase of the image, then the language build and the first commit, which
    writes the whole file (3,538,944 B in 2,349 ms);
  - later boots: the header (45 ms) and the language slot's page (36 ms), nothing built;
  - a language change: 24,576 B committed in 97 ms;
  - a TTF style (Literata) installed in 0.96 s plus its commit (262,144 B, 274 ms); a five-size .cpfont family
    (Bitter, 2.6 MB) copied in 1.1 s plus its commit (3,407,872 B, 1,611 ms);
  - book close after reading: 192-381 KB of glyph log committed in 434-662 ms; a book read in a .cpfont family
    commits nothing;
  - the first TTF book after a restart: the store's 52 pages (its log region is mapped whole) loaded in 2.0 s, where an
    install from the TTF file takes ~1 s. With the erased-page bitmap only its pages with data are read; in that
    state 12 of the 52 (the header, 4 style pages, 5 index pages, the pair page and the one live log segment), so
    about 0.45 s at ~37 ms a page is expected, not yet measured. Each further live 64 KB log segment adds a page;
  - a restart with nothing changed writes nothing (the file's CRC is the same before and after).
- **Smaller images.** `FLASH_AREA_PSRAM_BYTES` may be smaller: the font area shrinks with it, larger .cpfont families
  fall back to the card (their existing path), and the TTF planner puts more styles in SD mode. The language slot
  needs 128 KB at the end in any case.

### (c) SD only

What a board without the partition and without PSRAM (or whose image cannot be allocated) gets: `FlashArea::get()` is
`nullptr`. The .cpfont cache is skipped and fonts load from the card, which works today. The language slot and the
glyph store would need a different design (strings in heap RAM, ~30 KB, rejected for the C3 in `FlashLangPartition.h`;
a glyph store reading its index and log through the card), so on such a board the UI stays English and TTF families do
not load. No board we know of is in that case: the S3 boards have 8 MB of PSRAM, and the C3 boards' tables have a
large enough `spiffs` partition. The boot log says so (`[FLA] no flash area: ...`).

**What shows it.** System Information's Flash section has a **Flash area** row ("spiffs 3.4 MB"; "PSRAM + card" or
"-", then on the next line what the table holds: "spiffs 80.0 KB", "spiffs -") and a **Language** row: the language's code, and
when its pack is not in use, why: `no pack`, `bad pack` (I18n), or the slot's `FlashLangPartition::lastFailure()`:
`no flash area`, `area too small`, `build`, `mmap`, `CRC`. A slot that does not start on a 64 KB page is not a failure:
`esp_partition_mmap` maps from the page below and hands out the pointer advanced to the offset.

## How a board chooses

One place, `FlashArea.cpp`, at the first `FlashArea::get()`:

1. `FLASH_AREA_PSRAM_IMAGE=2` (a test build): the PSRAM image, whatever the partition table holds.
2. The `spiffs` partition, when the partition table has one larger than the language slot (128 KB). Every board
   flashed with `partitions.csv`: as before. A smaller one (the 80 KB of the X4 Pro's factory-compatible table) is
   left alone: the language slot could not live in it.
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
