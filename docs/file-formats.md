# File Formats

## `book.bin`

Written by `BookMetadataCache`. The version byte is `BOOK_CACHE_VERSION` in `lib/Epub/Epub/BookMetadataCache.cpp`; it is bumped when the layout changes, and an older cache is rebuilt on the next open. The pattern below matches version 11.

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 11
#define MAX_STRING_LENGTH 65535

// === String Structure ===

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

// === Metadata Structure ===

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("BCP47 language tag")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
    String series [[comment("Series name")]];
    String seriesIndex [[comment("Series index/position")]];
    String description [[comment("Book description / blurb")]];
} [[comment("Book metadata information")]];

// === Spine Entry Structure ===

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative size in bytes"), color("FF6B6B")]];
    s16 tocIndex [[comment("Index into TOC (-1 if none)"), color("4ECDC4")]];
} [[comment("Spine entry defining reading order")]];

// === TOC Entry Structure ===

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level (0-255)"), color("95E1D3")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)"), color("F38181")]];
} [[comment("Table of contents entry")]];

// === Book Bin Structure ===

struct BookBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];
    
    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }
    
    u32 lutOffset [[comment("Offset to lookup tables"), color("6BCB77")]];
    u16 spineCount [[comment("Number of spine entries"), color("4D96FF")]];
    u16 tocCount [[comment("Number of TOC entries"), color("FF6B9D")]];
    u8 tocReliable [[comment("1 if TOC has >=25% spine coverage, 0 otherwise"), color("F4A261")]];

    // Metadata section
    Metadata metadata [[comment("Book metadata")]];
    
    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }
    
    // Lookup Tables
    u32 spineLut[spineCount] [[comment("Spine entry offsets"), color("4D96FF")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets"), color("FF6B9D")]];
    
    // Data Entries
    SpineEntry spines[spineCount] [[comment("Spine entries (reading order)")]];
    TocEntry toc[tocCount] [[comment("Table of contents entries")]];
};

// === File Parsing ===

BookBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

One file per spine item and per set of render settings (the file name is the property hash). Written and read by `lib/Epub/Epub/Section.cpp`. `SECTION_FILE_VERSION` at the top of that file is bumped on every layout change, so read it there; a number quoted here would go stale within weeks. A file with another version is rejected and rebuilt.

The file is, in order:

```text
header                  fixed size, offsets patched in when the build ends
pages                   page after page, each serialized by Page::serialize (lib/Epub/Epub/Page.h)
page LUT                u32 file offset per page
anchor map              u16 count, then (String id, u16 pageNumber) per entry
page break label map    u16 count, then (u16 pageIndex, String label) per entry
paragraph LUT           u16 count, then one 8-byte entry per page
```

`String` is a `u32` byte length followed by the UTF-8 bytes. All integers are little-endian.

### Header

These fields are the `header::*` constants in `Section.cpp`, in file order. The `static_assert` in `Section::writeSectionFileHeader` ties their sum to `header::kSize`.

| Field                  | Type    | Meaning                                                                 |
| ---------------------- | ------- | ----------------------------------------------------------------------- |
| `version`              | u8      | `SECTION_FILE_VERSION`                                                  |
| `fontId`               | s32     | Render settings: they also feed the property hash                       |
| `lineCompression`      | float   |                                                                         |
| `extraParagraphSpacing`| bool    |                                                                         |
| `paragraphAlignment`   | u8      |                                                                         |
| `viewportWidth`        | u16     |                                                                         |
| `viewportHeight`       | u16     |                                                                         |
| `hyphenationEnabled`   | bool    |                                                                         |
| `embeddedStyle`        | bool    |                                                                         |
| `bionicReadingEnabled` | bool    |                                                                         |
| `imageRendering`       | u8      |                                                                         |
| status                 | u8      | Bit flags: parse complete, and the "degraded" flags for a build that ran short of heap (image, table row, CSS) or hit a fixed capacity (simplified). The `kStatus*` constants in `Section.cpp` name them. Written as a bool by early builds, so a flag-less file still reads correctly |
| `pageCount`            | u16     |                                                                         |
| page LUT offset        | u32     |                                                                         |
| anchor map offset      | u32     |                                                                         |
| page break map offset  | u32     |                                                                         |
| paragraph LUT offset   | u32     |                                                                         |

### Paragraph LUT entry

One entry per page: `u32 visibleTextOffset + u16 paragraphIndex + u16 listItemIndex`.

- `visibleTextOffset` is the number of visible bytes (`lib/Epub/Epub/VisibleText.h`) of the chapter's source text before the page's first element. KOReader sync pushes it and resolves pulled positions to a page with it. Two consecutive pages may share one offset (an image or rule page followed by the text at that offset); a lookup answers the first of them.
- `paragraphIndex` is the 1-based `<p>` sibling index, matching KOReader's XPath `p[N]`.
- `listItemIndex` is the running `<li>` count, likewise for `li[N]`.

### Page break label map

Printed-page labels, from inline `doc-pagebreak` markers and from the per-book `pagelist.bin` below. See [epub-toc-navigation.md](epub-toc-navigation.md) for the source-format selection rules.

## `content.bin`

`content.bin` (magic `WBC1`) is the settings-independent product of the Stage-1 compile: parsed, CSS-resolved blocks keyed by the book's ZIP fingerprint. The layout is defined by the structs and the reader/writer in `lib/Epub/Epub/content/CompiledContent.h` and `CompiledContent.cpp`; read them for the fields. It is not part of the default section-cache path in this tree.

## `pagelist.bin`

Per-book cache file produced at index time from one of the EPUB printed-page sources (NCX `<pageList>`, EPUB 3 nav `<nav epub:type="page-list">`, or EPUB 2.01 `page-map.xml`). Consumed once per section build by `Section::createSectionFile`. Absent for books that have no printed-page data.

```text
u16 entryCount
struct PageListEntry {
    String href;    // normalised spine href, e.g. "OEBPS/c9_split_000.xhtml"
    String anchor;  // fragment id; empty means "start of file"
    String label;   // printed-page label, e.g. "42" or "iv"
}
PageListEntry entries[entryCount];
```

Selection rules (see `docs/epub-toc-navigation.md`):

- The EPUB 3 nav page-list parser runs first.
- The NCX `<pageList>` writer runs only if the nav writer produced nothing.
- The EPUB 2.01 `page-map.xml` writer runs only if `pagelist.bin` doesn't already exist on disk.
- Inline `doc-pagebreak` markers in XHTML are matched at chapter parse time and don't need the cache file; they coexist with whichever source above won.

## `library.bin`

`/.crosspoint/library/library.bin`: the book index the Library's New and Authors tabs read, built by
`LibraryBuilder` and read by `LibraryIndexReader` (`lib/LibraryIndex/`). The structs are in
`LibraryFormat.h`; the design is in [design/library-index.md](design/library-index.md). Written under a
temporary name and renamed into place. Little-endian, packed. A wrong magic or version, or sections
that do not fit the file, make it invalid, and a build replaces it.

| Section | Layout |
|---|---|
| Header, 48 B | magic `WLIB`; `u8` version (3); `u8` flags (bit 0: partial, the card held more than 2,000 books); `u8` acceptRules (the *Show Hidden Files* setting the walk used); `u8` reserved; `u32` buildGen; `u16` bookCount, authorCount, newCount; `u16` reserved; `u32` offsets of the five sections below; `u32` blob length; `u32` newestDate, the newest date among the books and the folders the walk listed (0 for none) |
| Records | bookCount × 24 B, **in identity order**: `u32` identity, authorHash, date (FAT `date << 16 \| time`), pathOff, sidecarSig, firstSeen |
| New | newCount (≤ 10) × `u16` record index, newest first |
| Authors | authorCount × 12 B, **in sort-key order**: `u32` hash, nameOff; `u16` firstBook, count |
| Author books | bookCount × `u16` record index; an author's books are the slots `[firstBook, firstBook + count)` |
| Blob | Strings, each a `u16` length then that many bytes: book paths; for each author its name as the books spell it, then its filing name ("Pratchett, Terry") |

Reserved author hashes: `0`, no author ("Unknown author"); `0xFFFFFFFF`, not known yet ("Not yet
indexed"). Reserved sidecar signatures: `0`, no sidecar; `0xFFFFFFFF`, a sidecar the walk could not
pair with its book. Version 1 stored an author's folded sort key where version 2 stores its filing name; version 3 adds
`newestDate`.

A build's working files sit beside it and are removed when the build finishes: `stage.bin` (16 B per
book: identity, date, sidecarSig, pathOff), `paths.bin` (the paths), `records.bin` (the joined records)
and `names.bin` (per author: `u32` hash, `u8` flags with bit 0 set when the filing name came from a
file-as, then the name and filing name as blob strings).
