# CJK fonts, CFF outlines, synthetic bold and a glyph store that makes room: design

Status (2026-10-20): approved. The first version was approved, then revised the same day and approved again:
CFF outlines (section 9), the CJK families as CFF (`.otf`), a glyph store that makes room instead of being
emptied whole (section 8), and the serial test API first in the order of work (section 13, PR #531).

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
preparation 6–7 ms a page. One file serves every size.

Noto CJK is drawn in CFF outlines (`.otf`), and the engine reads TrueType (`glyf`) only. The spike converted the
outlines to TrueType quadratics. Kept as CFF, the same eight subsets are 27.5 MB against 34.6 MB as TrueType
(section 4.3, Appendix B), with the curves their designers drew. CFF also opens the books whose embedded fonts are
CFF, which Publisher Fonts refuses today, and the `.otf` fonts readers have.

What is missing is a font to install, and an engine that draws it as published.

## 2. Decided

By the maintainer, in conversation on 2026-10-20:
- **Per-language subsets of Noto CJK**, kept in `assets/sd-fonts` and downloadable from the font
  manager, both on the web page and on the device. The audience is Asian readers who need their books to
  render properly.
- **Sans and serif, as separate families:** eight in all.
- **Regular only.** No Bold files: they would double each download and the repository growth.
- **Synthetic bold** for every outline family that has no Bold of its own: the new CJK families, Latin
  families, and fonts embedded in books.
- **The three CJK bitmap families are removed** from `assets/sd-fonts` and from the manifest.
- **The UI fallback comes later, as its own design.** Book titles, TOC entries and the status line will
  fall back to the installed CJK font for characters the UI faces lack. That design builds on these fonts.

Added after the first approval:
- **CFF first, and the CJK families ship as `.otf`.** The engine learns CFF, the CID-keyed fonts Noto CJK uses
  included, before the fonts ship. Asked with the sizes in hand: 23.2 MB as CFF against 34.6 MB as TrueType for the
  eight, Serif KR 4.3 against 9.0 MB.
- **A full glyph store makes room instead of being emptied whole.** It keeps the glyphs in use, and a count of how
  often each glyph was used decides which (section 8).
- **The serial test API grows first**, so the device work below can be driven and watched (section 13).
- **Not now: the Font Manager staying on "Font installed!" until Back.** A screen that goes back by itself after a
  time is wanted in several places; it goes to future work as one mechanism (section 6.5).

## 3. Goals, non-goals, anti-goals

**Goals.**
- A reader installs the font for their language from the device or the web page in one step and reads CJK
  books with it, in sans or serif.
- The engine draws CFF (`.otf`) fonts: the hosted CJK families, `.otf` families readers put on the card, and the
  CFF fonts books embed.
- Bold text in a family without a bold file looks bold.
- A full glyph store keeps the glyphs a reader uses and makes room without a pause they notice.
- Reading performance as measured in the spike (Appendix A).

**Non-goals.**
- The UI fallback (section 2).
- Bold or italic files; synthetic italic or oblique. CJK typesetting does not use italics.
- CFF2 (variable CFF fonts), bare CFF and Type 1 files: they stay refused.
- Vertical text and vertical glyph forms.
- One family for all three languages. Glyph shapes differ by region (骨 is drawn differently in Chinese and
  Japanese), and per-language files are smaller.
- Fixing the CJK bitmap-path bugs the spike found (section 5). They no longer affect a hosted family.
- Uploading `.ttf` or `.otf` files through the web page (section 6.5).

**Anti-goals.** The work fails, even if it runs, if:
- an interrupted download leaves a family that the font selector lists but cannot draw;
- an older firmware's font manager stops working, or offers a family it cannot install (section 6.2);
- synthetic bold fills the counters of dense characters at reading sizes (a character like 鬱 at 12 pt);
- any regular glyph, any glyph of a family with a real Bold, or any TrueType glyph renders differently because
  of this work;
- making room in the store loses a glyph a page being drawn uses, or serves a glyph under another glyph's key;
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

The sources are pinned to releases Sans2.004 and Serif2.003, each file by its sha256 in `sd-fonts.yaml`;
Unihan 17.0.0 likewise.

Every family also carries the characters CJK books mix in:
- ASCII and Latin-1;
- general punctuation, U+2010–2027 and U+2030–205E;
- CJK symbols and punctuation, U+3000–303F;
- full-width and half-width forms, U+FF01–FF9F;
- U+FFFD, which the renderer falls back to.

**What Noto CJK lacks (measured).** Every region font lacks 39 characters of those punctuation ranges (‗, ‛, ‣,
‱, ‽, ⁂ …) and U+FFFD. Japanese also lacks ≒ (U+2252); Traditional Chinese ˍ, ‾, ∼ and ≒; Korean 34 characters of
KS X 1001's Latin and symbol rows (Ħ, ı, Ŀ, ŉ, ₁–₄, ⅓ …). The build leaves these out by an explicit list; any
other character a source lacks fails the build, so a source that lost glyphs is never shipped short. U+FFFD comes
from Noto Sans (OFL, bundled for the built-in fonts) at the same 1000-unit em (my call), its quadratic outline
written as a CFF charstring: a quadratic curve is exactly a cubic one.

**Sizes, measured** (CFF without subroutines, section 4.3):

| Family | Characters | Sans | Serif |
|---|--:|--:|--:|
| NotoSansJP / NotoSerifJP | 7,367 | 1.69 MB | 2.43 MB |
| NotoSansSC / NotoSerifSC | 9,216 | 2.01 MB | 2.88 MB |
| NotoSansTC / NotoSerifTC | 14,052 | 3.45 MB | 5.05 MB |
| NotoSansKR / NotoSerifKR | 17,304 | 2.70 MB | 7.33 MB |

27.5 MB for all eight. As TrueType the same sets were 34.6 MB (Appendix B).

**Why the additions (my call).** Two of the national sets miss characters modern books use:
- JIS X 0208 (1983) predates the 2010 Jōyō revision, which added characters such as 𠮟, 塡, 剝 and 頰.
- GB2312 (1980) misses characters the 2013 Table of General Standard Chinese Characters (通用规范汉字表,
  8,105 characters) includes.

Unihan's `kJoyoKanji`, `kJinmeiyoKanji` and `kTGH` fields list them. Unihan is Unicode data with a permissive
licence, so the build downloads it the same way it downloads the font sources. Big5 and KS X 1001 Hanja
need no addition.

**The symbol rows (my call).** The national standards' symbol rows (circled numbers, Roman numerals,
parenthesized Hangul, the Japanese NEC row) cost a few hundred glyphs and appear in modern books, so every set
keeps them. The spike's Korean set had kept only the Hanja rows.

**Left out (my call): Hong Kong characters (HKSCS).** They are about 4,700 characters more. Hong Kong Cantonese
text (嘅, 咗) draws as U+FFFD. If readers ask, a separate Hong Kong family would add them later.

**The sets are final before the first commit.** The fonts are committed binaries. Every later change to a set
adds another 2–7 MB a family to the repository's history, and installed readers have to download again.

A character outside the set draws as U+FFFD, as with any font.

### 4.2 Names and licence

- **Files:** `assets/sd-fonts/<Family>/<Family>-Regular.otf`, for example `NotoSansJP/NotoSansJP-Regular.otf`.
  The installed layout on the card is section 6's.
- **Internal names:** the build rewrites the `name` table's family, full and PostScript names to the new family
  ("Noto Sans JP", "NotoSansJP-Regular") and drops the typographic names; the CFF's own font name follows. No
  subset claims to be the full Noto Sans CJK.
- **Licence:** Noto CJK is under the SIL Open Font License 1.1. Its only Reserved Font Name is Adobe's
  "Source", so the "Noto" names may stay.
  - `OFL.txt` ships in `assets/sd-fonts/` beside the families, with the copyright lines of Noto Sans CJK, Noto
    Serif CJK and Noto Sans (the U+FFFD glyph).
  - The README's font credits name Noto CJK.

### 4.3 Outlines and tables

The outlines stay CFF; the build changes two things.

**Subroutines removed (my call).** Noto CJK's charstrings call shared subroutines: about 3 a glyph in sans and 10
in serif, out of 230–770 KB of local subroutines a family (Appendix B).
- Kept, they would cost SD mode either a dozen scattered card reads for a new serif glyph, or 230–770 KB of the
  flash mapping budget the glyph store's log lives on (section 9.4).
- Removed, every glyph is one charstring, read in one go, and the flash block stays as small as a TrueType
  family's.
- Cost: 27.5 MB instead of 23.2 MB for the eight (Serif KR 7.3 against 4.3 MB), still 7 MB under TrueType.

Other CFF fonts keep their subroutines (section 9.4).

**Tables.** An SD-mode family copies every table but its outline data into its flash style block, `GPOS`,
`GSUB` and `kern` included (`TtfFamily.cpp:28`), and resolves kerning and ligatures as a flash-mode family does.
`EpdFont::getKerning` skips any pair that touches a CJK character.
- **Kept (my call):** `GPOS` with only its `kern` feature, so Latin words in CJK books are kerned as in every
  other family.
- **Dropped:** `GSUB` (Noto CJK's Latin has no ligatures we use), the proportional and vertical features
  (`palt`, `vkrn`, `vpal`), `BASE`, `vhea`, `vmtx`, `VORG` and `DSIG`.
- **Recorded by the build:** the size the drops save, and the pair-index slots the kept kerning uses.

The `name` table is not read by the engine; it is rewritten for honesty only (section 4.2).

**Reproducible output.**
- The sources are pinned (section 4.1).
- The build keeps the source's `head.created` and `head.modified` and saves with `recalcTimestamp=False`.
- A rebuild with unchanged inputs produces identical bytes, so git sees no change. The TrueType prototype did.

**Self-check.** The build fails when:
- a code point of the set has no glyph in the source, other than the listed gaps;
- the output is not what the engine reads: sfnt version `OTTO` with a CFF version 1 table and no subroutines; a
  Unicode cmap covering the set and U+FFFD; a regular style (`macStyle` not bold, weight under 600).

### 4.4 The build

`lib/EpdFont/scripts/sd-fonts.yaml` stays the one list of hosted families:
- the eight families are added to it, each with `format: otf`, its character set, its pinned source and
  `inDownloadAll: false`; the pinned Unihan sits at the top level;
- the commented-out CJK bitmap entries are removed;
- descriptions flow from it to the manifest, as today. They say the language and the face ("Japanese, sans
  serif") and fit a Font Manager row: the X4 cut the prototype's longer ones off.

`build-sd-fonts.py` hands such an entry to a subset module, checks every pinned sha256, cached or downloaded, and
caches the downloads beside the existing `downloaded_fonts/`. `fonttools` is already in `requirements.txt`. The
prototype (`ttf_subset.py`, written for TrueType) becomes the CFF one.

## 5. Removing the CJK bitmap families

- **Delete** `assets/sd-fonts/NotoSansCJK`, `NotoSerifCJK` and `NotoSansHangul`, then regenerate the manifests.
- **Stop a repeat.** Only families listed in `sd-fonts.yaml` appear in the manifests; a leftover folder is
  reported, not listed. The generator learns the list with `--config sd-fonts.yaml`; used on its own for a folder
  of fonts, without it, it lists everything it finds, as now.
- **Devices that installed them** keep them on the card. They still load: the `.cpfont` path does not change.

**Bugs left behind.** The spike found two bugs on the CJK bitmap path. They still hit anyone who converts
a CJK `.cpfont` by hand, so they move to `docs/future_work/cjk.md`:
1. `SdCardFont::loadFromMmap` copies the interval table to the heap: 49 KB for Japanese, which turned
   pre-rendering off. A fix that reads it in place when 4-byte aligned is on branch
   `spike/mmap-intervals-in-flash`. `MAX_INTERVALS` is 4096; the Japanese set has 4,083.
2. A `.cpfont` larger than the partition erases the whole partition (4–5 s) on the first book open after
   every restart, then fails to append.

## 6. The font manager: outline font downloads

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

**Discovery handles TrueType.** `SdCardFontRegistry` lists a folder's `.ttf` files as one TrueType family
(`ttf::isTtfFileName`), unless the folder also holds a `.cpfont`. A family the glyph store cannot hold is not
listed (`ttfFamilyFits`). The font manager's "installed" state is a registry lookup, so such a family would show
as not installed after its download.

**Not present:**
- a free-space check before a download;
- size or style display for an outline family. The web list prints `sizes + 'pt'` (`FontsPage.html:272`), and
  `availableSizes()` is empty for TrueType.

**The generator.** `scripts/generate-font-manifest.py` finds `*.cpfont` files, takes the family from the file
name (`<Family>_<size>.cpfont`) and reads styles from the `.cpfont` header.

### 6.2 Older firmware must not see the new families

A firmware that does not know these downloads (everything before this work) would list an `.otf` family,
download 2–7 MB, and then reject it at the `CPFONT` magic check. Raising `fonts.json` to version 3 would be
worse: older readers reject an unknown version, so the whole font manager would fail for them.

So the new families go into a second manifest that only the new firmware reads (my call):
- **`fonts.json`** keeps its current format and lists only `.cpfont` families, without the CJK bitmap
  families (section 5).
- **`fonts-v3.json`** (version 3) lists every family. Each says its format (`cpfont`, `ttf` or `otf`), and the
  CJK families carry `"inDownloadAll": false`. A reader skips a family of a format it does not know, so a later
  format can be added the same way. `FONT_MANIFEST_URL`, defined once in `FontManifestReader.h` and used by the
  device and the web page, points at it.
- **The generator writes both** from the same scan.

### 6.3 Installing an outline family

- **Format check by file type,** in one place testable on the host (`FontDownloadRules`), used at all four call
  sites:
  - `.cpfont`: the `CPFONT` magic, as now;
  - `.ttf`: sfnt version `0x00010000` or `true`;
  - `.otf`: `OTTO`.

  Collections (`ttcf`) and WOFF are refused. A manifest family whose files are not of its format is left out.
- **Free space.** Before a download, both sides check that the card has the family's total size free, plus
  1 MB (my call). A 7 MB family fails before it starts, not at 90%. On the X4 the check added no visible delay.
- **Layout on the card:** `/.crosspoint/fonts/<Family>/<Family>-Regular.otf`, the manifest's file name with
  its family prefix removed, as for `.cpfont`.
- **Installed state** is "the registry lists it, or its folder is there". A family the glyph store cannot hold is
  then installed but not listed, and the font manager says so once instead of offering the download again.

Prototyped and run on the X4 (2026-10-20): Noto Sans TC (TrueType, 4.3 MB) downloaded from the device's font
manager and was installed and listed in 23 s.

### 6.4 How the families show

- **The device's font manager** lists each family with its description and its download size.
- **"Download all" leaves the CJK families out (my call):** a reader wants their own language's font, not 27 MB
  of all of them. On the X4 the prototype showed "Download All (5.5 MB)" with the CJK families listed below it.
- **The web page's installed list** shows an outline family as "all sizes" instead of a size list.
- **Delete** already removes the whole folder. The glyph store's contents are replaced at the next load, because
  the family identity changes.

### 6.5 Not in this round

- **Uploading a `.ttf` or `.otf` through the web page.** Today the upload accepts `.cpfont` only. The Convert
  card turns a font into a `.cpfont` in the browser. Installing one as-is needs its own choice in that card, so it
  is a follow-up.
- **A screen that goes back by itself.** "Font installed!" waits for Back. The maintainer wants a timed return,
  shared by the screens that end on a message: one mechanism, not one per screen. Future work.
- **The `txtSdFontFamilyName` setting.** `deleteFamily` does not clear it; this was found while mapping the
  path and is unrelated to this work. It goes to future work.

## 7. Synthetic bold

### 7.1 Which styles

**The rule (my call).** An outline family (TrueType or CFF) gets synthetic bold only when none of its files is
bold. A file counts as bold when its `macStyle` says bold or its OS/2 weight class is 600 or more, and so does any
file in the bold or bold-italic slot. Then:
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
- reader outline families;
- book-embedded families (accent families), which often embed a regular only. CSS `font-synthesis`
  defaults to synthesizing weight, so this matches browsers and KOReader;
- any later outline family.

**It does not cover** `.cpfont` families, built-in fonts or the UI faces.

There is no setting to turn it off (my call).

### 7.2 How

The outline is emboldened after it is loaded and scaled, before it is rasterized: `Rasterizer::embolden()` runs
between `load()` and `rasterize()` (`lib/EpdFont/ttf/TtfRasterizer.h`) and boxes the glyph again. It is
FreeType 2.13.3's `FT_Outline_EmboldenXY`, ported statement for statement in integers, with the vector length and
the outline-orientation helpers it uses:
- each point moves outward along the bisector of its two edges' normals;
- the move is limited where edges are short or the angle is sharp;
- the outline's orientation is computed, not assumed: book fonts come in either direction.

It works on any outline the loader leaves, quadratic or cubic. The FreeType licence is already credited.

**Strength (measured).** FreeType's strength across (`x_ppem * 0x0AAA / 1024`, about em/24) and none down. With
FreeType's vertical strength too, 鬱 書 慶 議 驚 lost 10–42% of their open space at 10–14 pt; across only, they keep
at least 78%. A test pins three quarters on Noto Sans and Serif JP samples. It is one constant in one place.

**Metrics.**
- The ink box grows across; the left side bearing, the baseline and the line height stay.
- The advance grows by the strength, rounded as FreeType rounds it.
- A synthetic family's scratch plan leaves room for the wider box.

**Storage.**
- **Glyph store:** synthetic bold glyphs are stored under the bold style's bits of the glyph key, with the face
  index of the size (`glyphstore::makeKey`).
- **No new style block:** the regular file's tables serve both, so `glyphstore::kMaxStyles` is untouched; kerning
  uses the source file's pairs.
- **`ttf::kRenderVersion` stays:** until now a regular-only family's bold fell back to another style's font, whose
  glyphs carry that style's bits, so no store holds a key a synthetic style uses.
- **After the first render** a bold glyph costs what a regular one costs.

**Layout.** Bold text in a regular-only family gets wider, so `SECTION_FILE_VERSION` goes up
(`lib/Epub/Epub/Section.cpp`).

### 7.3 Proof

- **The oracle is FreeType itself.** The host test `test/ttf_rasterizer` links FreeType's own code (the SDK's
  copy) and compares the engine with it byte for byte. Every glyph of the 12 bundled TrueType fonts at 8, 14 and
  40 pt is emboldened at the engine's strength and at FreeType's own (both axes): advances, boxes and bitmaps.
  CFF outlines join the oracle with section 9.
  - Target: byte-identical. The prototype reached it: zero differences.
- **Regular glyphs** stay byte-identical to before (the existing oracle and the device bench's hash).
- **Style resolution** has a test for each row of section 7.1's table, plus a regular-only family and a
  regular-and-italic family.
- **Render cost** (X4, ESP32-C3, prototype bench): a store miss goes from 480 to 819 µs at 14 pt, from 1,229 to
  1,610 µs at 40 pt, and from 670 to 1,321 µs for a dense CJK character at 12 pt. That is paid once, before the
  store holds the glyph.

The plan is `docs/superpowers/plans/2026-10-20-synthetic-bold.md`.

## 8. The glyph store makes room

### 8.1 Today

When the glyph store fills, writes stop and a wipe is set pending (`TtfFontSet::noteCommit`, "ring mode").
The wipe runs only at the next book open (`SdCardFontSystem::ensureLoaded` → `wipeTtfIfPending`), because
that is a point where no glyph record is in use. Until then every new glyph is rasterized again on every page,
through an 8 KB RAM ring.

How fast the store fills:
- the log holds about 9,500 glyphs at 12 pt (≈150 B each) and 4,900 at 18 pt (Appendix A);
- it is not emptied between books of the same family;
- bold glyphs, which this design adds, take their share;
- on the X4 the store-fill test book (`tools/make_cjk_store_fill_epub.py`) filled it within its first chapter at
  26 pt.

A CJK reader can fill it in the middle of a book. Emptying it whole is no cure either: it took 1.3–1.8 s on the
X4, and every glyph after it is drawn again, those of the very next page included.

### 8.2 The change: a ring of segments that keeps the glyphs in use

**The log becomes a ring of segments,** 64 KB each: one flash erase block. Glyphs are appended at the head, as
now. When less than a segment is free, the oldest segment is reclaimed:
1. its glyphs that were used since the reclaim last passed them are copied to the head;
2. the others are dropped;
3. the segment is erased and becomes free.

**A use count per glyph decides (the maintainer's idea, sized for the C3).** Each index slot has a 2-bit
saturating count in RAM. A full count would cost a byte or two per slot, 10–30 KB of heap a reader does not
have.
- **A use is a draw (my call):** once per glyph drawn on a page from the store (the draw path's
  `TtfFontSet::ownsBitmap`); the draw that first stores a glyph is its first use. Layout, prefetch and the scan
  pass look glyphs up too, but do not count: otherwise every glyph a chapter's layout touched would look used,
  and a reclaim could free nothing.
- **A reclaim** keeps a glyph whose count is above 0, then halves the count (3 → 1, 2 → 1, 1 → 0). That keeps
  what is used often and lately.
- **Each reclaim frees at least half a segment (my call).** When more than that is worth keeping, the least used
  are dropped too, the oldest first among equals.
- **The counts are allocated when the log first passes 75% full and freed with the set (my call).** A store that
  never fills, as with most Latin reading, never pays for them. They take 2.5–6 KB: 10,000–16,000 slots for a CJK
  store, 24,576 for a flash-mode one. If the allocation fails, the store falls back to today's full wipe.
- **The counts live in RAM only.** After a restart, or a reload of the set (reader exit, a font change, the memory
  ladder), every glyph starts at 0 again, so the first reclaim keeps only what has been drawn since. Accepted
  (my call).

**How much room (my call).** A reclaim pass frees segments until two are free. In idle time it reclaims one
segment a tick and yields to input between segments; at a chapter or book open it goes on until two are free,
behind the busy indicator. Until it has run, new glyphs fill what is free and then go to the RAM ring, as today.

**Index slots carry their segment's epoch.** Epochs come from one counter that only grows: the highest found at
open, plus one. Each segment's header holds the epoch it got at its last erase; the epochs also order the
segments into tail and head.
- A slot's value packs the record's offset with its segment's epoch at the time of writing. A lookup whose slot
  names an epoch its segment no longer has treats the slot as stale and probes on: a copied glyph has a newer
  slot further along its chain.
- No epoch is ever reused while a slot may name it: a segment whose header a power cut destroyed gets a new
  number. The field is wide enough never to wrap in a device's life, or the index is rebuilt before it would.
- So a slot can never serve whatever a reused segment now holds at its offset.

**The index is rebuilt now and then.** Stale slots cannot be cleared without an erase, so they fill the index.
- **When:** the index passes its load limit (my call: 70%), or a probe chain is full (`kMaxProbe`, today a
  reason to wipe). The rebuild runs in idle time or at a chapter or book open, never in the middle of a page.
- **How:** the index is erased, then every live record inserted again, since records carry their keys. Their
  counts move with them. Estimated at one to two seconds, as long as a wipe.
- **Power cuts:** the store marks a rebuild begun and done; an open that finds one begun and not done rebuilds
  again.
- **Lost by a rebuild:** the "no glyph" markers, which are slots without a record. They are learned again.

**Copies go through RAM.** Flash cannot be programmed from mapped flash, so a reclaim copies through a buffer
of 4 KB (the largest record a batch takes), borrowed for its duration. Without one the reclaim waits for the
next safe point.

**Where it runs.** A reclaim or a rebuild needs a safe point where no record of the log is in use: under
`RenderLock`, outside any lookup or draw. It runs in the reader's idle time, beside the pre-render in the idle
dispatch, and at a chapter or book open. The prototype of "wipe at the next chapter" established the chapter-open
safe point (`EpubReaderActivity::buildSection`, under the render pass's `RenderLock`) and a `RenderLock` guard on
the wipe; both carry over.

**The full wipe stays** for what a reclaim cannot fix: a full face table (32 faces, each a family at a size), a
failed write, a store whose format or identity changed.

### 8.3 Power, wear, format

- **Power cuts.** A segment's header is written after its erase, and at open the headers give head and tail.
  A cut after the copies and before the erase leaves duplicates, both valid and identical. A cut during an erase
  leaves a segment without a valid header, which is erased again before use.
- **Wear.** The ring spreads erases evenly: one 64 KB erase per segment's worth of new glyphs, about 400 at 12 pt.
  At NOR flash's 100,000 erase cycles a block, a ring of about 28 segments takes over a billion new glyphs.
- **Format.** `glyphstore::kFormatVersion` goes to 4. An older store is installed over, as at every format change.

### 8.4 Cost

A reclaim erases one 64 KB block and copies what it keeps; it leaves the index alone. A wipe's time does not
predict it: a wipe also erases every index sector holding a slot, and slots are hashed across the whole index. On
the X4 a wipe took 1.3 s with a full 1.83 MB log (prototype) and 1.8 s with 186 KB of log in use (master, read
with `CMD:TTF`). Estimated at a few hundred milliseconds a segment, in idle time; measured first. At 26 pt a
segment holds only about 100 glyphs, less than a page of new CJK characters, so a chapter open there may reclaim
several behind the busy indicator. The pages after it draw from the store. The builder measures, on the X3 and
the X4:
- a reclaim;
- a full CJK chapter at 12 and 26 pt: how many glyphs were drawn again because a reclaim dropped them.

## 9. CFF outlines

### 9.1 What the engine reads

OpenType fonts with CFF outlines:
- sfnt version `OTTO` with a `CFF ` table of CFF version 1;
- name-keyed or CID-keyed, the latter with an FDArray and an FDSelect of format 0 or 3 (Noto CJK has 9–10 font
  DICTs);
- Type 2 charstrings with global and local subroutines, curves and flex.

Hint operators are read past: the engine draws unhinted, as for TrueType. Operators Noto CJK does not use (the
arithmetic and storage operators, an accented `endchar`) are the builder's call: implemented, or reported as
Unsupported so the glyph draws as missing. The oracle (9.3) counts any glyph of the test fonts that draws as
missing.

**Not read:** CFF2 (variable OpenType), bare CFF and Type 1 files. They stay refused.

### 9.2 How it fits

- **Face:** `parseFace` accepts `OTTO` with a `CFF ` table: its header, Top DICT, CharStrings INDEX and global
  subroutines; for CID fonts the FDArray, the FDSelect, and each font DICT's Private DICT and local subroutines.
  The cmap, `hmtx`, `hhea`, `OS/2`, `GPOS` and `kern` tables are read as for TrueType.
- **Loader:** the rasterizer's loader decodes the charstring into the scratch block's points, tags (cubic control
  points) and contour ends, then scales them as FreeType's CFF driver does (9.3). ftgrays draws cubics already.
  Advances come from `hmtx`, as FreeType's do when the font has one.
- **Scratch plan:** a CFF font's `maxp` is version 0.5, without point maxima, so a CFF face is planned at the caps
  (512 points, 256 contours). Measured on the CJK subsets: at most 388 points (Serif KR), except one glyph, 龘 in
  Serif TC at 621 points (697 as TrueType). It draws as missing in either format; a larger block for every family
  is not worth one glyph (my call).
- **Synthetic bold** works on CFF outlines unchanged (section 7).
- **Store and identity:** unchanged. A CFF file is another file, so another identity.

### 9.3 Exactness

**The oracle is FreeType's CFF driver, loading unhinted.** The SDK's FreeType copy has the `cff`, `psaux` and
`pshinter` modules, and the host build already compiles them (CMake takes every `ft_*.c`).

**What the port follows.** The SDK's `ftoption.h` leaves `CFF_CONFIG_OPTION_OLD_ENGINE` off, so the oracle is the
Adobe engine (cf2). It interprets charstrings in 16.16 fixed point at unit scale, applies the font DICT's
FontMatrix (`FT_Outline_Transform`, `cffgload.c`), then scales every point with `FT_MulFix`; stem darkening is
off by default. Exactness means porting that path, in the same integer fixed point, which `check_no_float.py`
allows. The outlines it hands the rasterizer are flagged `REVERSE_FILL`, and `HIGH_PRECISION` below 24 ppem; the
builder confirms what those flags change in ftgrays' output.

**The test fonts (my call).**
- Small CID-keyed subsets of Noto Sans JP and Noto Serif JP, a few hundred glyphs each (dense kanji, kana, Latin,
  punctuation), with and without subroutines. They are made by a committed script and committed in
  `test/fixtures/fonts` with the OFL, beside the existing `-Dense.ttf` fixtures, so CFF work does not wait for the
  hosted fonts (section 13).
- The four bundled OpenDyslexic sources (`lib/EpdFont/builtinFonts/source/OpenDyslexic/*.otf`, name-keyed,
  Latin).

**The bar.**
- Every glyph of the test fonts at 8, 14 and 40 pt, regular and emboldened.
- Advances and boxes: exact.
- Bitmaps: byte-identical is the target. If rounding makes that impossible, at most 3% differing pixels per font
  and size in all (my call): the rate the TrueType oracle allows for overlap glyphs.
- TrueType output stays byte-identical (the existing oracle and the device bench's hash).

### 9.4 SD mode

An SD-mode family copies its tables into its flash style block and reads its outlines from the card through
`FontBytes`, as it reads `glyf` today. For CFF the outlines are the CharStrings INDEX's data:
- the block holds every other table, and of the `CFF ` table everything but that data: the header, the
  dictionaries, the string and subroutine INDEXes, the charset, the FDSelect and the charstring offsets;
- a new glyph costs one card read, of its charstring;
- a font with subroutines keeps them in the block. The hosted families have none (section 4.3).

**The install puts the charstring data last (my call).** `FontBytes` reads one far range, which must lie past the
block's in-place bytes, and in CID fonts the charstring data usually sits mid-table, before the FDArray, the
Private DICTs and the local subroutines. When a `CFF ` table is not already in that order, the install rewrites
it with the charstring data last and the DICT offsets recomputed, rather than giving `FontBytes` a gap.

### 9.5 Where CFF fonts come from

- **The font manager:** the hosted CJK families (`format: otf`).
- **The card:** discovery lists a folder's `.otf` files as a family, as it lists `.ttf` files; a folder holding
  both makes one family.
- **Books:** Publisher Fonts takes a book's CFF faces instead of refusing them (`lib/Epub/Epub/BookFonts.cpp`);
  WOFF, WOFF2 and encrypted fonts stay refused.

### 9.6 Cost

Decoding a charstring is the new work per store miss. The device bench gains CFF runs: the CJK samples as CFF
against the same characters as TrueType, at 12 pt, regular and bold. Recorded, not a pass mark; what blocks
shipping is section 10's list.

## 10. Validation

**Host.**
- The build's self-check (section 4.3), and its unit tests on made-up fonts and a made-up Unihan.
- A test that loads each shipped family through the engine, within the X3's font area and mapping budget, and
  renders a sample of its characters and U+FFFD.
- The emboldening oracle (section 7.3) and the CFF oracle (section 9.3).
- **Manifests:**
  - the reader accepts version 3, its formats and `inDownloadAll`;
  - the generator writes both manifests, and `fonts.json` lists no TrueType or CFF family;
  - a version-2 reader still reads `fonts.json`;
  - a family of an unknown format is skipped; a family whose files are not of its format is dropped.
- **Installer:** the format check accepts each shipped `.otf`, and refuses `OTTO` named `.ttf`, `ttcf`, WOFF
  and a `.cpfont` named `.otf`.
- **The store:**
  - a reclaim keeps the counted glyphs and drops the rest;
  - a stale slot is never served: an erased segment, a reused one, another record at its offset;
  - layout and prefetch do not count as uses, draws do;
  - a reclaim frees at least half a segment, even when every glyph in it was used;
  - the index rebuild, with the counts carried over;
  - a power cut at every step of a reclaim and of a rebuild, with several boots after each, varied offsets and
    every precondition.

**Device: the X4 and the X3,** driven through the serial test API.
- **Install:** every family from the device's font manager over Wi-Fi; at least one from the web page.
- **Books:** open the CJK test books (`test_cjk_ja_soseki`, `test_cjk_zh_honglou`, `test_cjk_ko_hyeon`,
  `test_cjk_zh_punctuation`) in sans and in serif, and a book that embeds a CFF font, with Publisher Fonts on.
- **Performance:** measured both cold and with glyphs stored: layout time, lowest free heap and glyph
  preparation. Appendix A is a reference, not a pass mark (my call). These block shipping:
  - a failed allocation or a reader abort on any test book;
  - pre-rendering refused on every page of a book, on either board;
  - on the X4, sans, a book from Appendix A that lays out or prepares glyphs more than 25% slower than it did
    there.

  Anything else, including X3 and serif figures, is recorded in the PR.
- **Bold:** headings and bold spans look bold, with open counters. Screenshots go through `CMD:SCREENSHOT`.
  The maintainer does not read CJK, so Claude reviews them for stroke weight and counters.
- **Latin:** a regular-only Latin family, and a book with a regular-only embedded font, show synthetic bold.
- **The store:** filled by `CMD:TTF:FULL` and by reading the store-fill book at 12 and 26 pt. Reclaims run in
  idle time (`EVT:TTF_*` lines); the PR records how many glyphs a page draws again after one; no page shows a
  wrong glyph.

## 11. Documentation

- **`docs/future_work/cjk.md`:**
  - the font section and the "TTF Stage 2c-4" section become "shipped";
  - the two bitmap bugs, and 龘 in Serif TC, are added;
  - the UI-text section points at these fonts as the fallback source.
- **`docs/future_work/`:** a screen that goes back by itself after its message, one mechanism for every screen
  that needs it (section 6.5).
- **The user guide:** the font manager page lists the CJK families and their download sizes, and says "Download
  all" leaves them out; the font pages mention synthetic bold and `.otf` families on the card.
- **`docs/file-formats.md`:** the glyph store's format 4, and the manifest's version 3.
- **README credits:** Noto CJK (OFL).

## 12. Left to the builder

- The build script's structure (section 4.4).
- The CFF loader's structure, within section 9.1's scope and 9.3's exactness.
- The rewrite of a `CFF ` table that puts its charstring data last (section 9.4).
- The segment header's layout, how a slot value packs offset and epoch, how the store marks a rebuild, and the
  reclaim's place in the reader's idle dispatch (section 8.2).

## 13. Order of work

Each is its own plan and PR; the device runs use the serial test API.
1. **The serial test API** (branch `feat/serial-api-ttf-fs`): `CMD:TTF` (the store's state), `CMD:TTF:FULL` (act
   as if it filled up), `CMD:FS:LS` and `CMD:FS:RM` (the card, without the USB Transfer screen), events for a full
   store, a wipe and a finished font install, and `devctl.py`'s `select:` step (Down until a row is selected).
2. **Synthetic bold** (plan written, from its prototype).
3. **The store makes room** (section 8). It replaces the plan "glyph store emptied at the next chapter", which is
   withdrawn; its safe point and `RenderLock` guard carry over.
4. **CFF outlines** (section 9).
5. **The CJK fonts as `.otf` in the font manager** (sections 4–6), once CFF has merged: the hosted fonts are only
   useful to firmware that draws them, and `fonts-v3.json` goes live with this.

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

The spike's subsets kept every layout feature, whose alternate glyphs made them larger: Japanese sans was
5.09 MB as TrueType, against 2.18 MB with only `kern`.

**Where it was run:** spike files are in `C:/tmp/cjkspike`. The subsets came from `spike_sets.py`; the device
runs used `tools/cold_open.sh`.

## Appendix B. Measurements of 2026-10-20

**The subsets** (the sets of section 4.1, `kern` only, the tables of 4.3 dropped):

| Family | CFF | CFF, no subroutines | TrueType |
|---|--:|--:|--:|
| NotoSansJP | 1.62 MB | 1.69 MB | 2.18 MB |
| NotoSerifJP | 2.25 | 2.43 | 3.00 |
| NotoSansSC | 1.98 | 2.01 | 2.60 |
| NotoSerifSC | 2.71 | 2.88 | 3.52 |
| NotoSansTC | 3.34 | 3.45 | 4.50 |
| NotoSerifTC | 4.63 | 5.05 | 6.20 |
| NotoSansKR | 2.31 | 2.70 | 3.58 |
| NotoSerifKR | 4.31 | 7.33 | 9.01 |
| all eight | 23.15 | 27.53 | 34.61 |

**CFF structure** (with subroutines): all CID-keyed, 9–10 font DICTs. Charstring offsets 22–51 KB, global
subroutines 5–20 KB, local subroutines with the dictionaries, charset and FDSelect 229–773 KB a family.
Subroutine calls made directly by a glyph (300 sampled a family): 2.2–3.0 on average in sans, 9.1–11.6 in serif,
at most 33. Largest glyph: 388 points, except 龘 in Serif TC (621).

**On the X4** (the three prototypes merged on master 67bb1e428, the fonts served from a PC over plain HTTP):
- Font manager: every row with its size; "Download All (5.5 MB)" without the CJK families; Noto Sans TC (4.3 MB)
  installed in 23 s; the free-space check without a visible delay; the long descriptions cut off.
- `test_cjk_zh_honglou` in Noto Sans TC: SD mode, a 1.83 MB log; the chapter heading in synthetic bold.
- The store-fill book at 26 pt: the store filled during chapter 1 and was emptied in about 1.3 s at the turn into
  chapter 2 (the chapter-wipe prototype).
- Bench (ESP32-C3): section 7.3.
- The serial test API on master (branch `feat/serial-api-ttf-fs`): `test_cjk_zh_honglou` in Noto Sans TC, SD
  mode, 186 KB of a 1.83 MB log in use after a chapter (630 slots); `CMD:TTF:FULL`, then the next book open wiped
  the store in 1.8 s (`EVT:TTF_WIPE ms=1789`).
