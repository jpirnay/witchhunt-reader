# CJK A2: CJK line breaking. Implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Chinese and Japanese text breaks between characters, with no hyphen, and keeps kinsoku: no 。、」 at the
start of a line, no 「 at its end. Korean keeps its words whole and, with hyphenation on, splits one between syllables
at a line end. A line break in a hard-wrapped CJK source no longer becomes a space, and a long CJK run no longer gains
a space every 66 characters.

**Architecture:**

- **Today:** CJK text without spaces is cut into 200-byte "words". Each cut becomes a word gap with a space, a word
  wider than the line breaks anywhere with an inserted hyphen, and nothing knows kinsoku.
- **The rules (Task 1):**
  - `utf8IsCjkBreakable`, `utf8IsHangul` and `utf8IsUnspacedCjk` in `Utf8.h` are the character classes.
  - A new module, `CjkLineBreak`, holds the kinsoku sets (upstream's plus crengine's, spec Appendix A.1) and the
    boundary rules. `breakByteOffsets(word)` says where a word with CJK characters may break, and
    `hangulLineEndBreaks(word)` where a Hangul word may split at a line end.
- **No hyphen, no kerning next to CJK (Task 2):** `Hyphenator::breakOffsets` and `EpdFont::getKerning`.
- **Tokens (Task 3):**
  - `ParsedText::addWord` splits a word at those offsets, one token per break. A1 gave every token a
    `TokenBoundary::Boundary`, and the tokens after the first are CJK gaps: zero-width, breakable and justifiable.
    Every layout site already reads them.
  - A token glued to the previous one in the source (an inline element between them, a parser flush) meets it at a
    CJK gap where the rules allow a break, and stays glued where kinsoku forbids one. Where the source had
    whitespace, the space stays.
  - The heap gate reserves room for all of a word's tokens at once.
- **Korean (Task 4):** `hyphenateWordAtIndex` offers the Hangul line-end breaks before the Hyphenator's, so a Hangul
  word splits between syllables, without a hyphen, when it has to.
- **The parser (Task 5):**
  - after a forced 200-byte flush, the next chunk continues the word (glued);
  - a CJK run is flushed every 32 codepoints, glued;
  - a table row is charged per token;
  - a source line break between two characters of unspaced CJK text vanishes (the CSS Text 3 segment-break rule).
- **Checked on books (Task 5):**
  - a rule checker runs four CJK books through the host pipeline and checks every line: kinsoku, no hyphen after
    CJK text, and no space between two Han, kana or CJK-punctuation characters;
  - the books are a fixture with one case per rule, and three public-domain excerpts in Japanese, Chinese and Korean;
  - they get goldens and heap-peak baselines;
  - `SECTION_FILE_VERSION` goes to 87.

**Tech Stack:** C++20 host tests (gtest via CMake, UCRT64 gcc); firmware gnu++2a (PlatformIO, ESP32-C3/S3);
clang-format 21.1.8; Python 3 for the EPUB generators.

**Spec:** `docs/superpowers/specs/2026-10-16-cjk-layout-design.md` (branch `docs/cjk-layout-design`), sections 5.1,
5.3-5.6, 8 (memory), 9.1 (the rule checker, minus justification, squeeze, grid and ruby, which A3 and B check) and
Appendix A.1. A1 (PR #481, merged) built section 5.2. This plan departs from the spec in the points under "Decisions"
below.

**Pre-validated:** every task's code was built and tested in a prototype (`C:/tmp/wt-cjk-proto2`), first on
`7084c65ea` and then rebased onto `origin/master` `236e25389`; the plan was replayed task by task on a fresh tree at
each. On `236e25389`:
- each "see it fail" step fails as stated. Task 1's stops at CMake's configure; Task 2's in both targets; Task 3's
  with exactly the 8 named failures; Task 4's with 6; Task 5's with 7;
- each count matches;
- every file the plan writes is identical to the prototype: 31 files, the generated EPUBs compared by content;
- the heap gate passes on all 20 entries;
- the whole host suite runs 2,591 tests (2,488 on master), and only the two Windows-only failures named under
  Global Constraints fail, as they do on master.

The firmware was built for both boards from the prototype on `7084c65ea` (`default`: RAM 16.7%, flash 96.6%, 3.4 KB
more than A1; `x4pro`: RAM 30.8%, flash 94.8%), with no warning from the files the plan touches. Task 5 builds it
again on the new base, and Task 6 is the device run.

## Global Constraints

- **Branch:** `feat/cjk-line-breaking` from `origin/master` (`236e25389`: A1 and the 2026-10-17 ports merged), in the worktree
  `/c/tmp/wt-cjk-a2`. The PR targets `master`, titled `feat: (Epub) CJK line breaking with kinsoku`.
- **Section cache:** `SECTION_FILE_VERSION` 87 (master is at 86); Task 5 bumps it.
- **No Latin layout change:** every existing golden stays byte-identical. A Latin word over 200 bytes would now stay
  glued across the parser's forced flush; no corpus book has one.
- **No dash state:** upstream's dash split (PR #3883) is not ported (spec section 3). A glued boundary becomes a CJK
  gap only with a CJK character on one side, so a glued Latin `foo—` + `bar` stays glued.
- **Attribution (the porting convention):** each task's commit cites the upstream PRs it takes from, and the source
  says what is theirs:
  - Task 1: the rules and sets are #2288 (Justin Mitchell, `22f35750`), with #3883's dashes (Phạm Bình An,
    `9dfa92ba`) and #3700's Hangul handling (Sung-jin Brian Hong, `794ac97f`). The commit carries `Co-authored-by`
    for Justin Mitchell `<justin@jmitch.com>` and Sung-jin Brian Hong `<serialx@serialx.net>`.
  - Task 2: #2288, `Co-authored-by` Justin Mitchell.
  - Task 3: our implementation of #2288's tokens and #2768's glued rule (Uri Tauber, `9163149a`); attribution, no
    trailer.
  - Task 4: #3700's tests, ported with new widths; `Co-authored-by` Sung-jin Brian Hong.
  - Task 5: #2652's continuation (Phạm Bình An, `3319aa17`); attribution, no trailer.
- **Heap:** the word lists keep their 128-entry steady state: 96 tokens before a mid-block layout, plus at most one
  32-codepoint flush.
- **The excerpts:** only the public-domain text is taken (no Project Gutenberg or Aozora headers, markup or
  trademarks). The generator names each source.
- **App host test commands** (Git Bash, from the worktree root): `export PATH=/c/msys64/ucrt64/bin:$PATH` once, then
  configure with
  `cmake -S test -B build/test -G "MinGW Makefiles" -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/c/_development/witchhunt-reader/build/test/_deps/googletest-src -DFETCHCONTENT_SOURCE_DIR_SDFAT=/c/tmp/SdFat-2.3.1`.
  CMake's own download of SdFat 2.3.1 (for `test/sdfat_alloc`) fails on this machine: its curl cannot read the MSYS2
  CA bundle. `/c/tmp/SdFat-2.3.1` is the release tarball, extracted, with the SHA256 the test pins.
  The steps below write `cmake -S test -B build/test` for short. The full suite needs
  `cmake --build build/test -j 8 -- -k` to get past `epub_build_inventory`, which does not build on Windows
  (`dlfcn.h: No such file or directory` is expected).
- **Two tests fail on Windows on master, outside this plan:** `LongChapterPageTables.AChapterFarPastTheReserveBuildsWhole`
  and `LongChapterPageTables.ARefusedChunkStopsTheBuildWithBothTablesInStep` delete a cache directory while the book's
  `book.bin` is still open, which Windows refuses ("cannot remove all"). Linux and CI pass them. The pipeline runs
  below leave them out by name; the whole suite lists them as its only failures.
- **Firmware builds:** `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then
  `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`; one env at a time.
- **A file changed right after a failed build:** mingw make compares timestamps in whole seconds, and CMake's Makefile
  generator can miss a header change for objects a failed build compiled; the test binary then links a stale object.
  If a test fails or a binary misbehaves right after a "see it fail" build, rebuild its target with `--clean-first`
  before debugging.
- **Formatting:** clang-format 21 (`/c/tmp/cf21/bin/clang-format-21.exe`) with the repo's `.clang-format`. The code
  below is formatted.
- **Commits:** semantic prefixes; no `Co-Authored-By` trailer for the assistant.

## Review Focus

The five situations most likely to bite that the spec implies but a unit test of one function would not reach, most
likely first. Each is pinned by a test in the task named:

1. **A hard-wrapped Chinese or Japanese source.** Many converted books keep the text file's line breaks inside each
   paragraph. Expected: no space where the source broke a line between two CJK characters; a space where it broke
   between CJK and Latin. Pinned by the rule checker on `test_cjk_zh_honglou` and the fixture's case 2 (Task 5).
2. **Japanese with ruby markup.** A2 does not lay out ruby (PR B does), so `<rt>` and `<rp>` text flows inline as
   today. Expected: the CJK rules still hold across the `<ruby>`, `<rt>` and `<rp>` seams. Pinned by the rule checker
   on `test_cjk_ja_soseki` (Task 5).
3. **A long CJK run inside a table cell.** Expected: the row budget sees one token per character and lets the row
   flow as text before the C3's heap fills. Pinned by `CjkTable.ALongCjkCellIsChargedPerToken` (Task 5).
4. **A Hangul word wider than the line, hyphenation off.** Expected: it splits between syllables without a hyphen,
   rather than overflowing or breaking mid-syllable. Pinned by `KoreanLayout.AWordWiderThanTheLineSplitsBetweenSyllables`
   (Task 4).
5. **Latin text glued across an inline element after a dash** (`foo—<i>bar</i>`). Expected: as today, glued and
   unbreakable. Pinned by `CjkSpaces.AGluedLatinWordAfterADashStaysGlued` (Task 3).

## File Structure

| File | Task | Responsibility |
| --- | --- | --- |
| `lib/Utf8/Utf8.h` | 1 | the CJK character classes |
| `lib/Epub/Epub/CjkLineBreak.{h,cpp}` (new) | 1 | kinsoku sets, boundary rules, break offsets, Hangul line-end breaks |
| `test/cjk_line_break/` (new target) | 1, 2 | the rules' and the Hyphenator's tests |
| `lib/Epub/Epub/hyphenation/Hyphenator.cpp`, `lib/EpdFont/EpdFont.cpp` | 2 | no hyphen, no kerning next to CJK |
| `test/glyph_provider/GlyphProviderTest.cpp` | 2 | the kerning test |
| `lib/Epub/Epub/ParsedText.{h,cpp}`, `blocks/TextBlock.h` (a comment) | 3, 4 | tokens; Hangul line-end breaks |
| `test/cjk_layout/` (new target), `test/word_size_layout/` and `test/epub_pipeline/CMakeLists.txt` | 3, 4 | the layout tests; link `CjkLineBreak.cpp` |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.{h,cpp}`, `lib/Epub/Epub/Section.cpp` | 5 | parser continuity, runs, table charge, segment breaks; version 87 |
| `test/epubs/make_test_cjk_*.py` (new, 2), `test/epubs/test_cjk_*.epub` (new, 4) | 5 | the fixture and the excerpts |
| `test/epub_pipeline/CjkRulesTest.cpp` (new), `goldens/test_cjk_*.golden.txt` (new, 4), `heap_peak_baseline.txt` | 5 | the rule checker; goldens; heap peaks |
| (none: device run, PR) | 6 | X3 validation with a locally built Noto CJK font; the PR |

## Decisions this plan makes

- **The rules live in their own module,** `CjkLineBreak`, not in `ParsedText.cpp` as upstream has them. They are pure
  functions over codepoints and are tested without a layout.
- **`utf8IsUnspacedCjk(cp)`** (`utf8IsCjkBreakable && !utf8IsHangul`) is the spec's Han/kana/CJK-punctuation class
  (5.1). The segment-break rule and the rule checker use it, and so will A3.
- **Where the parser sees a source line break:** in `consumeText`'s whitespace branch, raw, before it collapses
  whitespace (the spec asked the plan to confirm this first). The decision waits for the next character, which may
  arrive in a later SAX chunk or after an inline tag.
- **Runs are flushed every 32 codepoints.** Measured on the host (Task 5): without it the Sōseki excerpt's build peaks
  7 KB higher (arena 24,786 → 31,778 B, heap 56,099 → 63,843 B), because one 200-byte flush adds up to 66 tokens and
  doubles the word lists to 256 entries.
- **`TABLE_BUFFER_BYTES_PER_WORD` stays 48:** 32 bytes per token plus half again of growth headroom. Its comment is
  corrected (an A1 review note).
- **The rule checker runs the books left-aligned,** so each gap on a line is its natural gap. It tells a paragraph's
  first line by its indent, and checks kinsoku only at breaks with unspaced CJK on both sides. Elsewhere a break is
  at a source space, which may be followed by a dash.
- **Upstream's Korean tests port with new widths** (10 px glyphs and 5 px spaces against upstream's 8 and 4).
  `HangulWordsStayWholeAndWrapAtSpaces` uses 3개입니다 for 3개를: with 3개를 two layouts tie under our line breaker,
  which keeps the first it finds (upstream breaks ties toward the longer first line, #2781, not ported).
- **Two A1 review notes are folded in:** the breakers' `boundaries` parameter is the member itself (now a comment),
  and `TextBlock.h`'s "CJK already works" note is rewritten.
- **The pipeline shim keeps its synthetic widths** (6-12 px per codepoint). A3 may give CJK characters a full em for
  the grid.

---

### Task 1: The CJK character classes and break rules

**Files:**
- Modify: `lib/Utf8/Utf8.h`
- Create: `lib/Epub/Epub/CjkLineBreak.h`, `lib/Epub/Epub/CjkLineBreak.cpp`
- Create: `test/cjk_line_break/CjkLineBreakTest.cpp`, `test/cjk_line_break/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `TokenBoundary::allowsBreakAfterExplicitHyphen(uint32_t)` (`lib/Epub/Epub/TokenBoundary.h`, A1);
  `Hyphenator::BreakInfo { size_t byteOffset; bool requiresInsertedHyphen; }` (`hyphenation/Hyphenator.h`);
  `utf8NextCodepoint`, `utf8IsCombiningMark` (`Utf8.h`).
- Produces (later tasks use all of these):
  - in `Utf8.h`: `inline bool utf8IsCjkBreakable(uint32_t)`, `inline bool utf8IsHangul(uint32_t)`,
    `inline bool utf8IsUnspacedCjk(uint32_t)`;
  - in namespace `CjkLineBreak`: `bool isNoLineStart(uint32_t)`, `bool isNoLineEnd(uint32_t)`,
    `bool boundaryAllowsBreak(uint32_t leftCp, uint32_t rightCp)`, `bool isGapBetween(uint32_t leftCp,
    uint32_t rightCp)`, `std::vector<size_t> breakByteOffsets(const std::string&)`,
    `std::vector<Hyphenator::BreakInfo> hangulLineEndBreaks(const std::string&)`.

- [ ] **Step 1: The worktree**

```bash
git -C /c/_development/witchhunt-reader fetch origin
git -C /c/_development/witchhunt-reader worktree add -b feat/cjk-line-breaking /c/tmp/wt-cjk-a2 origin/master
cd /c/tmp/wt-cjk-a2
git submodule update --init freeink-sdk
git log --oneline -1   # 236e25389 Merge pull request #491 from jpirnay/port/2026-10-17-fonts
```

All paths below are relative to `/c/tmp/wt-cjk-a2`.

- [ ] **Step 2: Write the failing tests**

The classes, the kinsoku sets (upstream's and crengine's), and where a word breaks:

<!-- plan-file: test/cjk_line_break/CjkLineBreakTest.cpp -->
```cpp
// CJK character classes (Utf8.h) and the CJK line-break rules (CjkLineBreak.h): where text without
// spaces may break, and the kinsoku pairs that must stay together.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Epub/CjkLineBreak.h"
#include "Utf8.h"

TEST(CjkClasses, HanKanaHangulPunctuationAndFullwidthAreBreakable) {
  for (const uint32_t cp : {0x6F22u, 0x3042u, 0x30A2u, 0x3002u, 0xD55Cu, 0xFF21u, 0x20000u}) {
    EXPECT_TRUE(utf8IsCjkBreakable(cp)) << std::hex << cp;
  }
  for (const uint32_t cp : {uint32_t{'a'}, 0x00E9u, 0x2014u, 0x0410u}) {
    EXPECT_FALSE(utf8IsCjkBreakable(cp)) << std::hex << cp;
  }
}

TEST(CjkClasses, HangulIsBreakableButNotUnspaced) {
  EXPECT_TRUE(utf8IsHangul(0xD55C));   // 한
  EXPECT_TRUE(utf8IsHangul(0x3131));   // ㄱ
  EXPECT_FALSE(utf8IsHangul(0x6F22));  // 漢
  EXPECT_FALSE(utf8IsUnspacedCjk(0xD55C));
  for (const uint32_t cp : {0x6F22u, 0x3042u, 0x30A2u, 0x3002u, 0xFF0Cu}) {
    EXPECT_TRUE(utf8IsUnspacedCjk(cp)) << std::hex << cp;
  }
  EXPECT_FALSE(utf8IsUnspacedCjk('a'));
}

TEST(CjkKinsoku, ClosingPunctuationAndIterationMarksNeverStartALine) {
  // 。、」）！ from upstream; 々・〜‼｡﹐ added from crengine.
  for (const uint32_t cp : {0x3002u, 0x3001u, 0x300Du, 0xFF09u, 0xFF01u, 0x3005u, 0x30FBu, 0x301Cu, 0x203Cu, 0xFF61u,
                            0xFE50u, 0x2026u, 0x2014u}) {
    EXPECT_TRUE(CjkLineBreak::isNoLineStart(cp)) << std::hex << cp;
  }
}

TEST(CjkKinsoku, SmallKanaAndTheProlongedSoundMarkMayStartALine) {
  for (const uint32_t cp : {0x3041u, 0x3063u, 0x30C3u, 0x30E3u, 0x30FCu, 0x6F22u}) {  // ぁ っ ッ ャ ー 漢
    EXPECT_FALSE(CjkLineBreak::isNoLineStart(cp)) << std::hex << cp;
  }
}

TEST(CjkKinsoku, OpeningBracketsAndQuotesNeverEndALine) {
  for (const uint32_t cp : {0x300Cu, 0xFF08u, 0x201Cu, 0x301Du, 0xFF62u, 0x201Eu}) {  // 「 （ “ 〝 ｢ „
    EXPECT_TRUE(CjkLineBreak::isNoLineEnd(cp)) << std::hex << cp;
  }
  EXPECT_FALSE(CjkLineBreak::isNoLineEnd(0x300D));  // 」
}

TEST(CjkBreakOffsets, EveryHanCharacterIsABreak) {
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢字です"), (std::vector<size_t>{3, 6, 9}));
}

TEST(CjkBreakOffsets, KinsokuPairsStayTogether) {
  // 字。 and 「漢 never split; 漢|字 inside the brackets still may.
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢字。"), (std::vector<size_t>{3}));
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("「漢字」"), (std::vector<size_t>{6}));
}

TEST(CjkBreakOffsets, ARunOfEllipsesOrDashesNeverSplits) {
  // 漢|…… no (… may not start a line), …|… no, ……|字 yes (a break after a dash or ellipsis).
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢……字"), (std::vector<size_t>{9}));
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢——字"), (std::vector<size_t>{9}));
}

TEST(CjkBreakOffsets, SmallKanaAndTheProlongedSoundMarkAreBreaks) {
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("きゃく"), (std::vector<size_t>{3, 6}));
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("ラーメン"), (std::vector<size_t>{3, 6, 9}));
}

TEST(CjkBreakOffsets, LatinRunsStayWholeButMeetCjkAtABreak) {
  EXPECT_TRUE(CjkLineBreak::breakByteOffsets("Latin").empty());
  EXPECT_TRUE(CjkLineBreak::breakByteOffsets("漢").empty());
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("abc漢字"), (std::vector<size_t>{3, 6}));
}

TEST(CjkBreakOffsets, HangulWordsStayWhole) {
  EXPECT_TRUE(CjkLineBreak::breakByteOffsets("한국어").empty());
  EXPECT_EQ(CjkLineBreak::breakByteOffsets("漢字한글"), (std::vector<size_t>{3}));  // nothing touching Hangul
}

TEST(CjkGap, OnlyBoundariesTouchingCjkAndNoHangulAreGaps) {
  EXPECT_TRUE(CjkLineBreak::isGapBetween(0x6F22, 0x5B57));   // 漢|字
  EXPECT_TRUE(CjkLineBreak::isGapBetween('c', 0x6F22));      // c|漢
  EXPECT_FALSE(CjkLineBreak::isGapBetween('a', 'b'));        // Latin
  EXPECT_FALSE(CjkLineBreak::isGapBetween(0xD55C, 0xAD6D));  // 한|국
  EXPECT_FALSE(CjkLineBreak::isGapBetween(0x5B57, 0x3002));  // 字|。
  EXPECT_FALSE(CjkLineBreak::isGapBetween(0x6F22, 0x0301));  // a combining mark stays with its base
}

TEST(HangulLineEndBreaks, BetweenSyllablesWithoutAHyphen) {
  const auto breaks = CjkLineBreak::hangulLineEndBreaks("다라마바");
  ASSERT_EQ(breaks.size(), 3u);
  for (size_t i = 0; i < breaks.size(); ++i) {
    EXPECT_EQ(breaks[i].byteOffset, 3 * (i + 1));
    EXPECT_FALSE(breaks[i].requiresInsertedHyphen);
  }
}

TEST(HangulLineEndBreaks, NeverBeforeClosingPunctuationButAtDigitsAndBrackets) {
  const auto offsetsOf = [](const std::string& word) {
    std::vector<size_t> offsets;
    for (const auto& b : CjkLineBreak::hangulLineEndBreaks(word)) offsets.push_back(b.byteOffset);
    return offsets;
  };
  EXPECT_EQ(offsetsOf("사."), (std::vector<size_t>{}));
  EXPECT_EQ(offsetsOf("12월"), (std::vector<size_t>{2}));
  EXPECT_EQ(offsetsOf("소신(ab)이"), (std::vector<size_t>{3, 6, 10}));  // 소|신|(ab)|이
}
```

<!-- plan-file: test/cjk_line_break/CMakeLists.txt -->
```cmake
add_executable(CjkLineBreakTest
  CjkLineBreakTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CjkLineBreak.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(CjkLineBreakTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(CjkLineBreakTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CjkLineBreakTest)
```

Register the target after `token_boundary`:

<!-- plan-edit: test/CMakeLists.txt -->
```cmake
add_subdirectory(token_boundary)
```
with:

```cmake
add_subdirectory(token_boundary)
add_subdirectory(cjk_line_break)
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target CjkLineBreakTest -j 8 && ./build/test/cjk_line_break/CjkLineBreakTest.exe --gtest_brief=1`
Expected: CMake stops at configure, `Cannot find source file`: the new target lists `CjkLineBreak.cpp`, which Step 5
writes.

- [ ] **Step 4: The character classes**

Upstream's two classes, verbatim, and ours for the text written without spaces:

<!-- plan-edit: lib/Utf8/Utf8.h -->
```cpp
         || (cp >= 0x11A8 && cp <= 0x11C2);  // T: jongseong
}

// Apply lightweight NFC-like normalization for Vietnamese precomposed characters and for
// conjoining Hangul jamo (L+V -> LV, LV+T -> LVT).
// Converts NFD sequences (base vowel + combining marks) into NFC precomposed
```
with:

```cpp
         || (cp >= 0x11A8 && cp <= 0x11C2);  // T: jongseong
}

// Ported from crosspoint-reader (lib/Utf8/Utf8.h at 83f9cc05): utf8IsCjkBreakable from PR #2288
// ("feat: Support for Korean line breaks and glyph spacing", Justin Mitchell), utf8IsHangul from
// PR #3700 ("fix: justify Korean text by word spaces only", Sung-jin Brian Hong). utf8IsUnspacedCjk
// is ours.
//
// Returns true for CJK characters that break without hyphenation. Covers CJK Unified Ideographs,
// Hiragana, Katakana, Hangul, CJK punctuation, and fullwidth forms. Han and Kana have implicit
// word boundaries per character; Hangul is space-delimited, so layout keeps Hangul words whole
// (see utf8IsHangul) and splits one without a hyphen only through the hyphenation path.
inline bool utf8IsCjkBreakable(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x3000 && cp <= 0x303F)     // CJK Symbols and Punctuation
         || (cp >= 0x3040 && cp <= 0x309F)     // Hiragana
         || (cp >= 0x30A0 && cp <= 0x30FF)     // Katakana
         || (cp >= 0x3130 && cp <= 0x318F)     // Hangul Compatibility Jamo
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xAC00 && cp <= 0xD7AF)     // Hangul Syllables
         || (cp >= 0xD7B0 && cp <= 0xD7FF)     // Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2A6DF)   // CJK Extension B
         || (cp >= 0x2A700 && cp <= 0x2B73F);  // CJK Extension C
}

// Returns true for Hangul letters. Korean separates words with spaces, so a boundary touching
// Hangul is never an implicit line-break opportunity (CSS word-break: keep-all).
inline bool utf8IsHangul(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)      // Hangul Jamo
         || (cp >= 0x3130 && cp <= 0x318F)   // Hangul Compatibility Jamo
         || (cp >= 0xA960 && cp <= 0xA97F)   // Hangul Jamo Extended-A
         || (cp >= 0xAC00 && cp <= 0xD7FF)   // Hangul Syllables, Hangul Jamo Extended-B
         || (cp >= 0xFFA0 && cp <= 0xFFDC);  // Halfwidth Hangul
}

// Han, kana, CJK punctuation and fullwidth forms: the CJK text written without spaces between
// words. A source line break between two of these is not a word gap (CSS Text 3 segment-break rule).
inline bool utf8IsUnspacedCjk(const uint32_t cp) { return utf8IsCjkBreakable(cp) && !utf8IsHangul(cp); }

// Apply lightweight NFC-like normalization for Vietnamese precomposed characters and for
// conjoining Hangul jamo (L+V -> LV, LV+T -> LVT).
// Converts NFD sequences (base vowel + combining marks) into NFC precomposed
```

- [ ] **Step 5: The rules**

<!-- plan-file: lib/Epub/Epub/CjkLineBreak.h -->
```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hyphenation/Hyphenator.h"

// Where Chinese, Japanese and Korean text may break between characters, and where kinsoku (the CJK
// line-edge rules) forbids it.
//
// Ported from crosspoint-reader (lib/Epub/Epub/ParsedText.cpp at 83f9cc05): the boundary rules,
// breakByteOffsets and the punctuation sets are from PR #2288 ("feat: Support for Korean line breaks
// and glyph spacing", Justin Mitchell), the dashes in the no-line-start set from PR #3883 (Phạm Bình
// An), the Hangul exclusion and hangulLineEndBreaks from PR #3700 (Sung-jin Brian Hong). Ours: the
// characters added to both sets from KOReader's crengine (UAX #14 classes CL, EX, NS, IN and OP;
// spec Appendix A.1), and this module.
namespace CjkLineBreak {

// Kinsoku: characters that must not start a line (closing brackets and quotes, sentence and
// clause punctuation, iteration marks, dashes and ellipses), and characters that must not end one
// (opening brackets and quotes). Small kana and the prolonged sound mark may start a line, as in
// KOReader's default.
bool isNoLineStart(uint32_t cp);
bool isNoLineEnd(uint32_t cp);

// A break between these two codepoints is allowed by the CJK rules: one side is CJK (or the left
// side is a visible dash), and kinsoku does not forbid it.
bool boundaryAllowsBreak(uint32_t leftCp, uint32_t rightCp);

// A boundary that becomes a breakable zero-width gap: boundaryAllowsBreak, and neither side is
// Hangul. Korean separates words with spaces, so Hangul words stay whole.
bool isGapBetween(uint32_t leftCp, uint32_t rightCp);

// The byte offsets inside `text` where it splits into CJK tokens: after each codepoint followed by
// a gap (isGapBetween). Empty for text without a CJK codepoint or with a single codepoint, so Latin
// words are never split here. Kinsoku pairs such as 字。 or 「漢 produce no offset.
std::vector<size_t> breakByteOffsets(const std::string& text);

// Line-end split points inside a Hangul word: between syllables, and where Hangul meets digits,
// Latin or brackets, never before closing punctuation. No hyphen is drawn at any of them.
std::vector<Hyphenator::BreakInfo> hangulLineEndBreaks(const std::string& word);

}  // namespace CjkLineBreak
```

The sets are switch statements, as upstream has them; the crengine additions are marked. `boundaryAllowsBreak`
is the kinsoku test of one boundary, and `isGapBetween` adds the Korean exception: a boundary touching Hangul is
never a gap, because Korean separates its words with spaces.

<!-- plan-file: lib/Epub/Epub/CjkLineBreak.cpp -->
```cpp
#include "CjkLineBreak.h"

#include <Utf8.h>

#include "TokenBoundary.h"

namespace CjkLineBreak {

bool isNoLineStart(const uint32_t cp) {
  // A dash or an ellipsis never starts a line after a CJK character (- 058A 2010 2012-2015 2043
  // 207B 208B 2212 2E17 2E3A 2E3B FE58 FE63 FF0D _ ...), so a run of them never splits either.
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(cp)) {
    return true;
  }
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
    // Ours, from crengine (UAX #14 CL, EX, NS and IN):
    case 0x301E:  // 〞
    case 0x301F:  // 〟
    case 0xFF60:  // ｠
    case 0xFE50:  // ﹐
    case 0xFE52:  // ﹒
    case 0xFE56:  // ﹖
    case 0xFE57:  // ﹗
    case 0xFF61:  // ｡
    case 0xFF63:  // ｣
    case 0xFF64:  // ､
    case 0xFF65:  // ･
    case 0x3005:  // 々
    case 0x303B:  // 〻
    case 0x309D:  // ゝ
    case 0x309E:  // ゞ
    case 0x30FD:  // ヽ
    case 0x30FE:  // ヾ
    case 0x30FB:  // ・
    case 0x301C:  // 〜
    case 0x30A0:  // ゠
    case 0x203C:  // ‼
    case 0x203D:  // ‽
    case 0x2047:  // ⁇
    case 0x2048:  // ⁈
    case 0x2049:  // ⁉
    case 0x309B:  // ゛
    case 0x309C:  // ゜
    case 0xFF9E:  // ﾞ
    case 0xFF9F:  // ﾟ
    case 0x303C:  // 〼
    case 0x2025:  // ‥
    case 0x2024:  // ․
      return true;
    default:
      return false;
  }
}

bool isNoLineEnd(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0x00AB:  // «
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
    // Ours, from crengine (UAX #14 OP):
    case 0x301D:  // 〝
    case 0xFF5F:  // ｟
    case 0xFF62:  // ｢
    case 0x201E:  // „
    case 0x201A:  // ‚
      return true;
    default:
      return false;
  }
}

bool boundaryAllowsBreak(const uint32_t leftCp, const uint32_t rightCp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) {
    return !isNoLineStart(rightCp);
  }
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoLineEnd(leftCp) || isNoLineStart(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

bool isGapBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) {
    return boundaryAllowsBreak(leftCp, rightCp);
  }
  if (utf8IsHangul(leftCp) || utf8IsHangul(rightCp)) return false;
  return boundaryAllowsBreak(leftCp, rightCp);
}

std::vector<size_t> breakByteOffsets(const std::string& text) {
  struct CodepointEnd {
    uint32_t cp;
    size_t endOffset;
  };

  std::vector<CodepointEnd> codepoints;
  codepoints.reserve(text.size());
  bool hasCjk = false;

  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) break;
    if (utf8IsCjkBreakable(cp)) hasCjk = true;
    codepoints.push_back({cp, static_cast<size_t>(ptr - start)});
  }

  if (!hasCjk || codepoints.size() < 2) return {};

  std::vector<size_t> offsets;
  offsets.reserve(codepoints.size() - 1);
  for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
    if (isGapBetween(codepoints[i].cp, codepoints[i + 1].cp)) offsets.push_back(codepoints[i].endOffset);
  }
  return offsets;
}

std::vector<Hyphenator::BreakInfo> hangulLineEndBreaks(const std::string& word) {
  std::vector<Hyphenator::BreakInfo> breaks;
  if (word.empty()) return breaks;
  const auto* const start = reinterpret_cast<const unsigned char*>(word.c_str());
  const auto* ptr = start;
  uint32_t prev = utf8NextCodepoint(&ptr);
  while (*ptr) {
    const size_t offset = static_cast<size_t>(ptr - start);
    const uint32_t cur = utf8NextCodepoint(&ptr);
    if ((utf8IsHangul(prev) || utf8IsHangul(cur)) && boundaryAllowsBreak(prev, cur)) {
      breaks.push_back({offset, false});
    }
    prev = cur;
  }
  return breaks;
}

}  // namespace CjkLineBreak
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build build/test --target CjkLineBreakTest -j 8 && ./build/test/cjk_line_break/CjkLineBreakTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 14 tests.`

- [ ] **Step 7: Commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror lib/Utf8/Utf8.h lib/Epub/Epub/CjkLineBreak.h \
  lib/Epub/Epub/CjkLineBreak.cpp test/cjk_line_break/CjkLineBreakTest.cpp
git add lib/Utf8/Utf8.h lib/Epub/Epub/CjkLineBreak.h lib/Epub/Epub/CjkLineBreak.cpp test/cjk_line_break/ test/CMakeLists.txt
git commit -m "feat(epub): CJK character classes and line-break rules, ported from crosspoint-reader" \
  -m "Where text in Chinese, Japanese and Korean may break between characters, and where kinsoku forbids it. The boundary rules, the break offsets and the punctuation sets are crosspoint-reader's PR #2288 (\"feat: Support for Korean line breaks and glyph spacing\", 22f35750), with the dashes of PR #3883 (9dfa92ba) and the Hangul handling of PR #3700 (794ac97f). Ours: the characters added to both sets from KOReader's crengine (UAX #14 CL, EX, NS, IN and OP), utf8IsUnspacedCjk, and the module." \
  -m "Co-authored-by: Justin Mitchell <justin@jmitch.com>
Co-authored-by: Sung-jin Brian Hong <serialx@serialx.net>"
```

### Task 2: No hyphen and no kerning next to CJK

A word wider than the line is broken by `Hyphenator::breakOffsets`'s fallback breaks, which all draw a hyphen
today, so every over-wide CJK chunk ends a line in "-". Kerning a CJK pair is wrong too: CJK text is set on a grid.
A TTF family would also fill its kerning pair store with CJK zeros (the TTF 2c-4 decision).

**Files:**
- Modify: `lib/Epub/Epub/hyphenation/Hyphenator.cpp`, `lib/EpdFont/EpdFont.cpp`
- Modify: `test/cjk_line_break/CjkLineBreakTest.cpp`, `test/cjk_line_break/CMakeLists.txt`
- Modify: `test/glyph_provider/GlyphProviderTest.cpp`

**Interfaces:**
- Consumes: Task 1's `utf8IsCjkBreakable`.
- Produces: `Hyphenator::breakOffsets` returns `requiresInsertedHyphen == false` for a break next to a CJK character;
  `EpdFont::getKerning` returns 0 for any pair with a CJK character, without asking the font's provider.

- [ ] **Step 1: Write the failing tests**

The Hyphenator's tests join Task 1's target, which now compiles the hyphenation sources:

<!-- plan-edit: test/cjk_line_break/CjkLineBreakTest.cpp -->
```cpp
#include "Epub/CjkLineBreak.h"
```
with:

```cpp
#include "Epub/CjkLineBreak.h"
#include "Epub/hyphenation/Hyphenator.h"
```

<!-- plan-append: test/cjk_line_break/CjkLineBreakTest.cpp -->
```cpp
// Hyphenator::breakOffsets offers fallback breaks inside a word that has no pattern breaks. Next to
// a CJK character such a break draws no hyphen: CJK scripts are not hyphenated.
TEST(CjkHyphenator, FallbackBreaksNextToCjkDrawNoHyphen) {
  Hyphenator::setPreferredLanguage("ja");
  const auto breaks = Hyphenator::breakOffsets("漢字仮名交", /*includeFallback=*/true);
  ASSERT_FALSE(breaks.empty());
  for (const auto& b : breaks) EXPECT_FALSE(b.requiresInsertedHyphen) << "offset " << b.byteOffset;
}

TEST(CjkHyphenator, LatinFallbackBreaksStillDrawAHyphen) {
  Hyphenator::setPreferredLanguage("ja");
  const auto breaks = Hyphenator::breakOffsets("abcdefgh", /*includeFallback=*/true);
  ASSERT_FALSE(breaks.empty());
  for (const auto& b : breaks) EXPECT_TRUE(b.requiresInsertedHyphen) << "offset " << b.byteOffset;
}
```

<!-- plan-edit: test/cjk_line_break/CMakeLists.txt -->
```cmake
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
```
with:

```cmake
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
```

The kerning test, next to the provider's other kerning tests:

<!-- plan-edit: test/glyph_provider/GlyphProviderTest.cpp -->
```cpp
  EXPECT_TRUE(provider.kernAsked.empty());
}

TEST(GlyphProvider, AFontWithoutAProviderBehavesAsBefore) {
  const EpdFontData data{};
  const EpdFont font(&data);
```
with:

```cpp
  EXPECT_TRUE(provider.kernAsked.empty());
}

// CJK text is set on a grid: no pair touching a CJK character kerns, whatever the font's tables or
// provider say, and the provider is never asked (a TTF family would fill its pair store with zeros).
TEST(GlyphProvider, NoPairTouchingCjkKerns) {
  static const EpdKernClassEntry kLeft[] = {{0x6F22, 1}};   // 漢
  static const EpdKernClassEntry kRight[] = {{0x5B57, 1}};  // 字
  static const int8_t kMatrix[] = {-5};
  FakeProvider provider;
  provider.kernValue = -3;
  const EpdFontData providerData = providerOnly(&provider);
  const EpdFont providerFont(&providerData);
  EXPECT_EQ(0, providerFont.getKerning(0x6F22, 0x5B57));
  EXPECT_EQ(0, providerFont.getKerning('A', 0x3002));  // A。
  EXPECT_TRUE(provider.kernAsked.empty());
  EXPECT_EQ(-3, providerFont.getKerning('A', 'V'));  // Latin pairs still ask

  EpdFontData tableData = providerOnly(nullptr);
  tableData.kernLeftClasses = kLeft;
  tableData.kernRightClasses = kRight;
  tableData.kernMatrix = kMatrix;
  tableData.kernLeftEntryCount = 1;
  tableData.kernRightEntryCount = 1;
  tableData.kernLeftClassCount = 1;
  tableData.kernRightClassCount = 1;
  const EpdFont tableFont(&tableData);
  EXPECT_EQ(0, tableFont.getKerning(0x6F22, 0x5B57));  // the table has the pair; it is not used
}

TEST(GlyphProvider, AFontWithoutAProviderBehavesAsBefore) {
  const EpdFontData data{};
  const EpdFont font(&data);
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target CjkLineBreakTest -j 8 && ./build/test/cjk_line_break/CjkLineBreakTest.exe --gtest_brief=1`
Expected: `CjkHyphenator.FallbackBreaksNextToCjkDrawNoHyphen` fails (`[  PASSED  ] 15 tests.`); the Latin test passes.

Run: `cmake --build build/test --target GlyphProviderTest -j 8 && ./build/test/glyph_provider/GlyphProviderTest.exe --gtest_brief=1`
Expected: `GlyphProvider.NoPairTouchingCjkKerns` fails (`[  PASSED  ] 9 tests.`).

- [ ] **Step 3: No hyphen at a CJK break**

<!-- plan-edit: lib/Epub/Epub/hyphenation/Hyphenator.cpp -->
```cpp
#include "Hyphenator.h"

#include <algorithm>
#include <cassert>
#include <vector>
```
with:

```cpp
#include "Hyphenator.h"

#include <Utf8.h>

#include <algorithm>
#include <cassert>
#include <vector>
```
<!-- plan-edit: lib/Epub/Epub/hyphenation/Hyphenator.cpp -->
```cpp
  std::vector<Hyphenator::BreakInfo> breaks;
  breaks.reserve(indexes.size());
  for (const size_t idx : indexes) {
    breaks.push_back({byteOffsetForIndex(cps, idx), true});
  }

  return breaks;
```
with:

```cpp
  std::vector<Hyphenator::BreakInfo> breaks;
  breaks.reserve(indexes.size());
  for (const size_t idx : indexes) {
    // CJK scripts are not hyphenated: a break next to a CJK character draws no hyphen. Ported from
    // crosspoint-reader PR #2288 (Justin Mitchell).
    const bool cjkSide =
        (idx < cps.size() && utf8IsCjkBreakable(cps[idx].value)) || (idx > 0 && utf8IsCjkBreakable(cps[idx - 1].value));
    breaks.push_back({byteOffsetForIndex(cps, idx), !cjkSide});
  }

  return breaks;
```

- [ ] **Step 4: No kerning for a CJK pair**

<!-- plan-edit: lib/EpdFont/EpdFont.cpp -->
```cpp
}

int8_t EpdFont::getKerning(const uint32_t leftCp, const uint32_t rightCp) const {
  const bool hasKernTables =
      (data->kernMatrix || data->kernRowOffsets) && (data->kernLeftClasses || data->kernLeftCodepoints);
  if (!hasKernTables) {
```
with:

```cpp
}

int8_t EpdFont::getKerning(const uint32_t leftCp, const uint32_t rightCp) const {
  // CJK text is set on a grid, so no pair touching a CJK character kerns. This also keeps CJK pairs
  // away from a TTF family's pair store, which they would fill with zeros. Ported from crosspoint-reader
  // PR #2288 (Justin Mitchell).
  if (utf8IsCjkBreakable(leftCp) || utf8IsCjkBreakable(rightCp)) return 0;
  const bool hasKernTables =
      (data->kernMatrix || data->kernRowOffsets) && (data->kernLeftClasses || data->kernLeftCodepoints);
  if (!hasKernTables) {
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build build/test --target CjkLineBreakTest -j 8 && ./build/test/cjk_line_break/CjkLineBreakTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 16 tests.`

Run: `cmake --build build/test --target GlyphProviderTest -j 8 && ./build/test/glyph_provider/GlyphProviderTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 10 tests.`

- [ ] **Step 6: Commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror lib/Epub/Epub/hyphenation/Hyphenator.cpp lib/EpdFont/EpdFont.cpp \
  test/cjk_line_break/CjkLineBreakTest.cpp test/glyph_provider/GlyphProviderTest.cpp
git add lib/Epub/Epub/hyphenation/Hyphenator.cpp lib/EpdFont/EpdFont.cpp test/cjk_line_break/ \
  test/glyph_provider/GlyphProviderTest.cpp
git commit -m "fix(epub): no hyphen and no kerning next to CJK characters" \
  -m "A fallback break next to a CJK character draws no hyphen, and no pair touching a CJK character kerns (which also keeps CJK pairs out of a TTF family's kerning store). Ported from crosspoint-reader PR #2288 (\"feat: Support for Korean line breaks and glyph spacing\", 22f35750)." \
  -m "Co-authored-by: Justin Mitchell <justin@jmitch.com>"
```

### Task 3: CJK words become one token per character

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h`, `lib/Epub/Epub/ParsedText.cpp`, `lib/Epub/Epub/blocks/TextBlock.h` (a comment)
- Create: `test/cjk_layout/CjkLayoutTest.cpp`, `test/cjk_layout/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`, `test/word_size_layout/CMakeLists.txt`, `test/epub_pipeline/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `CjkLineBreak::breakByteOffsets`, `CjkLineBreak::isGapBetween`, `utf8IsCjkBreakable`; A1's
  `TokenBoundary::Boundary` (`{continues, noSpaceBefore}`; `{false, true}` is a CJK gap).
- Produces:
  - `ParsedText::addWord` (unchanged signature) pushes one token per CJK break. Each token's visible offset is
    the word's plus the visible bytes before it. A token glued in the source meets the previous one at a CJK gap
    where `isGapBetween` allows it.
  - Private `bool ParsedText::reserveWords(size_t additional)`: the heap gate for `additional` tokens. It sets
    `wordGrowthRefused_` and returns false when the heap cannot hold them.
  - Private `void ParsedText::pushToken(std::string, EpdFontFamily::Style, TokenBoundary::Boundary, uint8_t sizePct,
    uint32_t visibleOffset)`.
  - Every target that compiles `ParsedText.cpp` also compiles `CjkLineBreak.cpp`.

- [ ] **Step 1: Write the failing tests**

A new target over the fixed-width shim of `test/word_size_layout` (every codepoint 10 px, a space 5 px):

<!-- plan-file: test/cjk_layout/CjkLayoutTest.cpp -->
```cpp
// CJK text through the real ParsedText and TextBlock, against the fixed-width GfxRenderer shim of
// test/word_size_layout (every codepoint 10 px, a space 5 px, no kerning): a word with CJK characters
// becomes one token per allowed break, lines break between those tokens without a hyphen, and
// kinsoku pairs stay on one line.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  std::vector<bool> continues;
  std::vector<EpdFontFamily::Style> styles;
  uint32_t visibleOffset = 0;
};

BlockStyle leftAligned() {
  BlockStyle bs;
  bs.alignment = CssTextAlign::Left;
  bs.textIndent = 0;
  bs.textIndentDefined = true;
  return bs;
}

std::vector<Line> layout(ParsedText& text, const uint16_t width) {
  GfxRenderer renderer;
  std::vector<Line> lines;
  text.layoutAndExtractLines(
      renderer, kFontId, width,
      [&](std::unique_ptr<TextBlock> block, bool, bool) {
        Line& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
          line.continues.push_back(block->wordContinues(i));
          line.styles.push_back(block->wordStyle(i));
        }
        line.visibleOffset = text.lastLineVisibleOffset();
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  return lines;
}

std::vector<std::vector<std::string>> wordsOf(const std::vector<Line>& lines) {
  std::vector<std::vector<std::string>> result;
  for (const auto& line : lines) result.push_back(line.words);
  return result;
}

}  // namespace

TEST(CjkTokens, EachHanCharacterIsATokenWithNoGapBetween) {
  ParsedText text(false, false, leftAligned());
  text.addWord("漢字仮名", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words, (std::vector<std::string>{"漢", "字", "仮", "名"}));
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 10, 20, 30}));
  // A CJK gap is a break, not a continuation: the dictionary selects one character run at a time.
  EXPECT_EQ(lines[0].continues, (std::vector<bool>{false, false, false, false}));
}

TEST(CjkTokens, LinesBreakBetweenCharactersWithoutAHyphen) {
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四五六七八九十百千", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 55);
  EXPECT_EQ(wordsOf(lines), (std::vector<std::vector<std::string>>{
                                {"一", "二", "三", "四", "五"}, {"六", "七", "八", "九", "十"}, {"百", "千"}}));
}

TEST(CjkTokens, HyphenationOnStillBreaksWithoutAHyphen) {
  ParsedText text(false, /*hyphenationEnabled=*/true, leftAligned());
  text.addWord("一二三四五六七八九十百千", EpdFontFamily::REGULAR);
  for (const auto& line : layout(text, 55)) {
    for (const auto& word : line.words) EXPECT_EQ(word.find('-'), std::string::npos) << word;
  }
}

TEST(CjkKinsoku, ClosingPunctuationNeverStartsALine) {
  // 五。 is one token, so the line ends before 五 rather than leave 。 at the start of the next.
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四五。六", EpdFontFamily::REGULAR);
  EXPECT_EQ(wordsOf(layout(text, 50)),
            (std::vector<std::vector<std::string>>{{"一", "二", "三", "四"}, {"五。", "六"}}));
}

TEST(CjkKinsoku, AnOpeningBracketNeverEndsALine) {
  ParsedText text(false, false, leftAligned());
  text.addWord("一二三四「五」", EpdFontFamily::REGULAR);
  EXPECT_EQ(wordsOf(layout(text, 50)), (std::vector<std::vector<std::string>>{{"一", "二", "三", "四"}, {"「五」"}}));
}

TEST(CjkSpaces, AWordGapInTheSourceStaysASpace) {
  ParsedText text(false, false, leftAligned());
  text.addWord("漢字", EpdFontFamily::REGULAR);
  text.addWord("仮名", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 10, 25, 35}));
}

TEST(CjkSpaces, CjkGluedAcrossAnInlineStyleIsABreakableZeroGap) {
  ParsedText text(false, false, leftAligned());
  text.addWord("漢字", EpdFontFamily::REGULAR);
  text.addWord("仮名", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 10, 20, 30}));

  ParsedText narrow(false, false, leftAligned());
  narrow.addWord("漢字", EpdFontFamily::REGULAR);
  narrow.addWord("仮名", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  EXPECT_EQ(wordsOf(layout(narrow, 25)), (std::vector<std::vector<std::string>>{{"漢", "字"}, {"仮", "名"}}));
}

TEST(CjkSpaces, AGluedLatinWordAfterADashStaysGlued) {
  // Upstream's dash split is not ported: foo— + bar glued in the source stays one unbreakable unit.
  ParsedText text(false, false, leftAligned());
  text.addWord("foo—", EpdFontFamily::REGULAR);
  text.addWord("bar", EpdFontFamily::ITALIC, false, /*attachToPrevious=*/true);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].continues, (std::vector<bool>{false, true}));
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 40}));
}

TEST(CjkTokens, EachLineStartsAtItsFirstTokensVisibleOffset) {
  // 漢 (3 bytes) | 字。 (6 bytes) | 仮: tokens start at 100, 103 and 109.
  ParsedText text(false, false, leftAligned());
  text.addWord("漢字。仮", EpdFontFamily::REGULAR, false, false, ParsedText::DEFAULT_WORD_SIZE_PCT, 100);
  const auto lines = layout(text, 25);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].visibleOffset, 100u);
  EXPECT_EQ(lines[1].visibleOffset, 103u);
  EXPECT_EQ(lines[2].visibleOffset, 109u);
}

TEST(CjkTokens, BionicReadingLeavesCjkTokensAlone) {
  ParsedText text(false, false, leftAligned(), /*bionicReadingEnabled=*/true);
  text.addWord("漢字", EpdFontFamily::REGULAR);
  text.addWord("reading", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 400);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].words, (std::vector<std::string>{"漢", "字", "read", "ing"}));
  EXPECT_EQ(lines[0].styles[0], EpdFontFamily::REGULAR);
  EXPECT_EQ(lines[0].styles[1], EpdFontFamily::REGULAR);
  EXPECT_EQ(lines[0].styles[2], EpdFontFamily::BOLD);
}
```

<!-- plan-file: test/cjk_layout/CMakeLists.txt -->
```cmake
add_executable(CjkLayoutTest
  CjkLayoutTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CjkLineBreak.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# Include order matters: test/word_size_layout's GfxRenderer.h shim must win over the
# real lib/GfxRenderer (which drags in HalDisplay); lib/GfxRenderer is therefore
# deliberately absent from the include path.
target_include_directories(CjkLayoutTest PRIVATE
  ${REPO_ROOT}/test/word_size_layout
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(CjkLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CjkLayoutTest)
```

<!-- plan-edit: test/CMakeLists.txt -->
```cmake
add_subdirectory(cjk_line_break)
```
with:

```cmake
add_subdirectory(cjk_line_break)
add_subdirectory(cjk_layout)
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target CjkLayoutTest -j 8 && ./build/test/cjk_layout/CjkLayoutTest.exe --gtest_brief=1`
Expected: 8 failures, among them `CjkTokens.EachHanCharacterIsATokenWithNoGapBetween` and
`CjkKinsoku.ClosingPunctuationNeverStartsALine` (`[  PASSED  ] 2 tests.`). Two tests pass already:
`HyphenationOnStillBreaksWithoutAHyphen` (Task 2 removed the hyphen) and `AGluedLatinWordAfterADashStaysGlued`
(today's behaviour, which this task must keep).

- [ ] **Step 3: The helpers' declarations**

`addWord` grows a comment, the heap gate becomes `reserveWords` so it can reserve for several tokens at once, and
`pushToken` appends one token to the five per-token lists. The breakers' declaration comment also notes that their
`boundaries` parameter is the member itself (an A1 review note):

<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint8_t sizePct = DEFAULT_WORD_SIZE_PCT, uint32_t visibleOffset = 0);
```
with:

```cpp
  // Adds a word from the source: a Latin word as one token, a word with CJK characters as one token per
  // break the CJK rules allow (CjkLineBreak.h). attachToPrevious: no whitespace separated it from the
  // previous word in the source.
  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint8_t sizePct = DEFAULT_WORD_SIZE_PCT, uint32_t visibleOffset = 0);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
  int naturalGapBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
```
with:

```cpp
  int naturalGapBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
  // Room in all five per-word vectors for `additional` more tokens; false (and wordGrowthRefused_)
  // when the heap cannot hold the growth.
  bool reserveWords(size_t additional);
  void pushToken(std::string token, EpdFontFamily::Style style, TokenBoundary::Boundary boundary, uint8_t sizePct,
                 uint32_t visibleOffset);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
  // The three breakers fill `lineBreakIndices` (cleared first) rather than returning a vector,
  // so the caller's scratch member is reused instead of reallocated per paragraph.
```
with:

```cpp
  // The three breakers fill `lineBreakIndices` (cleared first) rather than returning a vector,
  // so the caller's scratch member is reused instead of reallocated per paragraph.
  // `boundaries` is always wordBoundaries itself: hyphenateWordAtIndex inserts into the member during
  // the call, and the reference sees the insert. Passing a copy would desynchronize the indices.
```

- [ ] **Step 4: Tokens in `addWord`**

`addWord` asks `CjkLineBreak::breakByteOffsets` where the word breaks and reserves room for all its tokens. It then
pushes the first token with the word's own boundary and every later one as a CJK gap, each at its visible offset.
The boundary of a word glued in the source becomes a CJK gap only if one side is CJK, which keeps a glued Latin
`foo—` + `bar` glued. A Latin word gets no offsets and is pushed whole, by move, as before:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
#include "Epub/VisibleText.h"
#include "hyphenation/HyphenationCommon.h"
```
with:

```cpp
#include "CjkLineBreak.h"
#include "Epub/VisibleText.h"
#include "hyphenation/HyphenationCommon.h"
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const uint8_t sizePct, const uint32_t visibleOffset) {
  if (word.empty()) return;
  if (wordGrowthRefused_) return;  // the parse is being aborted; see wordGrowthRefused()

  word = utf8NfcNorm(std::move(word));

  const size_t requiredSize = words.size() + 1;
  if (words.capacity() < requiredSize || wordStyles.capacity() < requiredSize ||
      wordBoundaries.capacity() < requiredSize || wordSizes.capacity() < requiredSize ||
      wordVisibleOffsets.capacity() < requiredSize) {
    size_t newCapacity = std::max<size_t>(16, words.capacity());
    while (newCapacity < requiredSize) {
      newCapacity *= 2;
    }
    // The five reserves below are unchecked heap growth (abort() on failure under
    // -fno-exceptions). Require the largest free block to hold all five, plus a header
    // each -- conservative, since they need not share a block, but this only bites when the
    // heap is nearly gone, and then a partial-cache abort beats a crash.
    {
      constexpr size_t ALLOC_HEADER_SLACK = 16;
      const size_t needed = newCapacity * (sizeof(std::string) + sizeof(EpdFontFamily::Style) + sizeof(uint8_t) +
                                           sizeof(uint32_t) + sizeof(TokenBoundary::Boundary)) +
                            5 * ALLOC_HEADER_SLACK;
      if (ESP.getMaxAllocHeap() < needed) {
        wordGrowthRefused_ = true;
        return;
      }
    }
    words.reserve(newCapacity);
    wordStyles.reserve(newCapacity);
    wordBoundaries.reserve(newCapacity);
    wordSizes.reserve(newCapacity);
    wordVisibleOffsets.reserve(newCapacity);
  }

  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordBoundaries.push_back(TokenBoundary::Boundary{attachToPrevious, false});
  wordSizes.push_back(std::min(std::max(sizePct, MIN_WORD_SIZE_PCT), MAX_WORD_SIZE_PCT));
  wordVisibleOffsets.push_back(visibleOffset);
}
```
with:

```cpp
bool ParsedText::reserveWords(const size_t additional) {
  const size_t requiredSize = words.size() + additional;
  if (words.capacity() >= requiredSize && wordStyles.capacity() >= requiredSize &&
      wordBoundaries.capacity() >= requiredSize && wordSizes.capacity() >= requiredSize &&
      wordVisibleOffsets.capacity() >= requiredSize) {
    return true;
  }
  size_t newCapacity = std::max<size_t>(16, words.capacity());
  while (newCapacity < requiredSize) {
    newCapacity *= 2;
  }
  // The five reserves below are unchecked heap growth (abort() on failure under
  // -fno-exceptions). Require the largest free block to hold all five, plus a header
  // each -- conservative, since they need not share a block, but this only bites when the
  // heap is nearly gone, and then a partial-cache abort beats a crash.
  {
    constexpr size_t ALLOC_HEADER_SLACK = 16;
    const size_t needed = newCapacity * (sizeof(std::string) + sizeof(EpdFontFamily::Style) + sizeof(uint8_t) +
                                         sizeof(uint32_t) + sizeof(TokenBoundary::Boundary)) +
                          5 * ALLOC_HEADER_SLACK;
    if (ESP.getMaxAllocHeap() < needed) {
      wordGrowthRefused_ = true;
      return false;
    }
  }
  words.reserve(newCapacity);
  wordStyles.reserve(newCapacity);
  wordBoundaries.reserve(newCapacity);
  wordSizes.reserve(newCapacity);
  wordVisibleOffsets.reserve(newCapacity);
  return true;
}

void ParsedText::pushToken(std::string token, const EpdFontFamily::Style style, const TokenBoundary::Boundary boundary,
                           const uint8_t sizePct, const uint32_t visibleOffset) {
  words.push_back(std::move(token));
  wordStyles.push_back(style);
  wordBoundaries.push_back(boundary);
  wordSizes.push_back(sizePct);
  wordVisibleOffsets.push_back(visibleOffset);
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const uint8_t sizePct, const uint32_t visibleOffset) {
  if (word.empty()) return;
  if (wordGrowthRefused_) return;  // the parse is being aborted; see wordGrowthRefused()

  word = utf8NfcNorm(std::move(word));

  // Chinese and Japanese are written without spaces: a word with CJK characters becomes one token
  // per break the CJK rules allow, so a line can end between its characters. Kinsoku pairs such as
  // 字。 share a token. Latin words get no offsets and stay whole.
  const std::vector<size_t> cjkBreaks = CjkLineBreak::breakByteOffsets(word);
  if (!reserveWords(cjkBreaks.size() + 1)) return;

  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  const uint8_t size = std::min(std::max(sizePct, MIN_WORD_SIZE_PCT), MAX_WORD_SIZE_PCT);

  // Glued to the previous token in the source (no whitespace between them): where the CJK rules
  // allow a break there, the two meet at a breakable zero-width gap. Where the source had
  // whitespace, the space is content and stays. From crosspoint-reader PR #2768 ("fix: restore
  // spaces between CJK words", Uri Tauber). A glued Latin "foo—" + "bar" stays glued: the gap needs
  // a CJK character on one side, since the dash rule of upstream PR #3883 is not ported.
  TokenBoundary::Boundary boundary{attachToPrevious, false};
  if (attachToPrevious && !words.empty()) {
    const uint32_t left = lastCodepoint(words.back());
    const uint32_t right = firstCodepoint(word);
    if ((utf8IsCjkBreakable(left) || utf8IsCjkBreakable(right)) && CjkLineBreak::isGapBetween(left, right)) {
      boundary = TokenBoundary::Boundary{false, true};
    }
  }

  if (cjkBreaks.empty()) {
    pushToken(std::move(word), combinedStyle, boundary, size, visibleOffset);
    return;
  }
  size_t start = 0;
  uint32_t offset = visibleOffset;
  for (size_t i = 0; i <= cjkBreaks.size(); ++i) {
    const size_t end = i < cjkBreaks.size() ? cjkBreaks[i] : word.size();
    pushToken(word.substr(start, end - start), combinedStyle, boundary, size, offset);
    // The next token starts where this one's visible bytes end (KOSync positions).
    offset += static_cast<uint32_t>(VisibleText::visibleBytes(word.data() + start, static_cast<int>(end - start)));
    boundary = TokenBoundary::Boundary{false, true};
    start = end;
  }
}
```

`TextBlock.h`'s note that "CJK already works" is no longer true of the old code, nor a description of the new
(an A1 review note):

<!-- plan-edit: lib/Epub/Epub/blocks/TextBlock.h -->
```cpp
// RTL / CJK note: this slice is not a barrier to either. CJK already works and
// needs nothing here -- its line breaking is a parse-time concern
// (ChapterHtmlSlimParser, "primary word-breaking mechanism" for spaceless text),
// so a CJK line reaches TextBlock already broken and positioned like any other.
```
with:

```cpp
// RTL / CJK note: this slice is not a barrier to either. CJK line breaking happens
// in ParsedText -- a word with CJK characters becomes one token per allowed break
// (CjkLineBreak.h) -- so a CJK line reaches TextBlock already broken and positioned
// like any other; of its token boundaries only the continuation bit is kept.
```

Every other target that compiles `ParsedText.cpp` now needs `CjkLineBreak.cpp`:

<!-- plan-edit: test/word_size_layout/CMakeLists.txt -->
```cmake
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
```
with:

```cmake
  ${REPO_ROOT}/lib/Epub/Epub/CjkLineBreak.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
```

<!-- plan-edit: test/epub_pipeline/CMakeLists.txt -->
```cmake
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
```
with:

```cmake
  ${REPO_ROOT}/lib/Epub/Epub/CjkLineBreak.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build build/test --target CjkLayoutTest -j 8 && ./build/test/cjk_layout/CjkLayoutTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 10 tests.`

Run: `cmake --build build/test --target WordSizeLayoutTest -j 8 && ./build/test/word_size_layout/WordSizeLayoutTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 37 tests.`

Run: `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_brief=1 --gtest_filter=-LongChapterPageTables.AChapterFarPastTheReserveBuildsWhole:LongChapterPageTables.ARefusedChunkStopsTheBuildWithBothTablesInStep`
Expected: `[  PASSED  ] 692 tests.` (2 skipped, as on master): no corpus book has CJK text yet, and no golden moves. The
filter leaves out the two tests that fail on Windows on master too (see Global Constraints).

- [ ] **Step 6: Commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror lib/Epub/Epub/ParsedText.h lib/Epub/Epub/ParsedText.cpp \
  lib/Epub/Epub/blocks/TextBlock.h test/cjk_layout/CjkLayoutTest.cpp
git add lib/Epub/Epub/ParsedText.h lib/Epub/Epub/ParsedText.cpp lib/Epub/Epub/blocks/TextBlock.h test/cjk_layout/ \
  test/CMakeLists.txt test/word_size_layout/CMakeLists.txt test/epub_pipeline/CMakeLists.txt
git commit -m "feat(epub): CJK words become one token per character" \
  -m "A word with CJK characters is split at every break the CJK rules allow: lines break between its characters, at zero-width gaps, and kinsoku pairs share a token. A token glued in the source meets the previous one at such a gap where the rules allow it; where the source had whitespace the space stays. Our implementation of crosspoint-reader PR #2288's tokens (22f35750) and PR #2768's glued rule (\"fix: restore spaces between CJK words\", Uri Tauber, 9163149a); the dash rule of PR #3883 is not ported, so glued Latin stays glued."
```

### Task 4: Korean words split between syllables at a line end

Korean separates its words with spaces, so Task 3 leaves a Hangul word whole. When one has to split, with hyphenation
on or because the word is wider than the line, it splits between syllables, without a hyphen, and never before
closing punctuation.

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.cpp`
- Modify: `test/cjk_layout/CjkLayoutTest.cpp`

**Interfaces:**
- Consumes: Task 1's `CjkLineBreak::hangulLineEndBreaks`; Task 3's `test/cjk_layout` target.
- Produces: `ParsedText::hyphenateWordAtIndex` tries the Hangul line-end breaks before the Hyphenator's. A Hangul split
  is a hyphenated line end, so the page-bottom retry re-lays the line with the word whole.

- [ ] **Step 1: Write the failing tests**

Upstream's six `KoreanLineBreaking` and three `KoreanLayout` tests, with the widths recomputed for this shim, and two
of ours: the page-bottom retry, and a Hangul word wider than the line with hyphenation off:

<!-- plan-edit: test/cjk_layout/CjkLayoutTest.cpp -->
```cpp
#include "Epub/blocks/TextBlock.h"
```
with:

```cpp
#include "Epub/blocks/TextBlock.h"
#include "Epub/hyphenation/Hyphenator.h"
```

<!-- plan-append: test/cjk_layout/CjkLayoutTest.cpp -->
```cpp
// Korean. Ported from crosspoint-reader's KoreanLineBreaking tests (PR #3700, Sung-jin Brian Hong) and
// KoreanLayout tests (PRs #2288 and #3700), with the widths recomputed for this shim (10 px per
// glyph, 5 px per space; upstream's is 8 and 4).
namespace {

BlockStyle justified() {
  BlockStyle bs = leftAligned();
  bs.alignment = CssTextAlign::Justify;
  return bs;
}

std::vector<Line> layoutWords(const std::vector<const char*>& words, const bool hyphenation, const uint16_t width,
                              const BlockStyle& style = justified()) {
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, hyphenation, style);
  for (const char* word : words) text.addWord(word, EpdFontFamily::REGULAR);
  return layout(text, width);
}

}  // namespace

TEST(KoreanLineBreaking, HyphenationOffWrapsAtSpacesOnly) {
  // 가나 다라마바 needs 65 px; with hyphenation off the Hangul word moves down whole.
  EXPECT_EQ(wordsOf(layoutWords({"가나", "다라마바", "사"}, false, 62)),
            (std::vector<std::vector<std::string>>{{"가나"}, {"다라마바", "사"}}));
}

TEST(KoreanLineBreaking, HyphenationOnSplitsBetweenSyllablesWithoutHyphen) {
  // 가나 + space leaves 37 px: the widest syllable prefix that fits is 다라마 (30 px).
  const auto lines = layoutWords({"가나", "다라마바사아", "자"}, true, 62);
  ASSERT_EQ(wordsOf(lines), (std::vector<std::vector<std::string>>{{"가나", "다라마"}, {"바사아", "자"}}));
  // 20 + 5 + 30 = 55 px leaves 7 px, all of it on the one word space.
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 32}));
}

TEST(KoreanLineBreaking, HyphenationOnKeepsTrailingPunctuationWithSyllable) {
  // 가나다 + space leaves 45 px. 라마바사 (40 px) would fit, but 사. stays together, so 라마바 is used.
  EXPECT_EQ(wordsOf(layoutWords({"가나다", "라마바사.", "자"}, true, 80)),
            (std::vector<std::vector<std::string>>{{"가나다", "라마바"}, {"사.", "자"}}));
}

TEST(KoreanLineBreaking, HyphenationOnSplitsShortWords) {
  // 가나다 + space leaves 25 px, so the three-syllable word splits after 라마 (20 px).
  EXPECT_EQ(wordsOf(layoutWords({"가나다", "라마바", "자"}, true, 60)),
            (std::vector<std::vector<std::string>>{{"가나다", "라마"}, {"바", "자"}}));
}

TEST(KoreanLineBreaking, HyphenationOnSplitsWhereHangulMeetsOtherScriptsOrBrackets) {
  // 가 + space leaves 65 px. 소신(ab) (60 px) fits; the split may fall before "(" or after ")",
  // never inside the brackets.
  EXPECT_EQ(wordsOf(layoutWords({"가", "소신(ab)이"}, true, 80)),
            (std::vector<std::vector<std::string>>{{"가", "소신(ab)"}, {"이"}}));
}

TEST(KoreanLineBreaking, HyphenationOnSplitsBetweenDigitAndHangul) {
  // 가나다 + space leaves 25 px: 12 (20 px) fits, 12월 (30 px) does not.
  EXPECT_EQ(wordsOf(layoutWords({"가나다", "12월부터", "자"}, true, 60)),
            (std::vector<std::vector<std::string>>{{"가나다", "12"}, {"월부터", "자"}}));
}

TEST(KoreanLayout, HangulWordsStayWholeAndWrapAtSpaces) {
  // 가나다 라마 is 55 px; adding 3개입니다 would need 110 px, and no break exists inside it. (Upstream's
  // 3개를 ties two layouts under our line breaker, which keeps the first it finds.)
  EXPECT_EQ(wordsOf(layoutWords({"가나다", "라마", "3개입니다", "iPhone을"}, false, 75, leftAligned())),
            (std::vector<std::vector<std::string>>{{"가나다", "라마"}, {"3개입니다"}, {"iPhone을"}}));
}

TEST(KoreanLayout, JustifiedHangulStretchesOnlyWordSpaces) {
  // 3 x 20 px words + 2 x 5 px spaces leave 4 px, split across the two spaces only.
  const auto lines = layoutWords({"가나", "다라", "마바", "사아"}, false, 74);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].xpos, (std::vector<int16_t>{0, 27, 54}));
}

TEST(KoreanLayout, HangulGluedAcrossInlineStyleIsUnbreakable) {
  ParsedText text(false, false, justified());
  text.addWord("가나", EpdFontFamily::REGULAR);
  text.addWord("한국", EpdFontFamily::REGULAR);
  text.addWord("어", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  // 가나 한국 fits in 50 px, but 어 is glued to 한국, so the whole word moves down.
  EXPECT_EQ(wordsOf(layout(text, 50)), (std::vector<std::vector<std::string>>{{"가나"}, {"한국", "어"}}));
}

// Ours: a Hangul split is a hyphenated line end, so at a page bottom the parser's retry re-lays the
// line with the word whole, as it does for a Latin hyphenation.
TEST(KoreanLayout, PageBottomRetryKeepsTheHangulWordWhole) {
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, /*hyphenationEnabled=*/true, justified());
  for (const char* word : {"가나", "다라마바사아", "자"}) text.addWord(word, EpdFontFamily::REGULAR);
  GfxRenderer renderer;
  std::vector<std::vector<std::string>> lines;
  bool retried = false;
  text.layoutAndExtractLines(
      renderer, kFontId, 62,
      [&](std::unique_ptr<TextBlock> line, const bool endsHyphenated, const bool suppressRetry) {
        if (endsHyphenated && !suppressRetry && !retried) {  // what the parser does at a page bottom
          retried = true;
          return ParsedText::LineProcessResult::RetryWithoutHyphenation;
        }
        std::vector<std::string> words;
        for (uint16_t i = 0; i < line->wordCount(); i++) words.emplace_back(line->wordText(i));
        lines.push_back(std::move(words));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  ASSERT_TRUE(retried);
  // The retried line takes 가나 alone; the word moves down whole and fills the next line (60 of 62 px).
  EXPECT_EQ(lines, (std::vector<std::vector<std::string>>{{"가나"}, {"다라마바사아"}, {"자"}}));
}

// Ours: with hyphenation off, a Hangul word wider than the whole line still splits between syllables,
// without a hyphen, rather than overflow the line.
TEST(KoreanLayout, AWordWiderThanTheLineSplitsBetweenSyllables) {
  const auto lines = layoutWords({"다라마바사아"}, false, 35);
  EXPECT_EQ(wordsOf(lines), (std::vector<std::vector<std::string>>{{"다라마"}, {"바사아"}}));
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build build/test --target CjkLayoutTest -j 8 && ./build/test/cjk_layout/CjkLayoutTest.exe --gtest_brief=1`
Expected: 6 failures, among them `KoreanLineBreaking.HyphenationOnSplitsBetweenSyllablesWithoutHyphen` and
`KoreanLayout.PageBottomRetryKeepsTheHangulWordWhole` (`[  PASSED  ] 15 tests.`). The tests of words that need no
split pass already. So does `AWordWiderThanTheLineSplitsBetweenSyllables`: the Hyphenator's fallback breaks reach it,
and Task 2 took their hyphen away. It stays as a guard.

- [ ] **Step 3: The Hangul line-end breaks**

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
```
with:

```cpp
  // Collect candidate breakpoints (byte offsets and hyphen requirements). A Hangul word may split
  // between syllables at a line end, without a hyphen; those breaks come first, so they win a width
  // tie. From crosspoint-reader PR #3700 ("fix: justify Korean text by word spaces only", Sung-jin
  // Brian Hong).
  std::vector<Hyphenator::BreakInfo> breakInfos = CjkLineBreak::hangulLineEndBreaks(word);
  const std::vector<Hyphenator::BreakInfo> hyphenatorBreaks = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  breakInfos.insert(breakInfos.end(), hyphenatorBreaks.begin(), hyphenatorBreaks.end());
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target CjkLayoutTest -j 8 && ./build/test/cjk_layout/CjkLayoutTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 21 tests.`

Run: `cmake --build build/test --target WordSizeLayoutTest -j 8 && ./build/test/word_size_layout/WordSizeLayoutTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 37 tests.`

- [ ] **Step 5: Commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror lib/Epub/Epub/ParsedText.cpp test/cjk_layout/CjkLayoutTest.cpp
git add lib/Epub/Epub/ParsedText.cpp test/cjk_layout/CjkLayoutTest.cpp
git commit -m "feat(epub): Korean words split between syllables at a line end" \
  -m "A Hangul word that has to split, with hyphenation on or because it is wider than the line, splits between syllables or where Hangul meets digits, Latin or brackets, without a hyphen and never before closing punctuation. From crosspoint-reader PR #3700 (\"fix: justify Korean text by word spaces only\", 794ac97f); its KoreanLineBreaking and KoreanLayout tests are ported with this shim's widths." \
  -m "Co-authored-by: Sung-jin Brian Hong <serialx@serialx.net>"
```

### Task 5: The parser, CJK books and the rule checker

Tasks 3 and 4 lay out what the parser hands them. The parser still cuts a long CJK run into 200-byte words separated
by spaces, turns a source line break between CJK characters into a space, and charges a table row once per flushed
run. This task adds four CJK books and a checker that finds those spaces, then fixes the parser so it finds none.

**Files:**
- Create: `test/epubs/make_test_cjk_line_breaking.py`, `test/epubs/make_test_cjk_excerpts.py`
- Create (generated by those scripts): `test/epubs/test_cjk_line_breaking.epub`, `test_cjk_ja_soseki.epub`,
  `test_cjk_zh_honglou.epub`, `test_cjk_ko_hyeon.epub`
- Create: `test/epub_pipeline/CjkRulesTest.cpp`; modify `test/epub_pipeline/CMakeLists.txt`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`, `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`,
  `lib/Epub/Epub/Section.cpp`
- Create (generated): `test/epub_pipeline/goldens/test_cjk_*.golden.txt` (4); modify `heap_peak_baseline.txt`

**Interfaces:**
- Consumes: Tasks 1-4. `utf8IsUnspacedCjk`, `utf8IsCjkBreakable`, `CjkLineBreak::isNoLineStart`/`isNoLineEnd`;
  `ParsedText::size()` (tokens so far); `pipeline_harness::runAndDump` and `Profile` (`paragraphAlignment = 1` is left
  aligned).
- Produces: `SECTION_FILE_VERSION` 87; the CJK books in the corpus (every corpus test now runs them).

- [ ] **Step 1: The books**

A fixture with one paragraph per case:

<!-- plan-file: test/epubs/make_test_cjk_line_breaking.py -->
```python
#!/usr/bin/env python3
"""Build test_cjk_line_breaking.epub: one paragraph per CJK line-breaking case, for the rule checker
(test/epub_pipeline/CjkRulesTest.cpp) and the goldens.

  1. kinsoku       punctuation that must not start a line (。、」！？…… 々 ・), brackets that must not end
                   one (「『（), and small kana and ー, which may start one
  2. newlines      hard-wrapped source: a line break between two CJK characters vanishes, one between
                   CJK and Latin stays a space
  3. long run      a CJK run longer than 200 bytes and 96 tokens, with no punctuation to break it
  4. inline        CJK split across <b>, <i> and <span>: the seams are breaks without spaces
  5. mixed         Latin, digits, fullwidth and halfwidth forms next to CJK
  6. korean        Hangul words stay whole and keep their spaces
  7. table         CJK text in a table cell
  8. table budget  a cell of 385 CJK characters, one token each: charged per token, the row passes the
                   12 KB row budget and flows as text (CjkTable.ALongCjkCellIsChargedPerToken)

Each case paragraph opens with an indented first line (CSS text-indent), so the rule checker tells a
paragraph's first line from a line the layout broke.

Regenerate with:  python test/epubs/make_test_cjk_line_breaking.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_cjk_line_breaking.epub")

CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
h2 { margin: 1em 0 0 0; text-indent: 0; font-size: 1em }
td { padding: 0 }
"""

KINSOKU = (
    "「こんにちは。」と彼は言った。『本当に？』――私は答えた。（もちろん、そうです！）……そして、静かになった。"
    "人々は時々笑い、ラーメンを食べ、きゃっきゃっと騒いだ。ああ……！　彼女は「ええ、そうね」と言い、"
    "窓の外を見た。雨は止まない。「どうしよう？」――誰も答えない。"
)

# The Thousand Character Classic: a long run of Han characters with no punctuation at all.
LONG_RUN = (
    "天地玄黃宇宙洪荒日月盈昃辰宿列張寒來暑往秋收冬藏閏餘成歲律呂調陽雲騰致雨露結為霜金生麗水玉出崑岡"
    "劍號巨闕珠稱夜光果珍李柰菜重芥薑海鹹河淡鱗潛羽翔龍師火帝鳥官人皇始制文字乃服衣裳推位讓國有虞陶唐"
)

CASES = [
    ("1. kinsoku", f"<p>{KINSOKU}</p>\n<p>{KINSOKU}</p>"),
    (
        "2. newlines",
        "<p>天下大勢，分久必合，\n合久必分。周末七國分爭，\n並入於秦。及秦滅之後，楚、漢分爭，\n"
        "又並入於漢。漢朝自高祖斬白蛇而起義，\n一統天下。</p>\n"
        "<p>漢字の文章の途中で\n改行があっても、\n空白にはならない。しかし漢字\nLatin の前では空白になる。\n"
        "「引用」の後も\n「次の引用」の前も同じ。</p>",
    ),
    ("3. long run", f"<p>{LONG_RUN}</p>"),
    (
        "4. inline",
        "<p>漢字の<b>強調された文字</b>と<i>斜体の文字</i>、<span>そして</span>普通の文字が"
        "<b>一つの段落</b>の中で<i>混ざって</i>いる。</p>",
    ),
    (
        "5. mixed",
        "<p>漢字 iPhone 漢字と漢字iPhone漢字。2026年10月17日に、ＡＢＣ全角と半角のｱｲｳｴｵ｡を書いた。"
        "価格は1,234円（税込）です。</p>",
    ),
    (
        "6. korean",
        "<p>가나다 라마 소신(ab)이 12월부터 한국어<b>문장</b>입니다. 새침하게 흐린 품이 눈이 올 듯하더니 "
        "눈은 아니 오고 얼다가 만 비가 추적추적 내리었다.</p>",
    ),
    (
        "7. table",
        "<table><tr><td><p>表のセルの中にある長い文章も、同じ規則で改行される。句読点は行頭に来ない。</p></td>"
        "<td><p>短い</p></td></tr></table>",
    ),
    ("8. table budget", f"<table><tr><td><p>鑑{LONG_RUN * 3}</p></td></tr></table>"),
]

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja" lang="ja">
<head>
  <title>CJK line breaking</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
{body}
</body>
</html>
"""


def main():
    body = "\n".join(f"<h2>{label}</h2>\n{content}" for label, content in CASES)
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>CJK Line Breaking</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>ja</dc:language>\n"
        '    <dc:identifier id="bookid">cjk-line-breaking</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="css" href="style.css" media-type="text/css"/>\n'
        '    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>\n'
        "  </manifest>\n"
        '  <spine>\n    <itemref idref="chapter1"/>\n  </spine>\n'
        "</package>\n"
    )
    container = (
        '<?xml version="1.0"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles>\n'
        "</container>\n"
    )
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/style.css", CSS)
        z.writestr("OEBPS/chapter1.xhtml", CHAPTER.format(body=body))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
```

Three public-domain excerpts, their text embedded in the generator:

<!-- plan-file: test/epubs/make_test_cjk_excerpts.py -->
```python
#!/usr/bin/env python3
"""Build three corpus books from short public-domain excerpts in Chinese, Japanese and Korean:
real text for the CJK line-breaking rules (CjkRulesTest) and their goldens.

  test_cjk_ja_soseki.epub   Natsume Soseki, Wagahai wa neko de aru, the opening of chapter 1, with
                            its ruby. Aozora Bunko, cards/000148/files/789_14547.html.
  test_cjk_zh_honglou.epub  Cao Xueqin, Hong Lou Meng (Dream of the Red Chamber), the opening of
                            chapter 1. Project Gutenberg eBook #24264. Its source line breaks are kept
                            inside each paragraph, as books converted from hard-wrapped text keep
                            them: they test the CSS segment-break rule.
  test_cjk_ko_hyeon.epub    Hyeon Jin-geon, Unsu joeun nal (A Lucky Day, 1924), the opening.
                            Korean Wikisource.

All three authors died before 1944; the texts are in the public domain. Only the text is taken: no
Project Gutenberg or Aozora markup, headers or trademarks. A leading ideographic-space indent is
replaced by CSS text-indent, so the first line of each paragraph starts indented.

Regenerate with:  python test/epubs/make_test_cjk_excerpts.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
h1 { margin: 0 0 1em 0; text-indent: 0; font-size: 1.2em }
"""

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
  <h1>{title}</h1>
{body}
</body>
</html>
"""

# Wagahai wa neko de aru, chapter 1 (Aozora Bunko XHTML, ruby kept).
JA = [
    "<ruby><rb>吾輩</rb><rp>（</rp><rt>わがはい</rt><rp>）</rp></ruby>は猫である。名前はまだ無い。",
    "どこで生れたかとんと<ruby><rb>見当</rb><rp>（</rp><rt>けんとう</rt><rp>）</rp></ruby>がつかぬ。何でも薄暗いじめじめした所でニャーニャー泣いていた事だけは記憶している。吾輩はここで始めて人間というものを見た。しかもあとで聞くとそれは書生という人間中で一番<ruby><rb>獰悪</rb><rp>（</rp><rt>どうあく</rt><rp>）</rp></ruby>な種族であったそうだ。この書生というのは時々我々を<ruby><rb>捕</rb><rp>（</rp><rt>つかま</rt><rp>）</rp></ruby>えて<ruby><rb>煮</rb><rp>（</rp><rt>に</rt><rp>）</rp></ruby>て食うという話である。しかしその当時は何という考もなかったから別段恐しいとも思わなかった。ただ彼の<ruby><rb>掌</rb><rp>（</rp><rt>てのひら</rt><rp>）</rp></ruby>に載せられてスーと持ち上げられた時何だかフワフワした感じがあったばかりである。掌の上で少し落ちついて書生の顔を見たのがいわゆる人間というものの<ruby><rb>見始</rb><rp>（</rp><rt>みはじめ</rt><rp>）</rp></ruby>であろう。この時妙なものだと思った感じが今でも残っている。第一毛をもって装飾されべきはずの顔がつるつるしてまるで<ruby><rb>薬缶</rb><rp>（</rp><rt>やかん</rt><rp>）</rp></ruby>だ。その<ruby><rb>後</rb><rp>（</rp><rt>ご</rt><rp>）</rp></ruby>猫にもだいぶ<ruby><rb>逢</rb><rp>（</rp><rt>あ</rt><rp>）</rp></ruby>ったがこんな<ruby><rb>片輪</rb><rp>（</rp><rt>かたわ</rt><rp>）</rp></ruby>には一度も<ruby><rb>出会</rb><rp>（</rp><rt>でく</rt><rp>）</rp></ruby>わした事がない。のみならず顔の真中があまりに突起している。そうしてその穴の中から時々ぷうぷうと<ruby><rb>煙</rb><rp>（</rp><rt>けむり</rt><rp>）</rp></ruby>を吹く。どうも<ruby><rb>咽</rb><rp>（</rp><rt>む</rt><rp>）</rp></ruby>せぽくて実に弱った。これが人間の飲む<ruby><rb>煙草</rb><rp>（</rp><rt>たばこ</rt><rp>）</rp></ruby>というものである事はようやくこの頃知った。",
    "この書生の掌の<ruby><rb>裏</rb><rp>（</rp><rt>うち</rt><rp>）</rp></ruby>でしばらくはよい心持に坐っておったが、しばらくすると非常な速力で運転し始めた。書生が動くのか自分だけが動くのか分らないが<ruby><rb>無暗</rb><rp>（</rp><rt>むやみ</rt><rp>）</rp></ruby>に眼が廻る。胸が悪くなる。<ruby><rb>到底</rb><rp>（</rp><rt>とうてい</rt><rp>）</rp></ruby>助からないと思っていると、どさりと音がして眼から火が出た。それまでは記憶しているがあとは何の事やらいくら考え出そうとしても分らない。",
    "ふと気が付いて見ると書生はいない。たくさんおった兄弟が一<ruby><rb>疋</rb><rp>（</rp><rt>ぴき</rt><rp>）</rp></ruby>も見えぬ。<ruby><rb>肝心</rb><rp>（</rp><rt>かんじん</rt><rp>）</rp></ruby>の母親さえ姿を隠してしまった。その上<ruby><rb>今</rb><rp>（</rp><rt>いま</rt><rp>）</rp></ruby>までの所とは違って<ruby><rb>無暗</rb><rp>（</rp><rt>むやみ</rt><rp>）</rp></ruby>に明るい。眼を明いていられぬくらいだ。はてな何でも<ruby><rb>容子</rb><rp>（</rp><rt>ようす</rt><rp>）</rp></ruby>がおかしいと、のそのそ<ruby><rb>這</rb><rp>（</rp><rt>は</rt><rp>）</rp></ruby>い出して見ると非常に痛い。吾輩は<ruby><rb>藁</rb><rp>（</rp><rt>わら</rt><rp>）</rp></ruby>の上から急に笹原の中へ棄てられたのである。",
    "ようやくの思いで笹原を這い出すと向うに大きな池がある。吾輩は池の前に坐ってどうしたらよかろうと考えて見た。別にこれという<ruby><rb>分別</rb><rp>（</rp><rt>ふんべつ</rt><rp>）</rp></ruby>も出ない。しばらくして泣いたら書生がまた迎に来てくれるかと考え付いた。ニャー、ニャーと試みにやって見たが誰も来ない。そのうち池の上をさらさらと風が渡って日が暮れかかる。腹が非常に減って来た。泣きたくても声が出ない。仕方がない、何でもよいから<ruby><rb>食物</rb><rp>（</rp><rt>くいもの</rt><rp>）</rp></ruby>のある所まであるこうと決心をしてそろりそろりと池を<ruby><rb>左</rb><rp>（</rp><rt>ひだ</rt><rp>）</rp></ruby>りに廻り始めた。どうも非常に苦しい。そこを我慢して無理やりに<ruby><rb>這</rb><rp>（</rp><rt>は</rt><rp>）</rp></ruby>って行くとようやくの事で何となく人間臭い所へ出た。ここへ<ruby><rb>這入</rb><rp>（</rp><rt>はい</rt><rp>）</rp></ruby>ったら、どうにかなると思って竹垣の<ruby><rb>崩</rb><rp>（</rp><rt>くず</rt><rp>）</rp></ruby>れた穴から、とある邸内にもぐり込んだ。縁は不思議なもので、もしこの竹垣が破れていなかったなら、吾輩はついに<ruby><rb>路傍</rb><rp>（</rp><rt>ろぼう</rt><rp>）</rp></ruby>に<ruby><rb>餓死</rb><rp>（</rp><rt>がし</rt><rp>）</rp></ruby>したかも知れんのである。一樹の蔭とはよく<ruby><rb>云</rb><rp>（</rp><rt>い</rt><rp>）</rp></ruby>ったものだ。この垣根の穴は<ruby><rb>今日</rb><rp>（</rp><rt>こんにち</rt><rp>）</rp></ruby>に至るまで吾輩が<ruby><rb>隣家</rb><rp>（</rp><rt>となり</rt><rp>）</rp></ruby>の三毛を訪問する時の通路になっている。さて<ruby><rb>邸</rb><rp>（</rp><rt>やしき</rt><rp>）</rp></ruby>へは忍び込んだもののこれから先どうして<ruby><rb>善</rb><rp>（</rp><rt>い</rt><rp>）</rp></ruby>いか分らない。そのうちに暗くなる、腹は減る、寒さは寒し、雨が降って来るという始末でもう一刻の<ruby><rb>猶予</rb><rp>（</rp><rt>ゆうよ</rt><rp>）</rp></ruby>が出来なくなった。仕方がないからとにかく明るくて暖かそうな方へ方へとあるいて行く。今から考えるとその時はすでに家の内に這入っておったのだ。ここで吾輩は<ruby><rb>彼</rb><rp>（</rp><rt>か</rt><rp>）</rp></ruby>の書生以外の人間を再び見るべき機会に<ruby><rb>遭遇</rb><rp>（</rp><rt>そうぐう</rt><rp>）</rp></ruby>したのである。第一に逢ったのがおさんである。これは前の書生より一層乱暴な方で吾輩を見るや否やいきなり<ruby><rb>頸筋</rb><rp>（</rp><rt>くびすじ</rt><rp>）</rp></ruby>をつかんで表へ<ruby><rb>抛</rb><rp>（</rp><rt>ほう</rt><rp>）</rp></ruby>り出した。いやこれは駄目だと思ったから眼をねぶって運を天に任せていた。しかしひもじいのと寒いのにはどうしても我慢が出来ん。吾輩は再びおさんの<ruby><rb>隙</rb><rp>（</rp><rt>すき</rt><rp>）</rp></ruby>を見て台所へ<ruby><rb>這</rb><rp>（</rp><rt>は</rt><rp>）</rp></ruby>い<ruby><rb>上</rb><rp>（</rp><rt>あが</rt><rp>）</rp></ruby>った。すると間もなくまた投げ出された。吾輩は投げ出されては這い上り、這い上っては投げ出され、何でも同じ事を四五遍繰り返したのを記憶している。その時におさんと云う者はつくづくいやになった。この間おさんの<ruby><rb>三馬</rb><rp>（</rp><rt>さんま</rt><rp>）</rp></ruby>を<ruby><rb>偸</rb><rp>（</rp><rt>ぬす</rt><rp>）</rp></ruby>んでこの返報をしてやってから、やっと胸の<ruby><rb>痞</rb><rp>（</rp><rt>つかえ</rt><rp>）</rp></ruby>が下りた。吾輩が最後につまみ出されようとしたときに、この<ruby><rb>家</rb><rp>（</rp><rt>うち</rt><rp>）</rp></ruby>の主人が騒々しい何だといいながら出て来た。下女は吾輩をぶら下げて主人の方へ向けてこの<ruby><rb>宿</rb><rp>（</rp><rt>やど</rt><rp>）</rp></ruby>なしの小猫がいくら出しても出しても<ruby><rb>御台所</rb><rp>（</rp><rt>おだいどころ</rt><rp>）</rp></ruby>へ<ruby><rb>上</rb><rp>（</rp><rt>あが</rt><rp>）</rp></ruby>って来て困りますという。主人は鼻の下の黒い毛を<ruby><rb>撚</rb><rp>（</rp><rt>ひね</rt><rp>）</rp></ruby>りながら吾輩の顔をしばらく<ruby><rb>眺</rb><rp>（</rp><rt>なが</rt><rp>）</rp></ruby>めておったが、やがてそんなら内へ置いてやれといったまま奥へ<ruby><rb>這入</rb><rp>（</rp><rt>はい</rt><rp>）</rp></ruby>ってしまった。主人はあまり口を聞かぬ人と見えた。下女は<ruby><rb>口惜</rb><rp>（</rp><rt>くや</rt><rp>）</rp></ruby>しそうに吾輩を台所へ<ruby><rb>抛</rb><rp>（</rp><rt>ほう</rt><rp>）</rp></ruby>り出した。かくして吾輩はついにこの<ruby><rb>家</rb><rp>（</rp><rt>うち</rt><rp>）</rp></ruby>を自分の<ruby><rb>住家</rb><rp>（</rp><rt>すみか</rt><rp>）</rp></ruby>と<ruby><rb>極</rb><rp>（</rp><rt>き</rt><rp>）</rp></ruby>める事にしたのである。",
]

# Hong Lou Meng, chapter 1 (Project Gutenberg #24264), with its source line breaks.
ZH = [
    "此開卷第一回也．作者自云：因曾歷過一番夢幻之后，故將真事隱去，\n而借\"通靈\"之說，撰此《石頭記》一書也．故曰\"甄士隱\"云云．但書中所記\n何事何人？自又云：“今風塵碌碌，一事無成，忽念及當日所有之女子，一\n一細考較去，覺其行止見識，皆出于我之上．何我堂堂須眉，誠不若彼裙釵\n哉？實愧則有余，悔又無益之大無可如何之日也！當此，則自欲將已往所賴\n天恩祖德，錦衣紈褲之時，飫甘饜肥之日，背父兄教育之恩，負師友規談之\n德，以至今日一技無成，半生潦倒之罪，編述一集，以告天下人：我之罪固\n不免，然閨閣中本自歷歷有人，万不可因我之不肖，自護己短，一并使其泯\n滅也．雖今日之茅椽蓬牖，瓦灶繩床，其晨夕風露，階柳庭花，亦未有妨我\n之襟怀筆墨者．雖我未學，下筆無文，又何妨用假語村言，敷演出一段故事\n來，亦可使閨閣昭傳，复可悅世之目，破人愁悶，不亦宜乎？\"故曰\"賈雨村\n\"云云．",
    "此回中凡用“夢”用“幻”等字，是提醒閱者眼目，亦是此書立意本旨\n．",
    "列位看官：你道此書從何而來？說起根由雖近荒唐，細按則深有趣味．\n待在下將此來歷注明，方使閱者了然不惑．",
    "原來女媧氏煉石補天之時，于大荒山無稽崖練成高經十二丈，方經二十\n四丈頑石三万六千五百零一塊．媧皇氏只用了三万六千五百塊，只單單剩了\n一塊未用，便棄在此山青埂峰下．誰知此石自經段煉之后，靈性已通，因見\n眾石俱得補天，獨自己無材不堪入選，遂自怨自歎，日夜悲號慚愧．",
    "一日，正當嗟悼之際，俄見一僧一道遠遠而來，生得骨骼不凡，丰神迥\n异，說說笑笑來至峰下，坐于石邊高談快論．先是說些云山霧海神仙玄幻之\n事，后便說到紅塵中榮華富貴．此石听了，不覺打動凡心，也想要到人間去\n享一享這榮華富貴，但自恨粗蠢，不得已，便口吐人言，向那僧道說道：“\n大師，弟子蠢物，不能見禮了．适聞二位談那人世間榮耀繁華，心切慕之．\n弟子質雖粗蠢，性卻稍通，況見二師仙形道体，定非凡品，必有補天濟世之\n材，利物濟人之德．如蒙發一點慈心，攜帶弟子得入紅塵，在那富貴場中，\n溫柔鄉里受享几年，自當永佩洪恩，万劫不忘也。”二仙師听畢，齊憨笑道\n：“善哉，善哉！那紅塵中有卻有些樂事，但不能永遠依恃，況又有`美中\n不足，好事多魔'八個字緊相連屬，瞬息間則又樂极悲生，人非物換，究竟\n是到頭一夢，万境歸空，倒不如不去的好。”這石凡心已熾，那里听得進這\n話去，乃复苦求再四．二仙知不可強制，乃歎道：“此亦靜极慫級*，無中\n生有之數也．既如此，我們便攜你去受享受享，只是到不得意時，切莫后悔\n。”石道：“自然，自然。”那僧又道：“若說你性靈，卻又如此質蠢，并\n更無奇貴之處．如此也只好踮腳而已．也罷，我如今大施佛法助你助，待劫\n終之日，复還本質，以了此案．你道好否？\"石頭听了，感謝不盡．那僧便\n念咒書符，大展幻術，將一塊大石登時變成一塊鮮明瑩洁的美玉，且又縮成\n扇墜大小的可佩可拿．那僧托于掌上，笑道：“形体倒也是個寶物了！還只\n沒有，實在的好處，須得再鐫上數字，使人一見便知是奇物方妙．然后攜你\n到那昌明隆盛之邦，詩禮簪纓之族，花柳繁華地，溫柔富貴鄉去安身樂業。\n”石頭听了，喜不能禁，乃問：“不知賜了弟子那几件奇處，又不知攜了弟\n子到何地方？望乞明示，使弟子不惑。”那僧笑道：“你且莫問，日后自然\n明白的。”說著，便袖了這石，同那道人飄然而去，竟不知投奔何方何舍．",
    "后來，又不知過了几世几劫，因有個空空道人訪道求仙，忽從這大荒山\n無稽崖青埂峰下經過，忽見一大塊石上字跡分明，編述歷歷．空空道人乃從\n頭一看，原來就是無材補天，幻形入世，蒙茫茫大士，渺渺真人攜入紅塵，\n歷盡离合悲歡炎涼世態的一段故事．后面又有一首偈云：",
]

# Unsu joeun nal (Korean Wikisource).
KO = [
    "새침하게 흐린 품이 눈이 올 듯하더니 눈은 아니 오고 얼다가 만 비가 추적추적 내리었다.",
    "이날이야말로 동소문 안에서 인력거꾼 노릇을 하는 김 첨지에게는 오래간만에도 닥친 운수 좋은 날이었다. 문안에(거기도 문밖은 아니지만) 들어간답시는 앞집 마나님을 전찻길까지 모셔다 드린 것을 비롯으로 행여나 손님이 있을까 하고 정류장에서 어정어정하며 내리는 사람 하나하나에게 거의 비는 듯한 눈결을 보내고 있다가 마침내 교원인 듯한 양복장이를 동광학교(東光學校)까지 태워다 주기로 되었다.",
    "첫번에 삼십 전, 둘째 번에 오십 전 - 아침 댓바람에 그리 흔치 않은 일이었다. 그야말로 재수가 옴붙어서 근 열흘 동안 돈 구경도 못한 김 첨지는 십 전짜리 백통화 서 푼, 또는 다섯 푼이 찰깍하고 손바닥에 떨어질 제 거의 눈물을 흘릴 만큼 기뻤었다. 더구나 이날 이때에 이 팔십 전이라는 돈이 그에게 얼마나 유용한지 몰랐다. 컬컬한 목에 모주 한 잔도 적실 수 있거니와 그보다도 앓는 아내에게 설렁탕 한 그릇도 사다줄 수 있음이다.",
    "그의 아내가 기침으로 쿨럭거리기는 벌써 달포가 넘었다. 조밥도 굶기를 먹다시피 하는 형편이니 물론 약 한 첩 써 본 일이 없다. 구태여 쓰려면 못 쓸 바도 아니로되 그는 병이란 놈에게 약을 주어 보내면 재미를 붙여서 자꾸 온다는 자기의 신조(信條)에 어디까지 충실하였다. 따라서 의사에게 보인 적이 없으니 무슨 병인지는 알 수 없으되 반듯이 누워 가지고, 일어나기는새로 모로도 못 눕는 걸 보면 중증은 중증인 듯. 병이 이대도록 심해지기는 열흘 전에 조밥을 먹고 체한 때문이다.",
    "그때도 김 첨지가 오래간만에 돈을 얻어서 좁쌀 한 되와 십 전짜리 나무 한 단을 사다 주었더니 김 첨지의 말에 의지하면 그 오라질 년이 천방지축(天方地軸)으로 남비에 대고 끓였다. 마음은 급하고 불길은 닿지 않아 채 익지도 않은 것을 그 오라질 년이 숟가락은 고만두고 손으로 움켜서 두 뺨에 주먹덩이 같은 혹이 불거지도록 누가 빼앗을 듯이 처박질 하더니만 그날 저녁부터 가슴이 땅긴다, 배가 켕긴다고 눈을 홉뜨고 지랄병을 하였다. 그때 김 첨지는 열화와 같이 성을 내며,",
    "“에이, 오라질 년, 조롱복은 할 수가 없어, 못 먹어 병, 먹어서 병, 어쩌란 말이야! 왜 눈을 바루 뜨지 못해!”하고 김 첨지는 앓는 이의 뺨을 한 번 후려갈겼다. 홉뜬 눈은 조금 바루어졌건만 이슬이 맺히었다. 김 첨지의 눈시울도 뜨끈뜨끈하였다.",
    "이 환자가 그러고도 먹는 데는 물리지 않았다. 사흘 전부터 설렁탕 국물이 마시고 싶다고 남편을 졸랐다.",
]

BOOKS = [
    ("test_cjk_ja_soseki.epub", "ja", "吾輩は猫である", "夏目漱石", "一", JA),
    ("test_cjk_zh_honglou.epub", "zh-Hant", "紅樓夢", "曹雪芹", "第一回", ZH),
    ("test_cjk_ko_hyeon.epub", "ko", "운수 좋은 날", "현진건", "운수 좋은 날", KO),
]


def write_book(name, lang, title, author, chapter_title, paragraphs):
    body = "\n".join(f"  <p>{p}</p>" for p in paragraphs)
    chapter = CHAPTER.format(lang=lang, title=chapter_title, body=body)
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f"    <dc:title>{title}</dc:title>\n"
        f"    <dc:creator>{author}</dc:creator>\n"
        f"    <dc:language>{lang}</dc:language>\n"
        f'    <dc:identifier id="bookid">{name}</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="css" href="style.css" media-type="text/css"/>\n'
        '    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>\n'
        "  </manifest>\n"
        '  <spine>\n    <itemref idref="chapter1"/>\n  </spine>\n'
        "</package>\n"
    )
    container = (
        '<?xml version="1.0"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles>\n'
        "</container>\n"
    )
    out = os.path.join(HERE, name)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/style.css", CSS)
        z.writestr("OEBPS/chapter1.xhtml", chapter)
    print("wrote", out)


def main():
    for book in BOOKS:
        write_book(*book)


if __name__ == "__main__":
    main()
```

Run: `python test/epubs/make_test_cjk_line_breaking.py && python test/epubs/make_test_cjk_excerpts.py`
Expected: `test_cjk_line_breaking.epub`, `test_cjk_ja_soseki.epub`, `test_cjk_zh_honglou.epub` and `test_cjk_ko_hyeon.epub`
each reported as written.

- [ ] **Step 2: The rule checker**

Each CJK book runs left-aligned, with hyphenation off and on. The checker asserts on every line: kinsoku where the
layout broke between unspaced CJK characters, no hyphen after CJK text, and no gap between two unspaced CJK
characters. A separate test asserts that the fixture's long table cell flows as text:

<!-- plan-file: test/epub_pipeline/CjkRulesTest.cpp -->
```cpp
// The CJK line-breaking rules (spec section 9.1), checked on every line the host pipeline lays out
// for the CJK corpus books: kinsoku at the edges of the lines the layout broke, no hyphen in CJK
// text, and no space between two characters of unspaced CJK text (Han, kana, CJK punctuation).
//
// The books run left-aligned, so every gap on a line is its natural gap: zero at a CJK gap or a
// glued seam, the space advance at a word gap. Each book's paragraphs open with an indented first
// line (CSS text-indent), which tells a paragraph's first line from a line the layout broke.
#include <Utf8.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/CjkLineBreak.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

struct DumpWord {
  int x = 0;
  int style = 0;
  int sizePct = 100;
  std::string text;
};

struct DumpLine {
  std::vector<DumpWord> words;
  float sizeMultiplier = 1.0f;  // the block's font-size multiplier (LINE mult=); table cell lines have none
  bool followsLine = false;     // the line before it in `lines` is the previous line of the same flow
};

uint32_t firstCodepoint(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  return utf8NextCodepoint(&p);
}

uint32_t lastCodepoint(const std::string& text) {
  size_t i = text.size();
  while (i > 0 && (static_cast<unsigned char>(text[i - 1]) & 0xC0) == 0x80) --i;
  if (i == 0) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str() + i - 1);
  return utf8NextCodepoint(&p);
}

// The text lines of a dump in reading order: body lines (LINE) and table cell lines (LN). A spine,
// table, row or cell starts a new flow; a page break does not.
std::vector<DumpLine> parseLines(const std::string& dump) {
  std::vector<DumpLine> lines;
  bool inFlow = false;
  std::istringstream in(dump);
  std::string raw;
  while (std::getline(in, raw)) {
    const size_t start = raw.find_first_not_of(' ');
    if (start == std::string::npos) continue;
    const std::string line = raw.substr(start);
    if (line.rfind("LINE ", 0) == 0 || line.rfind("LN ", 0) == 0) {
      DumpLine next;
      next.followsLine = inFlow;
      const size_t mult = line.find(" mult=");
      if (mult != std::string::npos) next.sizeMultiplier = std::stof(line.substr(mult + 6));
      lines.push_back(next);
      inFlow = true;
    } else if (line.rfind("W x=", 0) == 0 && !lines.empty()) {
      DumpWord word;
      const size_t textAt = line.find(" t=");
      std::istringstream fields(line.substr(0, textAt));
      std::string field;
      while (fields >> field) {
        if (field.rfind("x=", 0) == 0) word.x = std::stoi(field.substr(2));
        if (field.rfind("s=", 0) == 0) word.style = std::stoi(field.substr(2));
        if (field.rfind("z=", 0) == 0) word.sizePct = std::stoi(field.substr(2));
      }
      word.text = textAt == std::string::npos ? "" : line.substr(textAt + 3);
      lines.back().words.push_back(word);
    } else if (line.rfind("SPINE ", 0) == 0 || line.rfind("TABLE ", 0) == 0 || line.rfind("ROW ", 0) == 0 ||
               line.rfind("CELL ", 0) == 0) {
      inFlow = false;
    }
  }
  return lines;
}

std::string lineText(const DumpLine& line) {
  std::string text;
  for (const auto& w : line.words) text += w.text + "|";
  return text;
}

struct Case {
  std::string epub;
  bool hyphenation = false;
};

void PrintTo(const Case& c, std::ostream* os) {
  *os << fs::path(c.epub).filename().string() << (c.hyphenation ? " [hyphenation]" : "");
}

class CjkRules : public testing::TestWithParam<Case> {
 protected:
  void SetUp() override {
    const Case c = GetParam();
    const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" /
                          ("cjk_rules_" + fs::path(c.epub).stem().string() + (c.hyphenation ? "_hyph" : ""));
    fs::remove_all(cacheDir);
    fs::create_directories(cacheDir);
    pipeline_harness::Profile profile;
    profile.paragraphAlignment = 1;  // left: a line's gaps are its natural gaps
    profile.hyphenationEnabled = c.hyphenation;
    std::ostringstream dump;
    ASSERT_TRUE(pipeline_harness::runAndDump(c.epub, cacheDir.string(), profile, dump)) << dump.str();
    lines = parseLines(dump.str());
    ASSERT_FALSE(lines.empty());
  }

  // As ParsedText measures a word: the block's multiplier times the word's own size, rounded.
  int wordWidth(const DumpLine& line, const DumpWord& w) const {
    const int raw = renderer.getTextAdvanceX(1, w.text.c_str(), static_cast<EpdFontFamily::Style>(w.style));
    const float scale = line.sizeMultiplier * (w.sizePct / 100.0f);
    return scale == 1.0f ? raw : static_cast<int>(raw * scale + 0.5f);
  }

  GfxRenderer renderer;
  std::vector<DumpLine> lines;
};

// A line the layout broke between two unspaced CJK characters does not start with closing
// punctuation and the line before it does not end with an opening bracket (spec Appendix A.1).
TEST_P(CjkRules, KinsokuHoldsAtEveryCjkLineBreak) {
  for (size_t i = 1; i < lines.size(); ++i) {
    const DumpLine& prev = lines[i - 1];
    const DumpLine& line = lines[i];
    if (!line.followsLine || prev.words.empty() || line.words.empty()) continue;
    if (line.words.front().x > 0) continue;  // a paragraph's indented first line: not a layout break
    const uint32_t end = lastCodepoint(prev.words.back().text);
    const uint32_t start = firstCodepoint(line.words.front().text);
    if (!utf8IsUnspacedCjk(end) || !utf8IsUnspacedCjk(start)) continue;
    EXPECT_FALSE(CjkLineBreak::isNoLineEnd(end)) << "line ends with an opening bracket: " << lineText(prev);
    EXPECT_FALSE(CjkLineBreak::isNoLineStart(start)) << "line starts with closing punctuation: " << lineText(line);
  }
}

// CJK scripts are not hyphenated: no line ends with a hyphen after a CJK character.
TEST_P(CjkRules, NoHyphenAfterCjkText) {
  for (const auto& line : lines) {
    if (line.words.empty()) continue;
    const std::string& last = line.words.back().text;
    if (last.size() < 2 || last.back() != '-') continue;
    EXPECT_FALSE(utf8IsCjkBreakable(lastCodepoint(last.substr(0, last.size() - 1))))
        << "hyphen after CJK text: " << lineText(line);
  }
}

// Between two characters of unspaced CJK text there is no space: not where the source broke a line,
// not where the parser flushed a long run, not across an inline element.
TEST_P(CjkRules, NoSpaceBetweenUnspacedCjkCharacters) {
  for (const auto& line : lines) {
    for (size_t i = 0; i + 1 < line.words.size(); ++i) {
      const DumpWord& left = line.words[i];
      const DumpWord& right = line.words[i + 1];
      if (!utf8IsUnspacedCjk(lastCodepoint(left.text)) || !utf8IsUnspacedCjk(firstCodepoint(right.text))) continue;
      EXPECT_EQ(right.x - left.x - wordWidth(line, left), 0)
          << "gap between \"" << left.text << "\" and \"" << right.text << "\" in: " << lineText(line);
    }
  }
}

std::vector<Case> cjkBooks() {
  std::vector<Case> cases;
  for (const char* book :
       {"test_cjk_line_breaking", "test_cjk_ja_soseki", "test_cjk_ko_hyeon", "test_cjk_zh_honglou"}) {
    const std::string path = (fs::path(CORPUS_DIR) / (std::string(book) + ".epub")).string();
    cases.push_back(Case{path, false});
    cases.push_back(Case{path, true});
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(CjkCorpus, CjkRules, testing::ValuesIn(cjkBooks()),
                         [](const testing::TestParamInfo<Case>& info) {
                           return fs::path(info.param.epub).stem().string() + (info.param.hyphenation ? "_hyph" : "");
                         });

}  // namespace

// A CJK word is one token per character, and the table row budget charges every token: the fixture's
// case 8, a cell of 385 CJK characters (18 KB of tokens), passes the 12 KB row budget and flows as
// text, as a cell of 385 Latin words does. Charged once per flushed run, it would count as a dozen
// words and stay buffered.
TEST(CjkTable, ALongCjkCellIsChargedPerToken) {
  const auto cacheDir = fs::temp_directory_path() / "epub_pipeline_test" / "cjk_table_budget";
  fs::remove_all(cacheDir);
  fs::create_directories(cacheDir);
  std::ostringstream dump;
  ASSERT_TRUE(pipeline_harness::runAndDump((fs::path(CORPUS_DIR) / "test_cjk_line_breaking.epub").string(),
                                           cacheDir.string(), pipeline_harness::Profile{}, dump));
  std::istringstream in(dump.str());
  std::string raw;
  std::string lastLineRecord;
  while (std::getline(in, raw)) {
    const std::string line = raw.substr(raw.find_first_not_of(' '));
    if (line.rfind("LINE ", 0) == 0 || line.rfind("LN ", 0) == 0) lastLineRecord = line;
    if (line.find(" t=鑑") != std::string::npos) {
      EXPECT_EQ(lastLineRecord.rfind("LINE ", 0), 0u) << "case 8 stayed in a table cell: " << lastLineRecord;
      return;
    }
  }
  FAIL() << "case 8's first character is missing from the dump";
}
```

<!-- plan-edit: test/epub_pipeline/CMakeLists.txt -->
```cmake
  EpubPipelineTest.cpp
```
with:

```cmake
  EpubPipelineTest.cpp
  CjkRulesTest.cpp
```

- [ ] **Step 3: Run the checker to see it fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='CjkCorpus*:CjkTable*' --gtest_brief=1`
Expected: 7 failures (`[  PASSED  ] 18 tests.`):
- `CjkCorpus/CjkRules.NoSpaceBetweenUnspacedCjkCharacters/test_cjk_zh_honglou` fails, with and without hyphenation:
  the source's line breaks became spaces;
- so does the same test on the Japanese excerpt and on the fixture (`test_cjk_ja_soseki`, `test_cjk_line_breaking`),
  at the seams of the 200-byte flush (for example `gap between "ン" and "を"`);
- `CjkTable.ALongCjkCellIsChargedPerToken` fails: `case 8 stayed in a table cell`.

Kinsoku and the no-hyphen rule hold already, and the Korean book passes: its words have spaces.

- [ ] **Step 4: The parser**

The parser keeps three facts about the text it is buffering:
- how many CJK codepoints the buffer holds;
- the last codepoint it added;
- whether a whitespace run with a line break in it followed unspaced CJK text.

<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h -->
```cpp
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
```
with:

```cpp
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  // CJK codepoints in partWordBuffer: consumeText flushes a CJK run every kCjkRunCodepoints of them.
  int partWordCjkCodepoints_ = 0;
  // The last codepoint of text added to partWordBuffer, kept across flushes and whitespace.
  uint32_t lastTextCodepoint_ = 0;
  // A whitespace run with a line break in it followed unspaced CJK text (lastTextCodepoint_). If the
  // next character is unspaced CJK too, the run vanishes instead of becoming a space (CSS Text 3
  // segment-break rule): hard-wrapped Chinese and Japanese sources break lines mid-sentence.
  bool cjkSegmentBreakPending_ = false;
```

In `ChapterHtmlSlimParser.cpp`, in order:
- **the table cost comment:** corrected; the constant stays 48 (an A1 review note);
- **`kCjkRunCodepoints` and `codepointAt`:** the run length and a bounded decode, beside `isWhitespace`;
- **`flushPartWordBuffer`:** resets the run count, and charges the row for every token the word became;
- **`startNewTextBlock`:** a new block forgets a pending line break;
- **the ASCII fast path:** after its forced flush, the next chunk continues the word (glued). A Latin character at
  the start of a word keeps a pending line break as a space;
- **the whitespace branch:** a line break after unspaced CJK text is pending, not yet a space;
- **the multi-byte path, at the lead byte of each codepoint:**
  - a pending line break before unspaced CJK text vanishes (the word is glued);
  - a CJK run is flushed, glued, at 32 codepoints;
- **the forced 200-byte flush, on both of its branches:** the next chunk continues the word.

<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
// Attributed cost of one buffered word: the std::string header in ParsedText::words (24 B under
// 32-bit libstdc++) plus the parallel per-word vectors (wordStyles, wordSizes, wordBoundaries) and
// the geometric growth headroom all four carry. Deliberately a fixed constant rather than
// sizeof(std::string) so host goldens and device builds make the SAME switch decision -- a
// host-derived width (32 B) would charge more per word and trip earlier than the device.
constexpr size_t TABLE_BUFFER_BYTES_PER_WORD = 48;
```
with:

```cpp
// Attributed cost of one buffered token: the std::string header in ParsedText::words (24 B under
// 32-bit libstdc++) plus the four parallel per-token vectors (wordStyles, wordBoundaries, wordSizes,
// wordVisibleOffsets: 8 B), 32 B in all, and half again for the growth headroom the vectors carry on
// average (they double when full). Deliberately a fixed constant rather than sizeof(std::string) so
// host goldens and device builds make the SAME switch decision -- a host-derived width (32 B) would
// charge more per token and trip earlier than the device. A CJK word is one token per character,
// and each is charged.
constexpr size_t TABLE_BUFFER_BYTES_PER_WORD = 48;
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
// pointer) plus the heap-allocated ParsedText and its four empty vectors.
```
with:

```cpp
// pointer) plus the heap-allocated ParsedText and its five empty vectors.
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
```
with:

```cpp
bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// A CJK run has no spaces to end its words, so consumeText flushes it every this many codepoints,
// glued to the next chunk. One flush then adds at most this many tokens, and a paragraph's word
// lists stay within their 128-entry steady state (96 tokens before a mid-block layout, plus one
// flush); a 200-byte flush would add up to 66 and double them to 256 entries, a 6 KB block.
constexpr int kCjkRunCodepoints = 32;

// The codepoint whose lead byte is s[0], or 0 when its sequence runs past the `avail` bytes in hand
// (it continues in the next SAX chunk).
uint32_t codepointAt(const char* s, const int avail) {
  const auto lead = static_cast<uint8_t>(s[0]);
  const int length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
  if (length > avail) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  return utf8NextCodepoint(&p);
}
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
bool ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (streamFailed) {
```
with:

```cpp
bool ChapterHtmlSlimParser::flushPartWordBuffer() {
  partWordCjkCodepoints_ = 0;  // the run being counted ends with this flush
  if (streamFailed) {
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
    currentTableCell->text->addWord(partWordBuffer, fontStyle, false, nextWordContinues, effectiveSizePct,
                                    partWordVisibleOffset_);
    // Charge the word against the row budget. Only a row still headed for the grid accumulates —
    // a degraded cell is drained and freed at </td>.
    if (currentTable && !currentTable->degraded && !currentTable->rowDegraded) {
      currentTable->pendingRowBytes += TABLE_BUFFER_BYTES_PER_WORD;
```
with:

```cpp
    const size_t tokensBefore = currentTableCell->text->size();
    currentTableCell->text->addWord(partWordBuffer, fontStyle, false, nextWordContinues, effectiveSizePct,
                                    partWordVisibleOffset_);
    // Charge every token the word became against the row budget (a CJK word is one per character).
    // Only a row still headed for the grid accumulates — a degraded cell is drained and freed at </td>.
    if (currentTable && !currentTable->degraded && !currentTable->rowDegraded) {
      currentTable->pendingRowBytes += TABLE_BUFFER_BYTES_PER_WORD * (currentTableCell->text->size() - tokensBefore);
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
```
with:

```cpp
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  cjkSegmentBreakPending_ = false;
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
      if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
        // Buffer is full — flush before appending. Pure ASCII means no
        // partial multi-byte sequence can be at the boundary.
        if (!self->flushPartWordBuffer()) return;
      }
      if (self->partWordBufferIndex == 0) self->partWordVisibleOffset_ = byteOff;
      self->partWordBuffer[self->partWordBufferIndex++] = s[i];
      continue;
```
with:

```cpp
      if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
        // Buffer is full — flush before appending. Pure ASCII means no
        // partial multi-byte sequence can be at the boundary. The next chunk continues the same
        // word: glued, no space (crosspoint-reader PR #2652, Phạm Bình An).
        if (!self->flushPartWordBuffer()) return;
        self->nextWordContinues = true;
      }
      if (self->partWordBufferIndex == 0) {
        self->partWordVisibleOffset_ = byteOff;
        self->cjkSegmentBreakPending_ = false;  // a Latin character after a line break: it stays a space
      }
      self->lastTextCodepoint_ = c;
      self->partWordBuffer[self->partWordBufferIndex++] = s[i];
      continue;
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
```
with:

```cpp
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // A line break right after unspaced CJK text may vanish: the next character decides.
      if (s[i] == '\n' && utf8IsUnspacedCjk(self->lastTextCodepoint_)) self->cjkSegmentBreakPending_ = true;
      // Skip the whitespace char
      continue;
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
```
with:

```cpp
    // A codepoint starts here: decode it once for the CJK rules.
    if (c >= 0xC0) {
      const uint32_t cp = codepointAt(s + i, len - i);
      if (self->partWordBufferIndex == 0 && self->cjkSegmentBreakPending_) {
        // The line break before this word vanishes when the text on both sides is unspaced CJK:
        // the word is glued to the previous one, and ParsedText turns the seam into a CJK gap.
        if (utf8IsUnspacedCjk(cp)) self->nextWordContinues = true;
        self->cjkSegmentBreakPending_ = false;
      }
      if (cp != 0) self->lastTextCodepoint_ = cp;
      if (utf8IsCjkBreakable(cp)) {
        if (self->partWordCjkCodepoints_ >= kCjkRunCodepoints) {
          if (!self->flushPartWordBuffer()) return;
          self->nextWordContinues = true;  // the run goes on: glued, and ParsedText makes it a CJK gap
        }
        ++self->partWordCjkCodepoints_;
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret. The next chunk continues the same word: glued, no space
    // (crosspoint-reader PR #2652, Phạm Bình An).
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
        self->partWordBufferIndex = safeLen;
        if (!self->flushPartWordBuffer()) return;
        for (int j = 0; j < overflow; j++) {
```
with:

```cpp
        self->partWordBufferIndex = safeLen;
        if (!self->flushPartWordBuffer()) return;
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
```
<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
      } else {
        if (!self->flushPartWordBuffer()) return;
      }
    }

    if (self->partWordBufferIndex == 0) self->partWordVisibleOffset_ = byteOff;
```
with:

```cpp
      } else {
        if (!self->flushPartWordBuffer()) return;
        self->nextWordContinues = true;
      }
    }

    if (self->partWordBufferIndex == 0) self->partWordVisibleOffset_ = byteOff;
```

- [ ] **Step 5: Run the checker to see it pass**

Run: `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='CjkCorpus*:CjkTable*' --gtest_brief=1`
Expected: `[  PASSED  ] 25 tests.`

- [ ] **Step 6: The section cache version**

<!-- plan-edit: lib/Epub/Epub/Section.cpp -->
```cpp
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 86;  // v86: a <span id> is in the anchor map
                                              // v85: an <img> whose href has no extension is
                                              // laid out as the image its bytes are; v84 pages
                                              // hold its alt text
```
with:

```cpp
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 87;  // v87: CJK text breaks between characters, with kinsoku
                                              // v86: a <span id> is in the anchor map
                                              // v85: an <img> whose href has no extension is
                                              // laid out as the image its bytes are; v84 pages
                                              // hold its alt text
```

- [ ] **Step 7: Goldens for the CJK books**

Run: `UPDATE_GOLDENS=1 ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='*MatchesGolden/test_cjk*' --gtest_brief=1`
Expected: the four `MatchesGolden` cases skip (`golden regenerated`).

Run: `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_brief=1 --gtest_filter=-LongChapterPageTables.AChapterFarPastTheReserveBuildsWhole:LongChapterPageTables.ARefusedChunkStopsTheBuildWithBothTablesInStep`
Expected: `[  PASSED  ] 757 tests.` (2 skipped, as on master). Every golden that existed before this plan is
byte-identical: `git status --short test/epub_pipeline/goldens` lists only the four new ones.

- [ ] **Step 8: Heap-peak baselines for the CJK books**

The heap gate checks only the books its baseline lists.
- **The excerpts** peak within the Latin books' range: arena 24.9-25.1 KB against 26.5 KB for
  test_inline_footnotes, heap 47.1-57.2 KB against 42.8-62.9 KB.
- **The fixture** peaks higher (arena 48.3 KB, heap 71.6 KB). Its case 8 cell buffers about 256 tokens before the
  12 KB row budget trips, as a Latin cell of 256 words would.

<!-- plan-edit: test/epub_pipeline/heap_peak_baseline.txt -->
```text
# fixture mode heap-side-peak-bytes (see heap_peak_check.py; regenerate with UPDATE_HEAP_BASELINE=1)
epubs/test_inline_footnotes.epub arena 26465
epubs/test_inline_footnotes.epub heap 42763
epubs/test_jpeg_images.epub arena 28136
```
with:

```text
# fixture mode heap-side-peak-bytes (see heap_peak_check.py; regenerate with UPDATE_HEAP_BASELINE=1)
epubs/test_cjk_ja_soseki.epub arena 25114
epubs/test_cjk_ja_soseki.epub heap 57203
epubs/test_cjk_ko_hyeon.epub arena 24887
epubs/test_cjk_ko_hyeon.epub heap 47120
epubs/test_cjk_line_breaking.epub arena 48270
epubs/test_cjk_line_breaking.epub heap 71551
epubs/test_cjk_zh_honglou.epub arena 24885
epubs/test_cjk_zh_honglou.epub heap 55581
epubs/test_inline_footnotes.epub arena 26465
epubs/test_inline_footnotes.epub heap 42763
epubs/test_jpeg_images.epub arena 28136
```

Run: `cmake --build build/test --target epub_pipeline_dump -j 8 && python test/epub_pipeline/heap_peak_check.py ./build/test/epub_pipeline/epub_pipeline_dump.exe test test/epub_pipeline/heap_peak_baseline.txt`
Expected: every line `ok` (within 8 KB of its baseline).

- [ ] **Step 9: The whole suite and the firmware**

Run: `cmake --build build/test -j 8 -- -k; (cd build/test && ctest -j 8 2>&1 | tail -3)`
Expected: `99% tests passed, 2 tests failed out of 2591` (2,488 on master). The two are the LongChapterPageTables
tests that fail on Windows on master too (see Global Constraints); nothing else fails.

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default`, then the same with `-e x4pro`.
Expected: `[SUCCESS]` for each, with no new warning from the files this plan touches.

- [ ] **Step 10: Format and commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h \
  lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp lib/Epub/Epub/Section.cpp test/epub_pipeline/CjkRulesTest.cpp
git add lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp \
  lib/Epub/Epub/Section.cpp test/epubs/make_test_cjk_line_breaking.py test/epubs/make_test_cjk_excerpts.py \
  test/epubs/test_cjk_*.epub test/epub_pipeline/CjkRulesTest.cpp test/epub_pipeline/CMakeLists.txt \
  test/epub_pipeline/goldens/test_cjk_*.golden.txt test/epub_pipeline/heap_peak_baseline.txt
git commit -m "feat(epub): the parser keeps CJK runs whole; CJK books and a rule checker" \
  -m "After a forced 200-byte flush the next chunk continues the word, glued (crosspoint-reader PR #2652, Phạm Bình An, 3319aa17). A CJK run is flushed every 32 codepoints, glued, so the word lists keep their 128-entry steady state. A table row is charged for every token. A source line break between two characters of unspaced CJK text vanishes (CSS Text 3 segment-break rule). Four CJK books join the corpus: a fixture with one case per rule and public-domain excerpts in Japanese (Natsume Soseki), Chinese (Cao Xueqin) and Korean (Hyeon Jin-geon); a rule checker asserts kinsoku, no hyphen and no stray space on every line. SECTION_FILE_VERSION 87."
```

### Task 6: Validate on the X3, and open the PR

The host checker proves the rules on synthetic widths. The device shows them with a real CJK font, at the panel's
resolution and in the C3's heap. The maintainer does not read CJK (spec 9.2), so the executor drives the X3 over
serial and reviews the screenshots. The maintainer only copies the fonts and picks one in the reader settings.

**Files:** none in the repository. Scratch files go in the session's scratchpad.

**Interfaces:**
- Consumes: Task 5's firmware and books; the serial test commands (`CMD:ACT`, `CMD:BTN`, `CMD:STATE`,
  `CMD:SCREENSHOT`) and the serial book upload (`EPUB` magic, files land in `/books`).
- Produces: a PR comment with the device results; the PR.

- [ ] **Step 1: Build the CJK fonts locally**

They are not shipped (`sd-fonts.yaml` keeps them commented out). Copy `lib/EpdFont/scripts/sd-fonts.yaml` to the
scratchpad, keep only the `NotoSansCJK` and `NotoSansHangul` entries (uncommented, `sizes: [12, 14]`), and build:

```bash
python lib/EpdFont/scripts/build-sd-fonts.py --config <scratch>/cjk-fonts.yaml --output-dir <scratch>/fonts \
  --only NotoSansCJK,NotoSansHangul --timeout 1800
```

Expected: one folder per family with a `.cpfont` per size.

- [ ] **Step 2: Fonts on the card, firmware on the X3, books over serial** (the maintainer copies the fonts)

- The maintainer copies both font folders to `/fonts/` on the X3's card.
- Flash the branch: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6`
  (stop any serial logger first).
- Upload the four CJK books with the scratchpad's `tools/upload.py` (one per call; a busy device can take up to a
  minute to answer `READY`). They land in `/books`.
- The maintainer opens a CJK book once and picks NotoSansCJK as the reader font.

- [ ] **Step 3: Read the books on the device**

For each book, with the device log captured (`DEVCMD_LOG=<file> tools/devcmd.py COM6 ...`):
1. `ACT:READER:/books/<book>.epub`, then `until:<book>:60`;
2. a screenshot of the first page (`shot:<file>`);
3. five `BTN:RIGHT` page turns, three seconds apart;
4. another screenshot;
5. `STATE`.

Review every screenshot:
- no line starts with 。、」）！？ or ends with 「（『;
- no hyphen at a line end in CJK text;
- no gap inside a run of Han or kana characters;
- Korean breaks at its spaces;
- justified lines reach the right margin. The remainder is not spread yet (A3), so a few pixels short is expected.

From the log, record:
- the section build (`createSectionFile ... done` time and pages, the arena `highWater` and `failedAlloc`);
- the lowest `Watermark DROP`;
- a page turn's time (`PageTurn stats`).

Compare them with a Latin book opened the same way (spec 8).

- [ ] **Step 4: Open the PR**

```bash
git push -u origin feat/cjk-line-breaking
gh pr create --base master --title "feat: (Epub) CJK line breaking with kinsoku" --body-file <file>
gh pr comment <number> --body-file <device results>
```

The body says:
- the second of three PRs for CJK line breaking (spec `docs/superpowers/specs/2026-10-16-cjk-layout-design.md`);
- what changes for a reader of Chinese, Japanese and Korean books;
- the rule checker and the four books, with the excerpts' sources;
- the section cache version 87: every book re-lays once;
- what is upstream's (PRs #2288, #2652, #2768, #3700, #3883's dashes) and what is ours (the crengine additions,
  the segment-break rule, the run flush, the per-token table charge, the checker);
- the host heap figures (Task 5 Step 8) and the decisions on the 32-codepoint flush.

The comment gives Step 3's screenshots review and figures.

