# Writing modes and scripts: handover (2026-11-09)

This is the state of the right-to-left, Arabic, Indic, Hebrew and CJK work, and what is left, written so that the work
can continue on another computer. What shipped, and why it is shaped as it is, is in
[../design/writing-modes.md](../design/writing-modes.md) (the steps, page progression, vertical text, the shaping
analysis) and [../text-shaping.md](../text-shaping.md); for CJK, [cjk.md](cjk.md). This file adds:
- the open items in a working order;
- the measurements behind them;
- how the device runs are done;
- the traps that cost time.

## Where things stand

**Merged:**
- Writing modes steps 0, 3 and 4 (vertical text).
- Step 1's PR 1 and PR 2 (#599, #613): bidi, Hebrew layout and drawing.
- Step 2: #603 the engine and #624 the TrueType faces; #642 Indic books and #645 the 20 Indic font families; #651
  HarfBuzz 14.5's Arabic shaper, #656 Arabic, Persian and Urdu books with NotoNaskhArabic and NotoSansArabic, and
  #657 the fixes from its review.
- Since the first handover (2026-11-08):
  - #660 and #672: the shaped text's heap (the store's index, Tier A, and a 12 KB free-heap floor); item 1.
  - #661 and #662: the right-to-left baseline (the plain-token path, the kerning memory beyond Latin); item 2.
  - #663: a CJK link of several characters can be tapped; item 7.
  - #664, #665, #666: reading a Hebrew book (page progression and reading order, the Noto Sans and Serif Hebrew
    families picked by language, UI titles drawn right to left); item 4, now in
    [the design record](../design/writing-modes.md#4-reading-a-hebrew-book-step-1-pr-3).
  - #667: a paragraph whose only Arabic code points are digits shapes nothing; item 5.
  - #668, #671, #673: Myanmar (HarfBuzz 14.5's Myanmar shaper, Burmese books in the Noto Myanmar families, lines
    broken between syllables); item 6.
  - #669: the glyph store remembers its recent answers in RAM; item 2.
  - #670: book titles borrow the last book's font whatever its script; item 8.
  - #674, #675: a layout pass remembers the parts of an inter-word space, and the split search stops a kind of
    prefix at its first one too wide; figures under "Where the time goes" below.
- CJK: every step of [cjk.md](cjk.md)'s roadmap.

**Branches worth knowing:**
- `wip/shaper-profile` (pushed): the `OT_PROFILE` instrumentation and the host profile test from PR 3, the starting
  point for item 3. Throwaway; never merge it.
- The `assets/pr-*` branches hold the images of merged PR descriptions. Deleting one breaks that PR's images.

## What is left, in the suggested order

### 1. The heap follow-up for shaped text: Tier B and C, and a size-aware gate

The store's index fix and Tier A (#660) and the 12 KB free-heap floor (#672) shipped. What failed on the X3, how it
was measured and what each change bought are in [text-shaping.md](../text-shaping.md#the-shaped-stores-heap). What is
left:
- **Tier B and C, if more room is ever wanted:** the store's chunks above the page block, rehomed below it at each
  page end (about 4 KB, some 250 lines, and on the X4 a span-heavy paragraph at a full page empties the word cache
  for that page); then the index as arena arrays too (the whole store, 9.5 KB, off the heap). The design notes are
  under "Why the rest does not go into the lent arena" below. The realistic case, a tajweed book at body size, holds
  blocks of 96-144 tokens and a store of about 4-6 KB, like Zaydan's, and never needed either.
- **The paragraph-layout gate does not know the paragraph's size:** before Tier A the stress paragraph took the heap
  8.7 KB below the last gate reading, 3 KB under the hard floor, with nothing refused. A gate that adds the block's
  token count times a per-token cost would degrade predictably instead of by luck. Not built.

**Why the rest does not go into the lent arena** (the handover's two ideas, worked out against §8.2 of the memory
strategy; now its §8.5, rows 17 and 18):
- A slab reserved at the parse lane's start lifts every user above it. The word cache needs 32 KB free to arm: the
  X3's 41,428 B at the parse's start allows a slab of 8.6 KB, the X4's 37,156 B one of 4.3 KB. The 32 KB image walk
  stage fits while the open page block is at most 8,116 - S (X3) and 3,844 - S (X4) bytes: it leaves the X4 for any
  slab. The cache's tail moves down by S, so a TrueType miss at a full page (about 30.5 KB) empties it from
  S = 5.3 KB (X3) and 1.1 KB (X4), and it re-arms only between pages. The store the stress chapter needs is 10 KB.
- The free tail under the word cache is for data that may be lost. A CFF miss at a full page reaches 30.5 KB, above
  a store tail at 27.7 KB (X3) or 23.6 KB (X4): every such miss would throw the paragraph's records away mid-pass.
- What would work: chunks above the page block, rehomed below it at each page end, since they grow only while a
  shaped paragraph is in flight. It needs the chunks' arena offsets, a rehome inside `releasePageBlock()` before any
  other allocation, the line's piece views made after `beforeLine_` (they point into the chunks), and every live
  `ParsedText` told (the repeat header's cells keep theirs across pages). That buys the 4 KB of chunks, unless the
  index becomes arena arrays too. Not built.

### 2. Profile the right-to-left baseline (profiled and half done; the measure is what is left)

On the X3, Zaydan's Arabic chapter against its Latin twin:

| Build | Arabic (ms) | Latin twin (ms) | Ratio |
|---|---|---|---|
| master, bidi only, no shaping | 5,586 | 2,517 | 2.22x |
| With shaping (#656) | 7,880 | 2,489 | 3.17x |

So about 3 s of Arabic's 5.4 s excess is NOT the shaper. It is the bidi and TrueType path that Hebrew shares.

**Profiled (2026-11-08).** Hebrew is that path whole, so the timing book is now Bialik's *Aryeh Baal Guf*
(`tools/writing-modes-bench/make_bialik_books.py`, 2,511 words, with its Latin twin; `/test/test_bialik_{he,en}.epub`
on the X3's card). The host at -O3 reproduces the ratio (2.0x), and gprof's call counts (perf is locked on that
machine, `perf_event_paranoid=4`) showed the first two levers: in a paragraph that needs bidi the width pass ran
`BidiLine::tokenForm`, a UAX#9 pass, on every token on its own (2,686 passes a chapter against the 293 lines'), and
`placeBidiLine` measured every piece a second time (5,319 measures for 2,632 tokens). A device profile (throwaway
timers on branch `wip/layout-profile`, printed as `LAYOUTPROF`; build with `-DLAYOUT_PROFILE=1`) split the Hebrew
chapter's 2,980 ms parse in NotoSansHebrew into width pass 305 (forms) + 500 (measure), line pass 466, place 587 (287
of it measuring), emit 395 ms, against the Latin twin's 190 (measure) + 220 (emit) of 1,245.

**Done (`perf/rtl-baseline`, two commits):** a plain token takes a short path. Letters of one strong class, with
unmirrored punctuation among them when the letters run the paragraph's way (then N1 or N2 resolves the punctuation
that way in any line), and no control open: its form is its code points reversed, no pass, and the placed line
keeps the width pass's measure (`plainForm_`). Two thirds of Hebrew prose tokens are plain with letters alone, nine in
ten with the punctuation. Layout is byte-identical on every right-to-left golden and on the timing books. X3, cold,
median of three:

| | master | branch |
|---|---|---|
| Hebrew drawn by the fallback face from BookerlyTTF (the reader picks no family for Hebrew yet) | 5,531 ms, 2.13x of the twin's 2,600 | 4,918 ms, 1.89x |
| Hebrew in NotoSansHebrew as the body font | 4,680 ms, 1.81x of 2,592 | 4,244 ms, 1.66x of 2,557 |

**What is left of it, by size:**
- **The TrueType measure.** The renderer's split (same profile build): kerning 325 ms for the Hebrew chapter's 15,668
  glyphs against 80-150 for the twin's 16,616, the glyph lookups 182 against 90, `beginText` 14. A Hebrew kerning pair
  costs three to four times a Latin one and a Hebrew glyph twice; both go through the glyph store's flash index
  (`FlashGlyphStore::findPair`, `find`: a Fibonacci-hashed, linearly probed table read from mapped flash, then the
  record's header) and, for a pair the index lacks, the GPOS reader. The profile's store counters found the pairs:
  4,300 of the chapter's 19,000 kerning lookups went to the GPOS reader on every build in NotoSansHebrew, and 17,000
  of 20,000 drawn by the fallback face, because `TtfFont::Style`'s memory of left glyphs that kern with nothing (whose
  pairs are never stored, spec 8.1) was a bit set ending at U+0250. Fixed by #662 (a 64-entry set above it, and a
  left code point the face lacks counts too): 4,680 to 4,044 ms in NotoSansHebrew, 5,531 to 5,100 by the fallback face. The glyph lookups
  (2.3 to 2.7 probes each for Hebrew against 1.9 for Latin, 182 against 90 ms) were what remained of the measure.
  `perf/glyph-lookup-cache` puts `find()`'s recent answers in RAM (two ways of 32 sets, 512 B while a family is
  loaded). It saves less than the 182 ms suggested: X3, warm store, median of three, 2026-11-09: Bialik Hebrew in
  NotoSansHebrew 3,747 to 3,678 ms, its Latin twin 2,657 to 2,616, Zaydan Arabic 7,655 to 7,588, Gaban Hindi 6,800 to
  6,721; a page's pre-render 62 to 59 ms. 128 entries saved at most 70 ms more a chapter (cold store), so 64 stay. The
  rest of the 182 ms is outside the index walk. Note for any benchmark: since #665 a Hebrew book picks
  NotoSansHebrew, so alternating it with its Latin twin reinstalls the glyph store at every switch (7.5-10.5 s, an empty
  store); run one book's builds in a row for warm figures.
- **The line pass** (466 ms, 1.2 ms a line): one UAX#9 pass per line, with every token's HebrewShaper walk repeated
  and the forms re-encoded. The walk could stop at tokens without a yod (the divine name is all it changes).
- **The form bytes.** A Hebrew line writes each word's display form beside its text (`WD` in the dump): 26,091 B
  against the twin's 15,983, and `emit` 395 ms against 220. A plain token's form is its text reversed and could be
  derived when drawn; that is a section format change.
- **The plain path itself** (305 ms for 2,574 tokens, 118 µs each): `bidi_class` per code point (two binary searches
  in flash), the HebrewShaper walk, `workspaceAvailable`'s mutex.

### Plan for what is left (2026-11-09)

Item 3 and two of item 5's gaps wait for a full code audit (the maintainer, 2026-11-08). What is
left, in the order proposed, each a PR of its own with a device run:

1. **The shaping engine's speed** (item 3), after the audit. The profile below puts the engine at about a third of
   an Arabic or Hindi chapter's build, and their processor work at 4.5x and 3.5x their Latin twins'.
2. **What the profile found beyond item 3** ("Where the time goes" below): the Hebrew line pass (the bidi class
   lookups and the reordering), the Liang patterns of the hyphenation search, and the SD reads of a chapter's open.
3. **The smaller Arabic gaps** (item 5) with the audit; Nastaliq measured first, flash and speed, before any decision.
4. **UI text** (item 8): the Hebrew letters in the built-in UI faces (flash not measured), and TXT and Markdown
   books' bidi with the parked TXT pipeline ([txt-pipeline.md](txt-pipeline.md)).
5. **Myanmar's follow-ups** (item 6).
6. **Flash** (item 9) as each PR needs it.

Not planned: Khmer and Tibetan (item 6; their own shaper modules), vertical shaped runs, the vertical limits of item
10, CJK's out-of-scope list; each waits for a request.

### Where the time goes: the X3 profile (2026-11-09)

**How it was measured.** A sampling profiler over the C3's own USB-JTAG: Espressif's OpenOCD (the version
`tool-openocd-esp32`'s tools.json names, downloaded, its sha256 checked) with `set ESP_RTOS none`,
`set ESP_FLASH_SIZE 0` and `board/esp32c3-builtin.cfg`, `reset run` once so the firmware boots with the debugger
attached (else its memory protection faults on the first stack read and the chip panics). The firmware was master
built with frame pointers in `Epub`, `EpdFont`, `GfxRenderer`, `OtShaper`, `MiniBidi`, `TextShaping`, `Utf8` (a
`library.json` with `build.flags`) and `src/` (`build_src_flags`); all of `lib/` does not fit the 0x640000 slot. A Tcl
procedure in OpenOCD halts the core, reads pc, ra, sp and fp, walks the frame pointers (reading aligned SRAM only),
and resumes, about 40 times a second. GDB's `bt` at each halt was tried first and gave stale frames: its inclusive
figures were wrong. Check the `.bin` against the slot before flashing such a build: PlatformIO's "used" figure is
about 14 KB under the image.

**Benchmark caveats found on the way.** The logged `createSectionFile ... total=` includes 0.7 to 1.4 s in which the
build waits for the first page's render and refresh, the same for every script: compare `parse=` or the per-page
rate (`[SCT] Page N processed`), not `total`. And since #665 a Hebrew book picks NotoSansHebrew, so alternating it
with its Latin twin reinstalls the glyph store at every switch (7.5-10.5 s, an empty store): run one book's builds in
a row for warm figures.

**What a reader waits for.** A chapter not yet built takes about 2.7 s from the tap to its first page, for every
script: the book's open (about 550 ms, of which SD reads are some 40%: the book 334 ms, the progress 112 ms, the
glyph store 90 ms), the "Indexing" popup's FAST refresh (618 ms, the processor idle), the first page's build (310 ms
Latin, 680-720 ms Arabic and Hindi) and its render and refresh (about 1.2 s). The rest of the chapter builds while
the first page is read.

**The build, after its first page** (share of samples, inclusive):

| Twin | Where |
|---|---|
| Latin | layout 58%: line extraction 21%, the line breaker 19% (its split search 14%: measuring the candidates 5.3%, the Liang patterns 4.5%), word widths 12%; the space's width 7.2% (a glyph lookup and its kerning per gap) |
| Hebrew | line extraction 42%: the bidi line pass 18%, its placement 14%; the reordering 10%; `bidi_class` alone 7.3% |
| Arabic | `shapeToken` 61%, the engine (`ot::shape`) 33% of it, GSUB and GPOS 26%; glyph prefetch 4.6%; line extraction 16% |
| Hindi | `shapeToken` 65%, the engine 32% (GSUB and GPOS 19%, positioning 7%, the reordering pause 4.6%) |

Processor work a chapter (the build less its wait, warm store, master 74b8fc803): Latin twin 1,452 ms (48 ms a page),
Hebrew 2,629 (1.8x), Arabic 6,920 (about 4.5x its twin), Hindi 5,393 (about 3.5x).

**What came of it:**
- **The space's width** (#674): a layout pass remembers the space glyph's advance per font and style
  and its kerning with each neighbour (560 B static), summed and snapped as `getSpaceAdvance()` does. X3, warm,
  median of three, `parse=`: Latin 1,220 to 1,151 ms (-5.7%), Hebrew 2,346 to 2,210 (-5.8%), Hindi 5,472 to 5,311
  (-2.9%), Arabic 6,341 to 6,275 (-1.0%); the chapter files the same size.
- **The split search** (#675): a kind of prefix stops at its first one too wide, measured in
  one string. Latin 1,220 to 1,211 ms (-0.7%, near the noise): only the candidates past the first too wide are
  skipped. The Liang patterns (4.5%) are what is left of the 14%.
- **Building the first page during the popup's refresh: blocked.** The released build borrows the displayed buffer
  (`FreeInkDisplay::borrowSecondaryBuffer` lends `frameBufferActive` and drains a refresh in flight first), and on
  the X3 that buffer holds the popup frame that the post-waveform DTM1 sync reads. The build cannot take its arena
  until the popup's waveform ends. What would help instead changes what the reader sees: no popup when the first page
  is about to land, or an open-time popup drawn before the book's own open work.

### 3. Speed up the shaping engine (helps Indic most)

**Hindi:** 2.61x on real prose by the logged totals (Premchand's Gaban: 6,908 against 2,648 ms on the X3; 51%
word-cache hits), but those include about a second of waiting in both: by processor work about 3.5x (5,393 against
1,616 ms; "Where the time goes" above).
- A word the cache misses costs about 2.5 ms, mostly the conjunct ligature walks in GSUB.
- The X3 profile proved the cost is flash-cache misses, not instructions: a word shaped twice costs the same cycles.

**Arabic:** shaping adds about 0.9 ms a word (28% cache hits, because of clitics and prefixes); by processor work
Zaydan's chapter is about 4.5x its twin (6,920 against 1,569 ms).

**Candidates:** HarfBuzz's accelerator digests, which skip a lookup or subtable that cannot match without reading its
flash-resident coverage table:
- the second-glyph lookup digests (in 14.5.0, never ported);
- LigatureSubst's seconds digest;
- the mark-set digests;
- a GDEF glyph-props cache.

**Not the lever:** a bigger word cache only gets Gaban to about 2.2x, because its 852 distinct words in 2,587 cap any
cache at 67%.

**The pass line** (design): 2x the Latin twin.

**Upstream since 14.5.0 (surveyed 2026-11-09):** nothing changes Indic, Arabic or Myanmar output. To take: #6544's
Indic table fix (U+11B0A, U+11DF0-11DF1) at the next table regeneration (`VERSION` in `gen_ot_shaper_tables.py`);
with any port of HarfBuzz's coverage caches (not only the digests), their fixes 2994c4d0a (#6321), b98ffb1fb (#6419)
and c3fd6a885 (#6453). Open: #6558 makes the 64-glyph syllable cut the same in every pass; our `forEachSyllable` has
none, and a call holds at most 32 code points. uharfbuzz 0.56.4 bundles HarfBuzz 15.0.0.

### 5. Smaller Arabic gaps (each documented in `docs/text-shaping.md`, "What is not shaped yet")

- **A soft hyphen** (U+00AD) inside an Arabic word ends the run and cuts the joining; HarfBuzz joins across it (type T).
  The fix: a transparent run member dropped from the shaper's input. It touches the run scan and the soft-hyphen
  break paths.
- **A word past 32 code points split by a style span** can lose or double a long ligature (the piecewise loop
  reaches 2 clusters). The fix: size the buffer for run + 2 x 16 for Arabic calls.
- **The soft hyphen and the long split word above** sit in the shaper's run scan and its buffer: left for the code
  audit that also takes item 3 (2026-11-08).
- **Ayah and number signs**: done; a number sign with its digits is a run of its own, drawn around them
  ([text-shaping.md](../text-shaping.md#the-run-scan), "Number runs").
- **Fonts without GPOS:** HarfBuzz's fallback mark placement and legacy `kern` are not ported.
- **The line breaker** measures a consumed letter at width 0 when its partner degraded: a known parity gap, rare.
- **Optional:** Noto Nastaliq Urdu, the style Urdu readers expect. A heavy font with complex cursive positioning;
  measure flash and speed first.

### 6. Indic

- **Speed:** item 3.
- **Scripts not covered:** Khmer and Tibetan, named by the design as targets of the same shaper (the USE-based
  scripts need their own shaper modules).
- **Myanmar** (`docs/superpowers/specs/2026-11-09-myanmar-design.md`), merged: the engine #668 (0 differences against
  HarfBuzz 14.5.0), Burmese books #671 (the Noto Sans and Serif Myanmar families, the pick by language, the shaping
  glue, the overflow cut between syllables) and line breaks between syllables #673 (the design's option H). X3: a
  Burmese Genesis chapter in NotoSerifMyanmar, shaped, 15 pages with the breaks (16 without). Credit:
  Somnius/CrossInk-Indic (Lef) had the same shaper port. Later: Stribley's preference for a space near the line end,
  the justified-space cap, a dictionary, ZWSP breaks, `CMD:TTF:SHAPE` for Myanmar, Mon and Shan pages shaped under
  their own language systems, Zawgyi (out of scope: the user guide says to convert first).
- **Vertical sections:** a vertical section reorders Indic words only; no shaped run is set down a column.
- **No X4 figures for #642** (waived).

### 7. CJK (essentially complete)

**Out of scope so far** (each a candidate plan; [cjk.md](cjk.md) "Out of scope so far"):
- CSS `line-break`/`word-break`;
- `lang` attributes inside a book;
- hanging punctuation;
- ruby below the text and `<rtc>`;
- Chinese emphasis marks below the text;
- hyphenating Latin words inside CJK books.

### 8. Step 5: UI text

- **UI titles and the TOC:** reordered since #666
  ([the design record](../design/writing-modes.md#4-reading-a-hebrew-book-step-1-pr-3)), but not shaped: the UI faces are bitmap,
  so Arabic and Indic titles draw unjoined. A character the UI faces lack comes from a TrueType face
  (`CjkUiFonts.cpp`, `readerFaceAt`): in the reader the book's, outside it the last book's family, opened read-only by
  `openUiBookFonts` in `main.cpp` on the screens that draw book text. That opener took a family with CJK only, so a
  Hebrew or Arabic family never backed the Library rows (X3, 2026-11-09: the Details view of `/test`, NotoSansHebrew
  as the reader font, drew the Hebrew notes book's title blank). Since `feat/ui-book-fonts-any-script` it opens the
  family whatever its scripts: a character it lacks falls through to the UI faces. What is left: it is still the last book's family for every row (only that set is in the glyph store, and
  opening another would rewrite it), so a Hebrew title shows only after a book whose font has Hebrew; giving the
  built-in UI faces the Hebrew letters (flash cost not measured) would show it always; Arabic and Indic titles would
  still need shaping, which bitmap faces cannot do.
- **TXT and Markdown books:** they get no bidi.
- **The settings labels of vertical text and page progression** (Vertical Text, Automatic, Page Turn Buttons, Follow
  Book, Fixed: `STR_VERTICAL_TEXT`, `STR_AUTOMATIC`, `STR_PAGE_TURN_BUTTONS`, `STR_FOLLOW_BOOK`, `STR_FIXED`) exist only
  in `lib/I18n/translations/english.yaml`, so every other UI language shows them in English until a translation pass.

### 9. Flash

Master at 89ef34730 (2026-11-09, #673 merged) builds `default` to 6,329,273 B of the 6,553,600 B slot (224,327 B
free, by PlatformIO's count; the image itself is about 14 KB larger). Candidates to save it, from the reviews:
- one shared controls/joiners filter (150-300 B);
- fold `viewWithoutControls` into `viewShapedPieces(..., true)` (300-400 B);
- `ShapingState::piecesOf` out of line, about 7 call sites;
- one soft-hyphen helper for `formBidiPieces` and `viewShapedPieces`;
- a shared `LinePieces` struct for `BidiState`'s and `ShapingState`'s vectors.

### 10. Vertical text: limits with no next step

Step 3 shipped vertical text with these limits, and step 4 left them
([the design record](../design/writing-modes.md#5-vertical-text) has what vertical text does):
- **`text-orientation`** is parsed and cached (`CssStyle::textOrientation`) and never read: every character takes its
  UAX #50 class (CSS `mixed`).
- **`vertical-lr` books are set horizontally** under Automatic; only `vertical-rl` has a turned frame.
- **Drop caps are off in a vertical section** (`ChapterHtmlSlimParser::tryStartDropCapCapture`); their ink metrics are
  horizontal.
- **No vertical forms**, and the wave dashes stay rotated: their vertical form is a mirror image and the renderer
  cannot mirror a glyph (step 4 design: not built).

## How the work was done

**Process.**
- Each step is a design (`docs/superpowers/specs`), then a plan (`docs/superpowers/plans`; finished plans stay), then
  subagent-driven tasks with a review each.
- Then a whole-branch review and a device run.
- The PR is opened when the work is validated; the maintainer merges.
- PR titles are conventional (`feat(layout): ...`), because CI's Title Check fails otherwise.
- Images go on an orphan `assets/pr-<topic>` branch, linked via `raw.githubusercontent.com`.

**Builds and tests (Windows):**
- **Firmware:**
  - `PLATFORMIO_CORE_DIR=C:\pio` (a short path, against the Windows command-line limit), with `PYTHONUTF8=1` and
    `PYTHONIOENCODING=utf-8`;
  - `pio run -e default` and `-e gh_release`;
  - one pio run at a time, and never edit sources under a running build;
  - cppcheck is `pio check -e default` (about 35 min);
  - the S3 environments are left to CI.
- **Host tests:**
  - MSYS2 UCRT64's toolchain on PATH (`/c/msys64/ucrt64/bin` from Git Bash);
  - `cmake --build build/test -j 8 -- -k 0`, then `ctest --test-dir build/test -j 8`;
  - five tests always fail on Windows only: FirstChapter x2, LongChapterPageTables x2, KeptReadingState;
  - `epub_build_inventory` fails to build on MinGW.
- **clang-format 21.1.8** with `--dry-run --Werror` on every changed `.cpp`/`.h`; CI checks it.
- **Font and oracle scripts** pin fontTools 4.63.0 and uharfbuzz 0.56.2. `make_shaped_book_fonts.py` refuses another
  fontTools.

**The timing books.** Never committed; `tools/writing-modes-bench/` has the generators and how to fetch their sources.

**Devices** (`docs/serial-test-api.md`, `tools/devctl.py`):
- **Flashing:**
  - esptool `write-flash 0x0 <factory.bin>`, always with `PYTHONUTF8=1 PYTHONIOENCODING=utf-8`; without them it dies
    after the erase;
  - after every flash, copy the build's `web-<hash>.bin` to the card root, and never remove `/web-*.bin`;
  - on a device with older firmware, back up first: `read-flash` in 256 KB blocks, retrying a block with `--no-stub`,
    then `verify-flash`.
- **Fonts on the card:** families are discovered only at boot (or after a Font Manager install), so put fonts on the
  card BEFORE flashing.
- **X3:**
  - never pulse RTS by hand, and never send `CMD:REBOOT`, `CMD:SLEEP` or `POWER:LONG`;
  - it auto-sleeps after about an hour idle on Home (its COM port vanishes); a short Power press wakes it.
- **Restoring:** read `CMD:GET:*` before a run and compare it after.
- **Test books:** use the ones in `/test`. A book's cache is `/.crosspoint/epub_<murmur2 of its path>/`; wipe its
  `spines/*/*.bin` for a cold build.

**Traps that cost time:**
- **A `std::string_view` bound to a temporary** (`IndicShaper::Request::before`/`after` are views) passes the Windows
  suite and fails only in Linux CI: glibc overwrites the freed chunk's first 16 bytes.
- **CI jobs failing in about 90 s** at "Install PlatformIO Core" or the unit tests' "Configure" are github.com download
  flakes. Use `gh run rerun <id> --failed` after the run completes.
- **Before every push to a PR branch,** check the PR is still open:
  `[ "$(gh pr view <n> --json state -q .state)" = OPEN ] && git push`. A push to a merged PR is lost.
- **Heredoc-written scripts** turn `\n` and `\u` escapes into real characters; write files with an editor tool
  instead.
- **Never put literal bidi controls** (U+200E/F, U+202A-202E, U+2066-2069, U+061C) **or ZWJ/ZWNJ** in sources, commit
  messages or generated data; write code points. Goldens are the exception: they hold laid-out bytes.
- **`SECTION_FILE_VERSION`** collides with master often (#656 is 115); bump it after merging master.

**Claude's memory.** This machine's notes are in
`C:\Users\jens\.claude\projects\c---development-witchhunt-reader\memory\`. Copying that folder to the same place on
the other computer carries them. The folder name follows the repository's path, so keep the same path or rename the
folder to match.
