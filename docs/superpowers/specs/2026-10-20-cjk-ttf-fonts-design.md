# CJK TrueType fonts for readers, and synthetic bold: design

Status (2026-10-20): draft for review.

Working material, like every spec here: when the work merges, lasting parts move to reference docs, open
items to `docs/future_work/cjk.md`, and this file is deleted.

## 1. Why

A Chinese, Japanese or Korean reader has no good font to install today.

- **What the font manager offers.** Three CJK bitmap families from `assets/sd-fonts`: NotoSansCJK and
  NotoSerifCJK at 10 and 12 pt, NotoSansHangul at 10–16 pt. `sd-fonts.yaml` comments them out, but
  `scripts/generate-font-manifest.py` lists every `.cpfont` it finds in the folder, so the files from May are
  still in `fonts.json`.
- **How they perform.** NotoSansCJK 12 pt is 3.61 MB, larger than the 3.375 MB font partition, so it is read
  from the SD card. On the X4 a cold chapter build took 940 ms to lay out, the free heap fell to 5.4 KB, and
  glyph preparation took 104 ms a page (Appendix A).
- **Why bitmaps cannot be fixed by subsetting.** A per-language bitmap subset fits the partition at one or two
  sizes, but a family for every reading size does not: Japanese at 10–18 pt is 7.64 MB.

TrueType families too large for flash mode load from the SD card since #506 ("SD mode": the tables are
copied into the flash font partition, outlines are read from the card, rendered glyphs are kept in the
glyph store). The spike of 2026-10-19/20 measured that path on the X4 in all four languages. Once a chapter's
glyphs are stored, every language behaves alike: layout 156–497 ms, lowest free heap 21.5 KB, glyph
preparation 6–7 ms a page. One file serves every size. What is missing is a font to install.

## 2. Decided

By the maintainer, in conversation on 2026-10-20:
- **Per-language TrueType subsets of Noto CJK**, kept in `assets/sd-fonts` and downloadable from the font
  manager, both on the web page and on the device. The audience is Asian readers who need their books to
  render properly.
- **Sans and serif, as separate families:** eight in all.
- **Regular only.** No Bold files: they would double each download and the repository growth.
- **Synthetic bold** for every TrueType family that has no Bold of its own: the new CJK families, Latin
  families, and fonts embedded in books.
- **The three CJK bitmap families are removed** from `assets/sd-fonts` and from the manifest.
- **The UI fallback comes later, as its own design.** Book titles, TOC entries and the status line will
  fall back to the installed CJK font for characters the UI faces lack. That design builds on these fonts.

## 3. Goals, non-goals, anti-goals

**Goals.**
- A reader installs the font for their language from the device or the web page in one step and reads CJK
  books with it, in sans or serif.
- Bold text in a regular-only TrueType family looks bold.
- Reading performance as measured in the spike (Appendix A).

**Non-goals.**
- The UI fallback (section 2).
- Bold or italic files; synthetic italic or oblique. CJK typesetting does not use italics.
- Loading CFF (`.otf`) outlines. If that lands later, the subsets could ship as OTF, about 30% smaller.
- Vertical text and vertical glyph forms.
- Selective eviction from the glyph store (section 8.3).
- One family for all three languages. Glyph shapes differ by region (骨 is drawn differently in Chinese and
  Japanese), and per-language files are smaller.
- Fixing the CJK bitmap-path bugs the spike found (section 5). They no longer affect a hosted family.

**Anti-goals.** The work fails, even if it runs, if:
- an interrupted download leaves a family that the font selector lists but cannot draw;
- an older firmware's font manager stops working, or offers a TrueType family it cannot install
  (section 6.2);
- synthetic bold fills the counters of dense characters at reading sizes (a character like 鬱 at 12 pt);
- any regular glyph, or any glyph of a family with a real Bold, renders differently;
- floating point reaches the font engine (`scripts/check_no_float.py`);
- rebuilding the fonts without a change to their inputs rewrites their bytes and grows the repository.

## 4. The fonts

### 4.1 Families and character sets

| Family | Source (notofonts/noto-cjk, region OTF) | Character set |
|---|---|---|
| NotoSansJP / NotoSerifJP | `Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf`, `Serif/OTF/Japanese/NotoSerifCJKjp-Regular.otf` | JIS X 0208, the NEC special characters of cp932 row 13 (①–⑳, Ⅰ–Ⅹ, ㈱ …), Jōyō and Jinmeiyō kanji |
| NotoSansSC / NotoSerifSC | `…/SimplifiedChinese/Noto{Sans,Serif}CJKsc-Regular.otf` | GB2312, plus the Table of General Standard Chinese Characters |
| NotoSansTC / NotoSerifTC | `…/TraditionalChinese/Noto{Sans,Serif}CJKtc-Regular.otf` | Big5 and cp950, all of it |
| NotoSansKR / NotoSerifKR | `…/Korean/Noto{Sans,Serif}CJKkr-Regular.otf` | all 11,172 Hangul syllables, compatibility jamo, and all of KS X 1001: Hanja, symbol rows (①, ㉠, ㈜), Greek, Cyrillic, box drawing, kana |

Every family also carries the characters CJK books mix in:
- ASCII and Latin-1;
- general punctuation, U+2010–2027 and U+2030–205E;
- CJK symbols and punctuation, U+3000–303F;
- full-width and half-width forms, U+FF01–FF9F;
- U+FFFD, which the renderer falls back to.

**Sizes, sans, measured in the spike** (before the Jōyō/Jinmeiyō and General Standard additions):

| Family | Characters | TrueType size |
|---|--:|--:|
| NotoSansJP | 7,164 | 5.09 MB |
| NotoSansSC | 7,724 | 4.13 MB |
| NotoSansTC | 13,991 | 9.91 MB |
| NotoSansKR | 16,577 | 5.55 MB |

The serif families are not measured yet. Serif outlines carry more points, so expect them somewhat larger:
about 50–55 MB for all eight families. The build records the real sizes.

**Why the additions (my call).** Two of the national sets miss characters modern books use:
- JIS X 0208 (1983) predates the 2010 Jōyō revision, which added characters such as 𠮟, 塡, 剝 and 頰.
- GB2312 (1980) misses characters the 2013 Table of General Standard Chinese Characters (通用规范汉字表,
  8,105 characters) includes.

Unihan's `kJoyoKanji`, `kJinmeiyoKanji` and `kTGH` fields list them. Unihan is Unicode data with a permissive
licence, so the build can download it the same way it downloads the font sources. Big5 and KS X 1001 Hanja
need no addition.

**The symbol rows (my call).** The national standards' symbol rows (circled numbers, Roman numerals,
parenthesized Hangul, the Japanese NEC row) cost a few hundred glyphs and appear in modern books, so every set
keeps them. The spike's Korean set had kept only the Hanja rows.

**Left out (my call): Hong Kong characters (HKSCS).** They are about 4,700 characters more, roughly 3 MB on
the Traditional Chinese families. Hong Kong Cantonese text (嘅, 咗) draws as U+FFFD. If readers ask, a
separate Hong Kong family would add them later.

**The sets are final before the first commit.** The fonts are committed binaries. Every later change to a set
adds another 4–13 MB a family to the repository's history, and installed readers have to download again.

A character outside the set draws as U+FFFD, as with any font.

### 4.2 Names and licence

- **Files:** `assets/sd-fonts/<Family>/<Family>-Regular.ttf`, for example `NotoSansJP/NotoSansJP-Regular.ttf`.
  The installed layout on the card is section 6's.
- **Internal names:** the build rewrites the name table's family and full names to the new family ("Noto Sans
  JP", "Noto Serif TC"), so no subset claims to be the full Noto Sans CJK.
- **Licence:** Noto CJK is under the SIL Open Font License 1.1. Its only Reserved Font Name is Adobe's
  "Source", so the "Noto" names may stay.
  - `OFL.txt` ships in `assets/sd-fonts/` beside the families.
  - The README's font credits name Noto CJK.

### 4.3 Conversion

The subsets are converted from CFF to TrueType quadratics with fontTools' `cu2qu`, as the spike did:
- maximum error one font unit;
- contours reversed to TrueType direction;
- `VORG` dropped, `post` format 3, `maxp` version 1;
- no hinting.

**Tables.** An SD-mode family copies every table except `glyf` into its flash style block, `GPOS`, `GSUB` and
`kern` included (`TtfFamily.cpp:28`), and resolves kerning and ligatures as a flash-mode family does.
`EpdFont::getKerning` skips any pair that touches a CJK character.
- **Kept (my call):** `GPOS` with only its `kern` feature, so Latin words in CJK books are kerned as in every
  other family.
- **Dropped:** `GSUB` (Noto CJK's Latin has no ligatures we use), the proportional and vertical features
  (`palt`, `vkrn`, `vpal`), `BASE`, `vhea`, `vmtx` and `VORG`.
- **Recorded by the build:** the size the drops save, and the pair-index slots the kept kerning uses.

The `name` table is not read by the engine; it is rewritten for honesty only (section 4.2).

**Reproducible output.**
- The sources are pinned to a noto-cjk release, not `main`.
- The build sets `head.created` and `head.modified` to fixed values and saves with
  `recalcTimestamp=False`.
- A rebuild with unchanged inputs must produce identical bytes, so git sees no change.

**Self-check.** The build fails when:
- a code point of the set has no glyph in the source;
- the output does not load in the engine: style assigned, regular present, cmap covering the set.

### 4.4 The build

`lib/EpdFont/scripts/sd-fonts.yaml` stays the one list of hosted families:
- the eight families are added to it;
- the commented-out CJK bitmap entries are removed;
- descriptions flow from it to the manifest, as today.

**Builder's calls:**
- how the build script handles a TrueType-subset entry: a new key in the entry, a separate module, or both;
- caching the downloaded sources and Unihan beside the existing `downloaded_fonts/`.

`fonttools` is already in `requirements.txt`.

## 5. Removing the CJK bitmap families

- **Delete** `assets/sd-fonts/NotoSansCJK`, `NotoSerifCJK` and `NotoSansHangul`, then regenerate `fonts.json`.
- **Stop a repeat.** Only families listed in `sd-fonts.yaml` appear in `fonts.json`; a leftover folder is
  reported, not listed. How the generator learns the list is the builder's call. It is also used on its own
  for a folder of fonts, so that use must keep working.
- **Devices that installed them** keep them on the card. They still load: the `.cpfont` path does not change.

**Bugs left behind.** The spike found two bugs on the CJK bitmap path. They still hit anyone who converts
a CJK `.cpfont` by hand, so they move to `docs/future_work/cjk.md`:
1. `SdCardFont::loadFromMmap` copies the interval table to the heap: 49 KB for Japanese, which turned
   pre-rendering off. A fix that reads it in place when 4-byte aligned is on branch
   `spike/mmap-intervals-in-flash`. `MAX_INTERVALS` is 4096; the Japanese set has 4,083.
2. A `.cpfont` larger than the partition erases the whole partition (4–5 s) on the first book open after
   every restart, then fails to append.

## 6. The font manager: TrueType downloads

### 6.1 What exists

Mapped on master 532d79066.

**The manifest.** `fonts.json`, version 2:
`{version, baseUrl, families[{name, description, styles[], files[{name, size, crc32}]}]}`.
- `lib/JsonParser/FontManifestParser.h` streams it and skips `styles`.
- `src/util/FontManifestReader.cpp` accepts versions 1 and 2 only (:83).
- It checks every file name through `isValidFontFileName`, which tests the path's shape but not its
  extension. That function has two copies: `src/FontInstaller.cpp:33`, and `src/network/CrossPointWebServer.cpp:2185`.

**Downloads.** The loop is written twice:
- on the device, `src/activities/settings/FontDownloadActivity.cpp:422-644`;
- on the web, `CrossPointWebServer.cpp:2265-2407`.

Both stage into `/.crosspoint/fonts/<family>__staging`, then swap it in through `__backup`. Each file
downloads as `<dest>.part`. The CRC32 check (`FontInstaller::computeFileCrc32`) works for any file type.

**The only format check** is `FontInstaller::validateCpfontFile`, which compares the 8-byte `CPFONT` magic.
It is called when resuming and after each download, on both sides:
- device: `FontDownloadActivity.cpp:488, 580`;
- web: `CrossPointWebServer.cpp:2343, 2376`.

**Discovery already handles TrueType.** `SdCardFontRegistry` lists a folder's `.ttf` files as one TrueType
family (`ttf::isTtfFileName`), unless the folder also holds a `.cpfont`. A family the glyph store cannot hold
is not listed (`ttfFamilyFits`). The font manager's "installed" state is a registry lookup, so such a family
would show as not installed after its download.

**Not present:**
- a free-space check before a download;
- size or style display for a TrueType family. The web list prints `sizes + 'pt'` (`FontsPage.html:272`), and
  `availableSizes()` is empty for TrueType.

**The generator.** `scripts/generate-font-manifest.py` finds `*.cpfont` files, takes the family from the file
name (`<Family>_<size>.cpfont`) and reads styles from the `.cpfont` header.

### 6.2 Older firmware must not see TrueType families

A firmware that does not know TrueType downloads (everything before this work) would list a `.ttf` family,
download 4–11 MB, and then reject it at the `CPFONT` magic check. Firmware before #506 cannot draw a family
this large at all. Raising `fonts.json` to version 3 would be worse: older readers reject an unknown version,
so the whole font manager would fail for them.

So TrueType families go into a second manifest that only the new firmware reads (my call):
- **`fonts.json`** keeps its current format and lists only `.cpfont` families, without the CJK bitmap
  families (section 5).
- **The new manifest** (name the builder's, for example `fonts-v3.json`, version 3) lists every family,
  `.cpfont` and TrueType. Each family says which kind it is, so a reader version too old for a kind can skip
  it. The firmware's `FONT_MANIFEST_URL` (device and web) points at it.
- **The generator writes both** from the same scan.

### 6.3 Installing a TrueType family

- **Format check.** `validateCpfontFile` becomes a check by file type:
  - `.cpfont` checks the `CPFONT` magic, as now;
  - `.ttf` checks the sfnt version `0x00010000` or `true`, and rejects `OTTO` (CFF), `ttcf` (collections)
    and WOFF.

  All four call sites use it.
- **Free space.** Before a download, both sides check that the card has the family's total size free, plus
  a margin (my call). A 10 MB family should fail before it starts, not at 90%.
- **Layout on the card:** `/.crosspoint/fonts/<Family>/<Family>-Regular.ttf`, the manifest's file name with
  its family prefix removed, as for `.cpfont`.
- **Installed state.** After a TrueType install the registry must list the family. If the fit gate refuses it,
  the font manager says so rather than offering the download again.

### 6.4 How the families show

- **The device's font manager** lists each family with its description and download size, as now.
  - The descriptions say the language and the face. Example: "Japanese, sans serif (Noto Sans JP).
    TrueType, every size".
  - **"Download all" leaves the CJK families out (my call).** A reader wants their own language's font, not
    55 MB of all of them. A family-level field in the new manifest marks them.
- **The web page's installed list** shows a TrueType family as "TrueType, all sizes" instead of a size list.
- **Delete** already removes the whole folder, `.ttf` files included. The glyph store's contents are
  replaced at the next load, because the family identity changes.

### 6.5 Not in this round

- **Uploading a `.ttf` through the web page.** Today the upload accepts `.cpfont` only. The Convert card turns
  a TTF into a `.cpfont` in the browser. Installing a TTF as-is needs its own choice in that card, so it is a
  follow-up.
- **The `txtSdFontFamilyName` setting.** `deleteFamily` does not clear it; this was found while mapping the
  path and is unrelated to this work. It goes to future work.

## 7. Synthetic bold

### 7.1 Which styles

**The rule (my call).** A TrueType family gets synthetic bold only when none of its files is bold. A file
counts as bold when its `macStyle` says bold or its OS/2 weight class is 600 or more. Then:
- bold is drawn from the file serving the regular style;
- bold italic is drawn from the italic file when the family has one, otherwise from the regular.

**What that rules out.** Each of these cases would otherwise break an anti-goal or draw double bold:

| Family | Without the rule | With it |
|---|---|---|
| Regular, Bold and Italic, no BoldItalic | bold italic would become the emboldened italic | keeps today's fallback, the real upright Bold (`EpdFontFamily::getFont`) |
| No regular file, a Bold moved into the regular slot (`assignStyles`; display fonts books embed) | the moved Bold would be emboldened again | not emboldened |
| A single heavy face (weight ≥ 600 declared regular: "a regular with no bold") | heavier still | not emboldened |

A style the family has a file for is never synthesized.

**This covers:**
- reader TrueType families;
- book-embedded TrueType families (accent families), which often embed a regular only. CSS `font-synthesis`
  defaults to synthesizing weight, so this matches browsers and KOReader;
- any later TrueType family.

**It does not cover** `.cpfont` families, built-in fonts or the UI faces.

There is no setting to turn it off (my call).

### 7.2 How

The outline is emboldened after it is loaded and scaled, and before it is rasterized: between
`Rasterizer::load` and `Rasterizer::rasterize` (`lib/EpdFont/ttf/TtfRasterizer.h`).

**The algorithm is FreeType's `FT_Outline_EmboldenXY`** (`src/base/ftoutln.c`):
- each point moves outward along the bisector of its two edges' normals;
- the move is limited where edges are short or the angle is sharp;
- the outline's orientation is computed, not assumed: book fonts come in either direction.

**Its maths.** FreeType's version is fixed-point. The port stays integer-only, as the rest of the engine
(`check_no_float.py`). The FreeType licence is already credited.

**Strength.** FreeType's synthetic bold (`FT_GlyphSlot_Embolden`) uses em/24 on both axes, about 0.7 px at
12 pt. Skia eases this to em/32 at large sizes. The builder picks the rule, with these limits:
- it is one constant in one place;
- it is checked on screenshots of dense CJK characters at 10–14 pt and of Latin text;
- counters of characters like 鬱, 書 and 慶 stay open at 12 pt.

A smaller vertical strength than horizontal is allowed, because CJK characters stack many horizontal
strokes.

**Metrics.**
- The ink box grows by the strengths.
- The advance grows by the horizontal strength.
- The left side bearing, the baseline and the line height do not change.

**Storage.**
- **Glyph store:** synthetic bold glyphs are stored under the bold style's bits of the glyph key, with the
  regular's face index (`glyphstore::makeKey`).
- **No new style block:** the regular file's tables serve both, so `glyphstore::kMaxStyles` is untouched.
- **After the first render** a bold glyph costs what a regular one costs.

**Layout.** Bold text in a regular-only family gets wider, so `SECTION_FILE_VERSION` is bumped
(`lib/Epub/Epub/Section.cpp`).

### 7.3 Proof

- **The oracle is FreeType itself.** The host test `test/ttf_rasterizer` already links FreeType's own code
  (the SDK's copy, whose base module includes `FT_Outline_EmboldenXY`) and compares the engine with it
  byte for byte. The emboldened glyphs of every bundled TrueType font are compared the same way, at
  8, 14 and 40 pt, at the chosen strength.
  - Target: byte-identical.
  - If integer rounding makes that impossible, the builder records the differing-pixel rate. It must stay
    within the existing rasterizer tolerance.
- **Regular glyphs** stay byte-identical to before.
- **Style resolution** has a test for each row of section 7.1's table, plus a regular-only family and a
  regular-and-italic family.
- **Render cost:** a bold miss is measured against a regular miss on the X3.

## 8. The glyph store: emptied at the next chapter

### 8.1 Today

When the glyph store fills, writes stop and a wipe is set pending (`TtfFontSet::noteCommit`, "ring mode").
The wipe runs only at the next book open (`SdCardFontSystem::ensureLoaded` → `wipeTtfIfPending`), because
that is a point where no glyph record is in use. Until then every glyph is rasterized again on every page,
through an 8 KB RAM ring.

How fast the store fills:
- the log holds about 9,500 glyphs at 12 pt (≈150 B each) and 4,900 at 18 pt (Appendix A);
- it is not emptied between books of the same family;
- bold glyphs, which this design adds, take their share.

A CJK reader can fill it in the middle of a book.

### 8.2 Change

The pending wipe also runs when the next chapter opens, before its first layout or draw.

**Safe point.** The builder establishes, and a test pins, that no glyph record is in use there:
- no `EpdGlyph` pointer into the mapped log is held;
- no pre-render is running;
- `RenderLock` is held.

**Cost.** The wipe erases only the sectors holding data: about 2 s for a full log (X3: 1.9 s at the 32-page
budget). The existing busy callback (`onBusy`) shows the busy indicator. It happens once per fill.

### 8.3 Not done: selective eviction

Asked on 2026-10-20: keep glyphs in use and evict only the rest, instead of emptying the whole store.

**The sketch:**
- the log is split into 64 KB pages, with a RAM "used since last pass" bitmap (2 KB for 16,384 slots);
- when full, the oldest page's glyphs still in use are copied forward;
- the index slots of the rest are cleared, using the key each record carries;
- the page is erased (50–100 ms).

**Why not now:**
- the index needs periodic rebuilds;
- the copies add flash wear;
- emptying the store at the next chapter already removes most of the cost.

It goes to `docs/future_work/cjk.md`.

## 9. Validation

**Host.**
- The build's self-check (section 4.3).
- A test that loads each shipped family through the engine and renders a sample of its characters.
- The synthetic-bold oracle (section 7.3).
- **Manifest:**
  - the reader accepts version 3 and its TrueType entries;
  - the generator writes both manifests, and `fonts.json` lists no TrueType family;
  - a version-2 reader still reads `fonts.json`.
- **Installer:** the format check accepts each shipped `.ttf`, and rejects `OTTO`, `ttcf` and a `.cpfont`
  named `.ttf`.

**Device: the X4 and the X3.**
- **Install:** every family from the device's font manager over Wi-Fi; at least one from the web page.
- **Books:** open the CJK test books (`test_cjk_ja_soseki`, `test_cjk_zh_honglou`, `test_cjk_ko_hyeon`,
  `test_cjk_zh_punctuation`) in sans and in serif.
- **Performance:** measured both cold and with glyphs stored: layout time, lowest free heap and glyph
  preparation. Appendix A is a reference, not a pass mark (my call). These block shipping:
  - a failed allocation or a reader abort on any test book;
  - pre-rendering refused on every page of a book, on either board;
  - on the X4, sans, a book from Appendix A that lays out or prepares glyphs more than 25% slower than it did
    there.

  Anything else, including X3 and serif figures, is recorded in the PR.
- **Bold:** headings and bold spans look bold, with open counters. Screenshots go through `CMD:SCREENSHOT`.
  The maintainer does not read CJK, so Claude reviews them for stroke weight and counters.
- **A full store** is emptied at the next chapter (log line), and pages after it are stored again.
- **Latin:** a regular-only Latin TrueType family, and a book with a regular-only embedded font, show
  synthetic bold.

## 10. Documentation

- **`docs/future_work/cjk.md`:**
  - the font section and the "TTF Stage 2c-4" section become "shipped";
  - the two bitmap bugs and the eviction sketch are added;
  - the UI-text section points at these fonts as the fallback source.
- **The user guide's font manager page** lists the CJK families and their download sizes.
- **README credits:** Noto CJK (OFL).

## 11. Left to the builder

- The build script's structure (section 4.4).
- Which unread tables to drop (section 4.3).
- The bold strength rule within section 7.2's limits.
- How the safe point for the chapter wipe is reached (section 8.2).

## Appendix A. Spike measurements (X4, 2026-10-19/20)

Cold chapter builds at 12 pt. Arrows go from the first open (store empty) to glyphs stored.

| Setup | Layout (ms) | Lowest free heap (KB) | Glyph prep (ms/page) | Install |
|---|--:|--:|--:|--:|
| All-language bitmap (NotoSansCJK 12) from SD | 940 | 5.4 | 104 | – |
| Japanese bitmap in flash, today | 529 | 3.1 | 37 | 8.8 s |
| TrueType Japanese | 959 → 497 | 15.4 → 21.5 | 6–7 | 1.9 s (7.2 s first) |
| TrueType Traditional Chinese | 1,111 → 268 | 14.4 → 21.5 | – | 6.6 s |
| TrueType Korean | 449 → 229 | 16.4 → 21.5 | – | 4.7 s |
| TrueType Simplified Chinese (small book) | 148 → 156 | 14.5 → 21.6 | – | 4.8 s |
| TrueType Japanese at 18 pt | 1,149 → 524 | 14.9 → 21.5 | – | 4.0 s |

**Glyph store:** about 150 B a glyph at 12 pt (≈9,500 glyphs) and 305 B at 18 pt (≈4,900). The log is limited
by free MMU pages, not by the partition: 1.44–1.64 MB.

**Japanese bitmap family by size:** 0.88 / 1.15 / 1.47 / 1.86 / 2.28 MB at 10 / 12 / 14 / 16 / 18 pt,
7.64 MB in all.

**Where it was run:** spike files are in `C:/tmp/cjkspike`. The subsets came from `spike_sets.py`; the device
runs used `tools/cold_open.sh`.
