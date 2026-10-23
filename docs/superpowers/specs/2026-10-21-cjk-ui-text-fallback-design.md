# CJK book text in the UI: the reader's font behind the UI faces: design

Status (2026-10-22): **Decided 2026-10-22: the maintainer answered the questions below; the implementation plan comes next.**

Working material, like every spec here: when the work merges, lasting parts move to reference docs (`docs/i18n.md`),
open items to `docs/future_work/cjk.md`, and this file is deleted.

Roadmap step 6 in `docs/future_work/cjk.md`. It builds on the CJK fonts spec
(`docs/design/cjk-ttf-fonts.md`), which set the UI fallback aside as its own design
(section 2 there), and needs its step 5, the CJK families as `.otf` downloads. Until those ship it can be built and
tested with a hand-installed `.otf` family, since CFF outlines have merged (#542, #544).

## Maintainer decisions (2026-10-22)

1. Home and the Library use the CJK family in the glyph-store set of the last opened book, reopened read-only on the screens that show book text (7.2). It is never installed or wiped from there, and there is no "UI font" setting. After a book read in a Latin family, Home shows boxes for CJK titles until a CJK book is opened again.
2. `.cpfont` CJK families are out of scope. Only TrueType and OTF families serve as the UI fallback (7.4).
3. Book text (titles, authors, file names, TOC rows, the status-bar title) draws wholly in the reader's font where it has the glyph. About ten call sites are wrapped in a scope that puts the reader's font first (7.1). UI strings keep the UI faces.
4. The X3 cost is acceptable: about 2-3 KB while a book-text screen is open, a transient 14.5 KB block (plus 4.2 KB in SD mode) during a pass that draws a glyph for the first time (7.8). The device runs measure it, and the heap gate in section 8 stays.

## 1. Intent and why

A CJK reader with a CJK font installed still cannot find or navigate their books:
- Book titles and authors on Home and in the Library, TOC entries, the status-bar chapter title, file names and the
  finished-book screen draw a box (□) for every character no translation uses.
  - On the X3 (2026-10-18, 紅樓夢), before the boxes, the Home card showed a lone 雪 as if it were the title.
  - The boxes come from the CJK UI faces' stand-in (`src/CjkUiFonts.cpp:64`).
- The UI faces hold only what the translations use: zh 599, ja 500 and ko 442 glyphs (`docs/i18n.md`).

The reader's own font has the characters, but outside the reader no SD font is loaded at all:
- `SdCardFontSystem::begin()` only discovers (`src/SdCardFontSystem.cpp:329-342`).
- Leaving the reader unloads (`src/activities/ActivityManager.cpp:313-316, 333-335, 383-386`).

Inside the reader the font is loaded, but nothing chains it behind the UI faces.

The intent: book text in the UI draws its CJK characters from the reader's CJK font, at the CJK UI sizes, with no
cost to readers who have no CJK font and no change to how the UI's own strings look.

## 2. Goals, non-goals, anti-goals

**Goals.**
- With a CJK `.otf` (or `.ttf`) family installed and used for reading, these draw their CJK characters in that
  family:
  - Home cards and the Library's rows: titles, authors, file names;
  - the TOC, the status-bar chapter title, and the finished-book screen;
  - book information and the bookmark lists, which draw the same strings.
- Sizes match the CJK UI faces: 10 pt behind the 8 and 10 pt UI faces, 12 pt behind 12 and 14 pt
  (`CjkUiFonts::attach`, `src/CjkUiFonts.cpp:257`), so a string mixing both sources keeps one size.
- Once a glyph has been drawn, a screen draws as fast as today or faster: no RAM residency to manage, nothing to
  prewarm per repaint.
- Readers without a CJK font pay nothing: no heap, no lookups, no flash writes.

**Non-goals.**
- A CJK keyboard.
- Bold CJK in bold UI text. It stays regular, as the UI faces are (`docs/i18n.md`); synthetic bold could serve it
  later.
- 14 pt CJK for the large UI step. It stays 12 pt, as today.
- Network screens (OPDS, the web server's sessions, Wi-Fi). They unload SD fonts for heap
  (`NetworkMemoryTrim.cpp:38`, `ServerSession.cpp:24`), and keep today's boxes.
- Scripts other than CJK outside the reader (Greek, Armenian; upstream #3484). Inside the reader the chained font
  answers them too (7.1), at no extra cost.
- `.cpfont` families (decision 2).
- The UI faces' interval tables, the duplicate Han glyphs, and reviewing the machine translations
  (`docs/future_work/cjk.md`, "Smaller items"): independent of this.

**Anti-goals.** The work fails, even if it runs, if:
- a UI screen installs, erases or wipes the glyph store, or makes room in it;
- any UI frame changes for a reader with no CJK font, or the UI language's own strings render differently from today;
- a reader without a CJK family loses heap or page-turn time;
- a glyph record or bitmap of a closed, suspended or resized set is served (a dangling pointer);
- TrueType code runs off `RenderLock` (TTF spec section 3.4);
- a measured width differs from the drawn width, so truncation overflows;
- a font file is deleted or rewritten while the UI holds it open: USB mass storage, the web server, the font
  manager, `CMD:FS:RM`;
- a character the UI faces draw today draws as a box.

## 3. Constraints

### 3.1 Heap, per board

- **X3 (ESP32-C3, no PSRAM).** About 258 KB of heap. Two framebuffers of 52,272 B each.
  - While reading: about 30-41 KB free, 17-27 KB contiguous (`docs/memory-allocation-strategy.md:44`).
  - Home: earlier measurements put it near 53-56 KB free (X4, covers cached). The X3 is unmeasured for this work, so
    section 8 measures it first.
- **X4 (ESP32-C3).** The same constraint, with 48,000 B framebuffers.
- **X4 Pro and LilyGo T5S3 (ESP32-S3, PSRAM).** Allocations over 4 KB land in PSRAM by default. Heap is not the
  binding constraint there.

What the reader's TrueType path costs (TTF spec `2026-10-13-ttf-fonts-design.md` section 6.3, measured on the X3):
- **At rest:** 960 B per set, about 0.5-1.7 KB per family, and 20 B plus 132 B per style for each further face (a
  size).
- **Only during a pass that draws a glyph the store lacks:**
  - the scratch block, 14,528 B for a CJK family;
  - in SD mode, a 4.2 KB page cache.

  Both are freed when `RenderLock` is given back (`RenderLock::setReleaseHook`, `SdCardFontSystem.cpp:335`).
- **The ring:** up to 8 KB (`TtfFontSet::kRingSlots`), used only while the store is full.

### 3.2 Flash

- **The C3 app image** is over 90 % of its OTA slot. A change must end flash-negative or name what it buys
  (`docs/future_work/library.md`). Approach A adds code only, estimated 2-4 KB.
- **The glyph store** lives in the `spiffs` partition with the language pack slot (last 128 KB) and the `.cpfont`
  cache.
  - Its log holds 1.44-1.64 MB, limited by free MMU pages. A CJK glyph takes about 150 B at 12 pt (CJK spec,
    Appendix A).
  - The face table holds 32 faces (a family at a size), because keys carry a 5-bit face index
    (`GlyphStoreFormat.h:114-119`). A full face table leaves a wipe pending for the next book open.
  - UI glyphs share the log with the reader's.

### 3.3 Render time on list screens

- **Every repaint draws every row.** Upstream measured a CJK TOC row step at 3.2 s when each repaint re-read its
  glyphs from the card (#3071).
- **Today's cost for an English-UI reader.** CJK titles already draw the characters found in the zh, ja and ko faces.
  Those faces are compressed and decode per glyph, ~1.9 ms each on the X3 (`docs/i18n.md`), on every draw.
- **Drawing a glyph the store lacks** costs about 0.7-1 ms of CPU on the C3: 670 µs for a dense 12 pt CJK glyph as
  TrueType (CJK spec section 7.3), and CFF within a few percent of it since #544. In SD mode there is also one read
  from the card.

### 3.4 Concurrency and lifetimes

- **`RenderLock`.** Every TrueType lookup, render and store write runs under it (`TtfFont.cpp:69-75` logs a
  violation). UI screens build and draw inside `render()` on the render task, under the lock
  (`ActivityManager.cpp:135-162`). Any loop-side measuring of book text has to move there or stay off the chained
  font.
- **The set's faces come and go:**
  - `dropTtfNativeSizes()` at book open and at a body-size change (`SdCardFontManager.cpp:361-374`);
  - `closeTtfSet()` at unload (`:407-414`);
  - `suspendTtf()` before image decodes, the memory ladder (`EpubReaderActivity.cpp:2281`);
  - a wipe moves the log (`TtfFontSet::noteWiped` rebases every size).

  Anything the UI chain holds must be detached and reattached around each of these.
- **`onExit()` runs under `RenderLock`.** Never take a second lock there (`ActivityManager::exitActivity`).

## 4. What exists here

- **The UI chain.** Each of the seven UI faces is a RAM copy of its `EpdFontData` with a provider
  (`CjkUiFonts::attach`, `main.cpp:749-755`).
  - There are two `FallbackFaceProvider`s, one per CJK size. Each searches the three CJK UI faces, the UI language's
    first, then serves the box for a CJK character none has (`FallbackFaceProvider.cpp:17-47`,
    `CjkUiFonts.cpp:121-136`).
  - A served glyph is a copy in an 8-slot ring. `ownsBitmap()` finds its bitmap by the record's address
    (`FallbackFaceProvider.cpp:49-69`).
  - Every text path (`drawText`, `getTextWidth`, `truncatedText`, `wrappedText`, the scaled and rotated variants)
    reaches it through `EpdFont::getGlyph`. None of them knows it exists.
- **Depth.** The renderer draws a glyph at the asking font's depth (`GfxRenderer.cpp:938`). The UI faces are 1-bit,
  and `attach()` refuses 2-bit ones (`CjkUiFonts.cpp:261`).
  - A 2-bit glyph drawn in black and white draws every non-zero level black (`drawMaskFor2BitMode`,
    `GfxRenderer.cpp:695`). That is its ink mask.
  - `packInkMask()` already packs 2-bit glyphs to that 1-bit mask for #500's pre-render (`SdCardFont.cpp:1278`).
- **TrueType and CFF families.** They are drawn by the TTF engine from the glyph store, always 2-bit
  (`TtfFont.cpp:733`).
  - **Lookups.** A lookup is a hash probe in mapped flash. A glyph not stored is rendered and committed then, in the
    lookup (`TtfFont::glyph`, `TtfFont.cpp:861-879`).
  - **Prefetch.** `prefetch()` renders up to 64 missing glyphs in one batch, in glyph order, so SD-mode reads move
    forward through the file (`TtfFont.cpp:969-1016`).
  - **Use counts.** A draw counts as a use for the store's reclaims in `TtfFontSet::ownsBitmap`
    (`TtfFont.cpp:595-596`).
  - **Sizes.** A size is a face `(family, ppem)`. `ppemFromPoints()` uses 150 DPI (`TtfFace.h:70`), as the UI faces'
    converter does (`fontconvert.py:333`), so 12 pt UI and a 12 pt body are the same face and share their glyphs.
- **The store's set.** It holds one set of families at a time, under the set's identity. Opening another set
  installs it: erase, copy, seconds.
  - `TtfFontSet::Install::Never` opens a store only when it already holds the set; `resumeTtf()` uses it.
- **`.cpfont` families.** These are `SdCardFont`.
  - **A miss** opens the file, reads the glyph and allocates its bitmap on the heap, into an 8-slot ring
    (`SdCardFont.cpp:1913-2025`, open at `:1961`).
  - **The interval table** is copied to the heap even from the flash cache (`:1016`). For the Japanese set that is
    49 KB (CJK spec section 5).
  - **Sizes.** A family is loaded at one size, the reading size.

## 5. How upstream solved it

Upstream is crosspoint-reader, read at its merge commits (`git fetch upstream`).

| PR | What it does | Fits here | Does not fit |
|---|---|---|---|
| #2521 (Tsz Ho Sze, 10db60c4f) | Routes a whole string to an SD fallback font when it holds a CJK character the UI font lacks: `resolveTextFontId()` in `getTextWidth`, `drawText`, `getTextAdvanceX`, `drawTextRotated90CW`. Coverage is checked without card I/O (`hasCodepoint`, `coverageHandler`). Loads the family's `.cpfont` files at 8, 10 and 12 pt as extra sizes; a CJK probe (一 あ ア 가) skips Latin families. | Size matching; no card I/O to decide; the CJK probe. | Our UI fonts already fall back per glyph, and the box stand-in makes every CJK character "drawable" to a routing test. Routing needs every text entry point, and we have about ten (`GfxRenderer.cpp:1390-1631, 2942-2976, 3211, 3315`). A mixed title draws its Latin in the CJK font. Extra CJK `.cpfont` sizes cost ~49 KB of interval table each on the C3. |
| #3026 (Justin Mitchell, 6cfc55b6f) | Prewarms each redirected string into the SD font's page tables before measuring or drawing. | Batch before the per-glyph loop. | A TrueType store needs no RAM residency (7.6). |
| #3071 (Justin Mitchell, a0daab994) | Prewarms by union instead of replacing; one batch per screen through a getter (no concatenated `std::string`, which aborts on OOM); no kern tables for UI strings; a 24-row TOC window instead of all 547 rows (~60 KB); centres the fallback line box in the requested font's. Measured: row step 3.2 s → 0.8 s, TOC heap 10.7 → 33 KB free. | The getter-based per-screen batch, as a prefetch (7.6). No kerning for UI CJK. | Our TOC already reads only the visible rows (`EpubReaderChapterSelectionActivity.cpp:102-116`). Per-glyph fallback keeps the UI face's baseline, so no line-box centring. |
| #3484 (YUN-JIE, fdf5f76c0) | Redirects for any codepoint above ASCII the UI font lacks, not only CJK. | The chained font answers any codepoint the UI faces lack (7.1). | |
| #3646 (Justin Mitchell, aaf683798) | TrueType through FreeType with RAM glyph caches. UI sizes as separate 2-bit fonts with 16 KB caches each, loaded only with PSRAM or 160 KB internal heap free: in effect, PSRAM boards only. | | Our glyphs live in the flash glyph store. A UI size costs ~150 B of RAM, so the C3 can afford it (7.8). |

## 6. Approaches

**A. Glyph-level: the reader's font as one more source of the UI faces' fallback (recommended).**
`FallbackFaceProvider` gains a chained source: the reader's TrueType/CFF family at the CJK UI size, read through its
own provider, its 2-bit glyphs packed to 1-bit on the way. The renderer, every text path and every call site stay
as they are. Glyphs persist in the glyph store, so a screen pays for a glyph once per card, not per repaint.

**B. String-level: port upstream (#2521 + #3026 + #3071 + #3484).**
`resolveTextFontId()` in every text entry point routes a string to the reader's font, registered at UI sizes in the
font map, drawn at its own depth. `.cpfont` sizes get per-screen prewarms into their page tables.

**C. A read-only view of the store.**
Titles, authors and TOC entries are rendered into the store while the reader has its set loaded (at book open, and
by the Library's index build). Outside the reader, the UI reads them from the store's mapping without opening the
font files: nothing is open that a card change could hit.

| | A | B | C |
|---|---|---|---|
| Renderer changes | none | about ten text entry points, and every future one | none |
| Call sites | one prefetch per list screen; ~10 wrapped (decision 3) | none for routing; a prewarm per list screen | the book open and index build paths |
| Mixed title (三体 Vol.1) | Latin in Inter, CJK in the reader's font | the whole string in the reader's font | as A |
| Strings covered by the UI faces | drawn from them (decision 3) | drawn from them, so rows differ in font by coverage | as A |
| Depth | packed to 1-bit in the provider | drawn 2-bit, as ink in black and white | as A |
| Titles never seen with the font loaded | drawn on first sight | drawn on first sight | boxes until rendered |
| Heap outside the reader (C3) | ~2-3 KB while the screen is open; a transient block on first sight | the same for TrueType; ~49 KB+ per size for `.cpfont` | under 1 KB |
| `.cpfont` | not served (decision 2) | the upstream path, PSRAM boards only | not served |
| New risk | lifetimes of the chained faces (3.4) | text paths that disagree on routing | a store view with no font behind it: identity, reclaims |

**Recommended: A** (my call).
- **No text path changes.** It uses the mechanism already in place for the CJK UI faces, so measuring and drawing
  cannot disagree.
- **No new lifetime or caching subsystem.** The store already persists glyphs across boots, and a store hit costs
  less than today's per-glyph decode of the non-active UI faces.
- **The C3 can afford it.** A UI size is a face of the loaded family (~150 B), not a second font with its own cache.

B buys whole-string uniformity, which A gets for book text through decision 3. C saves ~2 KB on Home, but cannot
draw a title it has not seen. It also adds a third way of opening the store, and the store's identity and reclaim
rules would have to hold without a set behind it.

## 7. The recommended design (A)

### 7.1 The lookup chain

For a codepoint the UI face lacks, the provider at the face's CJK size asks, first match wins (my call):
1. the UI language's CJK face, when the UI language is Chinese, Japanese or Korean;
2. the chained reader font, at the same CJK size;
3. the other CJK UI faces;
4. the box, for a CJK character (`isCjkCharacter`, `CjkUiFonts.cpp:69`).

**Why the chained font before the other faces.** The non-active faces decode per glyph (~1.9 ms on the X3). A store
hit costs microseconds. An English-UI reader's titles also come out of one font this way.

**Book text (decision 3).** While a `BookTextScope` (name the builder's) is held, the chained font comes first. A
static counter is enough: it is used on the render task under `RenderLock` only. The scope wraps measuring and
drawing together:
- the status-bar title (`BaseTheme.cpp:1260-1287`);
- Home's book card title and author;
- the Library's row labels, and the file names;
- the TOC rows (`EpubReaderChapterSelectionActivity.cpp:102-116`);
- the finished-book screen's title and author;
- `BookInfoActivity`;
- the bookmark and reading-stats lists.

The builder lists them from the code. A call site missed keeps order (a), which still draws.

**Other scripts.** The chained font answers any codepoint the UI faces lack, not only CJK (as upstream #3484). The box
stays CJK-only, as today. The UI set outside the reader opens only for a family that covers CJK (7.2), so this
reaches Home only incidentally.

### 7.2 Which font, and when it is open

**In the reader** (TOC, status bar, finished-book screen pushed over the reader). This is the reader's loaded set:
- The chained source is the first family of the set whose cmap covers a CJK probe: 一 U+4E00, あ U+3042, 가 U+AC00
  (upstream #2521's probe). Under Publisher Fonts, a book's embedded CJK subset may cover only that book: other
  characters fall to the faces and the box, as for any partial font.
- It is attached after every `loadTtf`, `resumeTtf` and `setTtfBodySize`. It is detached before every
  `dropTtfNativeSizes`, `closeTtfSet` and `suspendTtf`. Detached, it draws boxes: the status bar during an image
  decode's suspension, for one pass.

**Outside the reader** (decision 1). The screens that draw book text open the set on entry and close it on exit,
through a small RAII member, `UiBookFonts` (name the builder's): Home, the Library, the finished-book screen opened
from the Library, book information, and the bookmark and reading-stats lists. My call, for three reasons:
- the set is closed by default, so the Font Manager, USB mass storage, the web server, the serial API's `CMD:FS:RM`
  and the font selector never meet an open font file;
- nothing else on those paths has to remember to close it;
- the cost is an open per screen entry, measured in section 8.

**What it opens.** It opens the set that `ensureSdFontLoadedForPath(APP_STATE.openEpubPath)` would load
(`main.cpp:805-856`): the book's family override or the global family, and its Publisher Fonts families.
- That code is refactored to compute the list without loading, and the list is loaded with `Install::Never`, as
  `resumeTtf()` loads (`SdCardFontManager::resumeTtf`).
- When the store does not hold that set, it is not opened: boxes, as today. A UI screen never installs, wipes or
  makes room.
- When no family of the set covers CJK, it is closed again at once. A Latin reader pays one open per Home entry and
  holds nothing; the builder can cache "this set has no CJK family" per set identity to skip even that.
- `.cpfont` families are skipped (7.4).

**What it keeps.**
- Only the faces the UI uses: two UI sizes for the CJK family.
- None of the reader's state: no native sizes, no use counts. Counts are allocated only through `makeRoom()`, which
  UI screens never call. A store that has none keeps the reclaim's first-in-first-out fallback (CJK spec 8.2).

### 7.3 From a TrueType or CFF family

1. **The UI sizes.** `TtfFont` gains two UI sizes, made on first use at `ppemFromPoints(10)` and
   `ppemFromPoints(12)` and regular only (my call). They are kept apart from `native_`, so `dropNativeSizes()`
   leaves them, and they are freed with the font. `noteWiped()` rebases them with the other sizes. A 12 pt UI size
   is the 12 pt body's face, so their glyphs are shared.
2. **Lookups.** `FallbackFaceProvider::serve()` asks the chained `EpdFontData` through `EpdFont::findGlyph()`, which
   goes to the size's `StyleProvider`:
   - a store hit is a pointer into the mapped log;
   - a miss renders, commits and returns;
   - a glyph the font lacks gets the store's "no glyph" marker once, then costs a probe.
3. **The served record.** It copies the metrics into the provider's ring as now. It also keeps the source record's
   pointer and the chained `EpdFontData`, so `ownsBitmap()` can find the bitmap later. The ring slot grows by one
   pointer.
4. **Drawing.** `ownsBitmap()`, called only for a glyph being drawn, gets the 2-bit bitmap:
   - from the source provider's `ownsBitmap()` for a ring glyph;
   - from `bitmap + dataOffset` for a stored one.

   It packs the bitmap to 1-bit (7.5) and returns that. Calling the source's `ownsBitmap()` here is also what counts
   the draw as a use for reclaims.
5. **Prefetch.** `FallbackFaceProvider::prefetch(text)` passes on to the chained size's `prefetch()` only the
   codepoints that sources 1 and 3 lack. That keeps Latin and the UI faces' characters out of the store. A list
   screen calls it once per build over its visible strings (7.6).
6. **No memory, or the lock not held.** A miss whose scratch block cannot be had returns nothing, and is counted
   (`glyph_miss::starvedCount`). A lookup off `RenderLock` returns nothing. Either way the provider serves the box.
   Nothing caches that box beyond the pass: the scaled-glyph mask cache is the reader's only, and the provider's
   ring is per lookup.

### 7.4 From a `.cpfont` family

Not served (decision 2). The reasons, from the code:
- **The size.** A `.cpfont` family is loaded at one size, the reading size. Serving the UI means its 10 and 12 pt
  files as extra `SdCardFont`s, and only a family that ships them.
- **The heap.** Each costs its interval table on the heap, from the card or from the flash cache (`SdCardFont.cpp:857,
  1016`): about 49 KB for a Japanese set. On top come its kern classes and its page arena. The C3 has no room for
  that outside the reader (3.1).
- **The misses.** Each miss is a file open, two seeks and a heap bitmap (`:1961-1999`). A per-glyph chain would
  repeat that every repaint unless screens prewarm per string, which is upstream's #3026 and #3071 machinery.
- **The future.** The CJK bitmap families leave the font manager in step 5 (CJK spec section 5).

Rejected (decision 2): a PSRAM-boards-only port of upstream, which would have:
- the family's 10 and 12 pt files loaded as extra sizes;
- `prewarm()` taking a union of codepoints;
- a per-screen batch through a getter, without kern tables;
- the same chained source and the same 1-bit packing.

A flash-cached family could serve glyphs and bitmaps in place without SD reads (`bitmapsFromMmap`), once the interval
table is read in place too (`spike/mmap-intervals-in-flash`). The font area holds either the `.cpfont` cache or the
glyph store, not both.

### 7.5 Depth conversion

**Where.** In the provider, into one static 256 B buffer (my call).
- The renderer draws a glyph's bitmap before it resolves the next glyph (`FallbackFaceProvider.h:61-64`), so one buffer
  serves both providers.
- A stack buffer cannot do it: the pointer outlives `ownsBitmap()`.
- 256 B holds a 1-bit glyph up to 45 × 45 px. A CJK glyph at 12 pt is about 25 × 25 px (≈ 80 B).
- A larger glyph is served as the box and logged once.
- The served record says 1-bit: `dataLength = glyphDataBytes(w, h, false)`.

Both streams are continuous bit streams with no row stride (`EpdFontData.h:91`), so `packInkMask()` moves from
`SdCardFont.cpp` to a shared header and is reused as is.

**Threshold: ink, level ≥ 1, by default (my call).**
- It is exactly what the panel shows for 2-bit text in a black-and-white pass, and what #500's pre-render draws.
- It never drops a thin stroke. That matters for serif families, whose horizontals are under a pixel at 10-12 pt.
- **Against the UI faces:** they threshold hinted coverage at 40 % (`fontconvert.py:29`). The engine's levels cut at
  25, 50 and 75 % (`TtfRasterizer.cpp:77-78`). So ink (≥ 25 %) is a little heavier than the faces, and level ≥ 2
  (≥ 50 %) a little lighter.
- **The choice is one constant,** made from device screenshots (section 8).

**If neither matches the faces,** the fallback is a 1-bit render mode in the engine at the converter's 40 %: a second
span packer, and a depth bit in the face key. It halves the UI records in the store, but the 12 pt UI face no longer
shares the body's glyphs.

**Rejected: a per-glyph depth in the renderer,** so that a 1-bit font could carry 2-bit glyphs. It would touch
`renderCharImpl`, the scaled emitters and the AA capture, for one provider's sake.

### 7.6 Prewarm and caching; the cost on Home, the Library and the TOC

**No prewarm is needed for correctness.** A lookup that misses renders the glyph into the store, where it stays across
repaints and boots. The upstream questions dissolve for the TrueType path:
- "prewarm per string": a lookup is its own prewarm;
- "bound it for lists": only drawn rows are looked up, and nothing is held in RAM.

**A prefetch is still worth one call per list screen build** (my call):
- **What it does.** Over the visible strings, through a getter (no concatenated `std::string`, as upstream #3071
  learned), it reaches `FallbackFaceProvider::prefetch()` through `EpdFontFamily::prefetch()`.
- **Why.** It batches a screen's first-sight glyphs: one scratch block, one commit, and SD-mode reads in file order.
  Per glyph, the reads would seek backwards; a backward seek costs up to 4.5 ms on a large file (TTF spec 5.1).
- **Its bound.** Up to 64 glyphs per call (`kPrefetchGlyphs`). The rest render on lookup.
- **Where.**
  - **Home:** the cards on screen.
  - **The Library:** the page's rows.
  - **The TOC:** the page's rows, which it already reads one by one in `render()`.
  - **The status bar:** the title, inside the page's render.

**Estimated costs, measured in section 8:**

| Screen | First sight of its glyphs | After |
|---|---|---|
| Home (a few cards, ~30-60 CJK glyphs) | ~30-150 ms more, once per glyph per card | a probe and a pack per glyph: well under 1 ms a screen; faster than today's ~1.9 ms per glyph decode for an English UI |
| Library page (~10 rows, ~100-150 glyphs) | ~0.1-0.5 s more, once | as above |
| TOC page (~15 rows of 10-20 characters) | ~0.1-0.4 s more per page first seen; at a 12 pt body nothing is shared, since TOC rows are 10 pt UI (`UI_10_FONT_ID`) | as above |
| Status bar | one title's glyphs once per chapter | as above |

**Store growth.** A glyph at 21 px is about 130 B with its header, at 25 px about 175 B. A library of a few hundred
CJK books might add 1,000-3,000 records, 0.2-0.5 MB of a 1.44-1.64 MB log. Reclaims keep what is drawn, and UI
glyphs drawn on every Home visit count as uses. Measured in section 8.

**A full store** (ring mode) re-renders UI glyphs on each draw until the next book open makes room. That is correct
but slow, and accepted (my call): UI screens never make room.

### 7.7 No CJK font installed

Nothing changes:
- no set opens outside the reader (no CJK family, or no TrueType set at all), and no chained source is attached in
  the reader;
- the chain is today's: UI faces, then the box.

Byte-identical frames are an anti-goal check (section 8).

### 7.8 The X3's heap

**Outside the reader, while a book-text screen is open** (estimated from the TTF spec's section 6.3):
- the set, 960 B;
- one regular-only CJK family: ~0.8 KB with its kerning reader;
- two UI faces: ~0.3 KB;
- the `NorFlash` object and, in SD mode, one open file.

About 2-3 KB in all, freed on exit. Publisher Fonts accent families of the last book add ~0.5-1 KB each.

**During a pass that draws a first-sight glyph:** the scratch block (14,528 B, one block) and in SD mode the page cache
(4.2 KB). If no block that large is free, the engine first frees the built-in fonts' page caches
(`TtfFont::Hooks::reclaim`); failing that, the glyph is the box for this pass and is asked again next time.

**Static RAM:** ~320 B: the 256 B buffer, and a pointer more per ring slot in two providers.

**Rejected (decision 4):** drawing the prefetch's misses in the lent secondary framebuffer through
`glyph_miss::Scope`, as the reader's builds and the font selector's previews do (`FontSelectionActivity.cpp:92`).
That costs X4 fast refreshes their baseline (`setSingleBufferFastDiff` while it is lent) and is more work.

**In the reader** the set is loaded already. The UI sizes add ~0.3 KB. A TOC pass with first-sight glyphs needs the
same transient block the reader's own misses need.

### 7.9 Code it touches

- **`lib/EpdFont/FallbackFaceProvider.{h,cpp}`:** the chained source and its place in the order, the book-text
  order, the source record in a ring slot, packing in `ownsBitmap()`, a filtered `prefetch()`, the box on
  starvation.
- **`lib/EpdFont/SdCardFont.cpp`:** `packInkMask()` moves to a shared header.
- **`lib/EpdFont/ttf/TtfFont.{h,cpp}`:**
  - two UI sizes kept out of `dropNativeSizes()` and rebased after a wipe;
  - `covers(codepoint)`, a cmap probe that renders nothing.
- **`lib/EpdFont/SdCardFontManager.{h,cpp}`:**
  - which family covers CJK;
  - a load with `Install::Never` for UI use;
  - attach and detach around load, resume, body size, drop, close and suspend.
- **`src/SdCardFontSystem.{h,cpp}`, `src/main.cpp`:**
  - the set list computed without loading, refactored out of `ensureSdFontLoadedForPath()`;
  - opening and closing the UI set.
- **`src/CjkUiFonts.{h,cpp}`:** taking the chained source per CJK size; `orderFaces()` with it; the book-text
  scope.
- **The activities that draw book text:** a `UiBookFonts` member; one prefetch per list build; the scope around book
  text (decision 3).
- **`SerialTestApi`:** the UI source in `CMD:STATE` (family, open or not, starved count) so device runs can see it.
- **Tests:**
  - `test/fallback_face_provider/`: a fake provider serving 2-bit records; order with and without the scope;
    packing; record identity; the box on starvation;
  - TTF host tests: UI sizes survive `dropNativeSizes()` and are rebased after a wipe.
- **Docs:**
  - `docs/i18n.md`, the CJK section's "Not covered";
  - `docs/future_work/cjk.md`, step 6 and its smaller items.

## 8. What to measure on the devices

On the X3 and the X4, through the serial test API (`CMD:STATE`, `CMD:TTF`, `CMD:SCREENSHOT`), with a CJK `.otf`
family in SD mode and one in flash mode where one fits:
- **Heap first.**
  - Free and largest block on Home, the Library and the TOC (a CJK book), before the work and with it.
  - The lowest free heap during a first-sight pass on Home and the Library.
  - Starved glyphs (`glyph_miss::starvedCount`).
  - The heap gate (decision 4): if an open book-text screen holds more than the 2-3 KB, or a first-sight pass needs more than the transient block of 7.8, stop and report before the work goes on.
- **Open time** of the UI set on Home entry with `Install::Never`, and Home's entry time with and without it.
- **First sight and after.**
  - Render-pass time on Home, a Library page and a TOC page, first and second visit.
  - Glyphs rendered (`TtfFont::renders()`) and SD pages read (`TtfFontSet::sdPageReads()`).
  - The same without the prefetch, to show it pays.
- **The reader.** Page-turn time with a CJK chapter title in the status bar, against master: no change once a
  chapter's title is stored.
- **Store growth.**
  - Log bytes added by browsing a CJK library of 100+ books, and by a 300-entry TOC (`CMD:TTF`).
  - The face count used.
- **Weight and shape.** Screenshots of mixed titles: UI-face glyphs beside chained ones, ink against level ≥ 2, at
  10 and 12 pt, sans and serif. Include 鬱 書 慶 議 驚, and the kana a translation uses. The maintainer does not read
  CJK, so Claude reviews them for stroke weight, counters and matching forms.
- **No regression.**
  - English UI with no CJK font: byte-identical screenshots of Home, the Library and the language picker.
  - The CJK UIs' own screens unchanged.
  - A Latin reader's Home heap and page turns unchanged.
- **Lifetimes.**
  - Open a CJK book, its TOC, an image page (the ladder's suspension), back to Home, the font selector, USB mass
    storage, and the Font Manager delete of the family in use.
  - No stale glyph, no crash, no open file during a card change.
  - A full store (`CMD:TTF:FULL`) on Home: correct, and how slow.

## 9. Left to the builder

- The names of the scope, the RAII member and the new accessors.
- How the UI sizes sit in `TtfFont`, within 7.3's rules.
- Whether to cache "this set has no CJK family" per set identity (7.2).
- The list of book-text call sites (7.1), from the code.
- How the filtered prefetch builds its text: a stack buffer of at most 256 B, or a codepoint-list overload of
  `TtfFont::prefetch()`.

## 10. Not in this round

- Bold CJK in bold UI text, from the reader's font (synthetic bold at UI sizes). Mixed with the regular UI faces it
  would need decision 3's scope to look right.
- 14 pt CJK behind the 14 pt UI faces, from the reader's font. The same mixing applies.
- Non-CJK scripts outside the reader (upstream #3484's full reach): a family covering only them opens no UI set.
- Network screens.
- `.cpfont` (decision 2).
