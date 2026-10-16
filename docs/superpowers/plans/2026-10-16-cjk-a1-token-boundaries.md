# CJK A1: one boundary per token. Implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `ParsedText` stores how each token joins the one before it as one `TokenBoundary::Boundary` (a space,
glued, or a breakable zero-width CJK gap), and every layout site reads it through one predicate. Nothing sets the
CJK gap yet, and every page lays out exactly as today. This is the first of three PRs that make up the spec's PR A;
A2 (CJK breaking) and A3 (justification, squeeze and grid) build on it.

**Architecture:**

- **Today** `ParsedText` keeps `std::vector<bool> wordContinues` beside four other per-word lists (`words`,
  `wordStyles`, `wordSizes`, `wordVisibleOffsets`). A token either has a space before it or is glued to the one
  before.
- **Upstream** (crosspoint-reader) adds a second bit vector, `wordNoSpaceBefore`, and a header of predicates over
  the two bits, `TokenBoundary.h`. Every site that copies, inserts or erases word lists has to move both vectors.
- **This plan** ports the header (Task 2) and replaces our `wordContinues` with
  `std::vector<TokenBoundary::Boundary> wordBoundaries`, a two-bool struct (Task 3). The two halves of a boundary
  cannot drift apart, and the compiler finds every site that used the old vector. Each site then asks one
  question:
  - **the natural gap** before a token: `naturalGapBefore()` (kerning if glued, nothing at a CJK gap, else the
    space advance). The four breakers and the text-indent pre-pass all use it;
  - **whether a line may break** before it: `TokenBoundary::allowsBreak()`;
  - **whether a space is drawn or measured** there: `TokenBoundary::hasSpace()`;
  - **whether it is glued** (justification of non-breaking spaces, the line's continuation bit): `.continues`.
- **`TextBlock`** keeps only the `continues` bit, as today: a CJK gap is a break, so the dictionary must not join
  across it. `WordRange::continues` (`std::vector<bool>*`) becomes `WordRange::boundaries`.
- **First, the paths the goldens do not reach are pinned (Task 1).** The golden dumps run with hyphenation and
  bionic reading off, so they cover one line breaker of four. Two new golden variants turn each on: hyphenation
  reaches the two greedy breakers and the page-bottom retry, bionic reading the word-list rebuild. They are
  generated before any layout code changes, and Task 3 must leave them byte-identical.

**Tech Stack:** C++20 host tests (gtest via CMake, UCRT64 gcc); firmware gnu++2a (PlatformIO, ESP32-C3/S3);
clang-format 21.1.8.

**Spec:** `docs/superpowers/specs/2026-10-16-cjk-layout-design.md` (branch `docs/cjk-layout-design`): section 5.2
(token boundaries), section 4 (delivery) and risks 1 and 2 of section 10. This plan departs from 5.2 and 9.1 in the
points recorded under "Decisions" below; the spec records them too.

**Pre-validated:** every task's code was built and tested in a prototype (`C:/tmp/wt-cjk-proto`); the prototype's
`_hyph` and `_bionic` goldens were generated from master's code in a separate tree, and the refactored code matches
them. The plan was then replayed task by task on a fresh tree at `origin/master` (`36de9968b`):
- each "see it fail" step fails as stated;
- each count matches. Task 3's range test needed a rebuild first: the replay tool wrote `TextBlock.cpp` in the same
  second as the failed build, and make kept the stale object (see "A file changed right after a failed build");
- every file the plan writes is identical to the prototype, including the goldens the replay generated itself;
- the whole host suite passes (2,311 tests).

The firmware was built for both boards from the prototype (`default`: RAM 16.7%, flash 96.5%; `x4pro`: RAM 30.8%,
flash 94.7%), with no warning from the files the plan touches.

## Global Constraints

- **Branch:** `feat/cjk-token-boundaries` from `origin/master` (`36de9968b`), in the worktree `/c/tmp/wt-cjk-a1`.
  The PR targets `master`, titled `refactor: (Epub) one boundary per token, ready for CJK gaps`.
- **No behaviour change:** every golden, old and new, stays byte-identical; `SECTION_FILE_VERSION` stays 83. The
  `_hyph` and `_bionic` goldens are generated once, in Task 1, from master's layout code, and never regenerated in
  this plan. A golden that changes in Task 3 is a bug in Task 3.
- **Nothing sets a CJK gap in this plan:** `addWord` pushes `Boundary{attachToPrevious, false}`. The code that reads
  `noSpaceBefore` is written now so that A2 changes no site, and A2's tests exercise it.
- **Attribution (the porting convention):** Task 2's header predicates and 7 of its tests are upstream's, verbatim:
  crosspoint-reader PR #2892 ("fix: hyphenate focus-split words and break after visible hyphens", Uri Tauber),
  commit `25cf99b2`. The files say so, and Task 2's commit carries `Co-authored-by: Uri Tauber <uritaube@gmail.com>`.
  Tasks 1 and 3 are ours.
- **Heap:** a `Boundary` is 2 bytes, against 1 bit in `std::vector<bool>`; `addWord`'s heap gate charges it.
  `TABLE_BUFFER_BYTES_PER_WORD` stays 48 (the real per-word cost goes from about 30 to 32 bytes, still under it), so
  table buffering decisions and their goldens do not change.
- **App host test commands** (Git Bash, from the worktree root): `export PATH=/c/msys64/ucrt64/bin:$PATH` once, then
  configure with
  `cmake -S test -B build/test -G "MinGW Makefiles" -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/c/_development/witchhunt-reader/build/test/_deps/googletest-src`.
  The steps below write `cmake -S test -B build/test` for short. The full suite needs
  `cmake --build build/test -j 8 -- -k` to get past `epub_build_inventory`, which does not build on Windows
  (`dlfcn.h: No such file or directory` is expected).
- **Firmware builds:** `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then
  `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`; one env at a time.
- **A file changed right after a failed build:** mingw make compares timestamps in whole seconds, and CMake's
  Makefile generator can miss a header change for objects the failed build compiled. A test binary then links a
  stale object: in the replay, Task 3's range tests read the new boundary list through the old `TextBlock.cpp`
  and 5 of them failed. If a test fails or a binary misbehaves right after a "see it fail" build, rebuild its
  target with `--clean-first` before debugging.
- **Formatting:** clang-format 21 (`/c/tmp/cf21/bin/clang-format-21.exe`) with the repo's `.clang-format`. The code
  below is formatted.
- **Commits:** semantic prefixes; no `Co-Authored-By` trailer for the assistant.

## Review Focus

The five situations most likely to bite that the old tests did not reach, most likely first. Each is now pinned by
a test in the task named:

1. **Hyphenation on.** The breaker switches to the two greedy hyphenating breakers, each with its own gap cache and
   backtrack over glued tokens. Expected: the same lines as today. Pinned by the `_hyph` goldens (Task 1).
2. **A hyphenated line at the bottom of a page.** The page-bottom retry re-lays that line without hyphenation
   (`computeSingleLineBreakNoHyphen`) and the rest with it (`computeHyphenatedLineBreaksFromIndex`). Expected: the
   same pages. Pinned by the `_hyph` goldens, whose books hit the retry 11 times per build (Task 1).
3. **Bionic reading on, in a paragraph laid out before it ends.** The rebuild glues each bold prefix to its rest,
   and resumes where it stopped when a paragraph over 96 words is laid out in pieces. Expected: the same halves,
   the second glued. Pinned by the `_bionic` goldens (Task 1) and `BionicHalvesAreMarkedAsOneWord`.
4. **A non-breaking space in a justified line.** It is a glued `" "` token that still stretches like a word gap;
   Task 3 rewrites the branch that counts it. No corpus book has one. Expected: it stretches by the same amount
   as a word gap. Pinned by `JustifiedNonBreakingSpaceStretchesLikeAWordGap` (Task 1).
5. **A CJK gap reaching a line.** A2 will produce `Boundary{false, true}`. Expected: `TextBlock` does not mark it as
   a continuation, so the dictionary selects one character run, not the whole line. Pinned by
   `ACjkGapIsNotAContinuation` (Task 3).

## File Structure

| File | Task | Responsibility |
| --- | --- | --- |
| `test/epub_pipeline/EpubPipelineTest.cpp`, `goldens/*_hyph.golden.txt` (3), `*_bionic.golden.txt` (2) | 1 | the hyphenation and bionic golden variants |
| `test/word_size_layout/WordSizeLayoutTest.cpp` | 1 | a non-breaking space in a justified line |
| `lib/Epub/Epub/TokenBoundary.h` (new) | 2 | the boundary kinds and their predicates |
| `test/token_boundary/TokenBoundaryTest.cpp`, `CMakeLists.txt` (new), `test/CMakeLists.txt` | 2 | the predicates' tests |
| `lib/Epub/Epub/ParsedText.{h,cpp}` | 3 | `wordBoundaries`; every site reads a predicate |
| `lib/Epub/Epub/blocks/TextBlock.{h,cpp}` | 3 | `WordRange::boundaries`; keeps the `continues` bit |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | 3 | a comment names the new list |
| `test/text_block_range/TextBlockRangeTest.cpp` | 3 | the range tests on boundaries; the CJK gap |

## Decisions this plan makes (the spec records them)

- **One struct per token, not a second bit vector.** Spec 5.2 threads a second flag beside `wordContinues` and
  asks for a debug check that the per-word lists keep equal lengths. With one `Boundary` per token there is no new
  list to fall out of step: the per-word lists are the same five as today, and the compiler found every site that
  used the old one. The length check is therefore not added. Cost: 2 bytes per token instead of 2 bits, about
  220 bytes at the word lists' 128-entry steady state.
- **`TokenBoundary.h` ports verbatim,** including `allowsBreakAfterExplicitHyphen`, the dash state's predicate that
  spec section 3 leaves out (upstream #3883 is not taken). Nothing calls it; it keeps later upstream diffs clean.
  `Boundary{true, true}`, the dash state, is never constructed.
- **The page dump does not print the boundary kind** (spec 9.1 says it should). `dumpPage` already prints each
  word's x position, so a zero-width CJK gap shows as adjacent positions; `TextBlock` does not store the kind after
  layout. PR B extends the dump for ruby groups.
- **Golden variants with hyphenation on (`_hyph`, three books) and bionic reading on (`_bionic`, two books):** the
  default goldens run with both off and reach one breaker of four (risks 1 and 2 of spec section 10). They stay
  for A2 and A3, which change the breakers again. Cost: 659 KB of goldens (the existing ones are 1.6 MB) and
  about 15 s of test time.

---

### Task 1: Pin the layout paths the goldens miss

The golden dumps run with hyphenation and bionic reading off. Task 3 rewrites code in the two greedy hyphenating
breakers, the page-bottom retry and the bionic rebuild, none of which those goldens reach. This task records
today's layout on those paths before any layout code changes, so that Task 3 has something to stay identical to.

**Files:**
- Modify: `test/epub_pipeline/EpubPipelineTest.cpp`
- Create (generated, not written by hand): `test/epub_pipeline/goldens/test_anchor_pagebreak_hyph.golden.txt`,
  `test_kerning_ligature_hyph.golden.txt`, `test_wrapped_paragraphs_hyph.golden.txt`,
  `test_long_paragraph_spacing_bionic.golden.txt`, `test_text_rendering_bionic.golden.txt`
- Modify: `test/word_size_layout/WordSizeLayoutTest.cpp`

**Interfaces:**
- Consumes: `pipeline_harness::Profile::hyphenationEnabled` and `::bionicReadingEnabled`
  (`test/epub_pipeline/PipelineRunner.h`, both exist and default to false).
- Produces: five goldens that Task 3 must leave byte-identical, checked by the cases
  `SyntheticCorpus/EpubPipelineTest.MatchesGolden/<book>_hyph` and `.../<book>_bionic`; and the test
  `WordContinuation.JustifiedNonBreakingSpaceStretchesLikeAWordGap`.

- [ ] **Step 1: The worktree**

```bash
git -C /c/_development/witchhunt-reader fetch origin
git -C /c/_development/witchhunt-reader worktree add -b feat/cjk-token-boundaries /c/tmp/wt-cjk-a1 origin/master
cd /c/tmp/wt-cjk-a1
git submodule update --init freeink-sdk
git log --oneline -1   # 36de9968b Merge pull request #480 from jpirnay/feat/serial-debug-commands
```

All paths below are relative to `/c/tmp/wt-cjk-a1`.

- [ ] **Step 2: Add the hyphenation and bionic variants**

A case now names the one render setting it turns on, as an enum, instead of a bool for normalization. The two new
variants use their own book lists and golden suffixes; the default and `_norm` cases keep their names and goldens.
`MatchesGolden`'s check that a variant still changes its book's layout now covers every variant.

The books were chosen by measurement on master. With hyphenation on, test_anchor_pagebreak ends 241 lines with a
hyphen and hits the page-bottom retry 7 times per build, test_kerning_ligature 110 and 4;
test_wrapped_paragraphs adds paragraphs that cross pages. For bionic reading, test_long_paragraph_spacing has seven
paragraphs over 96 words, which the parser lays out before they end; test_text_rendering mixes styles. One variant
with both settings on would hide hyphenation: bionic reading splits each word into short glued halves, which rarely
hyphenate.

<!-- plan-edit: test/epub_pipeline/EpubPipelineTest.cpp -->
```cpp
// with it ON (the ±10% band that snaps publisher near-body
// <span font-size:0.92em> wrappers back to native size), against a golden of
// their own: <book>.golden.txt for OFF, <book>_norm.golden.txt for ON.
```
with:

```cpp
// with it ON (the ±10% band that snaps publisher near-body
// <span font-size:0.92em> wrappers back to native size), against a golden of
// their own: <book>.golden.txt for OFF, <book>_norm.golden.txt for ON.
//
// The default goldens run with hyphenation and bionic reading off, so they reach one
// line breaker of four. The prose books therefore also run with hyphenation ON
// (<book>_hyph.golden.txt: the two greedy breakers and the page-bottom retry), and
// a few with bionic reading ON (<book>_bionic.golden.txt: the word-list rebuild).
```
<!-- plan-edit: test/epub_pipeline/EpubPipelineTest.cpp -->
```cpp
// One corpus book under one font-size-normalization setting.
struct Case {
  std::string epub;
  bool fontSizeNormalization = false;
};

// Without this gtest dumps the struct as raw bytes in failure messages.
void PrintTo(const Case& c, std::ostream* os) {
  *os << fs::path(c.epub).filename().string() << " [fontSizeNormalization=" << (c.fontSizeNormalization ? "on" : "off")
      << "]";
}
```
with:

```cpp
// The render setting a case turns ON; every other one stays as in the default.
enum class Variant { Default, Normalization, Hyphenation, Bionic };

// One corpus book under one variant.
struct Case {
  std::string epub;
  Variant variant = Variant::Default;
};

const char* variantName(const Variant v) {
  switch (v) {
    case Variant::Normalization:
      return "fontSizeNormalization";
    case Variant::Hyphenation:
      return "hyphenation";
    case Variant::Bionic:
      return "bionicReading";
    case Variant::Default:
      break;
  }
  return "default";
}

// Without this gtest dumps the struct as raw bytes in failure messages.
void PrintTo(const Case& c, std::ostream* os) {
  *os << fs::path(c.epub).filename().string() << " [" << variantName(c.variant) << "]";
}
```
<!-- plan-edit: test/epub_pipeline/EpubPipelineTest.cpp -->
```cpp
const char* const kNormalizationSensitiveBooks[] = {"test_font_normalization", "test_font_sizes"};

// Distinguishes the two variants' cache dirs and golden files. OFF keeps the
// historical unsuffixed golden name so its committed layout stays reviewable
// across the normalization change.
std::string variantSuffix(const Case& c) { return c.fontSizeNormalization ? "_norm" : ""; }

std::string runOnce(const Case& c, const std::string& cacheDir) {
  pipeline_harness::Profile profile;
  profile.fontSizeNormalization = c.fontSizeNormalization;
  std::ostringstream dump;
  const bool ok = pipeline_harness::runAndDump(c.epub, cacheDir, profile, dump);
  EXPECT_TRUE(ok) << "pipeline failed for " << c.epub
                  << " (fontSizeNormalization=" << (c.fontSizeNormalization ? "on" : "off") << ")\n"
                  << dump.str();
  return dump.str();
}
```
with:

```cpp
const char* const kNormalizationSensitiveBooks[] = {"test_font_normalization", "test_font_sizes"};

// Books with enough running text that hyphenation splits words, and a hyphenated line lands at
// the bottom of a page (the retry).
const char* const kHyphenationBooks[] = {"test_anchor_pagebreak", "test_kerning_ligature", "test_wrapped_paragraphs"};

// Bionic reading bolds a prefix of every word. Paragraphs over 96 words (test_long_paragraph_spacing
// has seven) are laid out before they end, and the rebuild resumes where it stopped.
const char* const kBionicBooks[] = {"test_long_paragraph_spacing", "test_text_rendering"};

// Distinguishes the variants' cache dirs and golden files. The default keeps the
// historical unsuffixed golden name so its committed layout stays reviewable
// across the normalization change.
std::string variantSuffix(const Case& c) {
  switch (c.variant) {
    case Variant::Normalization:
      return "_norm";
    case Variant::Hyphenation:
      return "_hyph";
    case Variant::Bionic:
      return "_bionic";
    case Variant::Default:
      break;
  }
  return "";
}

std::string runOnce(const Case& c, const std::string& cacheDir) {
  pipeline_harness::Profile profile;
  profile.fontSizeNormalization = c.variant == Variant::Normalization;
  profile.hyphenationEnabled = c.variant == Variant::Hyphenation;
  profile.bionicReadingEnabled = c.variant == Variant::Bionic;
  std::ostringstream dump;
  const bool ok = pipeline_harness::runAndDump(c.epub, cacheDir, profile, dump);
  EXPECT_TRUE(ok) << "pipeline failed for " << c.epub << " (" << variantName(c.variant) << ")\n" << dump.str();
  return dump.str();
}
```
<!-- plan-edit: test/epub_pipeline/EpubPipelineTest.cpp -->
```cpp
  EXPECT_EQ(golden, dump) << "layout drift vs golden for " << c.epub
                          << " (fontSizeNormalization=" << (c.fontSizeNormalization ? "on" : "off")
                          << ") — if intentional, regenerate with UPDATE_GOLDENS=1 and explain in the commit";

  // The ON case is only worth its golden while normalization still changes this book's layout.
  if (c.fontSizeNormalization) {
    std::string offGolden;
    ASSERT_TRUE(readFile(fs::path(GOLDEN_DIR) / (stem(c.epub) + ".golden.txt"), offGolden));
    EXPECT_NE(offGolden, dump) << c.epub << " no longer lays out differently with normalization ON; "
                               << "drop it from kNormalizationSensitiveBooks and delete its _norm golden";
  }
}
```
with:

```cpp
  EXPECT_EQ(golden, dump) << "layout drift vs golden for " << c.epub << " (" << variantName(c.variant)
                          << ") — if intentional, regenerate with UPDATE_GOLDENS=1 and explain in the commit";

  // A variant is only worth its golden while it still changes this book's layout.
  if (c.variant != Variant::Default) {
    std::string defaultGolden;
    ASSERT_TRUE(readFile(fs::path(GOLDEN_DIR) / (stem(c.epub) + ".golden.txt"), defaultGolden));
    EXPECT_NE(defaultGolden, dump) << c.epub << " no longer lays out differently with " << variantName(c.variant)
                                   << " ON; drop it from its book list and delete its " << variantSuffix(c)
                                   << " golden";
  }
}
```
<!-- plan-edit: test/epub_pipeline/EpubPipelineTest.cpp -->
```cpp
// Every corpus book with normalization OFF, plus ON for the books it changes.
std::vector<Case> corpusCases() {
```
with:

```cpp
template <size_t N>
bool listed(const char* const (&books)[N], const std::string& name) {
  return std::find(std::begin(books), std::end(books), name) != std::end(books);
}

// Every corpus book in the default variant, plus the other variants for the books they cover.
std::vector<Case> corpusCases() {
```
<!-- plan-edit: test/epub_pipeline/EpubPipelineTest.cpp -->
```cpp
    cases.push_back(Case{path, /*fontSizeNormalization=*/false});
    const auto* const sensitive =
        std::find(std::begin(kNormalizationSensitiveBooks), std::end(kNormalizationSensitiveBooks), stem(path));
    if (sensitive != std::end(kNormalizationSensitiveBooks)) {
      cases.push_back(Case{path, /*fontSizeNormalization=*/true});
    }
```
with:

```cpp
    cases.push_back(Case{path});
    if (listed(kNormalizationSensitiveBooks, stem(path))) cases.push_back(Case{path, Variant::Normalization});
    if (listed(kHyphenationBooks, stem(path))) cases.push_back(Case{path, Variant::Hyphenation});
    if (listed(kBionicBooks, stem(path))) cases.push_back(Case{path, Variant::Bionic});
```

- [ ] **Step 3: Run the new cases to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='*_hyph*:*_bionic*' --gtest_brief=1`
Expected: the five warm-cache cases pass (`[  PASSED  ] 5 tests.`); the five `MatchesGolden` cases fail with
`missing golden`, among them `SyntheticCorpus/EpubPipelineTest.MatchesGolden/test_anchor_pagebreak_hyph`.

- [ ] **Step 4: Generate the goldens from master's layout code**

Run: `UPDATE_GOLDENS=1 ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='*_hyph*:*_bionic*' --gtest_brief=1`
Expected: the five `MatchesGolden` cases skip (`golden regenerated`), and the five files listed above appear in
`test/epub_pipeline/goldens/`.

- [ ] **Step 5: Check what they pin**

```bash
cd test/epub_pipeline/goldens
for f in *_hyph.golden.txt; do echo "$f $(grep -cE 't=.*[[:alpha:]]-$' "$f")"; done
for b in test_long_paragraph_spacing test_text_rendering; do
  echo "$b bold words: $(grep -c ' s=1 ' $b.golden.txt) -> $(grep -c ' s=1 ' ${b}_bionic.golden.txt)"
done
cd -
```

Expected: hyphenated line ends 241 (test_anchor_pagebreak), 110 (test_kerning_ligature) and 20
(test_wrapped_paragraphs); bold words 0 -> 486 (test_long_paragraph_spacing) and 145 -> 648 (test_text_rendering). If a `_hyph` golden has no hyphenated line, or
a `_bionic` one no more bold words than its default golden, the variant is not reaching its code: stop and report.

- [ ] **Step 6: Run the new cases to see them pass**

Run: `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='*_hyph*:*_bionic*' --gtest_brief=1`
Expected: `[  PASSED  ] 10 tests.`

- [ ] **Step 7: A non-breaking space in a justified line**

The parser turns U+00A0 into a `" "` token glued to the words on both sides. Justification stretches it like a word
gap. Task 3 rewrites the branch of `extractLine` that counts it, and no corpus book contains one. This is a
characterization test: it pins what master does and passes at once. If it fails here, master does not behave as
described: stop and report.

<!-- plan-edit: test/word_size_layout/WordSizeLayoutTest.cpp -->
```cpp
// Hyphenation splits the word ACROSS lines, so the halves land in different blocks and no flag
```
with:

```cpp
// A non-breaking space reaches layout as a " " token glued to the words on both sides. In a
// justified line it stretches like a word gap; the glue around it does not. No corpus book has one,
// so the goldens do not pin this.
TEST(WordContinuation, JustifiedNonBreakingSpaceStretchesLikeAWordGap) {
  GfxRenderer renderer;
  BlockStyle style = noIndentStyle();
  style.alignment = CssTextAlign::Justify;
  ParsedText text(false, false, style);
  text.addWord("aaaa", EpdFontFamily::REGULAR);
  text.addWord("bbbb", EpdFontFamily::REGULAR);
  text.addWord(" ", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/true);
  text.addWord("cccc", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/true);
  text.addWord("dddd", EpdFontFamily::REGULAR);
  text.addWord("eeee", EpdFontFamily::REGULAR);

  // aaaa bbbb_cccc dddd is 175 px wide (the space token measures as a space); eeee would need 45
  // more, so the first line is justified.
  const auto result = layout(text, renderer, 200);
  ASSERT_EQ(result.lines.size(), 2u);
  const TextBlock& line = *result.lines[0];
  ASSERT_EQ(line.wordCount(), 5);
  constexpr int kWord = 4 * GfxRenderer::GLYPH_W;
  const int wordGapStretch = line.wordXpos(1) - line.wordXpos(0) - kWord - GfxRenderer::SPACE_W;
  EXPECT_GT(wordGapStretch, 0);
  EXPECT_EQ(line.wordXpos(2) - line.wordXpos(1), kWord);  // bbbb is glued to the space
  EXPECT_EQ(line.wordXpos(3) - line.wordXpos(2) - GfxRenderer::SPACE_W, wordGapStretch);
  EXPECT_EQ(line.wordXpos(4) - line.wordXpos(3) - kWord - GfxRenderer::SPACE_W, wordGapStretch);
}

// Hyphenation splits the word ACROSS lines, so the halves land in different blocks and no flag
```

- [ ] **Step 8: Run the layout tests**

Run: `cmake --build build/test --target WordSizeLayoutTest -j 8 && ./build/test/word_size_layout/WordSizeLayoutTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 36 tests.` (35 before).

- [ ] **Step 9: Commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror test/epub_pipeline/EpubPipelineTest.cpp test/word_size_layout/WordSizeLayoutTest.cpp
git add test/epub_pipeline/EpubPipelineTest.cpp test/epub_pipeline/goldens/*_hyph.golden.txt \
  test/epub_pipeline/goldens/*_bionic.golden.txt test/word_size_layout/WordSizeLayoutTest.cpp
git commit -m "test: pin hyphenated and bionic layout, and a justified non-breaking space" \
  -m "The goldens ran with hyphenation and bionic reading off, so they reached one line breaker of four. Three prose books now also run with hyphenation on (the greedy breakers and the page-bottom retry), and two with bionic reading on (the word-list rebuild). The new goldens are generated from master's layout code, before the token-boundary refactor that must keep them identical."
```

### Task 2: The boundary kinds, ported from upstream

**Files:**
- Create: `lib/Epub/Epub/TokenBoundary.h`
- Create: `test/token_boundary/TokenBoundaryTest.cpp`, `test/token_boundary/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `isExplicitHyphen(uint32_t)` and `isSoftHyphen(uint32_t)` from
  `lib/Epub/Epub/hyphenation/HyphenationCommon.h` (exist).
- Produces, in namespace `TokenBoundary` (Task 3 uses the last three):
  - `constexpr bool allowsBreak(bool continues, bool noSpaceBefore)`;
  - `constexpr bool isJustifiableGap(bool continues, bool noSpaceBefore, bool isSpaceToken)` (A3 uses it);
  - `inline bool allowsBreakAfterExplicitHyphen(uint32_t cp)` (nothing uses it; see Decisions);
  - `struct Boundary { bool continues = false; bool noSpaceBefore = false; };`
  - `constexpr bool allowsBreak(Boundary)`;
  - `constexpr bool hasSpace(Boundary)`: true only for an ordinary word gap.

- [ ] **Step 1: Write the failing tests**

Upstream's seven tests, verbatim, and two of ours for `Boundary`:

<!-- plan-file: test/token_boundary/TokenBoundaryTest.cpp -->
```cpp
// Ported from crosspoint-reader PR #2892 (Uri Tauber), commit 25cf99b2, verbatim; the last two tests are ours.
#include <gtest/gtest.h>

#include <iomanip>

#include "lib/Epub/Epub/TokenBoundary.h"

TEST(TokenBoundary, OrdinaryWordGapIsBreakableAndJustifiable) {
  EXPECT_TRUE(TokenBoundary::allowsBreak(/*continues=*/false, /*noSpaceBefore=*/false));
  EXPECT_TRUE(TokenBoundary::isJustifiableGap(/*continues=*/false, /*noSpaceBefore=*/false,
                                              /*isSpaceToken=*/false));
}

TEST(TokenBoundary, CjkZeroWidthGapIsBreakableAndJustifiable) {
  EXPECT_TRUE(TokenBoundary::allowsBreak(/*continues=*/false, /*noSpaceBefore=*/true));
  EXPECT_TRUE(TokenBoundary::isJustifiableGap(/*continues=*/false, /*noSpaceBefore=*/true,
                                              /*isSpaceToken=*/false));
}

TEST(TokenBoundary, AttachedTextIsNeitherBreakableNorJustifiable) {
  EXPECT_FALSE(TokenBoundary::allowsBreak(/*continues=*/true, /*noSpaceBefore=*/false));
  EXPECT_FALSE(TokenBoundary::isJustifiableGap(/*continues=*/true, /*noSpaceBefore=*/false,
                                               /*isSpaceToken=*/false));
}

TEST(TokenBoundary, ExplicitHyphenBoundaryIsBreakableButNotJustifiable) {
  EXPECT_TRUE(TokenBoundary::allowsBreak(/*continues=*/true, /*noSpaceBefore=*/true));
  EXPECT_FALSE(TokenBoundary::isJustifiableGap(/*continues=*/true, /*noSpaceBefore=*/true,
                                               /*isSpaceToken=*/false));
}

TEST(TokenBoundary, LiteralAttachedSpaceRemainsJustifiable) {
  EXPECT_TRUE(TokenBoundary::isJustifiableGap(/*continues=*/true, /*noSpaceBefore=*/false,
                                              /*isSpaceToken=*/true));
}

TEST(TokenBoundary, VisibleExplicitHyphensAllowBreaks) {
  constexpr uint32_t breakableCodepoints[] = {
      '-',    0x058A, 0x2010, 0x2012, 0x2013, 0x2014, 0x2015, 0x2043, 0x207B, 0x208B,
      0x2212, 0x2E17, 0x2E3A, 0x2E3B, 0xFE58, 0xFE63, 0xFF0D, 0x005F, 0x2026,
  };
  for (const uint32_t cp : breakableCodepoints) {
    EXPECT_TRUE(TokenBoundary::allowsBreakAfterExplicitHyphen(cp)) << "codepoint U+" << std::hex << cp;
  }
}

TEST(TokenBoundary, ConditionalAndNonBreakingHyphensKeepTheirSemantics) {
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen(0x00AD));  // soft hyphen
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen(0x2011));  // non-breaking hyphen
  EXPECT_FALSE(TokenBoundary::allowsBreakAfterExplicitHyphen('.'));
}

// Ours: the per-token Boundary ParsedText stores, and the predicates over it.
TEST(TokenBoundary, ADefaultBoundaryIsAnOrdinarySpace) {
  constexpr TokenBoundary::Boundary b;
  EXPECT_TRUE(TokenBoundary::hasSpace(b));
  EXPECT_TRUE(TokenBoundary::allowsBreak(b));
}

TEST(TokenBoundary, BoundaryKindsMatchTheTwoFlagPredicates) {
  constexpr TokenBoundary::Boundary glued{/*continues=*/true, /*noSpaceBefore=*/false};
  constexpr TokenBoundary::Boundary cjkGap{/*continues=*/false, /*noSpaceBefore=*/true};
  EXPECT_FALSE(TokenBoundary::hasSpace(glued));
  EXPECT_FALSE(TokenBoundary::allowsBreak(glued));
  EXPECT_FALSE(TokenBoundary::hasSpace(cjkGap));
  EXPECT_TRUE(TokenBoundary::allowsBreak(cjkGap));
}
```

<!-- plan-file: test/token_boundary/CMakeLists.txt -->
```cmake
add_executable(TokenBoundaryTest
  TokenBoundaryTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TokenBoundaryTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TokenBoundaryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TokenBoundaryTest)
```

Register the directory after `text_block_range`:

<!-- plan-edit: test/CMakeLists.txt -->
```cmake
add_subdirectory(text_block_range)
```
with:

```cmake
add_subdirectory(text_block_range)
add_subdirectory(token_boundary)
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target TokenBoundaryTest -j 8 && ./build/test/token_boundary/TokenBoundaryTest.exe --gtest_brief=1`
Expected: a compile error, `TokenBoundary.h: No such file or directory`.

- [ ] **Step 3: The header**

Upstream's predicates over the two flags, verbatim, then ours: the struct `ParsedText` will store, and the
predicates over it.

<!-- plan-file: lib/Epub/Epub/TokenBoundary.h -->
```cpp
#pragma once

#include <cstdint>

#include "hyphenation/HyphenationCommon.h"

// Ported from crosspoint-reader PR #2892 ("fix: hyphenate focus-split words and break after visible
// hyphens", Uri Tauber), commit 25cf99b2: the three predicates are theirs, verbatim; Boundary and the
// predicates over it, at the end, are ours.
// Upstream ParsedText stores each boundary in two bit-packed vectors. Together the bits distinguish
// ordinary word gaps, CJK-style stretchable zero-width gaps, unbreakable attachment, and
// breakable attachment after visible hyphens/dashes.
namespace TokenBoundary {

// A continuation normally forbids a line break. noSpaceBefore marks the fourth state where the
// text stays attached when unbroken but the line breaker may still stop at the boundary.
constexpr bool allowsBreak(const bool continues, const bool noSpaceBefore) { return !continues || noSpaceBefore; }

// Ordinary word gaps and CJK zero-width gaps participate in justification. Attached text does
// not, except for a literal space token such as a non-breaking space represented as content.
constexpr bool isJustifiableGap(const bool continues, const bool noSpaceBefore, const bool isSpaceToken) {
  return !continues || (isSpaceToken && !noSpaceBefore);
}

// Soft hyphens become visible only when their conditional break is taken. U+2011 exists to forbid
// a break. Every other character classified by isExplicitHyphen is a visible break opportunity.
inline bool allowsBreakAfterExplicitHyphen(const uint32_t cp) {
  constexpr uint32_t NON_BREAKING_HYPHEN_CP = 0x2011;
  return isExplicitHyphen(cp) && !isSoftHyphen(cp) && cp != NON_BREAKING_HYPHEN_CP;
}

// Ours: ParsedText keeps one of these per token instead of upstream's two bit vectors, so the two
// halves of a boundary travel together through every insert, erase and copy of the word lists.
struct Boundary {
  bool continues = false;      // glued to the previous token: no space, and no break
  bool noSpaceBefore = false;  // no space either; with continues=false, a breakable CJK gap
};

constexpr bool allowsBreak(const Boundary b) { return allowsBreak(b.continues, b.noSpaceBefore); }

// True for an ordinary word gap, the only kind measured as a space.
constexpr bool hasSpace(const Boundary b) { return !b.continues && !b.noSpaceBefore; }

}  // namespace TokenBoundary
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build build/test --target TokenBoundaryTest -j 8 && ./build/test/token_boundary/TokenBoundaryTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 9 tests.`

- [ ] **Step 5: Commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror lib/Epub/Epub/TokenBoundary.h test/token_boundary/TokenBoundaryTest.cpp
git add lib/Epub/Epub/TokenBoundary.h test/token_boundary/ test/CMakeLists.txt
git commit -m "feat(epub): token boundary kinds, ported from crosspoint-reader" \
  -m "TokenBoundary.h and its seven tests are crosspoint-reader's (PR #2892, \"fix: hyphenate focus-split words and break after visible hyphens\", commit 25cf99b2), verbatim. Ours: Boundary, the per-token struct ParsedText will store instead of upstream's two bit vectors, and the predicates over it." \
  -m "Co-authored-by: Uri Tauber <uritaube@gmail.com>"
```

### Task 3: One boundary per token in `ParsedText` and `TextBlock`

`wordContinues` (`std::vector<bool>`) becomes `wordBoundaries` (`std::vector<TokenBoundary::Boundary>`). Rename it,
and the compiler lists every site; each site then reads the predicate for its question, as the Architecture section
lists. With `noSpaceBefore` always false, each predicate gives what the old flag gave: `hasSpace(b)` is
`!continues`, `allowsBreak(b)` is `!continues`, and `naturalGapBefore` is the old kerning-or-space choice. So no
page changes; Task 1's goldens check that.

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h`, `lib/Epub/Epub/blocks/TextBlock.cpp`
- Modify: `lib/Epub/Epub/ParsedText.h`, `lib/Epub/Epub/ParsedText.cpp`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (a comment)
- Modify: `test/text_block_range/TextBlockRangeTest.cpp`

**Interfaces:**
- Consumes: Task 2's `TokenBoundary::Boundary`, `allowsBreak(Boundary)`, `hasSpace(Boundary)`.
- Produces (A2 builds on these):
  - `std::vector<TokenBoundary::Boundary> ParsedText::wordBoundaries` (private), in step with `words` through every
    insert, erase, copy and swap;
  - `int ParsedText::naturalGapBefore(const GfxRenderer& renderer, int fontId, size_t j) const` (private), the
    unscaled natural gap before `words[j]`, `j > 0`;
  - the four breakers and `extractLine` take `const std::vector<TokenBoundary::Boundary>& boundaries` where they
    took `std::vector<bool>& continuesVec`;
  - `TextBlock::WordRange::boundaries` (`const std::vector<TokenBoundary::Boundary>*`, may be null) replaces
    `WordRange::continues`. `TextBlock` keeps only `.continues` (style bit 7).

- [ ] **Step 1: Write the failing tests**

The four range tests that hand `TextBlock` continuation flags now hand it boundaries, and a fifth checks that a CJK
gap is not stored as a continuation:

<!-- plan-edit: test/text_block_range/TextBlockRangeTest.cpp -->
```cpp
#include "Epub/blocks/TextBlock.h"
```
with:

```cpp
#include "Epub/TokenBoundary.h"
#include "Epub/blocks/TextBlock.h"
```
<!-- plan-edit: test/text_block_range/TextBlockRangeTest.cpp -->
```cpp
  const std::vector<bool> continues = {false, false, true, true, false};
  TextBlock::WordRange range = makeRange(words, styles, sizes, 0, words.size());
  range.continues = &continues;

  TextBlock block(range, {0, 40, 70, 110, 130}, BlockStyle());
  ASSERT_TRUE(block.valid());
  for (uint16_t i = 0; i < words.size(); i++) {
    EXPECT_EQ(block.wordContinues(i), continues[i]) << "word " << i;
```
with:

```cpp
  const std::vector<TokenBoundary::Boundary> boundaries = {
      {false, false}, {false, false}, {true, false}, {true, false}, {false, false}};
  TextBlock::WordRange range = makeRange(words, styles, sizes, 0, words.size());
  range.boundaries = &boundaries;

  TextBlock block(range, {0, 40, 70, 110, 130}, BlockStyle());
  ASSERT_TRUE(block.valid());
  for (uint16_t i = 0; i < words.size(); i++) {
    EXPECT_EQ(block.wordContinues(i), boundaries[i].continues) << "word " << i;
```
<!-- plan-edit: test/text_block_range/TextBlockRangeTest.cpp -->
```cpp
  const std::vector<bool> continues = {false, true};
  TextBlock::WordRange range = makeRange(words, styles, sizes, 0, words.size());
  range.continues = &continues;
```
with:

```cpp
  const std::vector<TokenBoundary::Boundary> boundaries = {{false, false}, {true, false}};
  TextBlock::WordRange range = makeRange(words, styles, sizes, 0, words.size());
  range.boundaries = &boundaries;
```
<!-- plan-edit: test/text_block_range/TextBlockRangeTest.cpp -->
```cpp
  const std::vector<bool> continues = {false, false, true, true, false};
  TextBlock::WordRange range = makeRange(f.words, f.styles, f.sizes, 2, 2);
  range.continues = &continues;
```
with:

```cpp
  const std::vector<TokenBoundary::Boundary> boundaries = {
      {false, false}, {false, false}, {true, false}, {true, false}, {false, false}};
  TextBlock::WordRange range = makeRange(f.words, f.styles, f.sizes, 2, 2);
  range.boundaries = &boundaries;
```
<!-- plan-edit: test/text_block_range/TextBlockRangeTest.cpp -->
```cpp
  const std::vector<bool> tooShort = {true};
  TextBlock::WordRange range = makeRange(f.words, f.styles, f.sizes, 0, 3);
  range.continues = &tooShort;
  TextBlock guarded(range, {0, 40, 90}, BlockStyle());
  ASSERT_TRUE(guarded.valid());
  for (uint16_t i = 0; i < 3; i++) EXPECT_FALSE(guarded.wordContinues(i));
}
```
with:

```cpp
  const std::vector<TokenBoundary::Boundary> tooShort = {{true, false}};
  TextBlock::WordRange range = makeRange(f.words, f.styles, f.sizes, 0, 3);
  range.boundaries = &tooShort;
  TextBlock guarded(range, {0, 40, 90}, BlockStyle());
  ASSERT_TRUE(guarded.valid());
  for (uint16_t i = 0; i < 3; i++) EXPECT_FALSE(guarded.wordContinues(i));
}

// A CJK gap is a break opportunity, so the block must not record it as a continuation: the dictionary
// would otherwise join every character of a CJK run into one "word".
TEST(TextBlockContinues, ACjkGapIsNotAContinuation) {
  const Fixture f = uniformFixture();
  const std::vector<TokenBoundary::Boundary> boundaries = {{false, false}, {false, true}, {true, false}};
  TextBlock::WordRange range = makeRange(f.words, f.styles, f.sizes, 0, 3);
  range.boundaries = &boundaries;
  TextBlock block(range, {0, 40, 90}, BlockStyle());
  ASSERT_TRUE(block.valid());
  EXPECT_FALSE(block.wordContinues(1));  // the CJK gap
  EXPECT_TRUE(block.wordContinues(2));   // glued
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build build/test --target TextBlockRangeTest -j 8 && ./build/test/text_block_range/TextBlockRangeTest.exe --gtest_brief=1`
Expected: compile errors, `'struct TextBlock::WordRange' has no member named 'boundaries'`.

- [ ] **Step 3: `TextBlock` takes boundaries**

<!-- plan-edit: lib/Epub/Epub/blocks/TextBlock.h -->
```cpp
#include "Block.h"
#include "BlockStyle.h"
```
with:

```cpp
#include "../TokenBoundary.h"
#include "Block.h"
#include "BlockStyle.h"
```
<!-- plan-edit: lib/Epub/Epub/blocks/TextBlock.h -->
```cpp
    // Layout's per-word "attaches to the previous word with no space" flags, indexed like
    // `words`. May be null, meaning every word is space-separated.
    const std::vector<bool>* continues = nullptr;
```
with:

```cpp
    // Layout's per-word boundaries, indexed like `words`; only `continues` is kept (style bit 7).
    // May be null, meaning every word is space-separated.
    const std::vector<TokenBoundary::Boundary>* boundaries = nullptr;
```
<!-- plan-edit: lib/Epub/Epub/blocks/TextBlock.cpp -->
```cpp
  const std::vector<bool>* continuesSrc =
      (range.continues != nullptr && first + count <= range.continues->size()) ? range.continues : nullptr;
```
with:

```cpp
  const std::vector<TokenBoundary::Boundary>* boundariesSrc =
      (range.boundaries != nullptr && first + count <= range.boundaries->size()) ? range.boundaries : nullptr;
```
<!-- plan-edit: lib/Epub/Epub/blocks/TextBlock.cpp -->
```cpp
    styles[i] = packStyle(styleSrc[first + i], continuesSrc != nullptr && (*continuesSrc)[first + i]);
```
with:

```cpp
    styles[i] = packStyle(styleSrc[first + i], boundariesSrc != nullptr && (*boundariesSrc)[first + i].continues);
```

- [ ] **Step 4: `ParsedText.h`**

The list, the gap helper, and the signatures of the breakers and `extractLine`. `computeLineBreaks` and
`computeHyphenatedLineBreaks` took the flags by non-const reference; nothing in them writes the list (the inserts
in `hyphenateWordAtIndex` go to the member), so the new parameter is `const`.

<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"
```
with:

```cpp
#include "TokenBoundary.h"
#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
  std::vector<bool> wordContinues;  // true = word attaches to previous (no space before it)
```
with:

```cpp
  // How each word joins the one before it (TokenBoundary.h): a space, glued, or a breakable CJK gap.
  // In lockstep with `words` through every insert/erase, like the per-word vectors below.
  std::vector<TokenBoundary::Boundary> wordBoundaries;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
  int widthForLine(int lineIndex, int lineHeight, int16_t blockStartY, int pageWidth) const;
```
with:

```cpp
  int widthForLine(int lineIndex, int lineHeight, int16_t blockStartY, int pageWidth) const;
  // The natural gap before words[j] (j > 0), unscaled: kerning across a glued boundary, nothing at a
  // CJK gap, else the space advance. Every breaker measures gaps through this.
  int naturalGapBefore(const GfxRenderer& renderer, int fontId, size_t j) const;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
  void computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, std::vector<uint16_t>& wordWidths,
                         std::vector<bool>& continuesVec, int firstLineIndent, int16_t blockStartY, int lineHeight,
                         std::vector<size_t>& lineBreakIndices);
  void computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                   std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
```
with:

```cpp
  void computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, std::vector<uint16_t>& wordWidths,
                         const std::vector<TokenBoundary::Boundary>& boundaries, int firstLineIndent,
                         int16_t blockStartY, int lineHeight, std::vector<size_t>& lineBreakIndices);
  void computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                   std::vector<uint16_t>& wordWidths,
                                   const std::vector<TokenBoundary::Boundary>& boundaries,
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
                                            std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                            size_t startIndex, std::vector<bool>& lineEndsWithHyphenatedWord,
```
with:

```cpp
                                            std::vector<uint16_t>& wordWidths,
                                            const std::vector<TokenBoundary::Boundary>& boundaries, size_t startIndex,
                                            std::vector<bool>& lineEndsWithHyphenatedWord,
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
                                        const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                                        size_t lineStartIndex, int firstLineIndent, int16_t blockStartY = 0,
                                        int lineHeight = 0) const;
```
with:

```cpp
                                        const std::vector<uint16_t>& wordWidths,
                                        const std::vector<TokenBoundary::Boundary>& boundaries, size_t lineStartIndex,
                                        int firstLineIndent, int16_t blockStartY = 0, int lineHeight = 0) const;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.h -->
```cpp
      size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
      const std::vector<size_t>& lineBreakIndices,
```
with:

```cpp
      size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
      const std::vector<TokenBoundary::Boundary>& boundaries, const std::vector<size_t>& lineBreakIndices,
```

- [ ] **Step 5: `ParsedText.cpp`, the per-word list sites**

The log preview joins words with a space only at a word gap:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
std::string buildLinePreview(const std::vector<std::string>& words, const std::vector<bool>& continuesVec,
                             const size_t start, const size_t endExclusive, const size_t maxLen = 120) {
  // Build a readable line preview while preserving continuation semantics
  // (no synthetic spaces before attached tokens).
  std::string preview;
  for (size_t idx = start; idx < endExclusive; ++idx) {
    if (idx > start && idx < continuesVec.size() && !continuesVec[idx]) {
```
with:

```cpp
std::string buildLinePreview(const std::vector<std::string>& words,
                             const std::vector<TokenBoundary::Boundary>& boundaries, const size_t start,
                             const size_t endExclusive, const size_t maxLen = 120) {
  // Build a readable line preview while preserving continuation semantics
  // (no synthetic spaces before attached tokens or at CJK gaps).
  std::string preview;
  for (size_t idx = start; idx < endExclusive; ++idx) {
    if (idx > start && idx < boundaries.size() && TokenBoundary::hasSpace(boundaries[idx])) {
```

`addWord`: the heap gate charges 2 bytes per word for the boundary, where `std::vector<bool>` cost 1 bit
(`newCapacity / 8`), and the push records a space or glue:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  if (words.capacity() < requiredSize || wordStyles.capacity() < requiredSize ||
      wordContinues.capacity() < requiredSize || wordSizes.capacity() < requiredSize ||
```
with:

```cpp
  if (words.capacity() < requiredSize || wordStyles.capacity() < requiredSize ||
      wordBoundaries.capacity() < requiredSize || wordSizes.capacity() < requiredSize ||
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
      const size_t needed =
          newCapacity * (sizeof(std::string) + sizeof(EpdFontFamily::Style) + sizeof(uint8_t) + sizeof(uint32_t)) +
          newCapacity / 8 + 5 * ALLOC_HEADER_SLACK;
```
with:

```cpp
      const size_t needed = newCapacity * (sizeof(std::string) + sizeof(EpdFontFamily::Style) + sizeof(uint8_t) +
                                           sizeof(uint32_t) + sizeof(TokenBoundary::Boundary)) +
                            5 * ALLOC_HEADER_SLACK;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    wordContinues.reserve(newCapacity);
```
with:

```cpp
    wordBoundaries.reserve(newCapacity);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  wordContinues.push_back(attachToPrevious);
```
with:

```cpp
  wordBoundaries.push_back(TokenBoundary::Boundary{attachToPrevious, false});
```

The `preserveSource` snapshot and its restore:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  std::vector<bool> savedContinues;
```
with:

```cpp
  std::vector<TokenBoundary::Boundary> savedBoundaries;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    savedContinues = wordContinues;
```
with:

```cpp
    savedBoundaries = wordBoundaries;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    wordContinues = std::move(savedContinues);
```
with:

```cpp
    wordBoundaries = std::move(savedBoundaries);
```

The text handed to `ensureFontReady` (a space only at a word gap):

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
      if (i > 0 && !wordContinues[i]) totalSize += 1;
```
with:

```cpp
      if (i > 0 && TokenBoundary::hasSpace(wordBoundaries[i])) totalSize += 1;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
      if (i > 0 && !wordContinues[i]) allText += ' ';
```
with:

```cpp
      if (i > 0 && TokenBoundary::hasSpace(wordBoundaries[i])) allText += ' ';
```

The call sites in `layoutAndExtractLines`: the breakers, the log previews, the hyphenation-retry merge, the retry
itself and the consumed-prefix erase:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, lineEndsWithHyphenatedWord,
                                splitPrefixWordIndexes, splitInsertedHyphen, firstLineIndent, blockStartY, lineHeight,
                                lineBreakIndices);
  } else {
    computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, firstLineIndent, blockStartY, lineHeight,
                      lineBreakIndices);
```
with:

```cpp
    computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordBoundaries, lineEndsWithHyphenatedWord,
                                splitPrefixWordIndexes, splitInsertedHyphen, firstLineIndent, blockStartY, lineHeight,
                                lineBreakIndices);
  } else {
    computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordBoundaries, firstLineIndent, blockStartY, lineHeight,
                      lineBreakIndices);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    const auto result = extractLine(i, pageWidth, wordWidths, wordContinues, lineBreakIndices, processLine, renderer,
```
with:

```cpp
    const auto result = extractLine(i, pageWidth, wordWidths, wordBoundaries, lineBreakIndices, processLine, renderer,
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
      const std::string firstAttemptPreview = buildLinePreview(words, wordContinues, lineStart, lineEnd);
```
with:

```cpp
      const std::string firstAttemptPreview = buildLinePreview(words, wordBoundaries, lineStart, lineEnd);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
        wordContinues.erase(wordContinues.begin() + splitIndex + 1);
```
with:

```cpp
        wordBoundaries.erase(wordBoundaries.begin() + splitIndex + 1);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
      const size_t retryBreak = computeSingleLineBreakNoHyphen(renderer, fontId, pageWidth, wordWidths, wordContinues,
```
with:

```cpp
      const size_t retryBreak = computeSingleLineBreakNoHyphen(renderer, fontId, pageWidth, wordWidths, wordBoundaries,
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
        const std::string retryPreview = buildLinePreview(words, wordContinues, retryLineStart, retryLineEnd);
```
with:

```cpp
        const std::string retryPreview = buildLinePreview(words, wordBoundaries, retryLineStart, retryLineEnd);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
        extractLine(i, pageWidth, wordWidths, wordContinues, lineBreakIndices, processLine, renderer, fontId, false,
```
with:

```cpp
        extractLine(i, pageWidth, wordWidths, wordBoundaries, lineBreakIndices, processLine, renderer, fontId, false,
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
        computeHyphenatedLineBreaksFromIndex(renderer, fontId, pageWidth, wordWidths, wordContinues, resumeIndex,
```
with:

```cpp
        computeHyphenatedLineBreaksFromIndex(renderer, fontId, pageWidth, wordWidths, wordBoundaries, resumeIndex,
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
```
with:

```cpp
    wordBoundaries.erase(wordBoundaries.begin(), wordBoundaries.begin() + consumed);
```

`releaseLayoutScratch` and `reset`:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    std::vector<bool>().swap(wordContinues);
```
with:

```cpp
    std::vector<TokenBoundary::Boundary>().swap(wordBoundaries);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  wordContinues.clear();
```
with:

```cpp
  wordBoundaries.clear();
```

The bionic rebuild. Its first span keeps the word's own boundary and every later span is glued, as before:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  std::vector<bool> transformedSuffixContinues;
```
with:

```cpp
  std::vector<TokenBoundary::Boundary> transformedSuffixBoundaries;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  transformedSuffixContinues.reserve(transformedSuffix.capacity());
```
with:

```cpp
  transformedSuffixBoundaries.reserve(transformedSuffix.capacity());
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    const bool originalAttachToPrevious = wordContinues[i];
```
with:

```cpp
    const TokenBoundary::Boundary originalBoundary = wordBoundaries[i];
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    bool attachToPrevious = originalAttachToPrevious;
```
with:

```cpp
    // The first span keeps the word's own boundary; every span after it is glued to the one before.
    constexpr TokenBoundary::Boundary kGlued{true, false};
    TokenBoundary::Boundary boundary = originalBoundary;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
            transformedSuffixContinues.push_back(attachToPrevious);
            transformedSuffixSizes.push_back(originalSize);
            transformedSuffixOffsets.push_back(originalOffset);

            transformedSuffix.push_back(std::move(suffix));
            transformedSuffixStyles.push_back(originalStyle);
            transformedSuffixContinues.push_back(true);
            transformedSuffixSizes.push_back(originalSize);
            transformedSuffixOffsets.push_back(originalOffset);
            attachToPrevious = true;
```
with:

```cpp
            transformedSuffixBoundaries.push_back(boundary);
            transformedSuffixSizes.push_back(originalSize);
            transformedSuffixOffsets.push_back(originalOffset);

            transformedSuffix.push_back(std::move(suffix));
            transformedSuffixStyles.push_back(originalStyle);
            transformedSuffixBoundaries.push_back(kGlued);
            transformedSuffixSizes.push_back(originalSize);
            transformedSuffixOffsets.push_back(originalOffset);
            boundary = kGlued;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
      transformedSuffixContinues.push_back(attachToPrevious);
      transformedSuffixSizes.push_back(originalSize);
      transformedSuffixOffsets.push_back(originalOffset);
      attachToPrevious = true;
```
with:

```cpp
      transformedSuffixBoundaries.push_back(boundary);
      transformedSuffixSizes.push_back(originalSize);
      transformedSuffixOffsets.push_back(originalOffset);
      boundary = kGlued;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  wordContinues.resize(suffixStart);
```
with:

```cpp
  wordBoundaries.resize(suffixStart);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  wordContinues.insert(wordContinues.end(), transformedSuffixContinues.begin(), transformedSuffixContinues.end());
```
with:

```cpp
  wordBoundaries.insert(wordBoundaries.end(), transformedSuffixBoundaries.begin(), transformedSuffixBoundaries.end());
```

`hyphenateWordAtIndex` inserts the remainder after an ordinary gap, `Boundary{}` (it starts the next line):

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);
```
with:

```cpp
  // wordBoundaries[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordBoundaries.insert(wordBoundaries.begin() + wordIndex + 1, TokenBoundary::Boundary{});
```

- [ ] **Step 6: `ParsedText.cpp`, the gaps and the breaks**

One helper measures the natural gap before a token, for the text-indent pre-pass and all four breakers:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
void ParsedText::layoutAndExtractLines(
    const GfxRenderer& renderer, const int bodyFontId, const uint16_t viewportWidth,
```
with:

```cpp
int ParsedText::naturalGapBefore(const GfxRenderer& renderer, const int fontId, const size_t j) const {
  const TokenBoundary::Boundary boundary = wordBoundaries[j];
  const uint32_t left = lastCodepoint(words[j - 1]);
  const uint32_t right = firstCodepoint(words[j]);
  if (boundary.continues) return renderer.getKerning(fontId, left, right, wordStyles[j - 1]);
  if (boundary.noSpaceBefore) return 0;
  return renderer.getSpaceAdvance(fontId, left, right, wordStyles[j - 1]);
}

void ParsedText::layoutAndExtractLines(
    const GfxRenderer& renderer, const int bodyFontId, const uint16_t viewportWidth,
```

The span-indent pre-pass:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    for (size_t j = 1; j < words.size(); ++j) {
      natural +=
          wordWidths[j] + (wordContinues[j] ? renderer.getKerning(fontId, lastCodepoint(words[j - 1]),
                                                                  firstCodepoint(words[j]), wordStyles[j - 1])
                                            : renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]),
                                                                       firstCodepoint(words[j]), wordStyles[j - 1]));
    }
```
with:

```cpp
    for (size_t j = 1; j < words.size(); ++j) {
      natural += wordWidths[j] + naturalGapBefore(renderer, fontId, j);
    }
```

`computeLineBreaks` (the DP breaker: its gap cache, and "no break before a glued token"):

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
                                   std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                   const int firstLineIndent, const int16_t blockStartY, const int lineHeight,
                                   std::vector<size_t>& lineBreakIndices) {
```
with:

```cpp
                                   std::vector<uint16_t>& wordWidths,
                                   const std::vector<TokenBoundary::Boundary>& boundaries, const int firstLineIndent,
                                   const int16_t blockStartY, const int lineHeight,
                                   std::vector<size_t>& lineBreakIndices) {
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(totalWordCount, 0);
  for (size_t j = 1; j < totalWordCount; ++j) {
    if (!continuesVec[j]) {
      interWordGaps[j] =
          renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    } else {
      interWordGaps[j] =
          renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    }
  }
```
with:

```cpp
  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(totalWordCount, 0);
  for (size_t j = 1; j < totalWordCount; ++j) {
    interWordGaps[j] = naturalGapBefore(renderer, fontId, j);
  }
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
```
with:

```cpp
      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && !TokenBoundary::allowsBreak(boundaries[j + 1])) {
```

`computeSingleLineBreakNoHyphen` (the page-bottom retry's one line):

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
                                                  const std::vector<uint16_t>& wordWidths,
                                                  const std::vector<bool>& continuesVec, const size_t lineStartIndex,
                                                  const int firstLineIndent, const int16_t blockStartY,
                                                  const int lineHeight) const {
```
with:

```cpp
                                                  const std::vector<uint16_t>& wordWidths,
                                                  const std::vector<TokenBoundary::Boundary>& boundaries,
                                                  const size_t lineStartIndex, const int firstLineIndent,
                                                  const int16_t blockStartY, const int lineHeight) const {
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    int spacing = 0;
    if (!isFirstWord) {
      if (!continuesVec[currentIndex]) {
        spacing = renderer.getSpaceAdvance(fontId, lastCodepoint(words[currentIndex - 1]),
                                           firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      } else {
        spacing = renderer.getKerning(fontId, lastCodepoint(words[currentIndex - 1]),
                                      firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      }
    }
```
with:

```cpp
    const int spacing = isFirstWord ? 0 : naturalGapBefore(renderer, fontId, currentIndex);
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  while (currentIndex > lineStartIndex + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
    --currentIndex;
  }

  return currentIndex;
```
with:

```cpp
  while (currentIndex > lineStartIndex + 1 && currentIndex < wordWidths.size() &&
         !TokenBoundary::allowsBreak(boundaries[currentIndex])) {
    --currentIndex;
  }

  return currentIndex;
```

The two greedy hyphenating breakers share their gap cache and backtrack code; each replacement below applies to
both (`replace_all`):

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
void ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                             std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                             std::vector<bool>& lineEndsWithHyphenatedWord,
                                             std::vector<int>& splitPrefixWordIndexes,
                                             std::vector<bool>& splitInsertedHyphen, const int firstLineIndent,
                                             const int16_t blockStartY, const int lineHeight,
                                             std::vector<size_t>& lineBreakIndices) {
```
with:

```cpp
void ParsedText::computeHyphenatedLineBreaks(
    const GfxRenderer& renderer, const int fontId, const int pageWidth, std::vector<uint16_t>& wordWidths,
    const std::vector<TokenBoundary::Boundary>& boundaries, std::vector<bool>& lineEndsWithHyphenatedWord,
    std::vector<int>& splitPrefixWordIndexes, std::vector<bool>& splitInsertedHyphen, const int firstLineIndent,
    const int16_t blockStartY, const int lineHeight, std::vector<size_t>& lineBreakIndices) {
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp all -->
```cpp
  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(wordWidths.size(), 0);
  for (size_t j = 1; j < wordWidths.size(); ++j) {
    if (!continuesVec[j]) {
      interWordGaps[j] =
          renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    } else {
      interWordGaps[j] =
          renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
    }
  }
```
with:

```cpp
  std::vector<int>& interWordGaps = interWordGaps_;
  interWordGaps.assign(wordWidths.size(), 0);
  for (size_t j = 1; j < wordWidths.size(); ++j) {
    interWordGaps[j] = naturalGapBefore(renderer, fontId, j);
  }
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp all -->
```cpp
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }
```
with:

```cpp
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() &&
           !TokenBoundary::allowsBreak(boundaries[currentIndex])) {
      --currentIndex;
    }
```

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    const GfxRenderer& renderer, const int fontId, const int pageWidth, std::vector<uint16_t>& wordWidths,
    std::vector<bool>& continuesVec, const size_t startIndex, std::vector<bool>& lineEndsWithHyphenatedWord,
    std::vector<int>& splitPrefixWordIndexes, std::vector<bool>& splitInsertedHyphen, const int16_t blockStartY,
    const int lineHeight, const int startLineIdx, std::vector<size_t>& lineBreakIndices) {
```
with:

```cpp
    const GfxRenderer& renderer, const int fontId, const int pageWidth, std::vector<uint16_t>& wordWidths,
    const std::vector<TokenBoundary::Boundary>& boundaries, const size_t startIndex,
    std::vector<bool>& lineEndsWithHyphenatedWord, std::vector<int>& splitPrefixWordIndexes,
    std::vector<bool>& splitInsertedHyphen, const int16_t blockStartY, const int lineHeight, const int startLineIdx,
    std::vector<size_t>& lineBreakIndices) {
```

- [ ] **Step 7: `extractLine`**

A word gap and a CJK gap both count as justifiable gaps; only a word gap adds a space to the natural width and to
the drawn position. Glued tokens keep their kerning, and a glued `" "` still stretches. The line takes the boundary
list:

<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
    const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
```
with:

```cpp
    const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
    const std::vector<TokenBoundary::Boundary>& boundaries, const std::vector<size_t>& lineBreakIndices,
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    const uint32_t firstCp = firstCodepoint(words[lastBreakAt + wordIdx]);
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      const bool beforeClosing = isClosingPunctuation(firstCp);
      if (!beforeClosing) actualGapCount++;
      // Inter-word gaps scale with the preceding word's effective size, matching wordStyles usage.
      totalNaturalGaps +=
          static_cast<int>(renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]), firstCp,
                                                    wordStyles[lastBreakAt + wordIdx - 1]) *
                               wordScale(lastBreakAt + wordIdx - 1) +
                           0.5f);
    } else if (wordIdx > 0 && continuesVec[lastBreakAt + wordIdx]) {
```
with:

```cpp
    const uint32_t firstCp = firstCodepoint(words[lastBreakAt + wordIdx]);
    const TokenBoundary::Boundary boundary = boundaries[lastBreakAt + wordIdx];
    if (wordIdx > 0 && !boundary.continues) {
      // A word gap or a CJK gap: both stretch when justified, only the word gap has a width.
      const bool beforeClosing = isClosingPunctuation(firstCp);
      if (!beforeClosing) actualGapCount++;
      if (TokenBoundary::hasSpace(boundary)) {
        // Inter-word gaps scale with the preceding word's effective size, matching wordStyles usage.
        totalNaturalGaps +=
            static_cast<int>(renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]), firstCp,
                                                      wordStyles[lastBreakAt + wordIdx - 1]) *
                                 wordScale(lastBreakAt + wordIdx - 1) +
                             0.5f);
      }
    } else if (wordIdx > 0) {
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
```
with:

```cpp
    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && boundaries[lastBreakAt + wordIdx + 1].continues;
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
      if (words[lastBreakAt + wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
```
with:

```cpp
      if (words[lastBreakAt + wordIdx] == " " && boundaries[lastBreakAt + wordIdx].continues &&
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
        const uint32_t nextFirstCp = firstCodepoint(words[lastBreakAt + wordIdx + 1]);
        gap = static_cast<int>(renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                                                        nextFirstCp, wordStyles[lastBreakAt + wordIdx]) *
                                   wordScale(lastBreakAt + wordIdx) +
                               0.5f);
```
with:

```cpp
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
        const uint32_t nextFirstCp = firstCodepoint(words[lastBreakAt + wordIdx + 1]);
        // A CJK gap has no natural width; it only takes the justification share below.
        if (TokenBoundary::hasSpace(boundaries[lastBreakAt + wordIdx + 1])) {
          gap = static_cast<int>(renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                                                          nextFirstCp, wordStyles[lastBreakAt + wordIdx]) *
                                     wordScale(lastBreakAt + wordIdx) +
                                 0.5f);
        }
```
<!-- plan-edit: lib/Epub/Epub/ParsedText.cpp -->
```cpp
  range.continues = &continuesVec;
```
with:

```cpp
  range.boundaries = &boundaries;
```

And the parser's comment names the new list:

<!-- plan-edit: lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp -->
```cpp
// 32-bit libstdc++) plus the parallel per-word vectors (wordStyles, wordSizes, wordContinues) and
```
with:

```cpp
// 32-bit libstdc++) plus the parallel per-word vectors (wordStyles, wordSizes, wordBoundaries) and
```

- [ ] **Step 8: Run the tests to see them pass**

Run: `cmake --build build/test --target TextBlockRangeTest -j 8 && ./build/test/text_block_range/TextBlockRangeTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 15 tests.` (14 before).

Run: `cmake --build build/test --target WordSizeLayoutTest -j 8 && ./build/test/word_size_layout/WordSizeLayoutTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 36 tests.`

Run: `cmake --build build/test --target EpubPipelineTest -j 8 && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_brief=1`
Expected: `[  PASSED  ] 634 tests.` (2 skipped, as on master): every golden, Task 1's included, byte-identical. A failing
`MatchesGolden` here is a bug in this task: never regenerate a golden in this plan.

- [ ] **Step 9: The whole suite and the firmware**

Run: `cmake --build build/test -j 8 -- -k; (cd build/test && ctest -j 8 2>&1 | tail -3)`
Expected: `100% tests passed out of 2311` (2290 on master; Task 1 adds 11, Task 2 9 and this task 1).

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default`, then the same with `-e x4pro`.
Expected: `[SUCCESS]` for each, with no new warning from the files this plan touches.

- [ ] **Step 10: Format and commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe --dry-run --Werror lib/Epub/Epub/ParsedText.h lib/Epub/Epub/ParsedText.cpp \
  lib/Epub/Epub/blocks/TextBlock.h lib/Epub/Epub/blocks/TextBlock.cpp \
  lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp test/text_block_range/TextBlockRangeTest.cpp
# The old list and parameters are gone (32 hits on master, none now; TextBlock::wordContinues() stays):
git grep -nE "std::vector<bool>(\*|&)? ?(wordContinues|continues)|continuesVec|savedContinues|range\.continues" -- lib src test
git add lib/Epub/Epub/ParsedText.h lib/Epub/Epub/ParsedText.cpp lib/Epub/Epub/blocks/TextBlock.h \
  lib/Epub/Epub/blocks/TextBlock.cpp lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp \
  test/text_block_range/TextBlockRangeTest.cpp
git commit -m "refactor(epub): one boundary per token, ready for CJK gaps" \
  -m "ParsedText kept a continuation bit per word. It now keeps a TokenBoundary::Boundary (a space, glued, or a breakable zero-width CJK gap), and every layout site asks one predicate: naturalGapBefore() for the gap, allowsBreak() for a line break, hasSpace() for a drawn or measured space. Nothing sets the CJK gap yet; every golden, including the hyphenation and bionic ones, is byte-identical. TextBlock keeps only the continuation bit."
```

`TextBlock::wordContinues()` keeps its name: it reads the style byte, not the list.

- [ ] **Step 11: Open the PR**

```bash
git push -u origin feat/cjk-token-boundaries
gh pr create --base master --title "refactor: (Epub) one boundary per token, ready for CJK gaps" --body-file <file>
```

The body says:
- the first of three PRs for CJK line breaking (spec `docs/superpowers/specs/2026-10-16-cjk-layout-design.md`);
- no behaviour change: the new `_hyph` and `_bionic` goldens were generated from master's code (Task 1's commit),
  and the refactor keeps every golden identical;
- what is upstream's (TokenBoundary.h, PR #2892) and what is ours (`Boundary`, the storage, the golden variants);
- no section cache version bump.

