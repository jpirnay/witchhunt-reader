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
- #657 `fix/arabic-review-findings`: the fixes for the Fable review of #656, re-reviewed by Fable (merged 2026-11-08).
  - the bidi fallback splitting an interrupted segment;
  - a consumed letter drawn as text taking its width;
  - Arabic shaping only with a face that maps beh;
  - the fallback's piece bound;
  - the doc corrections.
- This file's own PR (#658, merged), which also adds the timing-book generators in `tools/writing-modes-bench/`.
- `fix/shaped-store-heap` (2026-11-08): item 1, below; X3 run done, no PR yet.

**Branches worth knowing:**
- `wip/shaper-profile` (pushed): the `OT_PROFILE` instrumentation and the host profile test from PR 3, the starting
  point for the speed work below. Throwaway; never merge it.
- The `assets/pr-*` branches hold the images of merged PR descriptions. Deleting one breaks that PR's images.

## What is left, in the suggested order

### 1. The heap follow-up for shaped text (done on the X3; the X4 figures and one policy open)

**What failed.** On the X3 in landscape, the test book `test/epubs/test_shaped_arabic.epub` chapter 2 degraded. That
chapter holds a 400-token paragraph at a quarter of the body size (every Allah split by a span) whose lines hold more
than 96 tokens, followed by a table. Master at 774e40dfc (with #656), cold cache, Landscape CW, 2026-11-08:
- Background-C's borrowed build reads 12,220 B free before the stress paragraph, then 9,188 free and 6,388 largest at
  the table row: the row is demoted and the parse aborts.
- The released blocking rebuild, from 97 KB free, is down to 16 KB by the long-block splits (the whole build is on the
  heap there) and aborts at the table too, at 11,292 free and 4,084 largest. The chapter is cached degraded AND
  truncated: 2 pages of 4, the table and the stress paragraph's tail missing, and the one rebuild per session fails
  the same way.
- The allocator's minimum since boot after the open: 6,508 B.

Real prose passes in both orientations: Zaydan's chapter, Background-B's lowest heap 29,204 B in landscape. A
tajweed-coloured Quran (a span on every word) is the realistic case.

**What the measurement showed (2026-11-08, Linux host).** `epub_build_inventory` now takes `--shaping-font=FILE`
(and `epub_pipeline_dump --viewport=WxH`), so a shaped build's heap and arena can be inventoried the way the memory
audit's were:

```
build/test/epub_pipeline/epub_build_inventory test/epubs/test_shaped_arabic.epub <cache> <out> \
    --arena=52272 --viewport=770x300 --shaping-font=NotoNaskhArabic-Book-Regular.ttf
python3 test/epub_pipeline/inventory_report.py build/test/epub_pipeline/epub_build_inventory <out>/spine_1.txt
```

At the build's heap peak of chapter 2 (the sizes below are the device's; the host's totals run 1.3-1.6x high on
object-holding sites):

| Site | Before | After |
|---|---:|---:|
| `ShapedStore` entries (32 B, doubled to 256; now 16 B, reserved to the pass's count) | 9,216 | 2,416 |
| `ShapedStore` pieces (16 B; now 14 B) | 4,608 | 3,038 |
| `ShapedStore` chunks | 4,096 | 4,096 |
| Heap above the build's base, host (unshaped: 51,869) | 81,510 | 73,203 |

So the handover's premise was wrong: three quarters of the store's 16.5 KB was its index, not its chunks. The fix on
`fix/shaped-store-heap` (this branch) shrinks the index: a 16 B entry (the text hash folded to 32 bits, halfword
length, begin and count), a 14 B piece, `reserve()` exact and the growth past it by half again. Real prose moves
little: the long Hindi chapter (`make_test_shaped_indic.py --long`, portrait) peaks 1.3 KB lower and costs under 1 KB
more shaped than unshaped. The stress chapter's store is now about 10 KB on the device (5.4 KB index, 4 KB chunks,
the state).

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

**Also done on the branch** (the second bullet of the old plan): every reserve the bidi line pass makes is behind
the heap check (`HeapGrowth.h`, one helper for `ParsedText`, `ShapedStore` and `BidiLine`): `layOutBidiLine`'s
token vector, and `BidiLine`'s Line buffers in `buildPieces` and `fallbackLine`, the fallback's word copies
included. Refused, the line takes the existing "laid out without bidi" path. Tests: `ShapedStore.ReserveTakes...`,
`ShapedStore.TheIndexStaysWithinItsHalfwordBounds`, `BidiLine.ALineTheHeapRefuses...`,
`BidiLine.AFallbackLineTheHeapRefuses...`, `ShapedLayout.ALineWhoseTokensTheHeapRefuses...`. The goldens are
unchanged (no layout changes).

**The X3 run (2026-11-08, Landscape CW, cold cache; the branch at ac63df86e against master 774e40dfc, both built
from a worktree with the full serial API, each written to the idle OTA slot and driven with `tools/devctl.py`:
`ACT:READER:/test/test_shaped_arabic.epub@2:1`):**

| | master | `fix/shaped-store-heap` |
|---|---|---|
| Background-C's borrowed build | aborts at the table row (9,188 free, 6,388 largest) | complete, 4 pages |
| Lowest gate reading in it | 9,188 free / 6,388 largest | 14,484 free / 11,252 largest, before the stress paragraph |
| Released rebuild | aborts too; cached degraded, 2 pages | none |
| Arena high water, borrowed build | 33,466 B | 31,618 B |
| Allocator minimum since boot, after the open | 6,508 B | 5,824 B |
| The chapter as cached | 2 pages, table missing | 4 pages, the table on page 4 |

The build is 1.8 KB lighter at the gate, and that is the difference between the hard floor and a clean build. No
`shaping degraded`, no demoted row, no released rebuild; a warm open after a reboot drew all four pages (the stress
paragraph on page 3, the table on page 4). The allocator's minimum is the build's, not the render's or the book
index's: a warm open after a reboot left the boot's 17,520 B minimum at 15,416, and a section-only rebuild (book.bin
kept, `spines/` removed, after a reboot) ended at 5,824 B again. So between the last gate reading and the end of the
stress paragraph the heap goes about 8.7 KB deeper, 3 KB under the hard floor, with nothing refused: the pass line
is met on the X3 with about 5.8 KB to spare. Logs and screenshots:
`~/.local/state/witchhunt-device-backups/x3-20261108-shaped-heap-state/runs/`.

**Tier A (2026-11-08, four commits on the same branch), measured the same way.** 5.8 KB was not much, and the host
inventory showed what else the stress paragraph held at the peak besides the store: the word arrays doubled to 256
entries for a block the parser splits at 175; the bidi fallback copied every token's text into a kept vector of
strings; the line's token array, which only the pass reads, was a kept heap vector; and all of it stayed until the
slice's yield, in front of the table. So: the word arrays stop at the parser's split capacity (146 entries, 3.6 KB on
the device); the fallback hands views and `computeVisualWordOrder` skips the marks itself (2 KB); the tokens come with
the pass's arena workspace (2 KB); and a paragraph's end gives back line buffers over 2 KB and a store over 8 KB.

| | index fix only (ac63df86e) | Tier A (8c7fe2422) |
|---|---|---|
| Host inventory, heap above the build's base at the peak | +73,203 | +62,627 |
| Borrowed build on the X3 | complete, 4 pages | complete, 4 pages |
| Lowest gate reading in it | 14,484 free / 11,252 largest | no gate reached |
| Allocator minimum, section build alone after a reboot | 5,824 B | about 19.8 KB |
| Arena high water, borrowed build | 31,618 | 26,000 |

The Tier A minimum had to be read with 8 KB of ballast held (`CMD:HEAP:HOLD=8000` before the open), because the build
no longer reaches the boot's own minimum of 15.4 KB: 11,824 B with the ballast, 11,496 B for the cold open with the
book index included (19.5 KB). Layout is unchanged on the host (goldens) and the device (4 pages, the table on the
last).

**What is left:**
- **The X4 figures** (no X4 was attached): 4,272 B less arena (the borrowed build's 26,000 B high water leaves 22 KB
  of it) and 8.5 KB more heap, so it should pass with more room than the X3; its figures for #656 were never taken
  either.
- **Tier B and C, if more room is ever wanted:** the store's chunks above the page block, rehomed below it at each
  page end (about 4 KB, some 250 lines, and on the X4 a span-heavy paragraph at a full page empties the word cache
  for that page); then the index as arena arrays too (the whole store, 9.5 KB, off the heap). The design notes are
  under "Why the rest does not go into the lent arena" above. The realistic case, a tajweed book at body size, holds
  blocks of 96-144 tokens and a store of about 4-6 KB, like Zaydan's, and never needed either.
- **The paragraph-layout gate does not know the paragraph's size:** before Tier A the stress paragraph took the heap
  8.7 KB below the last gate reading, 3 KB under the hard floor, with nothing refused. A gate that adds the block's
  token count times a per-token cost would degrade predictably instead of by luck. Not built.
- **Policy, open:** `ShapedStore::heapAllows` and the layout's `heapAllows` check the largest block only, with no
  floor, so a store can take the heap down to Background-C's abort. A floor there would turn that into a latched
  `shaping degraded` (unjoined Arabic, rebuilt once) instead of a released rebuild: a visible defect against an
  invisible one with a fragmented session, which is the maintainer's call. Not changed.

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

### 4. Step 1 PR 3: reading a Hebrew book (page progression and reading order done)

The scope is the step 1 design (`docs/superpowers/specs/2026-11-02-writing-modes-step1-design.md`, section "The
reader"):
- **Page progression (done, `feat/rtl-page-progression`):** `FlowContext::pageProgressionRtlFor` takes the book's
  direction (`bookIsRtl`). X3: a Hebrew book shows `progression=rtl` and Left turns to the next page; Murphy: the
  left tap zone shows the next page.
- **Reading order in the consumers (done, same branch):** the link-target walk and the QR page text read each line in
  its logical order (`TextBlock::readingOrder`, `appendReadingText`). Murphy: of two note markers on one Hebrew line,
  master gave only the first a target; both are tappable now. The dictionary's reading-order join was done in #656.
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
