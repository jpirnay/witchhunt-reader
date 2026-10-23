# CJK typography A3b: push-in and the CJK–Latin gap — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CJK line that is one token too long first shrinks its punctuation, then its CJK–Latin gaps, before it breaks earlier (push-in, in all four line breakers), and Chinese and Japanese books set a quarter em between Han or kana and a Latin letter or digit.

**Architecture:** `CjkPunctuation` gains the CJK–Latin gap predicate and the Japanese minimums JLREQ allows. `ParsedText` computes the gap on demand at breakable CJK gaps (`cjkLatinGapBefore`, called from `naturalGapBefore`, so every breaker and `extractLine` measure it) and keeps two bytes of layout scratch per token: what a mark can give up mid-line (`shrink_`) and at its line edge (`edgeShrink_`). All four breakers test a candidate line at its pushed-in width (`pushedInWidth`); the DP prices a pushed-in line just above an exact one; `extractLine` takes the need from the line-edge marks (whole), then the other marks, then the gaps (JLREQ 3.8.3), and the existing ink-placement and justification code places the result. The rule checker restates the whole line layout, push-in included, and checks every left-aligned and centred line token by token against a text width derived from the viewport.

**Tech Stack:** C++17 firmware (PlatformIO, ESP32-C3/S3), host tests with GoogleTest/CMake (`test/`), Python fixture generators (`test/epubs/`), the serial test API (`tools/devctl.py`, `docs/serial-test-api.md`) on the device.

**Spec:** `docs/superpowers/specs/2026-10-19-cjk-a3-typography-design.md`, sections 5.3 and 5.4 (5.1, 5.2, 5.6, 6 and 7 for context). A3a (5.1, 5.2, 5.5) shipped as #520; its plan, `docs/superpowers/plans/2026-10-19-cjk-a3a-punctuation-and-grid.md`, is the precedent. Unlike A3a's, this plan was not cut from a prototype: every expected value below is derived by hand from the host shims' metrics and the code at origin/master `5ae036f79`, and each test's comment carries its derivation. A value that does not reproduce is a finding to report, not a number to adjust silently.

## Maintainer decisions (2026-10-22)

1. A3b's budget is +5 % on a CJK chapter's section build on the X4 (log `total=`), measured against master. A3 as a whole stays near the spec's 10 % over A2 (A3a took about 4 %).
2. Still open, asked at the device run (Steps 4-6):
   - which font families on the X4's card cover kana (Claude checks with `CMD:FS:LS`);
   - the X3 session: the maintainer flashes it, and Claude uploads files without a reset.

## Global Constraints

From the spec, verbatim:
- 5.3: "When the next token does not fit, or kinsoku glues it to a character that does not fit, the line first tries to *shrink*: mid-line punctuation down to its minimum width, then the CJK–Latin gaps (5.4). Only if that is not enough does the line break earlier and get *stretched* (push-out)."
- 5.3: "In every breaker: the DP breaker (`computeLineBreaks`) and the three greedy ones (`computeSingleLineBreakNoHyphen`, `computeHyphenatedLineBreaks`, `computeHyphenatedLineBreaksFromIndex`)."
- 5.3: "a line fits when its width with every shrinkable part at its minimum is within the available width. In the DP, a shrunk line costs more than an exact one and less than a badly stretched one, so it is chosen when it avoids a loose line, not by default. (The exact cost is the builder's; the requirement is that pure-CJK paragraphs with no punctuation lay out exactly as before.)"
- 5.3: "`extractLine` applies the shrink it needs, in that order, and spreads any remainder as in 5.1."
- 5.4: "at a boundary between a Han or kana character and a Latin letter or digit, in either order, when the source has no space there. Not next to punctuation, not next to an existing space, not for Hangul". "a quarter em of the CJK character's size. It stretches with justification and shrinks under push-in (5.3), within JLREQ's limits." "Chinese and Japanese profiles." "Not a break opportunity on its own: A2's break rules decide where a line may end; the gap only adds width."
- 5.2, Japanese: "。 stays full-width mid-line and may become half-width at a line end; a comma or closing bracket at a line end is half or full, nothing in between; ：； may halve; ？！ stay full-width; a quarter em is kept before an opening bracket and after a closing bracket."
- 6: "No new settings." "Heap: no new per-token field beyond one byte ... A2's host measurements (25 KB arena peak, 47-57 KB heap on the CJK excerpts) must not grow by more than 5 %." "a section build of the CJK excerpts on the X3 may take at most 10 % longer than with A2, and page time must not change beyond noise". "Section file version bumps (layout changes)." "Latin books lay out exactly as before: the Latin goldens in `test/epub_pipeline` must not change. The CJK goldens (`test_cjk_*.golden.txt`) change, and each change must be explained by a rule in section 5." "KOSync positions are unchanged: this is layout only."

Decided for this plan (reasons in the tasks and in the report):
- JLREQ 3.8.3 limits: the CJK–Latin gap shrinks to 1/8 em, taken as half the gap rounded down; the gap is `(em × 2 + 4) / 8` px, as every eighth-of-an-em width in `CjkPunctuation` rounds.
- Push-in order: (1) the line-edge marks, each all or nothing (5.2's "half or full, nothing in between"); (2) every other mark; (3) the CJK–Latin gaps. Western word spaces do not shrink: JLREQ lists them first, but Latin lines must not move.
- Push-in follows the punctuation table, so it applies in every book; the gap only in Chinese and Japanese books (`TypographyProfile::cjkLatinGap`).
- Section file version 97 (from 96), bumped once, in Task 2.
- Heap: layout scratch only, two bytes a token (`shrink_`, `edgeShrink_`), members reused across paragraphs and released with the rest of the scratch; the gap is computed when a line is measured and stored nowhere; nothing new in the section file. Read as A3a read it: the spec's "one byte" limits stored per-token fields.
- Budget: A3a took about 4 % of the spec's 10 % over A2 on the X4 (#520). A3b may add at most 5 % to a cold section build of the CJK excerpts (log `total=`), measured on the X4 (ESP32-C3) against master; host heap-side peak within 1 KB of master on every CJK book.
- Comments say what the code does and why; no history narration, no "PR A3b".
- clang-format 21 is what CI runs (`bin/clang-format-fix`); version 22 pads differently. Run the version-21 check before pushing.
- Host tests build and run from Git Bash, never PowerShell. Configure once:
  `cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1 -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=C:/_development/witchhunt-reader/build/test/_deps/googletest-src -DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=C:/tmp/wt-serial-api/build/test/_deps/arduinojson-src`;
  build a target with `cmake --build build/test --target <T> -j 8`. On Windows a few tests fail on master too (A3a saw `BookFontsTest` ×2 and `LongChapterPageTables` ×2): compare against a master run, not this list.
- Firmware: `export PLATFORMIO_CORE_DIR='C:\pio'`, then `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default` and `-e gh_release`, one `pio run` at a time, in the background (a cold build passes the 600 s tool timeout). `x4pro` and `lilygo_t5s3` are left to CI.
- Code with backslash escapes (`"\xE4\xB8\x80"` for 一) goes in with the Write or Edit tool, never a heredoc or a non-raw Python string.
- No upstream code is ported (upstream has neither feature); KOReader's crengine is GPL and is not a source.

## Review Focus

1. **A pushed-in line that is justified, centred or set right** (justified is the default; CJK headings are centred): it must sit exactly in its measure, with no stretch and no overhang, so alignment must use the shrunk width. Pinned in Task 4: `CjkPushIn.APushedInLineFillsItsLineInEveryAlignment`.
2. **A line break at a CJK–Latin boundary** (Latin words are break points in CJK text): the gap is drawn on neither line and counts toward neither line's width. Pinned in Task 2: `CjkLatinGap.ALineBreakAtTheGapDropsIt`.
3. **A CJK character at another size than the Latin beside it** (an inline font-size span, a word in another of the book's families): the gap is a quarter em of the CJK side, not of the Latin side or the block. Pinned in Task 2: `CjkLatinGap.TheGapIsAQuarterEmOfTheCjkSide`.
4. **A CJK book read in a Latin reader font** (CJK drawn as replacement boxes): no gap, as there is no grid and no narrowed mark. Pinned in Task 2: `CjkLatinGap.AFontWithoutCjkGlyphsGetsNoGap`.
5. **A word hyphenated earlier in the same paragraph** (a Korean syllable split, a Latin word with hyphenation on): `hyphenateWordAtIndex` inserts a token, and the shrink scratch must move with it or a later line pushes in by the wrong mark. Pinned in Task 4: `CjkPushIn.AWordHyphenatedEarlierKeepsTheShrinkInStep`.

---

### Task 1: Japanese minimums and the CJK–Latin gap predicate

Push-in is the first reader of `CjkPunctuation::Width::minimum`. Two Japanese entries let marks shrink that spec 5.2 and JLREQ 3.8.3 keep full mid-line (。． and ！？); they are corrected before anything reads them. The predicate and the gap's width in eighths live here, beside the other eighths-of-an-em tables, so `ParsedText` and the rule checker share one definition.

**Files:**
- Modify: `lib/Epub/Epub/CjkPunctuation.h` (the `minimum` comment at :33; the gap declarations after `widthOf`)
- Modify: `lib/Epub/Epub/CjkPunctuation.cpp` (`japanese()`: `Dividing` at :148-149, `FullStop` at :151-163; its comment at :114-118; the predicates)
- Test: `test/typography/TypographyTest.cpp`

**Interfaces:**
- Produces, in namespace `CjkPunctuation`:
  - `constexpr uint8_t kCjkLatinGap = 2;` — the gap in eighths of the CJK side's em;
  - `bool isHanOrKana(uint32_t cp);` — U+3005–3007, U+3041–3096, U+309D–309F, U+30A1–30FA, U+30FC–30FF, U+31F0–31FF, U+3400–4DBF, U+4E00–9FFF, U+F900–FAFF, U+20000–2A6DF, U+2A700–2B73F (the ideograph blocks `utf8IsCjkBreakable` covers, 々〆〇, and the kana letters with ー; not ・, not half-width katakana);
  - `bool isLatinLetterOrDigit(uint32_t cp);` — ASCII letters and digits, U+00C0–024F except U+00D7 and U+00F7, U+1E00–1EFF;
  - `bool takesCjkLatinGap(uint32_t leftCp, uint32_t rightCp);` — one side `isHanOrKana`, the other `isLatinLetterOrDigit`.
- Changes: `widthOf(Japanese, FullStop, n, false)` is `{8, 8}` for every `n` but `Closing`, `MiddleDot`, `FullStop`, `Comma` (still `{4, 4}`); `widthOf(Japanese, Dividing, *, *)` is `{8, 8}`.

- [ ] **Step 1: Write the failing tests.** In `JapaneseKeepsTheFullStopAndLetsLineEndsChoose` replace the two `8, 6` expectations and add two:
```cpp
  expectWidth(ja, Class::FullStop, Class::None, false, 8, 8);              // 。 keeps its blank mid-line, pushed in too
  expectWidth(ja, Class::FullStop, Class::IdeographicSpace, false, 8, 8);  // 。　 as well
  expectWidth(ja, Class::Dividing, Class::None, false, 8, 8);              // ！？ keep their width (JLREQ 3.8.3)
  expectWidth(ja, Class::Dividing, Class::None, true, 8, 8);
```
and add:
```cpp
TEST(CjkPunctuation, NoMarkIsNarrowerMidLineThanAtALineEdge) {
  // The DP breaker stops widening a line at the first token whose pushed-in width overflows. That holds only if a
  // line's pushed-in width never drops when a token joins it: the token that was last goes from its line-edge minimum
  // to its mid-line one, which must be no smaller.
  for (const Table table : {Table::SimplifiedChinese, Table::TraditionalChinese, Table::Japanese}) {
    for (int m = 0; m <= static_cast<int>(Class::AmbiguousQuote); ++m) {
      const auto mark = static_cast<Class>(m);
      const CjkPunctuation::Width edge = CjkPunctuation::widthOf(table, mark, Class::None, true);
      EXPECT_LE(edge.minimum, edge.forced);
      for (int n = 0; n <= static_cast<int>(Class::AmbiguousQuote); ++n) {
        const CjkPunctuation::Width mid = CjkPunctuation::widthOf(table, mark, static_cast<Class>(n), false);
        EXPECT_GE(mid.minimum, edge.minimum) << static_cast<int>(table) << " " << m << " " << n;
        EXPECT_LE(mid.minimum, mid.forced);
      }
    }
  }
}

TEST(CjkPunctuation, TheCjkLatinGapSitsBetweenHanOrKanaAndLatinLettersOrDigits) {
  using CjkPunctuation::takesCjkLatinGap;
  EXPECT_TRUE(takesCjkLatinGap(0x6F22, 'A'));      // 漢A
  EXPECT_TRUE(takesCjkLatinGap('1', 0x5E74));      // 1年
  EXPECT_TRUE(takesCjkLatinGap(0x3044, 'i'));      // いi
  EXPECT_TRUE(takesCjkLatinGap(0x30FC, 'x'));      // ーx
  EXPECT_TRUE(takesCjkLatinGap(0x3005, 'e'));      // 々e
  EXPECT_TRUE(takesCjkLatinGap(0x00E9, 0x5B57));   // é字
  EXPECT_TRUE(takesCjkLatinGap(0x20B9F, 'Z'));     // 𠮟Z, Extension B
  EXPECT_FALSE(takesCjkLatinGap(0x3002, 'A'));     // 。A: punctuation
  EXPECT_FALSE(takesCjkLatinGap(0x30FB, 'A'));     // ・A
  EXPECT_FALSE(takesCjkLatinGap(0xAC00, 'A'));     // 가A: Hangul (KLREQ asks for nothing)
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0xFF21));  // 漢Ａ: a full-width letter sits on the grid
  EXPECT_FALSE(takesCjkLatinGap(0xFF71, 'A'));     // ｱA: half-width katakana is set like Latin
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0x03B1));  // 漢α
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, '%'));
  EXPECT_FALSE(takesCjkLatinGap(0x00D7, 0x6F22));  // ×漢
  EXPECT_FALSE(takesCjkLatinGap('A', 'B'));
  EXPECT_FALSE(takesCjkLatinGap(0x6F22, 0x5B57));
}
```
- [ ] **Step 2: Run to see them fail.** `cmake --build build/test --target TypographyTest -j 8` — Expected: compile error, `takesCjkLatinGap` is not a member of `CjkPunctuation`.
- [ ] **Step 3: Implement.** The declarations of this task's Interfaces block, each with a one-line comment; in `japanese()` the `Dividing` case returns `{8, 8}` and the `FullStop` switch keeps its `{4, 4}` cases and returns `{8, 8}` otherwise (the `IdeographicSpace` case goes). The comment above `japanese()` says: 。 and ！？ keep their width mid-line; at a line end 。、 and a closing bracket are half or full, push-in deciding (spec 5.3). The `Width` comment at :33 ends "(push-in, spec 5.3)".
- [ ] **Step 4: Run.** `build/test/typography/TypographyTest.exe` — Expected: 11 tests PASS. Then `cmake --build build/test --target CjkLayoutTest EpubPipelineTest -j 8` and `build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='SyntheticCorpus/*MatchesGolden*:CjkCorpus/*'` — Expected: all PASS, `git status test/epub_pipeline/goldens` clean (nothing reads `minimum` yet).
- [ ] **Step 5: Commit.** `git commit -m "feat(epub): Japanese full stops and ！？ keep their width under push-in; the CJK-Latin gap predicate"`

---

### Task 2: The CJK–Latin gap

The gap sits at a breakable zero-width CJK gap (`continues=false, noSpaceBefore=true`), which is what A2 makes of every Han/kana–Latin boundary without a source space (`CjkLineBreak::isGapBetween` allows the break; letters and digits are in neither kinsoku set). A glued Han/kana–Latin pair only arises next to an iteration mark such as 々 and gets no gap (my call: rare, and gluing means the parser saw no gap opportunity). `naturalGapBefore` returns the gap, so all four breakers, the DP's float pre-pass and the span-indent test measure it with no change of their own; it is not a new break opportunity, since the boundary already was one.

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h` (declare `cjkLatinGapBefore` after `naturalGapBefore`, :152)
- Modify: `lib/Epub/Epub/ParsedText.cpp` (`naturalGapBefore` :387-392; `extractLine`'s sums :1463-1490 and gap path :1571-1588; the new method)
- Modify: `lib/Epub/Epub/TypographyProfile.h` (:29, drop "(PR A3b)")
- Modify: `lib/Epub/Epub/Section.cpp` (:36, version 97)
- Modify: `test/cjk_layout/CjkTypographyTest.cpp`; `test/epub_pipeline/CjkRulesTest.cpp`
- Modify: `test/epubs/make_test_cjk_line_breaking.py` (case 12), regenerate `test/epubs/test_cjk_line_breaking.epub`; goldens

**Interfaces:**
- Consumes: `CjkPunctuation::takesCjkLatinGap`, `isHanOrKana`, `kCjkLatinGap` (Task 1); `cjkEm` (ParsedText.cpp anonymous namespace).
- Produces: `int ParsedText::cjkLatinGapBefore(const GfxRenderer& renderer, int fontId, size_t j) const` — the gap before `words[j]` (j > 0) in pixels: `(em * CjkPunctuation::kCjkLatinGap + 4) / 8`, `em = cjkEm(renderer, wordFontId(k, fontId), wordStyles[k] & BOLD_ITALIC, wordScale(k))` with `k` the side whose edge codepoint is Han or kana; 0 unless `typography_.cjkLatinGap`, the boundary is a breakable CJK gap, `takesCjkLatinGap(lastCodepoint(words[j - 1]), firstCodepoint(words[j]))` and `em > 0`. Already scaled: callers must not multiply it by `wordScale`.
- Produces (tests): helpers `TypographyProfile withoutGrid(const char* language)` (the language's profile with `wholeEmGrid = false`, so a test line is as wide as it asks) and `std::vector<Line> layoutText(const char* text, const TypographyProfile& profile, CssTextAlign alignment, uint16_t width, bool greedy = false, int fontId = kFontId)` (one `addWord`; `greedy` is `hyphenationEnabled`, which selects the greedy breaker) in `CjkTypographyTest.cpp`'s anonymous namespace.
- Produces (checker): `Case` gains `bool mixesLatin` and `bool forcesPushIn` (Task 4); `CjkRules` gains `TypographyProfile profile` (set in `SetUp` from `c.language`, beside `table`); `DumpLine` gains `int align` (the `LINE` record's `align=`; -1 for a cell's `LN`).

- [ ] **Step 1: Write the failing tests** in `CjkTypographyTest.cpp` (the shim: every codepoint 10 px, a space 5 px; 一 measures 10, so the gap is `(2 × 10 + 4) / 8 = 3` px):
```cpp
TEST(CjkLatinGap, AQuarterEmBetweenHanOrKanaAndLatinLettersOrDigits) {
  // 10 px ems: (2 × 10 + 4) / 8 = 3 px on each side of A and 1.
  EXPECT_EQ(layoutText("字A字1か", withoutGrid("ja"), CssTextAlign::Left, 400)[0].xpos, (X{0, 13, 26, 39, 52}));
  EXPECT_EQ(layoutText("字A", withoutGrid("zh"), CssTextAlign::Left, 400)[0].xpos, (X{0, 13}));
}
TEST(CjkLatinGap, NoGapAtASpaceAMarkAFullWidthLetterOrInOtherBooks) {
  const auto xpos = [](const TypographyProfile& profile, const std::vector<const char*>& words) {
    ParsedText text(false, false, aligned(CssTextAlign::Left));
    text.setTypography(profile);
    for (const char* word : words) text.addWord(word, EpdFontFamily::REGULAR);
    return layout(text, 400)[0].xpos;
  };
  EXPECT_EQ(xpos(withoutGrid("ja"), {"字", "A字"}), (X{0, 15, 28}));           // the source's space; the gap after A
  EXPECT_EQ(xpos(withoutGrid("ja"), {"字「A」字"}), (X{0, 10, 20, 30, 40}));   // next to brackets
  EXPECT_EQ(xpos(withoutGrid("ja"), {"字ＡＢ字"}), (X{0, 10, 20, 30}));         // full-width letters
  EXPECT_EQ(xpos(withoutGrid("ja"), {"ｱA"}), (X{0, 10}));                     // half-width katakana
  EXPECT_EQ(xpos(TypographyProfile::forLanguage("ko"), {"漢A"}), (X{0, 10}));  // Korean books (KLREQ)
  EXPECT_EQ(xpos(TypographyProfile{}, {"字A字"}), (X{0, 10, 20}));              // other books (spec 5.6)
}
TEST(CjkLatinGap, ALineBreakAtTheGapDropsIt) {
  // 字字字A is 43 px with its gap and fills 43 px; the last 字 would need 13 px more and starts the next line at 0.
  // The other way round, A starts the next line at 0.
  for (const bool greedy : {false, true}) {
    const auto after = layoutText("字字字A字", withoutGrid("ja"), CssTextAlign::Left, 43, greedy);
    ASSERT_EQ(wordsOf(after), (Words{{"字", "字", "字", "A"}, {"字"}})) << (greedy ? "greedy" : "DP");
    EXPECT_EQ(after[0].xpos, (X{0, 10, 20, 33}));
    EXPECT_EQ(after[1].xpos, (X{0}));
    const auto before = layoutText("字字字字A", withoutGrid("ja"), CssTextAlign::Left, 40, greedy);
    ASSERT_EQ(wordsOf(before), (Words{{"字", "字", "字", "字"}, {"A"}})) << (greedy ? "greedy" : "DP");
    EXPECT_EQ(before[1].xpos, (X{0}));
  }
}
TEST(CjkLatinGap, TheGapIsAQuarterEmOfTheCjkSide) {
  // 字 at 160 % is 16 px: the gaps on both of its sides are (2 × 16 + 4) / 8 = 4 px, though the letters are at 100 %.
  ParsedText text(false, false, aligned(CssTextAlign::Left));
  text.setTypography(withoutGrid("ja"));
  text.addWord("A", EpdFontFamily::REGULAR);
  text.addWord("字", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true, 160);
  text.addWord("A", EpdFontFamily::REGULAR, false, /*attachToPrevious=*/true);
  EXPECT_EQ(layout(text, 400)[0].xpos, (X{0, 14, 34}));
}
TEST(CjkLatinGap, AFontWithoutCjkGlyphsGetsNoGap) {
  // A Latin reader font has no 一 of its own: no em, so no gap, as no grid and no narrowed mark (A3a).
  EXPECT_EQ(layoutText("字A字", TypographyProfile::forLanguage("ja"), CssTextAlign::Left, 400, false,
                       GfxRenderer::LATIN_FONT)[0].xpos,
            (X{0, 10, 20}));
}
TEST(CjkLatinGap, TheGapStretchesWithJustification) {
  // 字A字字 is 46 px; the 4 px left on 50 go 1, 1 and 2 to its three gaps (the two CJK-Latin gaps among them), flush.
  const auto lines = layoutText("字A字字字字", withoutGrid("ja"), CssTextAlign::Justify, 50);
  ASSERT_EQ(wordsOf(lines), (Words{{"字", "A", "字", "字"}, {"字", "字"}}));
  EXPECT_EQ(lines[0].xpos, (X{0, 14, 28, 40}));
}
```
  In `CjkRulesTest.cpp` add the rule (its comment as below) and the `Case` fields; `cjkBooks()` becomes a list of `{book, language, mixesLatin, forcesPushIn}` with `mixesLatin` true for `test_cjk_line_breaking` and `test_cjk_zh_punctuation` (both put Latin and digits against Han), `forcesPushIn` false everywhere for now:
```cpp
// Between Han or kana and a Latin letter or European digit with no space in the source, a Chinese or Japanese book
// sets a quarter em of the CJK character's size, at least half of it on a line that was pushed in; other books set
// nothing (spec 5.4). A gap of the space advance or more is the source's own space.
TEST_P(CjkRules, CjkLatinGapsSitWhereTheSpecPutsThem)
```
  It walks every `LINE` record with `align=1` or `align=2` (no justification stretch) and every adjacent pair whose edge codepoints pass `takesCjkLatinGap`. Neither token is a mark, so the gap is exact: `x[k+1] - x[k] - wordWidth(k)`. A gap of at least `renderer.getSpaceWidth(1)` (6) is skipped as a source space; the CJK–Latin gap is at most 5 px at every size the books use. Otherwise, with `q = (em * 2 + 4) / 8` and `em` the width of 一 at the CJK token's `z` and the line's `mult`: with `profile.cjkLatinGap`, `EXPECT_GE(gap, q / 2)` and `EXPECT_LE(gap, q)`; without it, `EXPECT_EQ(gap, 0)`. It counts what it checked, and `if (GetParam().mixesLatin) EXPECT_GT(checked, 0u)`.
- [ ] **Step 2: Run to see them fail.** `cmake --build build/test --target CjkLayoutTest -j 8 && build/test/cjk_layout/CjkLayoutTest.exe --gtest_filter='CjkLatinGap.*'` — Expected: `AQuarterEm...`, `ALineBreak...` (line 0 is `{0,10,20,30}`), `TheGapIsAQuarterEm...` and `TheGapStretches...` FAIL; `NoGap...` and `AFontWithoutCjkGlyphs...` PASS (nothing sets a gap yet).
- [ ] **Step 3: Implement.**
  - `cjkLatinGapBefore` as in the Interfaces block. `naturalGapBefore`'s `noSpaceBefore` branch returns `cjkLatinGapBefore(renderer, fontId, j)` instead of 0; its header comment says so ("nothing at a CJK gap but the CJK-Latin gap").
  - `extractLine`: in the sums loop, a `!continues` boundary without a space adds `cjkLatinGapBefore(...)` to `totalNaturalGaps` (unscaled, see Interfaces); in the x-position loop, the gap path sets `gap` to it when `hasSpace` is false, before the justification share is added. The boundary already counts in `actualGapCount` and sets `cjkGapOnLine`, so the gap takes its share of the stretch and the remainder (spec 5.1) with no further change.
  - `TypographyProfile.h:29`: "A quarter em between Han or kana and a Latin letter or digit with no space between them (spec 5.4)."
  - `Section.cpp`: `SECTION_FILE_VERSION = 97`, the comment above the v96 entry: "v97: a quarter em between Han or kana and Latin in zh/ja books; lines push CJK punctuation in before breaking earlier".
- [ ] **Step 4: The fixture's case 12.** In `make_test_cjk_line_breaking.py` add to the docstring's list "12. cjk-latin  a quarter em between Han or kana and Latin letters or digits, and none next to brackets, full-width letters, half-width katakana or a source space (spec 5.4)", and:
```python
# Case 12: the CJK-Latin gap where it belongs (kana and Latin, digits and Han, a hyphenated Latin word), and the
# boundaries that take none: brackets, full-width letters, half-width katakana, the source's own spaces.
CJK_LATIN = (
    "<p>新しいiPhoneは2026年10月に出た。USB-C端子が付き、価格は約15万円だ。</p>\n"
    "<p>「Python」と書いた本の第3章とChapter 4、ＡＢＣ全角、abcｱｲｳ半角。漢字 Latin 漢字は空白のまま。</p>"
)
```
  with `("12. cjk-latin", CJK_LATIN)` appended to `CASES`. Run `python test/epubs/make_test_cjk_line_breaking.py` and commit the `.epub`.
- [ ] **Step 5: Run.** `build/test/cjk_layout/CjkLayoutTest.exe` — Expected: all PASS (A3a's and A2's tests unchanged: none puts Latin against Han in a zh/ja profile). Goldens: `UPDATE_GOLDENS=1 build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='SyntheticCorpus/*MatchesGolden*'`, then `git status test/epub_pipeline/goldens` — Expected: only `test_cjk_line_breaking` (case 12, and the gaps in cases 5 and 11: 11 alternates Han and digits, 600 gaps) and `test_cjk_zh_punctuation` (12元, Python, 2020年) changed; read the diff and check that every moved word follows a Han/kana–Latin boundary or a line those moved. Then `build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='SyntheticCorpus/*:CjkCorpus/*:Cjk*'` — Expected: all PASS, `CjkCorpus/CjkRules.CjkLatinGapsSitWhereTheSpecPutsThem/*` included.
- [ ] **Step 6: Commit.** `git commit -m "feat(epub): a quarter em between Han or kana and Latin in Chinese and Japanese books"`

---

### Task 3: The rule checker restates the whole line

A3a's `LineWidths` restates the punctuation widths and checks unspaced CJK pairs one by one. Push-in changes widths across a whole line at once (how much each mark gives depends on the line's need and its other marks), so the checker now lays each line out again from its dump: boxes, gaps and push-in, and the text width the line had to fit. This task changes no layout code: the new rules must pass on Task 2's layout, where nothing is pushed in yet, and they are made to bite by two deliberate breakages before the commit.

**Files:**
- Modify: `test/epub_pipeline/CjkRulesTest.cpp`

**Interfaces:**
- Consumes: `CjkPunctuation::widthOf`, `classOf`, `isCurlyQuote`, `takesCjkLatinGap`; `TypographyProfile`; `pipeline_harness::Profile{}.viewportWidth` (460).
- Produces (all in the file's anonymous namespace; `LineWidths` is removed, its users move to `LineLayout`):
  - `DumpLine` gains `int x` (the `LINE` record's `x=`; Task 2 added `align`).
  - `class LineLayout { public: LineLayout(const GfxRenderer& renderer, const DumpLine& line, const TypographyProfile& profile); int shrinkable() const; std::vector<int> xAt(int need) const; int startAt(int need) const; int rightAt(int need) const; int appliedAt(int need) const; int endsAt(bool lineEndPushedIn) const; };`
  - `struct TextBounds { int left; int right; }; TextBounds textBoundsOf(const GfxRenderer& renderer, const DumpLine& line, const TypographyProfile& profile);`
  - `int pushInOf(const LineLayout& layout, const DumpLine& line, const TextBounds& bounds);` — the push-in that reproduces the dumped line, or -1.

What `LineLayout` restates (ParsedText's rules after Task 4; with no push-in yet, every line resolves at need 0):
- **Boxes.** A token is a mark when it is one full-width codepoint whose class the table adjusts (A3a's `markClass`; not `IdeographicSpace` or `AmbiguousQuote`). Its place is the line start (an opening mark at k = 0), the line end (any other mark at k = last), or mid-line with A3a's neighbour rule. Its width there is `(natural * w.forced + 4) / 8` and its minimum `(natural * w.minimum + 4) / 8`, `w = widthOf(table, mark, neighbour, atEdge)`. Other tokens are their natural width and give nothing.
- **Gaps**, before token k: (a) both edge codepoints `utf8IsUnspacedCjk`: 0; (b) `takesCjkLatinGap` in a book whose profile has the gap: `q = (em * 2 + 4) / 8` (em: 一 at the CJK side's size), minimum `q / 2`, unless the dumped distance shows a source space (≥ the space advance), which is kept as it is; (c) anything else (word spaces, glued Latin, a mark beside a Latin letter): the dumped distance from the restated right edge of token k-1 at the need being tried, which must be 0 or at least the space advance (no gap next to a mark: spec 5.4). A (c) boundary re-anchors the next token at its dumped x.
- **Push-in at need n** (spec 5.3, JLREQ 3.8.3 order): tier 1, the line-edge marks, each gives its width minus its minimum, all of it as soon as n > 0; what n leaves after tier 1 goes to tier 2 (every other mark) and then tier 3 (the CJK–Latin gaps), each tier taking at most its total. Inside a tier the items share in line order by cumulative rounding: with `t` the tier's share, `T` its total and `C` the running sum of the items' amounts, an item gives `((C_before + amount) * t) / T - (C_before * t) / T`.
- **Positions.** Token 0's draw x is the dump's; each next box starts at the previous box's right edge plus the gap; a token's draw x is its box's left edge minus its ink shift (the shim's ink, as A3a's `left()`: an opening mark shifts by what it lost, a closing mark, 。 and 、 by nothing, other marks by half). `startAt` is box 0's left edge, `rightAt` the last box's right edge, `appliedAt` what the tiers took. `endsAt(b)` is the dumped x of the last token plus its ink shift plus its box at its line-end width (`b` false) or its line-end minimum (`b` true): the right edge a justified line ends at.
- **Text bounds.** Every `LINE` record in the corpus has `x=0`: the books set no side margins. A block's text then runs from `left` to `right`: in a book with `wholeEmGrid`, `right - left` is the viewport rounded down to whole ems of the line's own em (一 at its `mult`), centred (`left` = half the cut); otherwise 0 to 460. `textBoundsOf` asserts `x == 0` with a message naming that assumption.
- **`pushInOf`**: the n in `[0, shrinkable()]` for which `xAt(n)` equals every dumped x and `n == max(0, natural - available)`, where `natural = rightAt(n) + appliedAt(n) - startAt(n)` and `available = bounds.right - (align == Left ? startAt(n) : bounds.left)`; -1 when there is none.

- [ ] **Step 1: Write the checker.** `LineLayout`, `textBoundsOf`, `pushInOf` as above, and:
  - `TEST_P(CjkRules, LinesSitWhereTheirWidthsPutThem)` replaces `UnspacedCjkSitsWhereItsWidthsPutIt`. Comment: "Every line sits where ParsedText's rules, restated, put it: each CJK mark at its table width, each CJK–Latin gap at a quarter em, and on a line that would not fit, push-in taking the line-edge marks, then the other marks, then the gaps, exactly as much as the line needs (spec 5.2-5.4). No line is wider than its text width, and a line that shrank could not have kept its natural width." For each `LINE` with `align=1` or `2`: `const int need = pushInOf(...)`; `EXPECT_GE(need, 0) << "not where its widths put it: " << lineText(line)`; when found, `EXPECT_LE(layout.rightAt(need), bounds.right) << "wider than the text width"`; count `need > 0` in `pushedIn`. For each `LN` (a table cell, no known width): no unspaced CJK pair overlaps with each mark at its minimum width (A3a's `EXPECT_GE(gap, 0)` branch, shrink-aware).
  - `blockLeftEdges` takes the minimum `startAt(0)` over lines whose first token is not a CJK mark (a pushed-in opening mark at a line start would otherwise pull a block's left edge in).
  - `KinsokuHoldsAtEveryCjkLineBreak`: `LineLayout(...).startAt(0) > leftEdge` where it had `left(0)`.
  - `JustifiedLinesEndFlushOnAWholeEmGrid`: the right edge is `textBoundsOf(...).right`; a non-last justified line with CJK gaps (A3a's tests for that, unchanged) passes when `endsAt(false)` or `endsAt(true)` equals it; the grid assertion uses `right - left` of the text bounds.
- [ ] **Step 2: Run.** `cmake --build build/test --target EpubPipelineTest -j 8 && build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='CjkCorpus/*'` — Expected: all PASS, every line at need 0.
- [ ] **Step 3: Make it bite.** Two scratch edits, each built and run with the filter above, then reverted with `git checkout -- lib/Epub/Epub/ParsedText.cpp`: (1) `cjkLatinGapBefore` returns one pixel more — Expected: `LinesSitWhereTheirWidthsPutThem` and `CjkLatinGapsSitWhereTheSpecPutsThem` FAIL on `test_cjk_line_breaking` and `test_cjk_zh_punctuation`; (2) `applyPunctuationWidths` sets the mid-line width from `forced - 1` eighths — Expected: `LinesSitWhereTheirWidthsPutThem` FAILS on every zh/ja book. Record both in the commit message.
- [ ] **Step 4: Commit.** `git commit -m "test(epub): the CJK rule checker lays each line out again, push-in included"`

---

### Task 4: Push-in in the greedy breakers and `extractLine`

The three greedy breakers and `extractLine` change together: a breaker that accepts a line only pushed in needs `extractLine` to push it in, or the line overflows. The DP is Task 5; until then its lines always fit at their natural width, so `extractLine` finds no need on them and the goldens (all DP) keep their lines.

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h` (members after `edgeTrim_` :63; helpers after `withEdgeTrims` :207)
- Modify: `lib/Epub/Epub/ParsedText.cpp` (`applyPunctuationWidths` :745-810; `computeSingleLineBreakNoHyphen` :984-1002; `computeHyphenatedLineBreaks` :1174-1211; `computeHyphenatedLineBreaksFromIndex` :1272-1303; `hyphenateWordAtIndex` :1425-1426; `extractLine` :1449-1589; `releaseLayoutScratch` :689-690)
- Modify: `test/cjk_layout/CjkTypographyTest.cpp`; `test/epub_pipeline/CjkRulesTest.cpp` (the push-in guard)
- Modify: `test/epubs/make_test_cjk_line_breaking.py` (case 13), regenerate the `.epub`; golden `test_cjk_line_breaking.golden.txt`

**Interfaces:**
- Consumes: `cjkLatinGapBefore`, `naturalGapBefore` (Task 2); `markClass_`, `edgeTrim_`, `opensLine`, `withEdgeTrims` (A3a).
- Produces (ParsedText, private):
  - `std::vector<uint8_t> shrink_;` — what each mark gives up mid-line under push-in, px: its mid-line width minus `(full * minimum + 4) / 8` with the same neighbour; 0 for other tokens;
  - `std::vector<uint8_t> edgeShrink_;` — what each mark gives up at its line edge: its line-edge width minus its line-edge minimum. Comment both with `markClass_`/`edgeTrim_` ("Four bytes a token with them ...; `calculateWordWidths` fills all four, `hyphenateWordAtIndex` inserts into all four");
  - `bool closesLine(size_t i) const` — a mark that is not an opening one: a line's end edge acts on it;
  - `int gapShrinkBefore(size_t j, int gapBefore) const` — `gapBefore - gapBefore / 2` at a breakable CJK gap (only a CJK–Latin gap has a width there), else 0;
  - `int withEdgeShrinks(int shrinkSum, size_t first, size_t last) const` — `shrinkSum` plus `edgeShrink_[first] - shrink_[first]` when `opensLine(first)`, plus `edgeShrink_[last] - shrink_[last]` when `closesLine(last)`;
  - `int pushedInWidth(int naturalSum, int shrinkSum, size_t first, size_t last) const` — `withEdgeTrims(naturalSum, first, last) - withEdgeShrinks(shrinkSum, first, last)`, the line with everything that can give way at its minimum (spec 5.3's fit test).
- Produces (ParsedText.cpp, anonymous namespace): `int pushInShare(int cumulativeBefore, int amount, int tierNeed, int tierTotal)` — `((cumulativeBefore + amount) * tierNeed) / tierTotal - (cumulativeBefore * tierNeed) / tierTotal`, 0 when `tierTotal` is 0.
- A breaker's running shrink sum adds, for token j of a line starting at `first`: `shrink_[j] + (j > first ? gapShrinkBefore(j, gapBeforeJ) : 0)`.

- [ ] **Step 1: Write the failing tests** (greedy breaker; Task 5 adds the DP where it agrees):
```cpp
TEST(CjkPushIn, ALineEndMarkHalvesSoTheLineHoldsIt) {
  // Japanese: 字字字字。 is 50 px with 。 full at the line end, 45 with it at its line-end minimum (half): it holds 45 px.
  const auto lines = layoutText("字字字字。字字", withoutGrid("ja"), CssTextAlign::Left, 45, /*greedy=*/true);
  EXPECT_EQ(wordsOf(lines), (Words{{"字", "字", "字", "字", "。"}, {"字", "字"}}));
}
TEST(CjkPushIn, AMidLineMarkGivesWayBeforeTheLineBreaksEarlier) {
  // Simplified Chinese (every book's table): 字、字字字。 is 55 px with the line-end 。 at half; 、 gives its 5 px blank
  // mid-line, so the line fits 50 px and 、 is drawn 5 px wide.
  const auto lines = layoutText("字、字字字。字字", TypographyProfile{}, CssTextAlign::Left, 50, true);
  ASSERT_EQ(wordsOf(lines), (Words{{"字", "、", "字", "字", "字", "。"}, {"字", "字"}}));
  EXPECT_EQ(lines[0].xpos, (X{0, 10, 15, 25, 35, 45}));
}
TEST(CjkPushIn, LineEdgeMarksGiveWayFirst) {
  // Japanese 字、字字。 is 50 px. On 45 the line-end 。 halves and 、 keeps its width (JLREQ 3.8.3: the line-end blank
  // goes first); on 40 、 halves too.
  const auto wide = layoutText("字、字字。字字", withoutGrid("ja"), CssTextAlign::Left, 45, true);
  ASSERT_EQ(wordsOf(wide), (Words{{"字", "、", "字", "字", "。"}, {"字", "字"}}));
  EXPECT_EQ(wide[0].xpos, (X{0, 10, 20, 30, 40}));
  const auto narrow = layoutText("字、字字。字字", withoutGrid("ja"), CssTextAlign::Left, 40, true);
  ASSERT_EQ(wordsOf(narrow), (Words{{"字", "、", "字", "字", "。"}, {"字", "字"}}));
  EXPECT_EQ(narrow[0].xpos, (X{0, 10, 15, 25, 35}));
}
TEST(CjkPushIn, ALineEndMarkIsHalfOrFullNothingInBetween) {
  // On 48 px the line needs 2 px and the line-end 。 gives its whole 5 (spec 5.2: half or full); justification spreads
  // the 3 left over, 1 and 2, and the line ends flush with 。 at half width.
  const auto lines = layoutText("字、字字。字字", withoutGrid("ja"), CssTextAlign::Justify, 48, true);
  ASSERT_EQ(wordsOf(lines), (Words{{"字", "、", "字", "字", "。"}, {"字", "字"}}));
  EXPECT_EQ(lines[0].xpos, (X{0, 10, 21, 33, 43}));
}
TEST(CjkPushIn, TheCjkLatinGapGivesWayLast) {
  // Japanese 字、字A字 is 56 px: 50 of characters and two 3 px gaps. On 49 px 、 gives its 5 px first, then each gap
  // 1 px (shares of the last 2), down from 3 to 2. The DP stretches 字、字A instead (Task 5), so this is greedy only.
  const auto lines = layoutText("字、字A字字字", withoutGrid("ja"), CssTextAlign::Left, 49, true);
  ASSERT_EQ(wordsOf(lines), (Words{{"字", "、", "字", "A", "字"}, {"字", "字"}}));
  EXPECT_EQ(lines[0].xpos, (X{0, 10, 15, 27, 39}));
}
TEST(CjkPushIn, APushedInLineFillsItsLineInEveryAlignment) {
  // The pushed-in line of AMidLineMarkGivesWay... is exactly 50 px: no stretch when justified, no offset when centred
  // or set right. Placed by its natural 55 px it would start 2 or 5 px left of the margin.
  for (const CssTextAlign alignment : {CssTextAlign::Justify, CssTextAlign::Center, CssTextAlign::Right}) {
    const auto lines = layoutText("字、字字字。字字", TypographyProfile{}, alignment, 50, true);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].xpos, (X{0, 10, 15, 25, 35, 45})) << static_cast<int>(alignment);
  }
}
TEST(CjkPushIn, AWordHyphenatedEarlierKeepsTheShrinkInStep) {
  // 가나다라마바 splits after 가나다라마 (Korean, hyphenation on), inserting a token. The next line, 바 字、字字, is 55 px
  // and fits 50 only with 、 halved: the inserted token must not move 、's shrink onto another token.
  Hyphenator::setPreferredLanguage("ko");
  ParsedText text(false, /*hyphenationEnabled=*/true, aligned(CssTextAlign::Left));
  text.addWord("가나다라마바", EpdFontFamily::REGULAR);
  text.addWord("字、字字字。字字", EpdFontFamily::REGULAR);
  const auto lines = layout(text, 50);
  ASSERT_EQ(wordsOf(lines), (Words{{"가나다라마"}, {"바", "字", "、", "字", "字"}, {"字", "。", "字", "字"}}));
  EXPECT_EQ(lines[1].xpos, (X{0, 15, 25, 30, 40}));
}
```
- [ ] **Step 2: Run to see them fail.** `cmake --build build/test --target CjkLayoutTest -j 8 && build/test/cjk_layout/CjkLayoutTest.exe --gtest_filter='CjkPushIn.*'` — Expected: all seven FAIL (the lines break before the marks that should give way).
- [ ] **Step 3: Implement.**
  - **Scratch.** `applyPunctuationWidths` assigns `shrink_` and `edgeShrink_` (`assign(count, 0)`, as `edgeTrim_`) and fills both for each mark it narrows, from the `minimum` of the two `widthOf` calls it already makes, rounded as their `forced` is. `hyphenateWordAtIndex` inserts 0 into both after its `edgeTrim_` insert; `releaseLayoutScratch` swaps both out after `edgeTrim_` (A3a's `ReleasingTheLayoutScratchGivesBackTheMarksScratchToo` then covers them).
  - **The three greedy breakers.** Each line keeps a running `lineShrink` beside `lineWidth`; the fit test becomes `pushedInWidth(lineWidth + candidateWidth, lineShrink + candidateShrink, lineStart, currentIndex) <= effectivePageWidth`, `candidateShrink` being the running-sum term of the Interfaces block (gap: `spacing` or `interWordGaps[currentIndex]`), added to `lineShrink` when the token is taken. The hyphenation prefix test (`availableWidth`) stays at natural widths (my call: a hyphenated line never depends on push-in, so Latin and Korean hyphenation do not move).
  - **`extractLine`.** In the sums loop also total the line's three tiers: S1, the line-edge marks (token 0 if `opensLine`, the last token if `closesLine`: their `edgeShrink_`); S2, every other token's `shrink_`; S3, `gapShrinkBefore` of each CJK–Latin gap on the line. `need = clamp(natural - effectivePageWidth, 0, S1 + S2 + S3)`, natural being today's `lineWordWidthSum + totalNaturalGaps`. When `need > 0`: S1 is taken whole; the rest of `need` goes to S2 up to S2, then to S3, shared with `pushInShare` in line order. A mark's box is `boxWidth(wordIdx)` minus its give (so `inkShift`, which compares the box with the natural width, places a shrunk mark by its ink with no change); a gap is its width minus its give. Every use of the line's width after the sums (`spareSpace`, the right and centre positions) uses natural minus what was taken; the justification code is otherwise unchanged, so a tier-1 surplus is spread as in 5.1 (`ALineEndMarkIsHalfOrFull...`). An overfull line that cannot be saved still takes everything it can.
- [ ] **Step 4: The fixture's case 13 and the guard.** In `make_test_cjk_line_breaking.py` add to the docstring "13. push-in  a line that fits only pushed in (spec 5.3)" and:
```python
# Case 13: push-in. After its 1 em indent the first line holds 27 characters, the whole 28 em text width (the pipeline
# harness lays out 460 px in 16 px ems: 448). The second line holds 27 characters, 、 and the 。 glued to its last
# character: 29 ems at full width, 28 with 、 at its Japanese minimum (half) and the line-end 。 at half, so the greedy
# breaker (hyphenation on) pushes it in.
PUSH_IN = LONG_RUN[:32] + "、" + LONG_RUN[32:54] + "。" + LONG_RUN[54:]
```
  with `("13. push-in", f"<p>{PUSH_IN}</p>")` appended to `CASES`; regenerate the `.epub`. In `CjkRulesTest.cpp` set `forcesPushIn` true for `test_cjk_line_breaking`, and at the end of `LinesSitWhereTheirWidthsPutThem`: `if (GetParam().hyphenation && GetParam().forcesPushIn) EXPECT_GT(pushedIn, 0u) << "no line was pushed in";`.
- [ ] **Step 5: Run.** `build/test/cjk_layout/CjkLayoutTest.exe` — Expected: all PASS, A3a's and A2's tests unchanged. `UPDATE_GOLDENS=1 build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='SyntheticCorpus/*MatchesGolden*'` — Expected: only `test_cjk_line_breaking.golden.txt` changes, by case 13's new paragraph and nothing else (the goldens run the DP). Then `--gtest_filter='SyntheticCorpus/*:CjkCorpus/*:Cjk*'` — Expected: all PASS; on the `_hyph` cases the checker now finds pushed-in lines, and `test_cjk_line_breaking_hyph` has at least one.
- [ ] **Step 6: Commit.** `git commit -m "feat(epub): CJK lines push punctuation and CJK-Latin gaps in before breaking earlier (greedy breakers)"`

---

### Task 5: Push-in in the DP breaker

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.cpp` (`computeLineBreaks`' inner loop :893-936; the anonymous namespace)
- Modify: `test/cjk_layout/CjkTypographyTest.cpp` (two new tests, five extended to both breakers; the comment of A3a's `JapaneseLetsAFullStopEndingALineStayFull`)
- Modify: CJK goldens

**Interfaces:**
- Consumes: `pushedInWidth`, `gapShrinkBefore` (Task 4).
- Produces (anonymous namespace): `int cubicBadness(int amount, int width)` — today's stretch badness, `(amount / width)³ × 10000` clamped to [0, 10000] in integers, moved out of the loop unchanged and used for both cases; `constexpr long long kPushInPenalty = 1;`.

The cost (my call): a line that fits at its natural width costs what it costs today (the last line 0). A line that fits only pushed in, the last line too, costs `(1 + cubicBadness(lineLen - effectivePageWidth, effectivePageWidth))² + kPushInPenalty`. An exact line costs 1, a pushed-in one at least 2, and one stretched with any badness at least 4: push-in wins when the alternative is loose (badness ≥ 1, about 0.6 em of slack on a 28 em line) and loses to a near-exact line. Lines with nothing that can give way never reach the new branch, so pure-CJK paragraphs without punctuation break as before.

- [ ] **Step 1: Write the failing tests.** Turn `ALineEndMarkHalvesSoTheLineHoldsIt`, `AMidLineMarkGivesWayBeforeTheLineBreaksEarlier`, `ALineEndMarkIsHalfOrFullNothingInBetween` and `APushedInLineFillsItsLineInEveryAlignment` into loops over `for (const bool greedy : {false, true})` with `<< (greedy ? "greedy breaker" : "DP breaker")`, and in `LineEdgeMarksGiveWayFirst` the 45 px half (the 40 px half stays greedy: the DP stretches 字、字 there at 24,649 against 24,650 pushed in). Add:
```cpp
TEST(CjkPushIn, TheDpPrefersAsSlightAStretchToAPushIn) {
  // Simplified Chinese 字、字字字 is 50 px and fits 45 only pushed in; 字、字字 is 40 px, 5 short. Both have a cubic
  // badness of 13 (5 px of 45): stretched 196, pushed in 197. The DP stretches; the greedy breaker always pushes in.
  const auto dp = layoutText("字、字字字字字", TypographyProfile{}, CssTextAlign::Left, 45, /*greedy=*/false);
  ASSERT_EQ(wordsOf(dp), (Words{{"字", "、", "字", "字"}, {"字", "字", "字"}}));
  EXPECT_EQ(dp[0].xpos, (X{0, 10, 20, 30}));
  const auto greedy = layoutText("字、字字字字字", TypographyProfile{}, CssTextAlign::Left, 45, true);
  ASSERT_EQ(wordsOf(greedy), (Words{{"字", "、", "字", "字", "字"}, {"字", "字"}}));
  EXPECT_EQ(greedy[0].xpos, (X{0, 10, 15, 25, 35}));
}
TEST(CjkPushIn, APureCjkParagraphWithoutPunctuationLaysOutAsBefore) {
  // Nothing on these lines can give way: both breakers break and justify as before push-in (spec 5.3).
  for (const bool greedy : {false, true}) {
    const auto lines = layoutText("一二三四五六七八九十百千", withoutGrid("ja"), CssTextAlign::Justify, 55, greedy);
    ASSERT_EQ(wordsOf(lines), (Words{{"一", "二", "三", "四", "五"}, {"六", "七", "八", "九", "十"}, {"百", "千"}}));
    EXPECT_EQ(lines[0].xpos, (X{0, 11, 22, 33, 45}));
  }
}
```
  The DP values in the extended tests, for the reviewer: `ALineEndMarkHalves...` pushed in 197 against 137,641 for 字字字 | 字。字字; `AMidLineMark...` 122 against 6,561; `ALineEndMarkIsHalfOrFull...` 2 against 278,784; `LineEdgeMarks...` at 45 px 197 against 137,641.
- [ ] **Step 2: Run to see them fail.** `build/test/cjk_layout/CjkLayoutTest.exe --gtest_filter='CjkPushIn.*'` — Expected: the DP halves of the extended tests FAIL; `TheDpPrefersAsSlightAStretch...` and `APureCjkParagraph...` PASS already (nothing pushes in on the DP yet). That is expected: they pin what must not change.
- [ ] **Step 3: Implement.** In the inner loop keep a running `shrink` beside `currlen` (the Interfaces term of Task 4, `interWordGaps[j]` as the gap); break when `pushedInWidth(currlen, shrink, i, j) > effectivePageWidth`, and replace the comment "It only grows with j ..." with the reason this still holds: a token's mid-line minimum is never below its line-edge minimum (`CjkPunctuation.NoMarkIsNarrowerMidLineThanAtALineEdge`), so the pushed-in width grows with j. Cost as above. Update the comment of A3a's `JapaneseLetsAFullStopEndingALineStayFull`: the grid makes the line 40 px, and 字字字字。 is 45 px even with 。 at half, so 字。 does not fit after 字字字.
- [ ] **Step 4: Run.** `build/test/cjk_layout/CjkLayoutTest.exe` and `build/test/typography/TypographyTest.exe` — Expected: all PASS. Goldens: `UPDATE_GOLDENS=1 ... --gtest_filter='SyntheticCorpus/*MatchesGolden*'`, `git status test/epub_pipeline/goldens` — Expected: only `test_cjk_*` goldens change. Read every changed paragraph: a line that now holds more tokens has a narrower mark, or a CJK–Latin gap under its quarter em, and the paragraph's lines are no looser than before; the `W` records of case 3 (the long run without punctuation) in `test_cjk_line_breaking` are unchanged, whatever their `y`. Then `--gtest_filter='SyntheticCorpus/*:CjkCorpus/*:Cjk*'` — Expected: all PASS.
- [ ] **Step 5: Commit.** `git commit -m "feat(epub): the DP breaker pushes CJK lines in when that avoids a loose line"`

---

### Task 6: Budgets, the device, docs

**Files:** `docs/future_work/cjk.md`, `docs/design/README.md`, `docs/superpowers/specs/2026-10-19-cjk-a3-typography-design.md` (moved), `docs/superpowers/plans/2026-10-19-cjk-a3a-punctuation-and-grid.md` (deleted), `lib/Epub/Epub/CjkPunctuation.h` and `TypographyProfile.h` (one header line each); code only if a check below finds a fault.

- [ ] **Step 1: Full host suite.** `cmake --build build/test -j 8 -- -k 0 && cd build/test && ctest -j 8 --output-on-failure` — Expected: all PASS but the tests that fail on a master run on this machine.
- [ ] **Step 2: Firmware.** `pio run -e default`, then `pio run -e gh_release` (Global Constraints) — Expected: each SUCCESS; note `default`'s flash use against master's in the PR.
- [ ] **Step 3: Host heap and time.** Build `epub_pipeline_dump` at master too: `git worktree add C:/tmp/wt-a3b-base origin/master`, configured as in the Global Constraints into its own `build/test`. Heap: `python test/epub_pipeline/heap_peak_check.py <dump> test test/epub_pipeline/heap_peak_baseline.txt` with each dump tool — Expected: no `REGRESSION`; on every CJK row the branch is within 1 KB of master (two scratch bytes a token; more means an allocation to find, not a baseline to raise). Time: `epub_pipeline_dump test/epubs/<book>.epub <fresh dir> --bench`, five runs a tool for soseki, honglou, zh_punctuation and line_breaking, the median `BENCHMARK spine_0 ... time=` — Expected: within 5 % of master. The host figure is indicative; Step 4 is the gate.
- [ ] **Step 4: Layout time on the C3 (X4, COM6).** For each firmware, master's `default` then the branch's (flash with `pio run -e default -t upload --upload-port COM6`, no logger holding the port):
  - First run only: copy `test_cjk_ja_soseki`, `test_cjk_zh_honglou`, `test_cjk_zh_punctuation` and the branch's `test_cjk_line_breaking` `.epub` to `/test/` (`tools/serial_transfer.py` or the web upload, `POST /upload?path=/test`). Record `cmd:GET:fontFamily`, `fontSize`, `paragraphAlignment`, `hyphenationEnabled`, `sleepTimeoutMinutes`; set `sleepTimeoutMinutes=30` and the CJK family by its index from `cmd:GET:*` (the label form has answered `SET_ERROR:bad value`); `cmd:TTF` names the TrueType families loaded.
  - Per book, one warm-up and three measured cold builds, each from Home (removing an open book's cache is not guarded):
    `python tools/devctl.py --port COM6 --log x4-<firmware>-<book>-<n>.log cmd:ACT:HOME idle:30000 cmd:FS:RM:/.crosspoint/epub_<hash>/spines/0 "cmd:ACT:READER:/test/<book>.epub@1:1" "expect:createSectionFile spine=0 done:120" idle:60000:SCREEN`
    with `<hash>` 2247170459 (soseki; verified on the X4 2026-10-19), 1764791513 (honglou), 3251213462 (zh_punctuation), 781786345 (line_breaking): MurmurHash2 of the card path, seed 0xc70f6907. If `FS:RM` answers `not found`, `cmd:FS:LS:/.crosspoint` shows the folder.
  - Read the median `total=` and `parse=` of the `createSectionFile spine=0 done:` lines — Expected: the branch's `total` within 5 % of master's on every book (the A3b share of the spec's 10 % over A2). Report `parse` too. Over budget: split the time with the `PARSEPROF` lines before changing code; the DP's push-in adds two additions a step, so look first at the gap's renderer calls (`cjkLatinGapBefore`) and `extractLine`'s second pass.
  - Page time: three page turns on soseki and honglou per firmware, `PageTurn stats` to `Deferred AA` (docs/future_work/cjk.md, "Testing on a device") — Expected: within noise.
- [ ] **Step 5: Screenshots (X4), reviewed by Claude.** Open item: which families on the X4's card cover kana; list the card with `cmd:FS:LS` and pick one for the Japanese books. On each firmware, with `paragraphAlignment=0` (justified) and then `1` (left), and `hyphenationEnabled` off (DP) and on (greedy): `shot:` page 1 of soseki, honglou and zh_punctuation, and the pages of `test_cjk_line_breaking` that hold cases 5, 12 and 13 (page with `cmd:BTN:RIGHT idle:5000:SCREEN` and take each page until case 13's heading has been shown). Then soseki and the fixture's case-12 page once more in Bookerly (no CJK glyphs). Look at each branch page beside its master page (Read the PNGs) and check:
  - no line runs past the right margin; justified lines end at one edge;
  - a line that holds more than on master shows a tighter 、 or ， mid-line, or a half-width 。」 at its end; its neighbours are not tightened;
  - a thin space (a quarter em) between 漢字 and iPhone, 2026 and 年, 第 and 3; none beside 「Python」, before ＡＢＣ, after abc before ｱｲｳ, nor in addition to the source's spaces around Latin;
  - characters stay in columns on lines that were not pushed in; kinsoku still holds;
  - in Bookerly, no gap and the same line breaks as master.
  Record what each check found, page by page, in the PR. Restore every setting recorded in Step 4.
- [ ] **Step 6: X3.** Open item: the X3 session is not yet agreed. The maintainer flashes the branch's `default` firmware on the X3 and leaves it on USB; Claude uploads files without a reset. Drive it with devctl on the port the maintainer names, never `CMD:REBOOT`, an RTS pulse or esptool. Take the Step 5 screenshots of soseki, honglou, zh_punctuation and the fixture's case 12 and 13 pages, justified, at the X3's reading size, and review them the same way. The spec's budget names the X3, but the budget is judged on the X4 (decision 1); an X3 timing is optional.
- [ ] **Step 7: Docs.** A3 is complete, and its spec is cited from code ("spec 5.2" in `CjkPunctuation`, `ParsedText`, `TypographyProfile`, `CjkRulesTest`), so it moves rather than goes:
  - `git mv docs/superpowers/specs/2026-10-19-cjk-a3-typography-design.md docs/design/cjk-typography.md`; its status line reads "Shipped: A3a #520, A3b #<this PR>. Design record; the code is the authority."; add it to `docs/design/README.md`; the header comments of `CjkPunctuation.h` and `TypographyProfile.h` gain one line: section numbers ("spec 5.2") refer to `docs/design/cjk-typography.md`.
  - `docs/future_work/cjk.md`: the intro's typography bullet keeps only ruby and emphasis dots (B); roadmap row 7 is "shipped (#<this PR>)"; an "Already shipped" entry for A3b (push-in in all four breakers, the order and the DP cost; the CJK–Latin gap in zh/ja books; the Japanese minimums; the checker that lays each line out again; section version 97; the fixture's cases 12 and 13); the section "Push-in and the CJK–Latin gap (PR A3b)" goes; the B section's and "The design" bullet's references to the A3 spec name `docs/design/cjk-typography.md`.
  - `git rm docs/superpowers/plans/2026-10-19-cjk-a3a-punctuation-and-grid.md` (working material). This plan lives on the docs branch only and is not part of the PR.
- [ ] **Step 8: Format and commit.** clang-format 21 (`PATH="<dir>/clang_format/data/bin:$PATH" bash bin/clang-format-fix`, `git status`); commit any reformatting as `style: clang-format 21` and the docs as `docs(cjk): push-in and the CJK-Latin gap shipped; the A3 design moves to docs/design`. The PR title needs its type: `feat(epub): push-in and the CJK-Latin gap (A3b)`; the body records Steps 2-6.
