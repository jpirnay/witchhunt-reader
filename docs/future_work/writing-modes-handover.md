# Writing modes and scripts: handover (2026-11-08)

This is the state of the right-to-left, Arabic, Indic, Hebrew and CJK work, and what is left, written so that the work
can continue on another computer. The roadmap itself is [writing-modes.md](writing-modes.md) and, for CJK,
[cjk.md](cjk.md). This file adds:
- the open items in a working order;
- the measurements behind them;
- how the device runs are done;
- the traps that cost time.

## Where things stand

**Merged:**
- Writing modes steps 0, 3 and 4 (vertical text).
- Step 1's PR 1 and PR 2 (#599, #613): bidi, Hebrew layout and drawing.
- Step 2 complete, but for speed:
  - #603 the engine and #624 the TrueType faces;
  - #642 Indic books, and #645 the 20 Indic font families;
  - #651 HarfBuzz 14.5's Arabic shaper, and #656 Arabic, Persian and Urdu books with the NotoNaskhArabic and
    NotoSansArabic families.
- CJK: every step of [cjk.md](cjk.md)'s roadmap.

**Open (when this was written):**
- #657 `fix/arabic-review-findings`: the fixes for the Fable review of #656, re-reviewed by Fable.
  - the bidi fallback splitting an interrupted segment;
  - a consumed letter drawn as text taking its width;
  - Arabic shaping only with a face that maps beh;
  - the fallback's piece bound;
  - the doc corrections.
- This file's own PR, which also adds the timing-book generators in `tools/writing-modes-bench/`.
- #660 `fix/shaped-store-heap` (2026-11-08): item 1 (the shaped store's index, the word arrays' cap, the line pass's
  buffers), X3 validated.
- `perf/rtl-baseline` (2026-11-08): item 2's plain-token short path, X3 validated; its PR follows #660.

**Branches worth knowing:**
- `wip/shaper-profile` (pushed): the `OT_PROFILE` instrumentation and the host profile test from PR 3, the starting
  point for the speed work below. Throwaway; never merge it.
- The `assets/pr-*` branches hold the images of merged PR descriptions. Deleting one breaks that PR's images.

## What is left, in the suggested order

### 1. The heap follow-up for shaped text (next)

**What fails.** On the X3 in landscape, the test book `test/epubs/test_shaped_arabic.epub` chapter 2 degrades. That
chapter holds a 400-token paragraph (every Allah split by a span) whose lines hold more than 96 tokens, followed by a
table.
- Background-C runs low.
- The released blocking rebuild demotes the table row, and the chapter is cached with the status bit set.
- The heap's low point is 2,936 B; master builds it clean.

Real prose passes in both orientations: Zaydan's chapter, Background-B's lowest heap 29,204 B in landscape. A
tajweed-coloured Quran (a span on every word) is the realistic case.

**Why.** Measured on the host (`epub_pipeline_dump --bench --arena=52272` with a temporary viewport width of 770 px):
- the shaped build holds about 16 KB more heap than the unshaped one;
- all of it is `ShapedStore` chunks (11,776 B live at the peak), on the heap;
- the lent arena peaks at 34 KB of its 52,272 B.

**What to build:**
- **`ShapedStore`'s chunks in the lent arena's free room.**
  - Read `docs/memory-allocation-strategy.md` first: section 8.2, the arena timeline.
  - The arena is LIFO, and the open page block sits above everything reserved at a lane start, so a per-paragraph
    store cannot simply `alloc` there. The "opportunistic tail" (data kept in `[cap - S, cap)`, validated by a
    peak-since-mark watermark) is how PR 3's word cache solved the same problem; the store needs a variant of it, or a
    reserved slab at the parse lane's start.
- **Guard every reserve the bidi line pass makes**, with `heapAllows`, taking the existing "laid out without bidi"
  path when one does not fit. These run unguarded since step 1 PR 2:
  - `ParsedText::layOutBidiLine`'s `state.tokens.reserve`;
  - `BidiLine`'s `reserveAtLeast` on the Line's pieces and display, in `buildPieces` and `fallbackLine`.

  The Fable re-review measured the fallback's worst case at about 4.8 KB for a 300-code-point line where every
  token is cut.
- **The pass line:** the test book's landscape chapter 2 builds clean on the X3, and no golden changes.

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
  pairs are never stored, spec 8.1) was a bit set ending at U+0250. Fixed on `perf/ttf-uncovered-left` (a 64-entry
  set above it, and a left code point the face lacks counts too); its PR has the X3 figures. The glyph lookups
  (2.3 to 2.7 probes each for Hebrew against 1.9 for Latin, 182 against 90 ms) are what remains of the measure: a
  small RAM cache in front of `find` would serve every TrueType measure and draw for about 512 B while a family is
  loaded.
- **The line pass** (466 ms, 1.2 ms a line): one UAX#9 pass per line, with every token's HebrewShaper walk repeated
  and the forms re-encoded. The walk could stop at tokens without a yod (the divine name is all it changes).
- **The form bytes.** A Hebrew line writes each word's display form beside its text (`WD` in the dump): 26,091 B
  against the twin's 15,983, and `emit` 395 ms against 220. A plain token's form is its text reversed and could be
  derived when drawn; that is a section format change.
- **The plain path itself** (305 ms for 2,574 tokens, 118 µs each): `bidi_class` per code point (two binary searches
  in flash), the HebrewShaper walk, `workspaceAvailable`'s mutex.

### Plan for what is left (2026-11-08)

Items 1 and 2 are done to their measured lines (#660; `perf/rtl-baseline` and `perf/ttf-uncovered-left`, the kerning
fix found under item 2, which helps every non-Latin TrueType text). The rest, in the order proposed, each a PR of its
own with a device run:

1. **The CJK link tap** (item 7's open bug): small, host-testable in the LinkMarkerMatch tests, user-visible.
2. **The shaping engine's speed** (item 3): HarfBuzz 14.5's accelerator digests ported into `lib/OtShaper` (the
   second-glyph lookup digests, LigatureSubst's seconds digest, the mark-set digests, a GDEF glyph-props cache),
   measured on the X3 with Gaban (6,908 against 2,648 ms) and Zaydan shaped (7,880 against 2,489), the `OT_PROFILE`
   branch's counters telling how many subtables a digest skips. The pass line is 2x the Latin twin. The biggest
   win left for Indic and Arabic readers, and the largest piece of work.
3. **Reading a Hebrew book** (item 4): page progression from the book's direction (Hebrew and Arabic books turn left
   to right today), the reading order in the QR text and the footnote overlay, titles given a visual form once,
   and the two Hebrew families merged with Latin. The measurements under item 2 add a reason: until the reader picks
   a family for Hebrew, a Latin body font's fallback face draws it, 15 to 20 percent slower.
4. **What item 2 left**, as the measurements of item 2 are repeated for item 3 anyway: a RAM cache in front of the
   glyph store's lookups (a Hebrew glyph lookup costs twice a Latin one, 182 against 90 ms a chapter), the line pass's
   repeated HebrewShaper walk (a yod test first), and the plain path's own cost; the form bytes a Hebrew line writes
   are a section format change and wait for a reason beyond 180 ms.
5. **The smaller Arabic gaps** (item 5), one PR for the layout ones (the soft hyphen inside a word, the long word
   split by a span, the digit-only paragraph's shaping calls) and the X4 figures taken with it; Nastaliq measured
   first, flash and speed, before any decision.
6. **UI text** (item 8): the bitmap UI faces give titles and the TOC no bidi and no shaping; and TXT and Markdown
   books no bidi. Worth a design note before code: the UI faces are bitmaps by choice.
7. **Flash** (item 9) as each PR needs it: 235 KB free after #656, the candidates listed there.

Not planned: Khmer, Myanmar and Tibetan (item 6; their own shaper modules), vertical shaped runs, CJK's out-of-scope
list; each waits for a request.

### 3. Speed up the shaping engine (helps Indic most)

**Hindi:** 2.61x on real prose (Premchand's Gaban: 6,908 against 2,648 ms on the X3; 51% word-cache hits).
- A word the cache misses costs about 2.5 ms, mostly the conjunct ligature walks in GSUB.
- The X3 profile proved the cost is flash-cache misses, not instructions: a word shaped twice costs the same cycles.

**Arabic:** shaping adds about 0.9 ms a word (28% cache hits, because of clitics and prefixes).

**Candidates:** HarfBuzz's accelerator digests, which skip a lookup or subtable that cannot match without reading its
flash-resident coverage table:
- the second-glyph lookup digests (in 14.5.0, never ported);
- LigatureSubst's seconds digest;
- the mark-set digests;
- a GDEF glyph-props cache.

**Not the lever:** a bigger word cache only gets Gaban to about 2.2x, because its 852 distinct words in 2,587 cap any
cache at 67%.

**The pass line** (design): 2x the Latin twin.

### 4. Step 1 PR 3: reading a Hebrew book (never started)

The scope is the step 1 design (`docs/superpowers/specs/2026-11-02-writing-modes-step1-design.md`, section "The
reader"):
- **Page progression:** `FlowContext::pageProgressionRtlFor` must take the book's direction. Today Hebrew AND Arabic
  books turn left to right: `CMD:STATE` shows `progression=ltr` for them.
- **Reading order** in the consumers: the QR page text and the footnote-link overlay. The dictionary's reading-order
  join is done (#656).
- **The reader's titles:** the status bar's chapter title and the Home and Library book titles get a visual form
  once, when set (`applyBidiVisual`). Today a Hebrew title shows mirrored.
- **Fonts:** two Font Manager families, Noto Sans Hebrew and Noto Serif Hebrew, merged with Noto Sans/Serif Latin, as
  `lib/EpdFont/scripts/merge_noto_latin.py` does for Indic and Arabic. New family names, never a re-cut: the flash
  font cache is keyed on family and size.
- **Upstream:** crosspoint-reader#4295 (minibidi L4 mirroring order) was open.

### 5. Smaller Arabic gaps (each documented in `docs/text-shaping.md`, "What is not shaped yet")

- **A soft hyphen** (U+00AD) inside an Arabic word ends the run and cuts the joining; HarfBuzz joins across it (type T).
  The fix: a transparent run member dropped from the shaper's input. It touches the run scan and the soft-hyphen
  break paths.
- **A word past 32 code points split by a style span** can lose or double a long ligature (the piecewise loop
  reaches 2 clusters). The fix: size the buffer for run + 2 x 16 for Arabic calls.
- **A bidi paragraph whose only Arabic code points are digits**, in a face that maps Arabic, pays a shaping call per
  token. The output is near-identical: glued groups merge.
- **Ayah and number signs** (U+06DD and the others) do not enclose their digits.
- **Fonts without GPOS:** HarfBuzz's fallback mark placement and legacy `kern` are not ported.
- **The line breaker** measures a consumed letter at width 0 when its partner degraded: a known parity gap, rare.
- **No X4 figures for #656**, including the in-place gate line `heapAllowsInPlaceBuild=0 ... shapedTtf=1`.
- **Optional:** Noto Nastaliq Urdu, the style Urdu readers expect. A heavy font with complex cursive positioning;
  measure flash and speed first.

### 6. Indic

- **Speed:** item 3.
- **Scripts not covered:** Khmer, Myanmar and Tibetan, named by the design as targets of the same shaper (Myanmar and
  the USE-based scripts need their own shaper modules).
- **Vertical sections:** a vertical section reorders Indic words only; no shaped run is set down a column.
- **No X4 figures for #642** (waived).

### 7. CJK (essentially complete)

**An open bug:** a link whose text is longer than one CJK character cannot be tapped.
- **Cause:** `src/components/LinkMarkerMatch.h` `consumeToken` splits the marker at spaces only, while CJK is stored
  one character per word.
- **Fix sketch:** let a word consume a prefix of the current marker token when glued to the next. Host-test it in the
  LinkMarkerMatch tests.

**Out of scope so far** (each a candidate plan; [cjk.md](cjk.md) "Out of scope so far"):
- CSS `line-break`/`word-break`;
- `lang` attributes inside a book;
- hanging punctuation;
- ruby below the text and `<rtc>`;
- Chinese emphasis marks below the text;
- hyphenating Latin words inside CJK books.

### 8. Step 5: UI text

- **UI titles and the TOC:** they get no bidi and no shaping, because the UI faces are bitmap. Arabic and Indic
  titles draw unjoined and unreordered.
- **TXT and Markdown books:** they get no bidi.

### 9. Flash

Since #656, `default` is 6,314,579 B, against the 6,318,600 B floor (235,000 B free). Candidates to save it, from the
reviews:
- one shared controls/joiners filter (150-300 B);
- fold `viewWithoutControls` into `viewShapedPieces(..., true)` (300-400 B);
- `ShapingState::piecesOf` out of line, about 7 call sites;
- one soft-hyphen helper for `formBidiPieces` and `viewShapedPieces`;
- a shared `LinePieces` struct for `BidiState`'s and `ShapingState`'s vectors.

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
