# CJK ruby (furigana) and emphasis dots (PR B) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Japanese and Chinese books draw `<ruby>` readings at half size above their base, set as JLREQ asks (1:2:1 spacing, overhang over kana only, never past the margins), and draw CSS `text-emphasis` marks (傍点) above each character of a span; KOSync positions do not move.

**Architecture:** Two PRs from one plan. **B1 (Tasks 1-7), ruby:** the parser turns `<ruby>`/`<rt>` into groups of base tokens. `ParsedText` marks each group in the token's own `TokenBoundary::Boundary` (a new `annotation` byte: leader, follower), so the group travels through every insert and erase. The group's ruby texts are kept in token order. `ParsedText` widens a group whose ruby is longer than its base, spreading the base 1:2:1. Overhang lands on a kana or punctuation neighbour. What a line edge cannot absorb becomes a positive line-edge trim, so all four breakers and `extractLine` protect the margins through the existing `lineStartTrim`/`lineEndTrim`. `extractLine` hands each line its ruby boxes, and `TextBlock` stores them in its arena only on lines that have them, behind bit 1 of the existing flags byte. `TextBlock` draws them in a band of half the line's ascender above the base. **B2 (Tasks 8-10), emphasis dots:** a cascaded CSS property `text-emphasis-style` (with the `-epub-` and `-webkit-` prefixes) reaches every token of a span as 4 bits of the same `annotation` byte. It becomes per-line emphasis runs in the same arena region, and the marks are drawn in the same band.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3/S3), GoogleTest host tests (`test/`, CMake), Python fixture generators (`test/epubs/`), `tools/devctl.py` on the X4.

**Spec:**
- `docs/superpowers/specs/2026-10-16-cjk-layout-design.md` section 7 (ruby). It is on branch `docs/cjk-layout-design`, commit `7e1136008`, not on master; read it with `git show 7e1136008:docs/superpowers/specs/2026-10-16-cjk-layout-design.md`.
- `docs/design/cjk-typography.md` section 9 (ruby spacing, emphasis dots).
- `docs/future_work/cjk.md`, "Ruby (furigana) (PR B)" (the constraints).
- Replayed against master `095f7d667` (2026-10-22, after #547 test-suite cleanup, #548 synthetic bold, #549 web bundle identity). First checked against `d2594561d` and `upstream/develop` `f8936ba67` (2026-10-21).

## Global Constraints

- KOSync, from `docs/future_work/cjk.md`: "KOSync must not move. `VisibleText`'s non-visible tags stay unchanged, so `<rt>` and `<rp>` text keep counting toward visible offsets, as crengine counts them. Add a host test that pins the offsets of a ruby fixture."
  - `lib/Epub/Epub/VisibleText.h` is not edited.
  - `<rp>` is skipped with the parser's `SKIP_TAGS` (layout only), never with `nonVisibleTextDepth_`.
- Latin books lay out exactly as before. Only `test_cjk_line_breaking.golden.txt` and `test_cjk_ja_soseki.golden.txt` may change; every change is explained by a rule of spec section 7 or A3 spec section 9.
  - Upstream #2781's DP tie-break (`cost <= dp[i]`) is **not** ported: it moves Latin lines.
- **Known red from Task 4 to Task 6, and nothing else:** once the parser makes groups (Task 4), the Soseki golden (`SyntheticCorpus/EpubPipelineTest.MatchesGolden`, its `test_cjk_ja_soseki` case) and the `CjkCorpus` rules on `test_cjk_ja_soseki` that cannot see groups before Task 6 prints them (`UnspacedCjkSitsWhereItsWidthsPutIt`, `JustifiedLinesEndFlushOnAWholeEmGrid`, `KinsokuHoldsAtEveryCjkLineBreak`) may fail. Tasks 4 and 5 list the failing tests in their report; Task 6 ends with the whole suite green.
- No new settings, no new UI strings.
- **Versions.** Master `095f7d667` has `SECTION_FILE_VERSION` 97 (`lib/Epub/Epub/Section.cpp`) and `CSS_CACHE_VERSION` 29 (`lib/Epub/Epub/css/CssParser.h:119`).
  - B1 sets the section version to **99** (Task 5), B2 to **100** (Task 9). 98 belongs to synthetic italic (`feat/synthetic-italic`), which runs first in the roadmap.
  - B2 sets the CSS cache version to 30 (Task 8).
  - If master's version differs when the branch merges master before its PR, renumber to master's + 1 (B2: + 2) and say so in the PR. The same rule holds for the CSS cache version.
- **Heap (my call; the C3 has no PSRAM).**
  - Per token: one byte (`Boundary::annotation`). `reserveWords`' heap gate already multiplies `sizeof(TokenBoundary::Boundary)`.
  - Only in blocks with ruby: the ruby texts (`std::string`, inline up to 15 bytes, i.e. 5 kana) and the layout scratch `rubyEdge_`, 6 B a token.
  - A resident line with annotations adds 2 + 10 B per group + its ruby bytes to its arena. `sizeof(TextBlock)` stays 52 B on the 32-bit targets: the new `uint16_t` sits in the padding after `arenaOwned_`.
  - `TextBlock::render` allocates nothing for ruby or marks.
  - Host gate (`HeapPeakRegression`, `epub_pipeline_dump --bench`, heap-only and `--arena=52272`): `test_cjk_ja_soseki` rises by at most 2,048 B; fixtures without ruby by at most 512 B.
  - Device: `STATE heap=` on a Soseki ruby page stays within 2 KB of the same page on master.
- Attribution (the porting convention):
  - Task 3 and Task 4 port Phạm Bình An's design from crosspoint-reader #2665 (d0359edfc), #2781 (046827fc3) and #3102 (c6fe67d48), by hand: our histories are unrelated.
  - Their commits carry `Co-authored-by: Phạm Bình An <111893501+brianhuster@users.noreply.github.com>`.
  - Their code gets a source header saying which parts are theirs and which differ.
  - #2772 (Uri Tauber) and #2735 (William Floyd) inspired "lines without ruby cost nothing": credited in the `TextBlock.h` comment, no trailer.
- Comments say what the code does and why; no history narration. Format with clang-format 21 (`bin/clang-format-fix` with 21.1.8 first on the POSIX `PATH`).
- Code with backslash escapes (`"\xE4\xB8\x80"`) goes in with the Write or Edit tool, never a heredoc.
- Host tests run from Git Bash:
  - configure: `cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1` (googletest and ArduinoJson fetch over git);
  - full suite: `cmake --build build/test -j 8 -- -k 0 && ctest --test-dir build/test -j 8`;
  - binaries sit at `build/test/<test dir>/<target>.exe`;
  - parameterized suites carry their instantiation prefix (`SyntheticCorpus/EpubPipelineTest`, `CjkCorpus/CjkRules`, `HeapAndLentArena/KeepTogetherFixture`), so a `--gtest_filter` names it;
  - before Task 1, run the full suite on the merge base and record its failing tests (Windows-only failures) in the ledger; those are not this plan's.
- Firmware: `PLATFORMIO_CORE_DIR=C:/pio "$HOME/.platformio/penv/Scripts/pio.exe" run -e default`, plus `-e gh_release`, one `pio run` at a time, in the background. `x4pro` and `lilygo_t5s3` are left to CI.
- Devices:
  - the X4 only (the maintainer's standing preference); an X3 run happens only when the maintainer asks for one;
  - Claude flashes and drives the X4 (`tools/devctl.py`; stop any serial logger before flashing);
  - `CMD:GET` every setting before `CMD:SET` and restore it after.
- Claude reviews the Japanese screenshots against the rules below; the maintainer does not read CJK.

## Maintainer decisions (2026-10-22)

1. Ruby over a non-CJK base (an English gloss over kanji, a pronunciation in a Latin book) is centred over its base. JLREQ 1:2:1 spacing is for CJK ruby only.
2. Emphasis marks are drawn above the text for every language, Chinese included. `text-emphasis-position` stays in `future_work`.
3. Uneven line height in ruby paragraphs is acceptable for B1: only ruby lines grow. An even-height mode stays in `future_work`.
4. Two PRs, B1 ruby then B2 emphasis dots, as this plan splits them. My call (controller): B2 reuses B1's annotation byte and band, so it needs B1 merged first.
5. Still open, asked at the device run (Task 7, Step 5):
   - which Japanese-capable font is on the X4's card (Claude checks with `CMD:FS:LS`);
   - whether the maintainer can push a KOReader position on a ruby book, for the KOSync cross-check (layout spec 9.2). The host test pins only our own consistency.

## Review Focus

1. **Pretty-printed and stray ruby markup** (newlines between `</rt>` and the next base; `<rt>` outside `<ruby>`; `<ruby>` without `<rt>`; `<rt>` before any base; `<rtc>`). Expected: no lost text, no spaces between CJK characters. Pinned in Task 4: `RubyLayout.APrettyPrintedRubyLaysOutWithoutSpaces`, `RubyLayout.StrayRubyMarkupLosesNoText`.
2. **A ruby group at a line start or end, in every breaker and in the page-bottom retry.** The ruby stays inside the margins and a justified line ends flush with its ruby; upstream protects one breaker only. Pinned in Task 3: `RubyEdges.AGroupStartingALineKeepsItsRubyInside`, `RubyEdges.AGroupEndingALineKeepsItsRubyInside`, `RubyEdges.AJustifiedLineEndingInAGroupEndsFlushWithItsRuby`, `RubyEdges.APageBottomRetryKeepsTheRubyInside`.
3. **A ruby line at a page bottom in a build with a lent arena.** `beforeLineHook` decides the page break before the line exists; without the band it lets a line through that `addLineToPage` then pushes to the next page, so the line's bytes sit in the previous page's rewound block. Pinned in Task 5: `RubyLayout.ARubyBookPagesTheSameWithAndWithoutALentArena`; the band's two formulas agree by Task 1's `RubyTextBlock.TheBandIsTheSameBeforeAndAfterTheLineExists`.
4. **The next page pre-rendered with ruby on it.** `EpubReaderActivity::pageDrawsUnscaled` sends plain pages to the 1-bit ink-mask pre-render, which has no half-size glyphs. Pinned in Task 1: `RubyTextBlock.AnAnnotatedLineIsNotDrawnUnscaled`; Task 5 routes `pageDrawsUnscaled` through `TextBlock::drawsUnscaled()`; Task 7 reads the pre-render log on the X4.
5. **An emphasis span that wraps, and punctuation inside it.** Expected: both lines carry their marks and the taller band; 。、「」 and spaces take no mark; ruby wins over emphasis on the same base. Pinned in Task 9: `CjkEmphasis.ASpanKeepsItsMarksAcrossALineBreak`, `CjkEmphasis.PunctuationAndSpacesTakeNoMark`, `CjkEmphasis.RubyWinsOverEmphasisOnTheSameBase`.

## Numbers the tests use

- **The `test/word_size_layout` shim** (used by `CjkLayoutTest`):
  - every codepoint is 10 px wide and a space 5 px;
  - `ASCENDER` 16, `LINE_HEIGHT` 20;
  - scaled measures round (`x * scale + 0.5`).
  - So ruby at 0.5 is 5 px a character and the band is 8.
- **The `test/epub_pipeline` shim:**
  - a full-width CJK character is 16 px wide;
  - ascender 18, line height 24.
  - So ruby is 8 px a kana and the band is 9.
- **Group notation** in the `ParsedText` tests: `"と煙を", (1, 1, "けむり")` means `addWord("と煙を")` (one token per character) and `addRubyGroup(1, 1, "けむり")`: tokens [1, 2) become one group with that ruby.

---

# PR B1: ruby

### Task 1: Annotated lines in `TextBlock`

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h`, `lib/Epub/Epub/blocks/TextBlock.cpp`
- Create: `test/cjk_layout/CjkRubyTest.cpp`. Add it to `test/cjk_layout/CMakeLists.txt` after `CjkTypographyTest.cpp`.
- Modify: `test/cjk_layout/CjkTypographyTest.cpp`: one test beside its allocation counter (`gLiveAllocations`, file-local; the binary's global `operator new` is replaced there).

**Interfaces:**
- Produces, in `TextBlock` (public):
  ```cpp
  enum class AnnotationKind : uint8_t { Ruby = 1, Emphasis = 2 };  // Emphasis is filled from Task 9
  static constexpr size_t kMaxAnnotationText = 255;
  struct Annotation {
    uint16_t firstWord = 0;  // the line's word index
    uint16_t wordCount = 0;
    int16_t x = 0;           // the ruby box, in wordXpos's space
    uint16_t width = 0;
    AnnotationKind kind = AnnotationKind::Ruby;
    uint8_t mark = 0;        // an emphasis run's mark (Task 9); 0 for ruby
    std::string_view text;   // the ruby, at most kMaxAnnotationText bytes; empty for emphasis
  };
  uint16_t annotationCount() const;
  Annotation annotation(uint16_t i) const;
  bool hasAnnotations() const { return annotationBytes_ != 0; }
  // Half the line's ascender when the line has annotations, else 0.
  int annotationBand(const GfxRenderer& renderer, int fontId) const;
  static int annotationBandFor(const GfxRenderer& renderer, int effFontId, float blockScale, uint8_t maxSizePct);
  // No word draws scaled: block multiplier 1, no heading font, no word sizes, no SUP/SUB/SMALL_CAPS, no annotations.
  bool drawsUnscaled() const;
  ```
- Produces: `WordRange::annotations`, a `const std::vector<Annotation>*` (null: none), in `firstWord` order.
- Produces, the arena (the `TextBlock.h` layout comment gains it):
  - an annotation region after `text[textBytes]`: `u16 count`, then `count` records of 10 B (`u16 firstWord, u16 wordCount, s16 x, u16 width, u8 kind, u8 len`), then the ruby texts back to back, `len` bytes each, no NUL. For `Emphasis`, `len` is the mark and no text follows.
  - It is read with `memcpy` (it starts at an odd offset).
  - Member `uint16_t annotationBytes_ = 0;`, declared right after `arenaOwned_`.
- Produces, the disk format. Today's flags byte holds the sizes in bit 0 and the word-font count in bits 4-7, and `deserialize` refuses `flags & 0x0E`. Now bit 1 is "annotations present" (ruby, and from Task 9 emphasis runs): header `u16 numWords, u8 flags, u16 textBytes`, then `u16 annotationBytes` only when `flags & 0x02`, then the arena verbatim, then the render style as today. `deserialize` refuses `flags & 0x0C`.

- [ ] **Step 1: Write the failing tests** in `CjkRubyTest.cpp`. Helpers:
  - `serialized()` as in `test/word_size_layout/WordFontSlotTest.cpp:51`; `restore(bytes)` writes the bytes to `HalFile::forReadWrite()`, seeks to 0 and returns `TextBlock::deserialize(file)` (as `WordFontSlotTest.cpp:200-203`).
  - `std::unique_ptr<TextBlock> rubyLine(std::vector<TextBlock::Annotation> annotations, std::vector<std::string> words = {"漢", "字"}, float multiplier = 1.0f, std::vector<uint8_t> sizePct = {})`: a hand-built `WordRange`, each word at x = 10 px per codepoint before it, `BlockStyle` left-aligned with indent 0 and the given `fontSizeMultiplier`. Define it outside the anonymous namespace; `CjkTypographyTest.cpp` declares it and uses it.
  ```cpp
  TEST(RubyTextBlock, AnAnnotatedLineKeepsItsGroupsThroughSerialization) {
    auto line = rubyLine({{0, 2, 0, 20, TextBlock::AnnotationKind::Ruby, 0, "かんじ"}});
    ASSERT_EQ(line->annotationCount(), 1);
    const std::string bytes = serialized(*line);
    EXPECT_EQ(bytes[2], 0x02);  // flags: annotations only
    const auto back = restore(bytes);
    ASSERT_NE(back, nullptr);
    const TextBlock::Annotation a = back->annotation(0);
    EXPECT_EQ(a.firstWord, 0); EXPECT_EQ(a.wordCount, 2); EXPECT_EQ(a.x, 0); EXPECT_EQ(a.width, 20);
    EXPECT_EQ(std::string(a.text), "かんじ");
    EXPECT_EQ(std::string(back->wordText(1)), "字");
  }
  TEST(RubyTextBlock, ALineWithoutAnnotationsIsTheBytesItWasBefore)  // annotations == nullptr and == empty
      // vector serialize identically; flags byte 0; no annotationBytes field (bytes.size() is the same).
  TEST(RubyTextBlock, CorruptAnnotationsAreRefused)  // three corruptions of `bytes` each give nullptr:
      // firstWord + wordCount = 3 > 2 words; flags |= 0x04; a record len past the region's end.
  TEST(RubyTextBlock, RubySitsInABandOfHalfAnAscenderAboveItsBase) {
    // render(renderer, kFontId, 0, 100): the base at y 108, the ruby one character a call at y 100
    // (108 - round(16 * 0.5)), scale 0.5, spread 1:2:1 over the 20 px box: 15 px of ruby, 5 px to spread.
    // drawCalls with scale 0.5: texts {"か","ん","じ"}, x {0, 7, 14}, y 100; base calls y 108.
  }
  TEST(RubyTextBlock, ARubyThatIsNotCjkIsCentred)  // ruby "abc" (15 px) over 20 px: x {2, 7, 12}.
  TEST(RubyTextBlock, AKanaRubyOverALatinBaseIsCentred)  // rubyLine({{0, 1, 0, 20, Ruby, 0, "かな"}}, {"ab"}):
      // the 10 px ruby at x {5, 10}, not 1:2:1's {2, 12} (decision 1).
  TEST(RubyTextBlock, TheBandFollowsTheBlocksSize)  // annotationBand: 8; with fontSizeMultiplier 2.0, 16;
      // the same line without annotations, 0.
  TEST(RubyTextBlock, TheBandIsTheSameBeforeAndAfterTheLineExists) {
    // For multipliers {1.0, 1.25} and word sizes {all 100, one word at 150}:
    // line->annotationBand(r, kFontId) == TextBlock::annotationBandFor(r, kFontId, multiplier, line->maxSizePct()).
  }
  TEST(RubyTextBlock, WordBoxSitsBelowTheBand)  // wordBox(r, 1, kFontId, 0, 100).y == 108 == the base draw y.
  TEST(RubyTextBlock, AnAnnotatedLineIsNotDrawnUnscaled)  // drawsUnscaled(): false with the ruby, true without.
  ```
  In `CjkTypographyTest.cpp`:
  ```cpp
  TEST(RubyTextBlock, DrawingRubyAllocatesNothing) {
    // rubyLine({{0, 2, 0, 20, Ruby, 0, "かんじ"}}); renderer.drawCalls.reserve(16) first; then gLiveAllocations is
    // the same before and after render().
  }
  ```
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/test --target CjkLayoutTest -j 8`. Expected: compile errors (`TextBlock::Annotation`, `WordRange::annotations`, `annotationBand` …).
- [ ] **Step 3: Implement.**
  - `arenaOffsets`/`arenaSize` gain the region: `arenaSize(wordCount, hasSizes, fontCount, textBytes, annotationBytes = 0)`.
  - The range constructor validates each annotation and writes the region. Invalid means: `firstWord + wordCount > count`, `wordCount == 0`, or text longer than `kMaxAnnotationText`; it makes the block invalid, logged as "TXB".
  - `serialize` sets bit 1 and writes `annotationBytes_` after `textBytes`.
  - `deserialize` reads it, sizes the arena with it, and validates the region before `annotation()` can read it: the records fit; every `firstWord + wordCount <= numWords`; `wordCount >= 1`; the `len`s of the ruby records sum to the rest; the kinds are known.
  - **Render.** `render` and `wordBox` draw and box the base at `y + annotationBand(...)`; everything that used `y` uses that baseY. The reader's link areas are `wordBox` boxes (`EpubReaderActivity.cpp:7124`), so this covers the dictionary and the links.
  - **The ruby, per record.**
    - The font is `wordFontId(firstWord, effFontId)`, the style REGULAR (my call: one glyph style per ruby, fewer SD glyph-cache entries), the scale `0.5f * wordScale(firstWord)`.
    - Each character is copied into a 5-byte stack buffer and drawn by `drawTextScaled` at `y + band - getFontAscenderSizeScaled(font, scale)`.
    - Its advance is `static_cast<int>(getTextAdvanceX(font, buffer, REGULAR) * scale + 0.5f)`, as `ParsedText` measures (`getTextAdvanceX` takes UTF-8 text, not a codepoint).
    - The slack is `S = width - sum`. When every codepoint of the ruby and of the group's words is `utf8IsUnspacedCjk`, character `i` of `n` starts at `x + S * (2i + 1) / (2n) + (advances before i)`; otherwise at `x + S / 2 + (advances before i)` (decision 1).
    - Ruby is drawn in the scanning pass too: the SD page prewarm learns its glyphs there.
    - Its ascender is fetched once per record, not through `ascenderCache`, which would allocate.
  - `annotationBandFor` returns half of:
    - `getFontAscenderSize(effFontId)` when `maxSizePct == 100 && blockScale == 1.0f`;
    - else `getFontAscenderSizeScaled(effFontId, blockScale * maxSizePct / 100.0f)`.
  - `annotationBand` calls it with the block's font and multiplier and `maxSizePct()`.
  - `drawsUnscaled` is `EpubReaderActivity::pageDrawsUnscaled`'s per-line test (`EpubReaderActivity.cpp:1435-1438`) plus `!hasAnnotations()`.
  - Add `static_assert(sizeof(void*) != 4 || sizeof(TextBlock) == 52, "a resident page holds ~28 TextBlocks")` in `TextBlock.cpp` (32-bit layout: vptr 4, `RenderStyle` 12, four scalars and a pad byte 8, `arena` 4, `arenaOwned_` and its padding 4, five views 20). The Task 7 firmware build checks it.
  - Header comment, at the arena layout: "Lines without annotations cost nothing: the idea of crosspoint-reader #2772 (Uri Tauber) and #2735 (William Floyd), which drop empty ruby vectors; here the bytes are not written at all."
- [ ] **Step 4: Run the tests.** `cmake --build build/test --target CjkLayoutTest TextBlockRangeTest WordSizeLayoutTest -j 8 && ./build/test/cjk_layout/CjkLayoutTest.exe --gtest_filter='RubyTextBlock.*' && ./build/test/text_block_range/TextBlockRangeTest.exe && ./build/test/word_size_layout/WordSizeLayoutTest.exe`. Expected: all PASS (the `WordFontSlots` byte checks unchanged).
- [ ] **Step 5: Commit.** `git add lib/Epub/Epub/blocks/TextBlock.* test/cjk_layout/` → `feat(cjk): TextBlock keeps ruby boxes on the lines that have them`

### Task 2: Ruby groups as token roles in `ParsedText`

**Files:**
- Modify: `lib/Epub/Epub/TokenBoundary.h`, `lib/Epub/Epub/ParsedText.h`, `lib/Epub/Epub/ParsedText.cpp`
- Modify: `test/token_boundary/TokenBoundaryTest.cpp`, `test/cjk_layout/CjkRubyTest.cpp`, `test/cjk_layout/CjkTypographyTest.cpp`

**Interfaces:**
- Consumes: `TextBlock::Annotation`, `WordRange::annotations` (Task 1).
- Produces, in `TokenBoundary.h`:
  ```cpp
  enum class RubyRole : uint8_t { None = 0, Leader = 1, Follower = 2 };
  struct Boundary {
    bool continues = false;
    bool noSpaceBefore = false;
    uint8_t annotation = 0;  // bits 0-1 RubyRole; bits 2-5 the emphasis mark (Task 9)
  };
  constexpr RubyRole rubyRole(Boundary b);
  constexpr Boundary withRubyRole(Boundary b, RubyRole role);
  ```
  `allowsBreak` and `hasSpace` ignore `annotation`.
- Produces, in `ParsedText` (public):
  ```cpp
  // Tokens [firstToken, firstToken + tokenCount) become one ruby group: the first keeps its boundary (a break may
  // come before the group), the rest are glued to it. False when the range is empty, runs past size(), or a token
  // in it already has a role.
  bool addRubyGroup(size_t firstToken, size_t tokenCount, std::string text);
  // An <rt> with no new base: appended to the block's last group. False when the block has none.
  bool appendRubyText(std::string_view text);
  ```
- Produces, private:
  - `std::vector<std::string> rubyTexts_`: one per `Leader` token of the block, in token order. A follower that no leader before it reaches is ignored.
  - `std::vector<TextBlock::Annotation> lineAnnotations_`: per-line scratch, reused.

- [ ] **Step 1: Write the failing tests.** In `CjkRubyTest.cpp` (anonymous namespace): `aligned()` as in `CjkTypographyTest.cpp`; every block is `aligned(CssTextAlign::Left)` (indent 0) unless a test says Justify. `layout(text, width)` returns `Line`s holding each line's words, xpos and its annotations copied out (`first, count, x, w, text` as `std::string`); `wordsOf(lines)`; `rubyOf(line)` lists `(first, count, text)`; `boxesOf(line)` lists `{x, w}`.
  ```cpp
  // TokenBoundaryTest.cpp
  TEST(TokenBoundaryRuby, ARoleChangesNeitherBreakNorSpace)
      // for each of {}, {false,true}, {true,false}: allowsBreak/hasSpace equal with and without
      // withRubyRole(b, Follower); rubyRole(withRubyRole(b, Leader)) == Leader.
  // CjkRubyTest.cpp
  TEST(RubyGroups, AGroupNeverBreaksInside)  // for hyphenation {false, true}: "は吾輩", (1, 2, "わがはい");
      // at 25 px: {{"は"}, {"吾","輩"}}; line 2's ruby {(0, 2, "わがはい")}.
  TEST(RubyGroups, AnRtWithoutANewBaseAppendsToTheLastGroup)  // "漢", (0, 1, "かん"); appendRubyText("じ")
      // -> ruby text "かんじ"; on a fresh block appendRubyText returns false.
  TEST(RubyGroups, ConsumedLinesTakeTheirRubyWithThem)  // "吾輩は猫", (0, 2, "わがはい"), (3, 1, "ねこ");
      // layoutAndExtractLines(30 px, includeLastLine=false) emits {吾,輩,は} with ruby {(0, 2, "わがはい")};
      // the second pass (includeLastLine=true) emits {猫} with ruby {(0, 1, "ねこ")}.
  TEST(RubyGroups, PreserveSourceLaysTheSameGroupsOutTwice)  // the same text and groups, 30 px,
      // includeLastLine=true, preserveSource=true, twice: both passes give {{吾,輩,は},{猫}} with the two rubies above.
  TEST(RubyGroups, ALatinBaseSplitByBionicReadingStaysOneGroup)  // bionicReadingEnabled; "Tokyo",
      // (0, 1, "とうきょう"): words {"Tok","yo"}, ruby {(0, 2, "とうきょう")}.
  TEST(RubyGroups, AGroupBaseIsNeverHyphenated)  // hyphenation on, Hyphenator::setPreferredLanguage("en");
      // "internationalization", (0, 1, "x"): at 60 px the word stays one token.
  // CjkTypographyTest.cpp, beside the counter:
  TEST(RubyGroups, ReleasingAnEmptyBlocksScratchGivesBackItsRuby)  // "漢字", (0, 2, "かんじ"), laid out whole at
      // 400 px, then releaseLayoutScratch(): gLiveAllocations back where it was before addWord.
  ```
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/test --target CjkLayoutTest TokenBoundaryTest -j 8`. Expected: compile errors (`addRubyGroup`, `rubyRole`).
- [ ] **Step 3: Implement.** Keep `rubyTexts_` in step with the tokens at every site that moves them:
  - the consumed-prefix erase: count the leaders among the erased tokens and erase that many texts from the front;
  - the `preserveSource` snapshot and restore;
  - `reset()` (clear);
  - `releaseLayoutScratch()` (swap away when `words` is empty; `lineAnnotations_` always).

  `hyphenateWordAtIndex` returns false for a token whose role is not `None`. `applyBionicReadingTransform`: a span after the first gets `kGlued` with the original's role turned into `Follower` when the original had any role.

  `extractLine` fills `lineAnnotations_`:
  - one record per leader in `[lastBreakAt, lineBreak)`, its followers clipped to the line;
  - x is the leader's box left, the width the sum of the group's widths, the text a view of `rubyTexts_[ordinal]` (Task 3 gives the box its JLREQ geometry);
  - `range.annotations` points at the scratch.

  `addRubyGroup` refuses tokens that already have a role, sets the roles, and inserts the text at the leader's ordinal. The parser (Task 4) calls it with `tokenCount = size() - firstToken`.
- [ ] **Step 4: Run them.** `cmake --build build/test --target CjkLayoutTest TokenBoundaryTest WordSizeLayoutTest EpubPipelineTest -j 8`, then `./build/test/cjk_layout/CjkLayoutTest.exe`, `./build/test/token_boundary/TokenBoundaryTest.exe`, `./build/test/word_size_layout/WordSizeLayoutTest.exe` and `./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='SyntheticCorpus/*:CjkCorpus/*'`. Expected: all PASS; goldens unchanged.
- [ ] **Step 5: Commit.** `feat(cjk): ruby groups ride on the token boundaries`

### Task 3: Ruby widths, 1:2:1 spacing, overhang and line edges in every breaker

**Files:**
- Modify: `lib/Utf8/Utf8.h`
- Modify: `lib/Epub/Epub/ParsedText.h`, `lib/Epub/Epub/ParsedText.cpp`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:1396` (the hook lambda takes the new parameter and ignores it)
- Modify: `test/cjk_layout/CjkRubyTest.cpp`

**Interfaces:**
- Consumes: `rubyRole`, `rubyTexts_`, `lineAnnotations_` (Task 2).
- Produces:
  - `inline bool utf8IsKana(uint32_t cp)`: 3040-309F, 30A0-30FF, 31F0-31FF, FF66-FF9F.
  - `struct RubyEdge { int16_t start = 0; int16_t end = 0; int16_t lead = 0; };`
  - `std::vector<RubyEdge> rubyEdge_` (layout scratch, empty when the block has no ruby).
  - `void applyRubyWidths(const GfxRenderer&, int fontId, std::vector<uint16_t>& widths)`, called at the end of `calculateWordWidths`.
  - `int rubyWidth(const GfxRenderer&, int fontId, size_t leader, const std::string& text) const`: `measureWordWidth` in the leader's font at `0.5f * wordScale(leader)`, regular style.
- Produces: `lineStartTrim(i)` adds `rubyEdge_[i].start` and `lineEndTrim(i)` adds `rubyEdge_[i].end` when `rubyEdge_` is not empty. That is how the four breakers and `extractLine` see ruby: no breaker body changes.
- Produces: `beforeLine_` becomes `std::function<void(uint8_t maxSizePct, bool annotated)>`, and `setBeforeLineHook` takes it; `extractLine` passes `!lineAnnotations_.empty()`. Task 5 changes the parser's lambda to use it.

- [ ] **Step 1: Write the failing tests** (`CjkRubyTest.cpp`, the word_size_layout shim, left-aligned, indent 0; group notation as in "Numbers the tests use"):
  ```cpp
  TEST(RubyWidths, AShorterRubyIsSpreadOverItsBase)  // "煙草", (0, 2, "たばこ"): xpos {0,10}; ruby {x 0, w 20}.
  TEST(RubyWidths, ALongerRubyOverhangsTheKanaBesideIt)  // "と煙を", (1, 1, "けむり"): xpos {0,10,20}; ruby {8, 15}.
  TEST(RubyWidths, ALongerRubyPushesHanNeighboursAndSpreadsItsBase)  // "漢字字漢", (1, 2, "ああああああ"):
      // xpos {0, 12, 27, 40}; ruby {10, 30}.
  TEST(RubyWidths, AdjacentGroupsShareNoSpace)  // "の煙草が", (1, 1, "けむり"), (2, 1, "くさき"): xpos {0,10,20,30};
      // ruby {5,15} and {20,15}: the first ends where the second starts.
  TEST(RubyWidths, RubyFollowsTheWordsSize)  // "と煙を" at sizePct 200, (1, 1, "けむり"): xpos {0,20,40}; ruby {15, 30}.
  TEST(RubyWidths, TheRubyIsReadiedWithItsWords)  // "と煙を", (1, 1, "けむり"): readyCalls[0].text == "と煙を けむり-".
  TEST(RubyEdges, AGroupStartingALineKeepsItsRubyInside)  // for hyphenation {false,true}: "あああと煙を",
      // (4, 1, "けむり"), 40 px: {{あ,あ,あ,と},{煙,を}}; line 2 xpos {2,12}, ruby {0, 15}.
  TEST(RubyEdges, AGroupEndingALineKeepsItsRubyInside)  // for hyphenation {false,true}: "あああ煙を", (3, 1, "けむり"):
      // at 42 px {{あ,あ,あ},{煙,を}}; at 43 px {{あ,あ,あ,煙},{を}} with line 1's ruby {28, 15}.
  TEST(RubyEdges, AJustifiedLineEndingInAGroupEndsFlushWithItsRuby)  // Justify, "あああ煙をを", (3, 1, "けむり"),
      // 47 px: line 1 is {あ,あ,あ,煙} and its ruby's x + width == 47.
  TEST(RubyEdges, APageBottomRetryKeepsTheRubyInside)  // as CjkPunctuationWidths.APageBottomRetryKeepsTheMarksWidths
      // (Justify, "ko", hyphenation on, addWord "가나", "다라마바사아", "あああ煙を", then (5, 1, "けむり")), 55 px, the
      // first hyphenated line retried: ASSERT_TRUE(retried); lines {{가나},{다라마바사},{아,あ,あ,あ},{煙,を}}: the
      // ruby's end reserve moves 煙 to a line it starts; line 4's xpos {2, 12}, ruby {0, 15}.
  ```
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/test --target CjkLayoutTest -j 8 && ./build/test/cjk_layout/CjkLayoutTest.exe --gtest_filter='RubyWidths.*:RubyEdges.*'`. Expected: FAIL on the x positions, the ruby boxes and the line breaks (Task 2's boxes have no overhang, no spreading, no edges).
- [ ] **Step 3: Implement `applyRubyWidths`.** It runs after `applyPunctuationWidths`, so a narrowed mark lends its narrowed width. This is the algorithm, with every value an `int`:
  ```text
  rubyEdge_ = {} when rubyTexts_ is empty, else count zeroed entries
  for each leader i, its followers up to last, its text (rubyTexts_ in order):
    B = widths[i] + ... + widths[last];  R = rubyWidth(i, text)
    if R <= B: continue                                   // the ruby is spread over the base when drawn
    room(n, boundaryIndex) = 0 unless n exists, boundaries[boundaryIndex] has no space, n has no ruby role,
        and n's edge codepoint cp (last of i-1, first of last+1) has utf8IsKana(cp) or
        CjkPunctuation::classOf(cp) != Class::None;
        then min(widths[n], the advance of cp at n's font and scale) / 2
    aL = room(i - 1, i);  aR = room(last + 1, last + 1)
    Wc = max(B, R - aL - aR);  X = R - Wc
    oL = min(aL, X / 2);  if (X - oL > aR) oL = X - aR;  oR = X - oL
    extra = Wc - B;  n = last - i + 1
    for k in 0..n-1:  e = extra * (k + 1) / n - extra * k / n;  widths[i + k] += e;  rubyEdge_[i + k].lead = e / 2
    rubyEdge_[i].start = oL;  rubyEdge_[last].end = oR
  ```
  - `hyphenateWordAtIndex` inserts a zero `rubyEdge_` entry beside its `markClass_` insert when `rubyEdge_` is not empty; `releaseLayoutScratch()` swaps `rubyEdge_` away with the other scratch.
  - In `extractLine`:
    - a group token's draw x adds `rubyEdge_[k].lead`, and the line's first token adds `rubyEdge_[k].start` too (its box already holds the reserve);
    - the record's box: `containerLeft` is the leader's box left, plus `start` when the leader opens the line; `Wc` is the sum of the group's widths on the line; with `R <= Wc` the box is `{containerLeft, Wc}`, else `{containerLeft - start, R}`.
  - `layoutAndExtractLines`' font prewarm appends each ruby text, preceded by a space, to its leader's font text, before the trailing `-`.
  - The DP's `break` on `lineLen > width` stays valid: an end reserve is at most half the neighbour it replaces, so `lineLen` still grows with `j`. Say so in its comment.
  - Source header above `applyRubyWidths`:
    > Ruby widths after crosspoint-reader PR #2665 ("feat(epub): native <ruby> support for Chinese, Japanese", Phạm Bình An / @brianhuster) and #2781 (ruby inside the margins). Theirs: a longer ruby overhangs its neighbours, a Han neighbour is pushed away, a line edge keeps the overhang inside. Ours: overhang only over kana and CJK punctuation, the 1:2:1 spreading JLREQ asks for, and the line-edge reserves as edge trims, so all four breakers see them.
- [ ] **Step 4: Run them.** `cmake --build build/test --target CjkLayoutTest EpubPipelineTest -j 8 && ./build/test/cjk_layout/CjkLayoutTest.exe && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='SyntheticCorpus/*:CjkCorpus/*'`. Expected: all PASS; goldens unchanged (the parser makes no groups yet).
- [ ] **Step 5: Commit.**
  ```text
  feat(cjk): ruby overhangs kana, spreads 1:2:1 and stays inside the margins

  Ported by hand from crosspoint-reader #2665 (d0359edfc) and #2781 (046827fc3): the overhang
  rules and the line-edge protection are their design; the spreading and the edge trims in every
  breaker are ours.

  Co-authored-by: Phạm Bình An <111893501+brianhuster@users.noreply.github.com>
  ```

### Task 4: The parser: `<ruby>`, `<rt>`, `<rp>`

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`, `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- Create: `test/epub_pipeline/RubyLayoutTest.cpp`. Add it to `EpubPipelineTest`'s sources in `test/epub_pipeline/CMakeLists.txt` after `CjkRulesTest.cpp`.
- Modify: `test/epub_pipeline/VisibleTextOffsetLutTest.cpp`

**Interfaces:**
- Consumes: `ParsedText::addRubyGroup`, `appendRubyText` (Task 2).
- Produces, private members:
  ```cpp
  static constexpr size_t kNoRubyGroup = SIZE_MAX;
  static constexpr size_t kRubyGroupSlack = 32;  // tokens the 96-token split may wait for a group
  int rubyDepth_ = -1;               // the open <ruby>'s depth
  int rtDepth_ = -1;                 // collecting <rt> text while >= 0
  int rtcDepth_ = -1;                // inside <rtc>: its <rt> flows inline
  size_t rubyGroupStart_ = kNoRubyGroup;
  bool rubyHasGroup_ = false;        // this <ruby> made a group: a later <rt> without a base appends to it
  std::string rubyText_;
  uint32_t rtVisibleOffset_ = 0;     // where the <rt> text starts, for the inline fallback
  ParsedText* wordTarget();          // the open table cell's text, else currentTextBlock
  ```

- [ ] **Step 1: Write the failing tests.**
  - `RubyLayoutTest.cpp`: a fixture like `BlockEmphasisFixture` (`test/epub_pipeline/BlockEmphasisTest.cpp`) that builds a synthetic book (`syntheticBookFromDocument`, `dc:language` `en`, embedded style on). It returns each line's words, its ruby `(firstWord, wordCount, text)` list and its y.
  ```cpp
  TEST_F(RubyLayout, MonoRubyAnnotatesEachBaseAndDropsTheParentheses)
      // <p><ruby>漢<rp>（</rp><rt>かん</rt><rp>）</rp>字<rp>（</rp><rt>じ</rt><rp>）</rp></ruby>は</p>
      // words {漢,字,は}; ruby {(0,1,かん),(1,1,じ)}.
  TEST_F(RubyLayout, GroupRubySpansItsWholeBase)  // <p><ruby><rb>吾輩</rb><rt> わが はい </rt></ruby>は猫</p>
      // words {吾,輩,は,猫}; ruby {(0,2,"わが はい")} (trimmed, inner whitespace collapsed to one space).
  TEST_F(RubyLayout, APrettyPrintedRubyLaysOutWithoutSpaces)  // kPrettyPrinted below: words
      // {漢,字,は,難,し,い,で,す,が,、,面,白,い,で,す,。}; ruby texts {かん,じ,むずか,おも,しろ};
      // は's x == 字's x + 16 (no space token, no gap).
  TEST_F(RubyLayout, StrayRubyMarkupLosesNoText)  // one paragraph each, none with ruby; the concatenated words:
      // 前<rt>まえ</rt>後                              -> "前まえ後"   (<rt> outside <ruby> is inline)
      // <ruby>漢字</ruby>です                          -> "漢字です"   (no <rt>: the base alone)
      // <ruby><rt>よみ</rt></ruby>本                   -> "よみ本"     (<rt> before any base: inline)
      // <ruby>漢<rtc><rt>かん</rt></rtc></ruby>        -> "漢かん"     (<rtc> is out of scope: inline)
      // 漢<ruby>字<rt>じ</rt></ruby><ruby><rt>よみ</rt></ruby> -> "漢字よみ", ruby {(1,1,じ)} only: an <rt>
      //   without a base appends only to a group of its own <ruby>
  TEST_F(RubyLayout, RubyInATableCellAnnotatesTheCellsWords)  // <table><tr><td>前文<ruby>漢<rt>かん</rt></ruby>
      // </td></tr></table>: the cell's line has ruby {(2,1,かん)}; no body line has ruby.
  TEST_F(RubyLayout, ALongGroupIsKeptWholeAndAnOverlongOneFallsBackInline)
      // 90 Han characters then <ruby>{10 Han}<rt>ながいよみ</rt></ruby>: one group of wordCount 10.
      // 90 Han then <ruby>{140 Han}<rt>よみ</rt></ruby>: no ruby; よみ is laid out as text.
  ```
  The pretty-printed paragraph, as authoring tools write it (newlines and indentation between `</rt>` and the next base):
  ```cpp
  constexpr const char* kPrettyPrinted = R"(<p>
    <ruby>
      漢<rp>（</rp><rt>かん</rt><rp>）</rp>
      字<rp>（</rp><rt>じ</rt><rp>）</rp>
    </ruby>は<ruby>難<rt>むずか</rt></ruby>しいですが、<ruby>
      面<rt>おも</rt>
      白<rt>しろ</rt>
    </ruby>いです。
  </p>)";
  ```
  `VisibleTextOffsetLutTest.cpp`:
  ```cpp
  TEST_F(SyntheticLutFixture, RubyAndRpTextCountTowardTheContentOffsets) {
    // 200 x <p><ruby>吾輩<rp>（</rp><rt>わがはい</rt><rp>）</rp></ruby>は猫である。</p>: 42 visible bytes each
    // (6 + 3 + 12 + 3 + 18), one line each. Each page starts at 42 x (the lines on the pages before it), and
    // ChapterXPathIndexerInternal::countTotalTextBytes is 200 x 42. Every page round-trips to itself (roundTrip()).
  }
  ```
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='RubyLayout.*:SyntheticLutFixture.Ruby*'`. Expected: `RubyLayout.*` FAIL (the readings are inline, the parentheses laid out); the LUT test PASSES already. It pins that the parser change keeps it.
- [ ] **Step 3: Implement**, in this order of `startElement`, `consumeText` and `endElementBody`:
  - **`SKIP_TAGS`** becomes `{"head", "rp"}` (`ChapterHtmlSlimParser.cpp:311`). Visible counting stays in `characterData`, before `consumeText` sees the skip.
  - **`startElement`**, right after the `SKIP_TAGS` check (`:2813`): CSS is resolved and `display:none` handled by then.
    - **`ruby`** when `rubyDepth_ < 0`:
      - flush the word buffer, and when something was flushed `nextWordContinues = true`;
      - `rubyDepth_ = depth`; `rubyHasGroup_ = false`; `rubyGroupStart_` = `wordTarget()->size()`, or `kNoRubyGroup` without a target;
      - fall through to the generic inline path.
    - **`rtc`** inside a ruby: `rtcDepth_ = depth`; fall through.
    - **`rt`** inside a ruby and not inside an `rtc`:
      - flush the word buffer;
      - `rtDepth_ = depth`, `rubyText_.clear()`, `rtVisibleOffset_ = visibleTextOffset_`;
      - `depth += 1; return`.
    - Any other `rt` is an ordinary inline element.
  - **`consumeText`**, after the SVG check (`:3408`): while `rtDepth_ >= 0`, append to `rubyText_` up to `TextBlock::kMaxAnnotationText` bytes, cut at a UTF-8 boundary (`utf8SafeTruncateBuffer`), and return.
  - **`endElementBody`**, after the `streamFailed` check (`:3709`):
    - **`</rt>`** (`rtDepth_ == depth - 1`):
      - `rtDepth_ = -1`; collapse whitespace runs to one space and trim (upstream's `trimAndNormalize`);
      - if the group is valid and the target grew since `rubyGroupStart_`: `addRubyGroup(rubyGroupStart_, size() - rubyGroupStart_, text)`, then `rubyGroupStart_ = size()` and `rubyHasGroup_ = true`;
      - otherwise, when `rubyHasGroup_` and the group is valid, `appendRubyText(text)`;
      - if neither took it, the text flows inline: `consumeText(self, text, len, rtVisibleOffset_, /*source=*/false)`;
      - `nextWordContinues = true` when the target is not empty; `depth -= 1; return`.
    - **`</rtc>`**: `rtcDepth_ = -1`.
    - **`</ruby>`** (`rubyDepth_ == depth - 1`): flush the word buffer; reset `rubyDepth_` and `rubyGroupStart_`; `nextWordContinues = true` when the target is not empty; fall through.
  - **The 96-token split** (in `flushPartWordBuffer`, `:1198`):
    - it waits while a ruby is open and the block holds at most `96 + kRubyGroupSlack` tokens;
    - a split that runs while a ruby is open sets `rubyGroupStart_ = kNoRubyGroup` (the group's start was consumed);
    - so do `startNewTextBlock` and `degradeRowAtOpenCell` while a ruby is open.
  - **In a table cell**, each group added charges `TABLE_BUFFER_BYTES_PER_WORD` to `pendingRowBytes`, as a word does.
  - **Source header** above the ruby branch in `startElement`:
    > Ported from crosspoint-reader PR #2665 ("feat(epub): native <ruby> support for Chinese, Japanese", Phạm Bình An / @brianhuster) and #3102 ("fix(ruby): don't flush while in a ruby group"). Theirs: the state machine, the collapsed `<rt>` text, an `<rt>` without a base appending to the last group, no split inside a group. Ours: the groups go to a table cell's own text, the split waits a bounded number of tokens, stray `<rt>`/`<rtc>` text stays inline, and `<rp>` stays visible text for KOSync.
- [ ] **Step 4: Run them.** `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='RubyLayout.*:SyntheticLutFixture.*:Corpus/*:VisibleText*:TextNodeRules*:ProgressMapper*'`. Expected: all PASS. Then the full `EpubPipelineTest`: only the known-red set of the Global Constraints fails (the Soseki golden, its readings gone from the text, and the `CjkCorpus` rules on Soseki that do not know groups yet). List them in the report; leave them for Task 6.
- [ ] **Step 5: Commit.**
  ```text
  feat(cjk): the parser makes ruby groups from <ruby> and <rt>, skips <rp>

  Ported by hand from crosspoint-reader #2665 (d0359edfc) and #3102 (c6fe67d48). <rt> and <rp>
  text keeps counting toward KOSync's visible offsets.

  Co-authored-by: Phạm Bình An <111893501+brianhuster@users.noreply.github.com>
  ```

### Task 5: Taller ruby lines on the page and in tables; the pre-render; section version 99

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (`beforeLineHook`, the hook lambda, `addLineToPage`, `layoutTableRow`), `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` (`beforeLineHook` signature)
- Modify: `lib/Epub/Epub/Page.cpp` (`PageTableFragment::render`)
- Modify: `lib/Epub/Epub/Section.cpp` (`SECTION_FILE_VERSION`)
- Modify: `src/activities/reader/EpubReaderActivity.cpp` (`pageDrawsUnscaled`)
- Modify: `test/epub_pipeline/RubyLayoutTest.cpp`

**Interfaces:**
- Consumes: `TextBlock::annotationBand`, `TextBlock::annotationBandFor`, `TextBlock::drawsUnscaled`, `hasAnnotations` (Task 1); the `beforeLine_(maxSizePct, annotated)` call (Task 3).
- Produces: `void ChapterHtmlSlimParser::beforeLineHook(uint8_t maxSizePct, bool annotated)`.

- [ ] **Step 1: Write the failing tests** (`RubyLayoutTest.cpp`):
  ```cpp
  TEST_F(RubyLayout, ARubyLineIsTallerByHalfAnAscender)  // <p>前</p><p><ruby>漢<rt>かん</rt></ruby></p><p>後</p>:
      // (y2 - y1) - (y1 - y0) == 9.
  TEST_F(RubyLayout, ATableRowWithRubyIsTallerByTheBand)  // two rows, one with ruby: the PageTableFragment's
      // getRows()[1].height - getRows()[0].height == 9.
  TEST_F(RubyLayout, ARubyBookPagesTheSameWithAndWithoutALentArena)
      // pipeline_harness::runAndDump(test_cjk_ja_soseki.epub) with Profile{} and with Profile{}.lentArenaBytes =
      // 52272: equal dumps, more than one page.
  ```
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='RubyLayout*'`. Expected: the height tests FAIL (difference 0); the arena test may pass by luck. It is the guard for the hook.
- [ ] **Step 3: Implement.**
  - `addLineToPage`: after the `maxPct` scaling (`:4404-4407`), `lineHeight += line->annotationBand(renderer, fontId)`.
  - `beforeLineHook(maxSizePct, annotated)`: when `annotated`, add `TextBlock::annotationBandFor(renderer, effectiveFontId(bs), bs.fontSizeMultiplier, maxSizePct)`, so the two tests give one answer.
  - The lambda at `:1396` passes both arguments.
  - `layoutTableRow`: `contentHeight` is the sum over the cell's lines of `lineHeight + line->annotationBand(renderer, fontId)`, instead of `cell.lines.size() * lineHeight` (`:5263`).
  - `PageTableFragment::render`: `lineY += lineHeight + line->annotationBand(renderer, fontId)` (`Page.cpp:120`).
  - `EpubReaderActivity::pageDrawsUnscaled`: the per-line test becomes `block->drawsUnscaled()`.
  - `SECTION_FILE_VERSION = 99` (see the Global Constraints for renumbering), its comment line first:
    > v99: ruby (furigana) is laid out above its base: a line with ruby keeps its groups (TextBlock annotations) and is taller by half an ascender; `<rp>` is not laid out.
- [ ] **Step 4: Run them.** `./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='RubyLayout*:*KeepTogether*:SaxStateInArena*:TableRowFallbackArena*'`. Expected: all PASS. The full `EpubPipelineTest`: only the known-red set fails (the Soseki golden now also has taller lines).
- [ ] **Step 5: Commit.** `feat(cjk): a line with ruby is taller by half an ascender, in tables too`

### Task 6: The goldens see ruby: `dumpPage`, the fixture, the rule checker

**Files:**
- Modify: `test/epub_pipeline/PipelineRunner.cpp`, `test/epub_pipeline/CjkRulesTest.cpp`
- Modify: `test/epubs/make_test_cjk_line_breaking.py`, `test/epubs/test_cjk_line_breaking.epub` (regenerated)
- Modify: `test/epub_pipeline/goldens/test_cjk_line_breaking.golden.txt`, `test/epub_pipeline/goldens/test_cjk_ja_soseki.golden.txt`
- Modify: `test/epub_pipeline/heap_peak_baseline.txt`

**Interfaces:**
- Consumes: `TextBlock::annotationCount`, `annotation` (Task 1).
- Produces, the dump record after a line's `W` records (body `LINE` and cell `LN` lines alike): `R first=<f> count=<c> x=<x> w=<w> t=<text>`, one per ruby group. It does not match the word-stream pattern `\bW x=\S+ s=\S+ z=\S+ t=(.*)`.

- [ ] **Step 1: Extend the fixture.** New cases in `make_test_cjk_line_breaking.py`, each a paragraph with the docstring entry the others have:
  - **12, ruby markup:**
    - mono-ruby;
    - group ruby with `<rb>`;
    - `<rp>`;
    - the pretty-printed paragraph of `RubyLayout.APrettyPrintedRubyLaysOutWithoutSpaces` (Task 4), verbatim;
    - an `<rt>` with no new base;
    - `<rt>` outside `<ruby>`;
    - `<rtc>`.
  - **13, ruby at the line edges:** `煙<rt>けむり</rt>` between kana, every 27 characters for 30 lines. At 28 ems a line, the group lands in every column, the first and last included.
  - **14, longer ruby:**
    - between Han characters (the base spreads);
    - 御台所 with おだいどころ;
    - two adjacent groups between kana.
  - **15, ruby in a table cell.**

  Run `python test/epubs/make_test_cjk_line_breaking.py`.
- [ ] **Step 2: Write the failing rule** in `CjkRulesTest.cpp`:
  - **The parser.** `parseLines` reads `R` records into `DumpLine::rubies` (`first`, `count`, `x`, `w`, `text`); `DumpLine` gains `int x`, the `LINE` record's `x=` (0 for `LN`).
  - **A line's left** (`lineLeft`): the smaller of `LineWidths::left(0)` and its rubies' `x`. `blockLeftEdges`, `KinsokuHoldsAtEveryCjkLineBreak` and `JustifiedLinesEndFlushOnAWholeEmGrid` compare lines by it.
  - **New rule:**
  ```cpp
  // Every ruby box lies inside its block and the page, no two boxes on a line overlap, and a box covers no word
  // outside its group but kana and CJK punctuation outside any group (spec 7.2): never Han, Latin or another
  // group's base.
  TEST_P(CjkRules, RubyStaysOverItsBaseAndOffTheHan)
      // x >= leftEdge.at(keys[i]); a body line's x + w <= pipeline_harness::Profile{}.viewportWidth - line.x;
      // sorted boxes: x[k] + w[k] <= x[k+1]; every word not in the box's group whose [x, x + wordWidth) meets
      // (x, x + w) is in no group and is utf8IsKana or CjkPunctuation::classOf != None.
      // test_cjk_line_breaking and test_cjk_ja_soseki must have at least 20 boxes (ASSERT_GE); another book
      // without any is skipped (GTEST_SKIP).
  ```
  - **Updated rules:**
    - `UnspacedCjkSitsWhereItsWidthsPutIt` skips a pair when either word is in a ruby group: the base is spread there.
    - `JustifiedLinesEndFlushOnAWholeEmGrid` takes a line's right edge as the larger of its last word's and its rubies' `x + w`.
- [ ] **Step 3: Run it to see it fail.** `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='CjkCorpus/CjkRules.Ruby*'`. Expected: the two ruby books FAIL on `ASSERT_GE(boxes, 20)` (no `R` records yet); the others SKIP.
- [ ] **Step 4: Print the groups.** `PipelineRunner.cpp`: after each `dumpWords` call, `dumpAnnotations(out, block, indent)` prints the `R` records. Lines without annotations print nothing new.
- [ ] **Step 5: Run the rules.** `./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='CjkCorpus/*'`. Expected: all PASS (CjkRules and CjkJustifiedRules, every book, both breakers): the known red of Tasks 4-5 ends here, but for the goldens.
- [ ] **Step 6: Regenerate the goldens and read them.**
  - Run `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`, then `git status test/epub_pipeline/goldens`.
  - Expected: only `test_cjk_line_breaking.golden.txt` and `test_cjk_ja_soseki.golden.txt` change.
  - In the Soseki diff:
    - the readings and （） are gone from the `W` records;
    - `R` records appear;
    - ruby lines step by 33 instead of 24.
  - Any other change is a bug.
- [ ] **Step 7: The heap gate.**
  - Run `./build/test/epub_pipeline/epub_pipeline_dump.exe test/epubs/test_cjk_ja_soseki.epub <tmp> --bench`, with and without `--arena=52272`.
  - Expected: the heap-side peak within 2,048 B of the baseline's lines (`heap_peak=` of the heap-only run; `heap_peak=` minus 52,272 for the arena run, as `heap_peak_check.py` counts it).
  - Then run `UPDATE_HEAP_BASELINE=1 ctest --test-dir build/test -R HeapPeakRegression`, and read `git diff test/epub_pipeline/heap_peak_baseline.txt`:
    - only the two CJK books may move by more than 512 B;
    - the fixture grows because it has new cases, which the commit message says.
- [ ] **Step 8: Commit.** `test(cjk): the goldens and the rule checker see ruby` (the fixture, its generator, the goldens, the baseline).

### Task 7: Docs, firmware, the X4, PR B1

**Files:**
- Modify: `docs/future_work/cjk.md`:
  - "Ruby (furigana) (PR B)" moves to "Already shipped" (what B1 does, in the house bullet style);
  - the roadmap row 8 says "B1 shipped; B2 emphasis dots next";
  - "Out of scope so far" keeps `<rtc>`, ruby below the text and even line height in ruby paragraphs.
- Modify: `lib/Epub/Epub/blocks/TextBlock.h`, `lib/Epub/Epub/ParsedText.h` comments, if a step left them behind the code.

- [ ] **Step 1: Full host suite.** `cmake --build build/test -j 8 -- -k 0 && ctest --test-dir build/test -j 8`. Expected: all PASS but the merge base's failures recorded in the ledger.
- [ ] **Step 2: Format.** `PATH="<cf21>/clang_format/data/bin:$PATH" bash bin/clang-format-fix`, then `git status`. Expected: no changes left.
- [ ] **Step 3: Firmware.** `pio run -e default` and then `-e gh_release`. Expected: both link; the `sizeof(TextBlock)` assert holds; note the flash delta against Step 4's master build (upstream's #2665 cost 4,136 B).
- [ ] **Step 4: Master's numbers on the X4.** The acceptance numbers compare with master, measured the same way:
  - `git worktree add C:/tmp/wt-ruby-base $(git merge-base HEAD origin/master)`; build its `default` there (same command) and note the firmware size;
  - flash it; with Step 5's font and a cold build (its first three bullets), run Step 5's script up to `shot:b1-soseki-2.png`;
  - note `createSectionFile spine=… done: total=…` for the Soseki chapter and `STATE heap=` on its first page;
  - `git worktree remove C:/tmp/wt-ruby-base`.
- [ ] **Step 5: The X4.**
  - Flash `default`. Put `test_cjk_ja_soseki.epub` and `test_cjk_line_breaking.epub` in `/books`.
  - Open item: which font on the card covers kana. List `cmd:FS:LS:/.crosspoint/fonts`, then choose a Japanese-capable one with `CMD:SET:fontFamily=…`, after `CMD:GET:fontFamily`.
  - Cold builds: from Home (`cmd:ACT:HOME`), remove each book's `/.crosspoint/epub_<h>/spines` with `cmd:FS:RM:`; `<h>` is the `epub_` folder `cmd:FS:LS:/.crosspoint` shows for it (it appears on the book's first open).
  - Run the script (`cmd:LOG:DBG`: the pre-render lines are debug level):
    ```text
    cmd:LOG:DBG
    cmd:ACT:READER:/books/test_cjk_ja_soseki.epub@1:1
    idle:5000:SCREEN
    state
    shot:b1-soseki-1.png
    cmd:BTN:RIGHT
    idle:3000:SCREEN
    shot:b1-soseki-2.png
    cmd:ACT:READER:/books/test_cjk_line_breaking.epub@1:1
    ```
    Then `BTN:RIGHT` until cases 12-15, a screenshot each.
  - Expected:
    - `createSectionFile spine=… done: total=…` within 10 % of master's (Step 4);
    - `STATE heap=` within 2 KB of master's on the same page (Step 4);
    - "Pre-rendered page … lowest" without dropped glyphs on a ruby page, or "PreRender skipped" (the 2-bit path's floor).
  - Claude checks each screenshot:
    - every reading sits above its base, at half size, inside the margins;
    - a short CJK reading over a CJK base is spread 1:2:1 (any other is centred), a long one overhangs only kana or punctuation;
    - no reading over a kanji outside its group, no two readings touching;
    - no （） and no inline readings;
    - only ruby lines are taller.
  - Open the dictionary's word selection on a ruby line: the highlight boxes the base, not the ruby.
  - Open item: the KOSync cross-check. Ask the maintainer whether they can push a KOReader position on the Soseki excerpt, and compare it with ours if so.
  - X3 run: ask the maintainer; not planned.
- [ ] **Step 6: Commit and PR.**
  - Commit `docs(cjk): ruby shipped, emphasis dots next`.
  - Push the branch, open the PR titled `feat(cjk): ruby (furigana) above its base, JLREQ spacing`.
  - The PR body:
    - the screenshots (orphan assets branch, raw URLs);
    - the heap, time and flash numbers against master;
    - the section version, and a renumbering if master moved;
    - the upstream PRs ported, with the Co-authored-by note.

---

# PR B2: emphasis dots

### Task 8: The CSS property `text-emphasis-style`

**Files:**
- Create: `lib/Epub/Epub/TextEmphasis.h`
- Modify: `lib/Epub/Epub/css/CssStyle.h`, `lib/Epub/Epub/css/CssParser.h`, `lib/Epub/Epub/css/CssParser.cpp`
- Modify: `docs/file-formats.md`: the three mentions of `CSS_CACHE_VERSION` (29) become (30).
- Modify: `test/epub_css/CssParserTest.cpp`, `test/epub_css/CssParserCacheTest.cpp`, `test/epub_css/CssCascadeTest.cpp`

**Interfaces:**
- Produces, `TextEmphasis.h` (header-only):
  ```cpp
  namespace TextEmphasis {
  enum Shape : uint8_t { None = 0, Dot = 1, Circle = 2, DoubleCircle = 3, Triangle = 4, Sesame = 5 };
  constexpr uint8_t kOpen = 0x08;  // a mark: Shape | (open ? kOpen : 0); 0 is no mark
  // • ◦ ● ○ ◉ ◎ ▲ △ ﹅ ﹆ (CSS Text Decoration 3); 0 for none.
  constexpr uint32_t markCodepoint(uint8_t mark);
  // Not a space, a control character or punctuation (ASCII punctuation, U+2010-205E, a CjkPunctuation class).
  inline bool takesMark(uint32_t cp);
  }
  ```
- Produces, in `CssStyle`:
  - `uint8_t textEmphasis = 0;` and `CssPropertyFlags::textEmphasis : 1`, in `anySet`, `clearAll`, `reset`;
  - `hasTextEmphasis()`;
  - `applyOver` at rank index 28, `kCascadeProperties = 29`.
- Produces: `static std::optional<uint8_t> CssParser::parseTextEmphasisStyle(std::string_view value)`.
- Storage, rule payload: the smallCaps byte gains bit 2 (defined) and bits 3-6 (the mark). `CSS_FIXED_STYLE_BYTES` stays 77.
- Storage, compressed pool: `extra` bit 4 (`0x10`) says a mark byte follows the font-family hash. `kMaxCompressedStyle` becomes 75.
- `CSS_CACHE_VERSION = 30`.

- [ ] **Step 1: Write the failing tests.**
  ```cpp
  // CssParserTest.cpp
  TEST(CssTextEmphasis, ParsesStyleAndShorthandWithVendorPrefixes)  // parseInlineStyle of:
      // "text-emphasis-style: sesame" -> Sesame; "-epub-text-emphasis-style: filled sesame" -> Sesame;
      // "-webkit-text-emphasis: open dot red" -> Dot|kOpen; "text-emphasis: filled" -> Circle;
      // "text-emphasis-style: none" -> defined, 0.
  TEST(CssTextEmphasis, AStringMarkIsNotSupported)  // "text-emphasis-style: '*'" -> hasTextEmphasis() false.
  // CssParserCacheTest.cpp: CssParserArena.ResidentPreservesAllStyleFields' rule .a gains
  // "-epub-text-emphasis-style: open sesame"; stylesEqual compares textEmphasis and its defined bit.
  // CssCascadeTest.cpp
  TEST(CssCascade, TextEmphasisResolvesAlikeInEveryStore)  // ".em { -epub-text-emphasis-style: filled sesame }
      // p.em { text-emphasis-style: open dot }": resolveEverywhere {"p","em"} -> Dot|kOpen, {"span","em"} -> Sesame.
  ```
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/test --target CssParserTest EpubCssCacheTest EpubCssCascadeTest -j 8`. Expected: compile errors.
- [ ] **Step 3: Implement.**
  - The six property names: `text-emphasis-style`, `text-emphasis` and their `-epub-` and `-webkit-` forms.
  - The shorthand takes its style keywords and ignores a colour.
  - Only `filled`/`open` gives `Circle` (horizontal text).
  - `text-emphasis-position` is not read: marks go above (decision 2).
- [ ] **Step 4: Run them.** `./build/test/epub_css/CssParserTest.exe`, `./build/test/epub_css/EpubCssCacheTest.exe`, `./build/test/epub_css/EpubCssCascadeTest.exe`; then `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe`. Expected: all PASS, goldens unchanged.
- [ ] **Step 5: Commit.** `feat(css): text-emphasis-style, with the -epub- and -webkit- forms`

### Task 9: Emphasis marks from the parser to the page; section version 100

**Files:**
- Modify: `lib/Epub/Epub/TokenBoundary.h`, `lib/Epub/Epub/ParsedText.h`, `lib/Epub/Epub/ParsedText.cpp`
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`, `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- Modify: `lib/Epub/Epub/Section.cpp`
- Modify: `test/text_block_range/GfxRenderer.h`: add `hasGlyph`, as the other shims have it. `test/text_block_range/CMakeLists.txt`: add `${REPO_ROOT}/lib/Epub/Epub/CjkPunctuation.cpp`, which `TextEmphasis::takesMark` calls.
- Modify: `test/epub_pipeline/PipelineRunner.cpp`, `test/epubs/make_test_cjk_line_breaking.py`, the regenerated `.epub`, its golden
- Create: `test/cjk_layout/CjkEmphasisTest.cpp` (registered after `CjkRubyTest.cpp`)
- Modify: `test/epub_pipeline/RubyLayoutTest.cpp`

**Interfaces:**
- Consumes: `TextEmphasis` (Task 8); the annotation region and band (Task 1).
- Produces: `constexpr uint8_t emphasisOf(Boundary)`, `constexpr Boundary withEmphasis(Boundary, uint8_t mark)` (bits 2-5).
- Produces: `ParsedText::addWord(..., uint8_t fontSlot = 0, uint8_t textEmphasis = 0)`: every token of the word carries the mark.
- Produces, parser:
  - `StyleStackEntry` gains `bool hasTextEmphasis; uint8_t textEmphasis;`, and the parser `uint8_t effectiveTextEmphasis_`;
  - `applyCssEmphasisToEntry` sets them;
  - `pushBlockStyleEntry` and the span path also push for `hasTextEmphasis()`;
  - the bold and italic paths call `applyCssEmphasisToEntry` for it.
- Produces, the dump record `E first=<f> count=<c> m=<mark>`.

- [ ] **Step 1: Write the failing tests** (`CjkEmphasisTest.cpp`, the word_size_layout shim):
  ```cpp
  TEST(CjkEmphasis, EachCharacterGetsAMarkCentredAboveIt)  // addWord("強調", ..., Sesame): ﹅ drawn at scale 0.5,
      // x {2, 12}, y 0; the base at y 8; the line's record {0, 2, Emphasis, mark Sesame}.
  TEST(CjkEmphasis, PunctuationAndSpacesTakeNoMark)  // "強調。" and "強　調": two marks each.
  TEST(CjkEmphasis, ASpanKeepsItsMarksAcrossALineBreak)  // "一二三四五" with Dot at 30 px: two lines, each with one
      // record over all its words, each with annotationBand 8.
  TEST(CjkEmphasis, RubyWinsOverEmphasisOnTheSameBase)  // "強調" with Dot, addRubyGroup(0, 1, "きょう"):
      // records {Ruby over 強}, {Emphasis over 調}.
  TEST(CjkEmphasis, AFaceWithoutTheMarkDrawsADot)  // LATIN_FONT and Sesame: no ﹅ draw call, one fillRect a
      // character (side max(2, 8 / 4)).
  TEST(CjkEmphasis, ABionicHalfKeepsItsMark)  // bionic on, "emphasis" with Dot: "emph" and "asis" in one run.
  // RubyLayoutTest.cpp
  TEST_F(RubyLayout, ACssClassMarksItsSpan)  // CSS ".bouten { -epub-text-emphasis-style: sesame }",
      // <p>前<span class="bouten">強調</span>後</p>: one Emphasis record over 強,調.
  ```
- [ ] **Step 2: Run them to see them fail.** `cmake --build build/test --target CjkLayoutTest EpubPipelineTest -j 8`. Expected: compile errors (`addWord`'s new argument, `withEmphasis`).
- [ ] **Step 3: Implement.**
  - **The mark per token.** `hyphenateWordAtIndex`'s remainder keeps the mark bits (it never has a ruby role: Task 2 does not split a group token). A bionic later half keeps the mark bits and takes Task 2's `Follower` role when the original had a role.
  - **Runs.** `extractLine` adds a record per run of consecutive tokens with one mark, outside ruby groups, in `firstWord` order with the ruby records.
  - **Drawing.**
    - Each `takesMark` codepoint of a run's tokens gets `markCodepoint(mark)`, by `drawTextScaled` at `0.5f * wordScale`, centred on the codepoint's advance.
    - It sits at `y + band - getFontAscenderSizeScaled(font, 0.5f * wordScale)`, in the scanning pass too.
    - A face without the mark (`hasGlyph` false) gets a filled square of side `max(2, rubyAscender / 4)` centred at the same spot.
  - **Prewarm.** `layoutAndExtractLines` appends the block's mark glyphs to the prewarm text.
  - **Version.** `SECTION_FILE_VERSION = 100` (see the Global Constraints for renumbering), its comment line first: "v100: CSS text-emphasis marks (傍点) are drawn above each character of their span".
  - **The fixture's case 16:**
    - spans in sesame, dot, open circle, by class with `-epub-` and `-webkit-`, and by `style=`;
    - a span that wraps;
    - a span over 。 and 「」.
- [ ] **Step 4: Run them.**
  - Run `./build/test/cjk_layout/CjkLayoutTest.exe` and `./build/test/epub_pipeline/EpubPipelineTest.exe`.
  - Then regenerate with `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline` and read the diff. Expected: only `test_cjk_line_breaking.golden.txt` changes, by `E` records and the taller lines of case 16 (Soseki has no `text-emphasis`).
  - Then `ctest --test-dir build/test -R HeapPeakRegression`. Expected: PASS without a new baseline.
- [ ] **Step 5: Commit.** `feat(cjk): emphasis dots (傍点) above each character of a span`

### Task 10: Docs, firmware, the X4, PR B2

**Files:**
- Modify: `docs/future_work/cjk.md`: B2 to "Already shipped"; `text-emphasis-position` (Chinese marks below the text) to "Out of scope so far".

- [ ] **Step 1: Host suite, format, firmware,** as Task 7 Steps 1-3. Expected: the same.
- [ ] **Step 2: The X4.** As Task 7 Step 5, on the fixture's case 16, screenshots `b2-emphasis-*.png`. Claude checks:
  - one mark above each character of every span, centred, at half size;
  - none over punctuation or spaces;
  - a wrapped span marked on both lines;
  - the marks inside the band, not touching the line above.

  X3 run: ask the maintainer; not planned.
- [ ] **Step 3: Commit and PR.**
  - Commit `docs(cjk): emphasis dots shipped`.
  - Open the PR titled `feat(cjk): emphasis dots (text-emphasis)`, with the screenshots, the CSS cache bump and the section version (and a renumbering if master moved) in the body.
