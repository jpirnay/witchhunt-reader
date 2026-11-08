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

### 2. Profile the right-to-left baseline (cheap, helps Hebrew and Arabic)

On the X3, Zaydan's Arabic chapter against its Latin twin:

| Build | Arabic (ms) | Latin twin (ms) | Ratio |
|---|---|---|---|
| master, bidi only, no shaping | 5,586 | 2,517 | 2.22x |
| With shaping (#656) | 7,880 | 2,489 | 3.17x |

So about 3 s of Arabic's 5.4 s excess is NOT the shaper. It is the bidi and TrueType path that Hebrew shares, and it
has never been profiled.

Candidates to check:
- the bidi line pass per line (`BidiLine::layoutLine`, the Scratch);
- word-width measurement of non-Latin glyphs through the glyph store;
- kerning lookups;
- UTF-8 decoding.

The section file is also larger: 27,360 B against 16,042 B.

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
