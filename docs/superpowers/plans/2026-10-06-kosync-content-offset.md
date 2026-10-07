# KOSync Content Offsets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** KOReader sync positions round-trip exactly in both directions for every book, and a push is one streamed inflate stopped at the page, by recording a visible-text offset per page in the section cache.

**Architecture:** The layout parser counts "visible bytes" of the chapter's source text with the same rule the XPath mappers already use, and stores the offset of each page's first element in the paragraph LUT entry (replacing a dead field). Push seeks to the stored offset; pull resolves an XPath to an offset and the reader turns it into a page by LUT lookup. Both mapper directions stream the inflate through the SAX parser instead of a temp file.

**Tech Stack:** C++20 firmware (ESP32-C3, Arduino/PlatformIO, `-fno-exceptions`), yxml-backed `SaxParser`, host tests with GoogleTest/CMake (MinGW UCRT64), clang-format.

**Spec:** `docs/superpowers/specs/2026-10-06-kosync-content-offset-design.md`

## Global Constraints

- Branch `feat/kosync-content-offset`, stacked on `feat/kosync-progress-comparison` (PR #397). Every task ends in a commit; the PR is opened in Task 8.
- Heap rules from `.claude/skills/heap-discipline`: no bare `new`; fallible allocations through `makeUniqueNoThrow`; `reserve` before `push_back` loops; every new allocation carries a one-line size and why-not-stack note. The sync path must add **no** allocation (spec, "Memory and flash").
- `SECTION_FILE_VERSION` 79 → 80 happens exactly once, in Task 3.
- Goldens (`test/epub_pipeline/goldens`) stay byte-identical throughout: run the full suite after every task.
- Host tests build from Git Bash (not PowerShell), `cmake --build . -j 8 -- -k 0` in `test/build`; `epub_build_inventory` fails on Windows (`dlfcn.h`), ignore it. Firmware builds from PowerShell with `$env:PLATFORMIO_CORE_DIR = 'C:\pio'` from the repo root; `pio` is `$HOME\.platformio\penv\Scripts\pio.exe`; clang-format is `/c/Program Files/LLVM/bin/clang-format.exe`.
- Commit messages: `type(scope): subject`, body in plain prose, ending with `Co-Authored-By: Claude <noreply@anthropic.com>`.
- Test names describe behaviour. Every behaviour change has a test that was seen failing first.

## Review Focus

1. **A page whose first element is an image, rule or table fragment** shares its start offset with the text page after it. A push from it must pull back to the same page, not the next one: `getPageForVisibleTextOffset` resolves ties to the first page of the run (Task 3 test `TiesResolveToTheFirstPageOfTheRun`; Task 8 round trip over `test_jpeg_images`, `test_png_images`, `test_mixed_images`).
2. **A page that starts with a synthetic word** (list marker, footnote preview, drop cap) must take the offset of the source text it stands before (Task 3: synthetic words take `visibleTextOffset_`; Task 8 round trip over `test_lists`, `test_inline_footnotes`).
3. **An entity reference at a page start** (`&mdash;`, `&#x2019;`) must count as its expansion on both sides (Task 5 test `EntityAtThePageStartRoundTrips`).
4. **A truncated section cache** (the build aborted on heap) has fewer LUT entries than the reader may ask for: `getVisibleTextOffsetForPage(page >= count)` must be `nullopt` and the push must fall back to the fraction path (Task 3 test `PageBeyondTheLutHasNoOffset`, Task 5 test `WithoutAnOffsetThePushUsesTheFraction`).
5. **Text inside `<style>` or `<script>` placed in `<body>`** must be counted by the parser exactly as the mapper skips it: not at all (Task 3 test `StyleTextInsideBodyIsNotCounted`).

---

### Task 0: Record the baseline

**Files:**
- Create: `docs/superpowers/plans/2026-10-06-kosync-content-offset-baseline.md`

**Interfaces:** none. This task produces the numbers every later "after" is compared with.

- [ ] **Step 1: Confirm the branch and a clean tree**

Run:
```bash
cd /c/_development/witchhunt-reader && git branch --show-current && git status --short | wc -l
```
Expected: `feat/kosync-content-offset` and `0`.

- [ ] **Step 2: Run the full host suite and record the count**

Run:
```bash
cd /c/_development/witchhunt-reader/test/build && cmake .. > /dev/null && cmake --build . -j 8 -- -k 0 2>&1 | grep -E "error:" | grep -v dlfcn; ctest -j 8 2>&1 | grep -E "tests passed|tests failed"
```
Expected: `100% tests passed out of 1464`.

- [ ] **Step 3: Record the round-trip drift as it stands**

Run:
```bash
cd /c/_development/witchhunt-reader/test/build && ./epub_pipeline/EpubPipelineTest.exe --gtest_filter='Corpus/RoundTripFixture.EveryPageComesBackOnOrShortlyBeforeItself/*' 2>&1 | grep -E "worstPagesBehind|worstPagesAhead|OK \]|FAILED" | head -70
```
Note the two books with allowances (`test_table_cell_overflow`: 2 behind; `test_spine_toc_edges`: 2 behind, 1 ahead) into the baseline file.

- [ ] **Step 4: Fingerprint the goldens**

Run:
```bash
cd /c/_development/witchhunt-reader && find test/epub_pipeline/goldens -type f | sort | xargs sha256sum | sha256sum
```
Record the hash in the baseline file.

- [ ] **Step 5: Record the device heap baseline (manual, X3)**

Flash the firmware built from this branch's HEAD (`pio run -e default`, then the usual upload), open a book with body-child paragraphs and a long chapter (the reporter's book from #268 qualifies) on a chapter that is not cached yet, and capture the serial log through the chapter build. Record from the log: the `[MEM] Free: ... Min Free: ...` line after the build, and the lowest `free=`/`contig=` in `[ERS] BG work` lines during it. Repeat for a Calibre-wrapped book (`<body><div><p>`). Then run one manual KOReader sync (compare) and record the `[KOSync] Sync mem[...]` lines around the push and the apply.

- [ ] **Step 6: Write the baseline file and commit**

```markdown
# Content-offset baseline (before any change)

- Host suite: 1464/1464.
- Round-trip allowances: test_table_cell_overflow {behind 2, ahead 0}; test_spine_toc_edges {behind 2, ahead 1}; all others {1, 0}.
- Goldens sha256-of-sha256s: <hash>
- X3, body-child book, chapter build: Min Free <n> B; lowest BG free=<n> contig=<n>.
- X3, wrapped book, chapter build: Min Free <n> B; lowest BG free=<n> contig=<n>.
- X3, sync: Sync mem[before_performSync] free=<n> contig=<n>; [after_trim_before_updateProgress] free=<n> contig=<n>.
```

```bash
git add docs/superpowers/plans/2026-10-06-kosync-content-offset-baseline.md
git commit -m "docs(kosync): record the content-offset baseline

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 1: One definition of "visible text"

**Files:**
- Create: `lib/Epub/Epub/VisibleText.h`
- Modify: `lib/KOReaderSync/ChapterXPathIndexerInternal.cpp:16-58` (`isSkippableTag`, `countVisibleBytes`)
- Test: `test/epub_pipeline/VisibleTextTest.cpp`, `test/epub_pipeline/CMakeLists.txt`

**Interfaces:**
- Produces: `namespace VisibleText { bool isSpace(unsigned char); size_t visibleBytes(const char* text, int len); bool isNonVisibleTag(const char* tag); }` in `lib/Epub/Epub/VisibleText.h`. Tasks 3, 4 and 5 call these.

- [ ] **Step 1: Write the failing test**

`test/epub_pipeline/VisibleTextTest.cpp`:
```cpp
// The one definition of "visible text" the layout parser and the KOReader XPath mappers share.
// A page's content offset is a count of these bytes, so the rule has to be the same on both
// sides down to the byte, and it has to be stated once.
#include <gtest/gtest.h>

#include "Epub/VisibleText.h"

TEST(VisibleText, CountsEveryNonWhitespaceByte) {
  EXPECT_EQ(VisibleText::visibleBytes("ab cd", 5), 4u);
  EXPECT_EQ(VisibleText::visibleBytes(" \t\r\n", 4), 0u);
  EXPECT_EQ(VisibleText::visibleBytes("", 0), 0u);
}

TEST(VisibleText, MultiByteSequencesCountPerByte) {
  // U+2019 is three bytes; none of them is whitespace.
  EXPECT_EQ(VisibleText::visibleBytes("\xE2\x80\x99", 3), 3u);
  // U+00A0 (no-break space) is not ASCII whitespace: two visible bytes, on both sides.
  EXPECT_EQ(VisibleText::visibleBytes("\xC2\xA0", 2), 2u);
}

TEST(VisibleText, HeadScriptAndStyleAreNotVisible) {
  EXPECT_TRUE(VisibleText::isNonVisibleTag("head"));
  EXPECT_TRUE(VisibleText::isNonVisibleTag("script"));
  EXPECT_TRUE(VisibleText::isNonVisibleTag("STYLE"));
  EXPECT_FALSE(VisibleText::isNonVisibleTag("title"));
  EXPECT_FALSE(VisibleText::isNonVisibleTag("p"));
}
```

Add `VisibleTextTest.cpp` to the `add_executable(EpubPipelineTest ...)` list in `test/epub_pipeline/CMakeLists.txt`, after `XPathExtractionTest.cpp`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake .. > /dev/null && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3`
Expected: a compile error, `Epub/VisibleText.h: No such file or directory`.

- [ ] **Step 3: Write the helper**

`lib/Epub/Epub/VisibleText.h`:
```cpp
#pragma once

#include <cctype>
#include <cstddef>
#include <strings.h>

// The one definition of "visible text" for KOReader sync.
//
// A page's content offset (Section's paragraph LUT) is the number of visible bytes of the
// chapter's source text before the page's first element. The layout parser counts them while it
// lays the chapter out; the XPath mappers count them while they resolve a position. The two have
// to agree down to the byte, so the rule lives here and nowhere else:
//   every non-whitespace byte of SAX-delivered character data inside <body> and outside head,
//   script and style; entity references as their expansion; text the layout hides or drops all
//   the same, because the rule is about the source, not the layout.
namespace VisibleText {

inline bool isSpace(const unsigned char c) { return std::isspace(c) != 0; }

inline size_t visibleBytes(const char* text, const int len) {
  size_t count = 0;
  for (int i = 0; i < len; i++) {
    if (!isSpace(static_cast<unsigned char>(text[i]))) {
      count++;
    }
  }
  return count;
}

// Tags whose text is never visible. Case-insensitive: the mappers lowercase tag names, the
// layout parser does not.
inline bool isNonVisibleTag(const char* tag) {
  return strcasecmp(tag, "head") == 0 || strcasecmp(tag, "script") == 0 || strcasecmp(tag, "style") == 0;
}

}  // namespace VisibleText
```

- [ ] **Step 4: Make the mapper use it**

In `lib/KOReaderSync/ChapterXPathIndexerInternal.cpp` add `#include <Epub/VisibleText.h>` after `#include <Epub/htmlEntities.h>`, and replace the three functions:
```cpp
bool isSkippableTag(const std::string& tag) { return VisibleText::isNonVisibleTag(tag.c_str()); }

bool isWhitespaceOnly(const char* text, const int len) { return VisibleText::visibleBytes(text, len) == 0; }

static size_t countVisibleBytesInUtf8String(const char* str) {
  return VisibleText::visibleBytes(str, static_cast<int>(strlen(str)));
}

size_t countVisibleBytes(const char* text, const int len) {
  if (isEntityRef(text, len)) {
    const char* resolved = lookupHtmlEntity(text, static_cast<size_t>(len));
    if (resolved) {
      return countVisibleBytesInUtf8String(resolved);
    }
  }
  return VisibleText::visibleBytes(text, len);
}
```
(`isWhitespaceOnly`'s old loop and `countVisibleBytesInUtf8String`'s old loop go; add `#include <cstring>` for `strlen`.)

- [ ] **Step 5: Run the tests**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3; ./epub_pipeline/EpubPipelineTest.exe --gtest_filter='VisibleText.*:XPath*:ProgressMapperFixture.*' 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]"`
Expected: `[  PASSED  ] 51 tests.` (3 new + 17 + 31).

- [ ] **Step 6: Format and commit**

```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i lib/Epub/Epub/VisibleText.h lib/KOReaderSync/ChapterXPathIndexerInternal.cpp test/epub_pipeline/VisibleTextTest.cpp
git add lib/Epub/Epub/VisibleText.h lib/KOReaderSync/ChapterXPathIndexerInternal.cpp test/epub_pipeline/VisibleTextTest.cpp test/epub_pipeline/CMakeLists.txt
git commit -m "refactor(epub): one definition of visible text for layout and sync

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: `ParsedText` carries a visible-text offset per word

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h:32-80,159-160`
- Modify: `lib/Epub/Epub/ParsedText.cpp:195-236` (addWord), `283-296` and `520-530` (preserveSource), `436-446` (split merge), `508-520` (consume), `562-575` (reset), `895-910` (bionic), `1160-1170` (hyphenation), `1207-1209` + the `processLine` call at `1349` (extractLine)
- Test: `test/epub_pipeline/ParsedTextOffsetTest.cpp`, `test/epub_pipeline/CMakeLists.txt`

**Interfaces:**
- Produces: `void ParsedText::addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false, uint8_t sizePct = DEFAULT_WORD_SIZE_PCT, uint32_t visibleOffset = 0)`; `uint32_t ParsedText::lastLineVisibleOffset() const` (the first word's offset of the line most recently handed to `processLine`); `std::optional<uint32_t> ParsedText::firstWordVisibleOffset() const`. Task 3 uses all three.

- [ ] **Step 1: Write the failing test**

`test/epub_pipeline/ParsedTextOffsetTest.cpp`:
```cpp
// Each word a ParsedText holds knows where it begins in the chapter's visible text, and every
// line it hands out reports its first word's offset. That is what lets a page know where it
// starts (Section's content-offset LUT) without the layout engine keeping any text around.
#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "Epub/ParsedText.h"
#include "GfxRenderer.h"

namespace {

// Ten words, offsets 0, 10, 20, ... so a line's first-word offset identifies the word.
ParsedText tenWords() {
  ParsedText text(/*extraParagraphSpacing=*/false);
  for (int i = 0; i < 10; ++i) {
    text.addWord("word" + std::to_string(i), EpdFontFamily::REGULAR, false, false, ParsedText::DEFAULT_WORD_SIZE_PCT,
                 static_cast<uint32_t>(i * 10));
  }
  return text;
}

std::vector<uint32_t> lineOffsets(ParsedText& text, const int width) {
  GfxRenderer renderer;
  std::vector<uint32_t> offsets;
  text.layoutAndExtractLines(renderer, 0, static_cast<uint16_t>(width),
                             [&](std::unique_ptr<TextBlock>, bool, bool) {
                               offsets.push_back(text.lastLineVisibleOffset());
                               return ParsedText::LineProcessResult::Accepted;
                             });
  return offsets;
}

}  // namespace

TEST(ParsedTextOffset, TheFirstWordOfTheBlockIsWhereTheBlockStarts) {
  ParsedText text = tenWords();
  EXPECT_EQ(text.firstWordVisibleOffset(), std::optional<uint32_t>(0));
}

TEST(ParsedTextOffset, AnEmptyBlockHasNoOffset) {
  ParsedText text(false);
  EXPECT_EQ(text.firstWordVisibleOffset(), std::nullopt);
}

TEST(ParsedTextOffset, EveryLineReportsItsFirstWord) {
  ParsedText text = tenWords();
  // Narrow enough that the stub renderer breaks the ten words over several lines.
  const auto offsets = lineOffsets(text, 120);
  ASSERT_GT(offsets.size(), 1u);
  EXPECT_EQ(offsets.front(), 0u);
  for (size_t i = 1; i < offsets.size(); ++i) {
    EXPECT_GT(offsets[i], offsets[i - 1]);
    EXPECT_EQ(offsets[i] % 10, 0u) << "a line began at an offset no word has";
  }
}

TEST(ParsedTextOffset, ConsumedWordsTakeTheirOffsetsWithThem) {
  ParsedText text = tenWords();
  const auto offsets = lineOffsets(text, 120);
  // layoutAndExtractLines consumes every word it laid out except the last line's (which stays
  // for a continuation), so the block now starts at the last line's first word.
  ASSERT_GT(offsets.size(), 1u);
  EXPECT_EQ(text.firstWordVisibleOffset(), std::optional<uint32_t>(offsets.back()));
}

TEST(ParsedTextOffset, ResetForgetsTheOffsets) {
  ParsedText text = tenWords();
  text.reset(BlockStyle());
  EXPECT_EQ(text.firstWordVisibleOffset(), std::nullopt);
}
```

Add `ParsedTextOffsetTest.cpp` to the `EpubPipelineTest` sources in `test/epub_pipeline/CMakeLists.txt`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake .. > /dev/null && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3`
Expected: compile errors: `no matching function for call to 'ParsedText::addWord(...)'` (six arguments) and `'firstWordVisibleOffset' is not a member`.

- [ ] **Step 3: Add the vector and the accessors**

In `lib/Epub/Epub/ParsedText.h`, after `std::vector<uint8_t> wordSizes;` (line ~75):
```cpp
  // Where each word begins in the chapter's visible text (VisibleText.h's rule), in lockstep with
  // `words` through every insert/erase. The section cache records the first word of each page from
  // it (KOReader sync, content offsets). 4 B per word; grown with the other word vectors in one
  // step and counted in addWord's heap gate.
  std::vector<uint32_t> wordVisibleOffsets;
  // The first word's offset of the line most recently handed to processLine. Read by the parser's
  // line callbacks, which only get the TextBlock.
  uint32_t lastLineVisibleOffset_ = 0;
```
Add `#include <optional>` at the top. Change the `addWord` declaration (line 159) to:
```cpp
  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint8_t sizePct = DEFAULT_WORD_SIZE_PCT, uint32_t visibleOffset = 0);
```
After `size_t size() const { return words.size(); }` add:
```cpp
  uint32_t lastLineVisibleOffset() const { return lastLineVisibleOffset_; }
  std::optional<uint32_t> firstWordVisibleOffset() const {
    if (wordVisibleOffsets.empty()) return std::nullopt;
    return wordVisibleOffsets.front();
  }
```

- [ ] **Step 4: Maintain it at every site**

In `lib/Epub/Epub/ParsedText.cpp`:

`addWord` (line 195): add the parameter `const uint32_t visibleOffset` after `sizePct`; extend the capacity test with `|| wordVisibleOffsets.capacity() < requiredSize`; in the gate change `needed` to
```cpp
      const size_t needed = newCapacity * (sizeof(std::string) + sizeof(EpdFontFamily::Style) + sizeof(uint8_t) +
                                           sizeof(uint32_t)) +
                            newCapacity / 8 + 5 * ALLOC_HEADER_SLACK;
```
add `wordVisibleOffsets.reserve(newCapacity);` after `wordSizes.reserve(newCapacity);`, and `wordVisibleOffsets.push_back(visibleOffset);` after the `wordSizes.push_back(...)`.

preserveSource snapshot (line ~283): add `std::vector<uint32_t> savedOffsets;` and `savedOffsets = wordVisibleOffsets;` inside `if (preserveSource)`; restore (line ~523): `wordVisibleOffsets = std::move(savedOffsets);`.

Split merge (line ~441): add `wordVisibleOffsets.erase(wordVisibleOffsets.begin() + splitIndex + 1);`.

Consume (line ~514): add `wordVisibleOffsets.erase(wordVisibleOffsets.begin(), wordVisibleOffsets.begin() + consumed);`.

`reset` (line ~568): add `wordVisibleOffsets.clear();`.

Bionic (line ~899): the transformed suffix is built from the words at `suffixStart..`; before the `words.resize(suffixStart)` copy the suffix offsets, then re-append them one per transformed word:
```cpp
  // Each original word became one or two transformed words; both halves keep the word's offset.
  std::vector<uint32_t> transformedSuffixOffsets;
  transformedSuffixOffsets.reserve(transformedSuffix.size());
```
Find the loop that fills `transformedSuffix`/`transformedSuffixStyles` and, wherever it pushes a transformed word for source index `i`, push `wordVisibleOffsets[i]` to `transformedSuffixOffsets`. Then after `wordSizes.resize(suffixStart);` add `wordVisibleOffsets.resize(suffixStart);` and after the `wordSizes.insert(...)` add `wordVisibleOffsets.insert(wordVisibleOffsets.end(), transformedSuffixOffsets.begin(), transformedSuffixOffsets.end());`.

Hyphenation (line ~1166): after `wordSizes.insert(...)` add
```cpp
  // The remainder starts where the prefix's visible bytes end (the hyphen is not source text).
  wordVisibleOffsets.insert(wordVisibleOffsets.begin() + wordIndex + 1,
                            wordVisibleOffsets[wordIndex] +
                                static_cast<uint32_t>(VisibleText::visibleBytes(words[wordIndex].data(),
                                                                                 static_cast<int>(words[wordIndex].size())) -
                                                      (chosenNeedsHyphen ? 1 : 0)));
```
with `#include "Epub/VisibleText.h"` added at the top of the file.

`extractLine` (line ~1208): after `const size_t lastBreakAt = ...;` add
```cpp
  lastLineVisibleOffset_ = lastBreakAt < wordVisibleOffsets.size() ? wordVisibleOffsets[lastBreakAt] : 0;
```

Check `wordContinues.insert` in the hyphenation path and any other `wordStyles` site the grep `grep -n "wordStyles\." lib/Epub/Epub/ParsedText.cpp` lists; every one gets its `wordVisibleOffsets` twin.

- [ ] **Step 5: Run the tests**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3; ./epub_pipeline/EpubPipelineTest.exe --gtest_filter='ParsedTextOffset.*' 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]|Failure"`
Expected: `[  PASSED  ] 5 tests.` If `EveryLineReportsItsFirstWord` finds only one line, lower the width in `lineOffsets` until the stub renderer breaks the text (its advance is fixed per glyph; 120 px is below ten words).

- [ ] **Step 6: Run the whole suite: goldens must not move**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . -j 8 -- -k 0 2>&1 | grep -E "error:" | grep -v dlfcn; ctest -j 8 2>&1 | grep -E "tests passed|tests failed"`
Expected: `100% tests passed out of 1472`.

- [ ] **Step 7: Format and commit**

```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i lib/Epub/Epub/ParsedText.h lib/Epub/Epub/ParsedText.cpp test/epub_pipeline/ParsedTextOffsetTest.cpp
git add lib/Epub/Epub/ParsedText.h lib/Epub/Epub/ParsedText.cpp test/epub_pipeline/ParsedTextOffsetTest.cpp test/epub_pipeline/CMakeLists.txt
git commit -m "feat(epub): every word in a ParsedText knows its visible-text offset

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: The parser counts, the page records, the cache stores

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h:98-100,253-260,440-456,664-670,761-762`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`: `characterData` (2909), the drop-cap capture (2984-3010), the consume loop (3012-3167), `flushPartWordBuffer` (855-935), `emitPage` (987-996), `addLineToPage` (3798-3874), the three line lambdas (969, 4072, 4691), the eight `elements.push_back` sites (1156, 1322, 2288, 2679, 3874, 4405, 4435, 4647), `startElement` (1488) and `endElementBody` (3199), the synthetic-text sites (1263, 1302, 1316, 1344, 1944, 3039, 3437), `flushTableFragment` (4632), the row push (4276), every `emitPage(lastBodyChildByteOffset)` call and the `lastBodyChildByteOffset` assignment (2339)
- Modify: `lib/Epub/Epub/Section.cpp:34,135-141,1693-1712,2629-2742`; `lib/Epub/Epub/Section.h:360-384`
- Modify: `docs/file-formats.md:239-245`, `docs/contributing/section-indexing.md:88-95`
- Test: `test/epub_pipeline/VisibleTextOffsetLutTest.cpp`, `test/epub_pipeline/CMakeLists.txt`

**Interfaces:**
- Consumes: `VisibleText::visibleBytes/isSpace/isNonVisibleTag` (Task 1); `ParsedText::addWord(..., visibleOffset)`, `lastLineVisibleOffset()`, `firstWordVisibleOffset()` (Task 2).
- Produces: `std::optional<uint32_t> Section::getVisibleTextOffsetForPage(uint16_t page) const`; `std::optional<uint16_t> Section::getPageForVisibleTextOffset(uint32_t offset) const` (ties resolve to the first page of the run); `ChapterHtmlSlimParser::ParagraphLutEntry{uint32_t visibleTextOffset; uint16_t paragraphIndex; uint16_t listItemIndex}`; `SECTION_FILE_VERSION == 80`. Tasks 5–8 use the accessors.

- [ ] **Step 1: Write the failing tests**

`test/epub_pipeline/VisibleTextOffsetLutTest.cpp`:
```cpp
// The section cache's content-offset LUT: for every page, the number of visible bytes of the
// chapter's source text before the page's first element (VisibleText.h's rule, counted by the
// layout parser), and the lookup the reader uses to turn an offset back into a page.
//
// The layout parser and the KOReader XPath mappers count with the same rule over the same SAX
// stream, so for every chapter the last page's start must not exceed the mapper's own count of
// the chapter, and the starts must be in page order. Two pages can share a start (a page whose
// first element is an image, a rule or a table fragment, followed by the text at the same
// offset); the lookup then answers the FIRST page of the run, the one the reader reaches first.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "KOReaderSync/ChapterXPathIndexerInternal.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  return p;
}

std::vector<std::string> corpusBooks() {
  std::vector<std::string> books;
  for (const auto& entry : fs::directory_iterator(CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") books.push_back(entry.path().string());
  }
  std::sort(books.begin(), books.end());
  return books;
}

struct LutFixture : testing::TestWithParam<std::string> {
  fs::path work;
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void SetUp() override {
    std::string tag = testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char& c : tag) {
      if (c == '/') c = '_';
    }
    work = fs::temp_directory_path() / "visible_text_lut" / tag;
    fs::remove_all(work);
    fs::create_directories(work);
    epub = std::make_shared<Epub>(GetParam(), (work / "cache").string());
    ASSERT_TRUE(epub->load(true)) << GetParam();
    epub->setupCacheDir();
  }
  void TearDown() override {
    epub.reset();
    fs::remove_all(work);
  }

  std::unique_ptr<Section> build(const int spine) {
    auto section = std::make_unique<Section>(epub, spine, renderer);
    EXPECT_TRUE(section->createSectionFile(params(), {}, /*skipEviction=*/true)) << "spine " << spine;
    EXPECT_TRUE(section->loadSectionFile(params())) << "spine " << spine;
    return section;
  }
};

TEST_P(LutFixture, StartsAreInPageOrderAndWithinTheChapter) {
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine);
    const std::string tmp = ChapterXPathIndexerInternal::decompressToTempFile(epub, spine);
    ASSERT_FALSE(tmp.empty());
    const size_t total = ChapterXPathIndexerInternal::countTotalTextBytes(tmp);
    fs::remove(tmp);
    uint32_t previous = 0;
    for (uint16_t page = 0; page < section->pageCount; ++page) {
      const auto start = section->getVisibleTextOffsetForPage(page);
      ASSERT_TRUE(start.has_value()) << "spine " << spine << " page " << page;
      if (page == 0) EXPECT_EQ(*start, 0u) << "spine " << spine;
      EXPECT_GE(*start, previous) << "spine " << spine << " page " << page;
      EXPECT_LE(*start, total) << "spine " << spine << " page " << page;
      previous = *start;
    }
  }
}

TEST_P(LutFixture, EveryStartLooksUpItsOwnPage) {
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto section = build(spine);
    for (uint16_t page = 0; page < section->pageCount; ++page) {
      const uint32_t start = *section->getVisibleTextOffsetForPage(page);
      // The first page of a run of equal starts is the one an offset resolves to.
      uint16_t first = page;
      while (first > 0 && *section->getVisibleTextOffsetForPage(first - 1) == start) --first;
      EXPECT_EQ(section->getPageForVisibleTextOffset(start), std::optional<uint16_t>(first))
          << "spine " << spine << " page " << page << " start " << start;
      // An offset inside the page (one byte before the next start) resolves to the page itself
      // or to the last page of its run.
      if (page + 1 < section->pageCount) {
        const uint32_t next = *section->getVisibleTextOffsetForPage(page + 1);
        if (next > start) {
          EXPECT_EQ(section->getPageForVisibleTextOffset(next - 1), std::optional<uint16_t>(page))
              << "spine " << spine << " page " << page;
        }
      }
    }
  }
}

TEST_P(LutFixture, PageBeyondTheLutHasNoOffset) {
  const auto section = build(0);
  EXPECT_EQ(section->getVisibleTextOffsetForPage(section->pageCount), std::nullopt);
  EXPECT_EQ(section->getVisibleTextOffsetForPage(0xFFFF), std::nullopt);
}

INSTANTIATE_TEST_SUITE_P(Corpus, LutFixture, testing::ValuesIn(corpusBooks()),
                         [](const testing::TestParamInfo<std::string>& info) {
                           std::string name = fs::path(info.param).stem().string();
                           for (char& c : name) {
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           }
                           return name;
                         });

// --- Shapes the corpus does not pin ---------------------------------------------------------------

struct SyntheticLutFixture : testing::Test {
  fs::path work;
  GfxRenderer renderer;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("visible_text_lut_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::shared_ptr<Epub> book(const std::string& body) {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">lut</dc:identifier>"
            "<dc:title>LUT</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n"
            "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "</manifest>\n<spine><itemref idref=\"c\"/></spine>\n</package>\n");
    zip.add("chapter.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
            "<head><title>C</title></head><body>\n" +
                body + "\n</body></html>\n");
    const std::string path = (work / "book.epub").string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }

  std::unique_ptr<Section> build(const std::shared_ptr<Epub>& epub) {
    auto section = std::make_unique<Section>(epub, 0, renderer);
    EXPECT_TRUE(section->createSectionFile(params(), {}, true));
    EXPECT_TRUE(section->loadSectionFile(params()));
    return section;
  }

  static std::string paragraphs(const int count) {
    std::string out;
    for (int i = 1; i <= count; ++i) {
      out += "<p>Paragraph " + std::to_string(i) +
             " carries enough ordinary words to give it some weight on the page and more.</p>\n";
    }
    return out;
  }
};

TEST_F(SyntheticLutFixture, StyleTextInsideBodyIsNotCounted) {
  // The mapper skips <style> wherever it is; so must the parser, or the two drift apart by the
  // length of the stylesheet for every page after it.
  const auto plain = build(book(paragraphs(80)));
  const auto styled = build(book("<style>p { margin: 0 } " + std::string(400, 'x') + "</style>\n" + paragraphs(80)));
  ASSERT_EQ(plain->pageCount, styled->pageCount);
  for (uint16_t page = 0; page < plain->pageCount; ++page) {
    EXPECT_EQ(styled->getVisibleTextOffsetForPage(page), plain->getVisibleTextOffsetForPage(page)) << "page " << page;
  }
}

TEST_F(SyntheticLutFixture, HiddenTextIsCountedLikeTheMapperCounts) {
  // display:none text is not laid out but it IS source text: the mappers count it, so the LUT
  // must too, or a page after it would be reported too early by the hidden text's length.
  const auto plain = build(book(paragraphs(80)));
  const std::string hidden = "<p style=\"display:none\">" + std::string(300, 'h') + "</p>\n";
  const auto withHidden = build(book(hidden + paragraphs(80)));
  ASSERT_EQ(plain->pageCount, withHidden->pageCount);
  for (uint16_t page = 1; page < plain->pageCount; ++page) {
    EXPECT_EQ(*withHidden->getVisibleTextOffsetForPage(page), *plain->getVisibleTextOffsetForPage(page) + 300)
        << "page " << page;
  }
}

TEST_F(SyntheticLutFixture, TiesResolveToTheFirstPageOfTheRun) {
  // A page break forced before a paragraph gives two consecutive pages the same start only when
  // the first holds no text; a page-break rule (<hr>) before the paragraph makes a rule-only page.
  // The lookup must answer that page, the one the reader reaches first.
  const auto section = build(book(paragraphs(40) + "<hr/>\n" + paragraphs(40)));
  for (uint16_t page = 1; page < section->pageCount; ++page) {
    const uint32_t start = *section->getVisibleTextOffsetForPage(page);
    if (*section->getVisibleTextOffsetForPage(page - 1) == start) {
      EXPECT_LT(*section->getPageForVisibleTextOffset(start), page);
    }
  }
}
```

Add `VisibleTextOffsetLutTest.cpp` to the `EpubPipelineTest` sources.

- [ ] **Step 2: Run it to verify it fails**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake .. > /dev/null && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3`
Expected: compile error, `'getVisibleTextOffsetForPage' is not a member of 'Section'`.

- [ ] **Step 3: The LUT entry and the Section accessors**

`lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` (line ~451), replace the struct:
```cpp
  struct ParagraphLutEntry {
    uint32_t visibleTextOffset;  // visible bytes of source text before the page's first element
    uint16_t paragraphIndex;     // 1-based <p> index at page completion
    uint16_t listItemIndex;      // running <li> count at page completion (any depth)
  };
```
and delete the `lastBodyChildByteOffset` member and its comment (lines ~444-449).

`lib/Epub/Epub/Section.cpp`: line 34 becomes
```cpp
constexpr uint8_t SECTION_FILE_VERSION = 80;  // v80: the paragraph LUT entry carries the page's visible-text offset
```
(keep the older version notes below it as the file does). Lines 135-139: the comment becomes
```cpp
// On-disk paragraph LUT entry: u32 visibleTextOffset + u16 paragraphIndex + u16 listItemIndex.
// visibleTextOffset is the number of visible bytes (VisibleText.h) of the chapter's source text
// before the page's first element: KOReader sync pushes it and resolves pulled positions to a
// page with it. paragraphIndex and listItemIndex let <p>- and <li>-anchored XPaths snap to a
// page when a resolver had no offset.
```
LUT write (line ~1693): the comment becomes `// Write per-page paragraph LUT: count + array of {visibleTextOffset(u32), paragraphIndex(u16), listItemIndex(u16)}.` and `serialization::writePod(file, entry.xhtmlByteOffset);` becomes `serialization::writePod(file, entry.visibleTextOffset);`.

After `getPageForListItemIndex` (line ~2742) add:
```cpp
std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  FsFile f;
  uint16_t count = 0;
  uint32_t lutStart = 0;
  if (!readParagraphLutHeader(f, count, lutStart)) {
    return std::nullopt;
  }
  if (page >= count) {
    f.close();
    return std::nullopt;
  }
  const uint32_t entryOffset = paragraphLutEntryOffset(lutStart, page);
  if (static_cast<uint64_t>(entryOffset) + sizeof(uint32_t) > f.size()) {
    f.close();
    return std::nullopt;
  }
  f.seek(entryOffset);
  uint32_t offset;
  serialization::readPod(f, offset);
  f.close();
  return offset;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset) const {
  FsFile f;
  uint16_t count = 0;
  uint32_t lutStart = 0;
  if (!readParagraphLutHeader(f, count, lutStart)) {
    return std::nullopt;
  }
  const uint32_t fileSize = f.size();
  // Starts are in page order. The answer is the last page whose start is <= offset, except that
  // a run of pages sharing one start (an image, rule or table page followed by the text at the
  // same offset) answers its FIRST page: the one the reader reaches first.
  std::optional<uint16_t> found;
  uint32_t foundStart = 0;
  for (uint16_t i = 0; i < count; i++) {
    const uint32_t entryOffset = paragraphLutEntryOffset(lutStart, i);
    if (static_cast<uint64_t>(entryOffset) + sizeof(uint32_t) > fileSize) {
      break;
    }
    f.seek(entryOffset);
    uint32_t start;
    serialization::readPod(f, start);
    if (start > offset) {
      break;
    }
    if (!found || start > foundStart) {
      found = i;
      foundStart = start;
    }
  }
  f.close();
  return found;
}
```

`lib/Epub/Epub/Section.h`, after `getParagraphIndexForPage` (line ~380):
```cpp
  // The number of visible bytes of the chapter's source text before the page's first element
  // (VisibleText.h's rule, counted by the layout parser). What a KOReader sync pushes for the
  // page. nullopt if the LUT is unavailable or the page is beyond it (a truncated build).
  std::optional<uint32_t> getVisibleTextOffsetForPage(uint16_t page) const;
  // The page a visible-text offset falls on: the last page whose start is <= offset, with a run
  // of pages sharing one start answering its first page. How a pulled KOReader position becomes
  // a page. nullopt if the LUT is unavailable or empty.
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t offset) const;
```

- [ ] **Step 4: Build; the fixture tests now fail on values**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3; ./epub_pipeline/EpubPipelineTest.exe --gtest_filter='*LutFixture*' 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]" | head -3`
Expected: failures (the entry still holds the old byte offset, so starts exceed the totals and `StartsAreInPageOrderAndWithinTheChapter` fails). This is the RED for the parser change.

- [ ] **Step 5: The parser counts**

`lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`: add `#include "Epub/VisibleText.h"`; after `int partWordBufferIndex = 0;` (line ~99):
```cpp
  // KOReader sync content offsets (docs/superpowers/specs/2026-10-06-kosync-content-offset-design.md).
  // visibleTextOffset_ counts VisibleText.h's visible bytes of the source text the SAX parser has
  // delivered: inside <body>, outside head/script/style, before any layout decision. The mappers
  // count the same stream with the same rule, so a page's recorded start means the same to both.
  uint32_t visibleTextOffset_ = 0;
  int nonVisibleTextDepth_ = 0;         // > 0 inside head, script or style
  uint32_t partWordVisibleOffset_ = 0;  // offset of the byte that opened partWordBuffer
  uint32_t currentPageVisibleOffset_ = 0;
  bool currentPageVisibleOffsetSet_ = false;
  // Records the page's start from its first element; later calls on the same page are ignored.
  void noteElementOnPage(uint32_t offset);
  // The body of characterData: lays `s` out. `baseOffset` is the visible-text offset of s[0];
  // `source` says the bytes are chapter text (the running offset advances) rather than text the
  // parser made up (alt text, a footnote preview), whose words take baseOffset unchanged.
  static void consumeText(void* userData, const char* s, int len, uint32_t baseOffset, bool source);
```
In `struct PendingDropCap` add `uint32_t visibleOffset = 0;  // offset of the first captured byte`. In `struct TableFragmentPacker` add `uint32_t firstVisibleOffset = UINT32_MAX;  // start of the fragment's first data row`. Change `void emitPage(uint32_t xhtmlByteOffset);` to `void emitPage();` and `addLineToPage`'s declaration to
```cpp
  ParsedText::LineProcessResult addLineToPage(std::unique_ptr<TextBlock> line, bool lineEndsWithHyphenatedWord,
                                              bool suppressHyphenationRetry, uint32_t lineVisibleOffset);
```

`lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`:

(a) `characterData` (line 2909) becomes the SAX entry:
```cpp
void ChapterHtmlSlimParser::characterData(void* userData, const char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  const bool source = self->xpathBodyDepth >= 0 && self->nonVisibleTextDepth_ == 0;
  const uint32_t base = self->visibleTextOffset_;
  if (source) {
    self->visibleTextOffset_ += static_cast<uint32_t>(VisibleText::visibleBytes(s, len));
  }
  consumeText(userData, s, len, base, source);
}

void ChapterHtmlSlimParser::consumeText(void* userData, const char* s, const int len, const uint32_t baseOffset,
                                        const bool source) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  // Running offset of s[i]; advances only for source text, so synthetic words sit at baseOffset.
  uint32_t off = baseOffset;
```
and the rest of the old body follows unchanged except for the edits below.

(b) The drop-cap capture loop: at the top of its `for` body add
```cpp
      const uint32_t byteOff = off;
      if (source && !VisibleText::isSpace(static_cast<unsigned char>(s[i]))) off++;
```
before `if (isWhitespace(s[i])) continue;`; where the ignorable sequence is skipped (`i += ignorableLen - 1;`) add `if (source) off += ignorableLen - 1;` first; where the byte is captured add, before the append, `if (self->pendingDropCap_.textLen == 0) self->pendingDropCap_.visibleOffset = byteOff;`. The overflow re-dispatch becomes
```cpp
    consumeText(userData, self->pendingDropCap_.text, captured, self->pendingDropCap_.visibleOffset, source);
    consumeText(userData, s + i, len - i, byteOff, source);
    return;
```
(declare `uint32_t byteOff = off;` before the loop so it is in scope after it).

(c) The main loop: at the top of the `for` body add the same two lines (`byteOff`, the conditional `off++`). Every place that appends to an empty buffer records the word's start: in the ASCII fast path replace `self->partWordBuffer[self->partWordBufferIndex++] = s[i];` with
```cpp
      if (self->partWordBufferIndex == 0) self->partWordVisibleOffset_ = byteOff;
      self->partWordBuffer[self->partWordBufferIndex++] = s[i];
```
likewise the final generic append at the loop's end. The two no-break-space tokens set `self->partWordVisibleOffset_ = byteOff;` next to `self->partWordBufferIndex = 1;`. The skips that jump over source bytes add their count: after `i += ignorableLen - 1;` add `if (source) off += ignorableLen - 1;`; after `i++;  // Skip the second byte (0xA0)` add `if (source) off++;`; after `i += 2;  // Skip the remaining two bytes (0x80 0xAF)` add `if (source) off += 2;`; after the BOM's `i += 2;` add `if (source) off += 2;`. The `MAX_WORD_SIZE` overflow path keeps the saved continuation bytes as the start of a new word: after `self->partWordBufferIndex = overflow;` add `self->partWordVisibleOffset_ = byteOff - static_cast<uint32_t>(overflow);` (those bytes were counted at their own positions).

(d) `flushPartWordBuffer` passes the offset: both `addWord(partWordBuffer, fontStyle, false, nextWordContinues, effectiveSizePct)` calls gain a sixth argument `, partWordVisibleOffset_`. The synthetic words take the current counter: at 1263, 1302 and 1316 `currentTextBlock->addWord(pendingDropCap_.text, pendingDropCap_.style)` becomes `currentTextBlock->addWord(pendingDropCap_.text, pendingDropCap_.style, false, false, ParsedText::DEFAULT_WORD_SIZE_PCT, pendingDropCap_.visibleOffset)`; at 1344 the list marker gets `, false, false, ParsedText::DEFAULT_WORD_SIZE_PCT, visibleTextOffset_`; the `<pre>` blank-line space at ~3039 gets `, false, false, ParsedText::DEFAULT_WORD_SIZE_PCT, off`.

(e) The re-entrant callers: alt text (1944) becomes `self->consumeText(userData, alt.c_str(), alt.length(), self->visibleTextOffset_, /*source=*/false);`; the preview (3437) becomes `consumeText(self, preview.c_str(), static_cast<int>(preview.size()), self->visibleTextOffset_, /*source=*/false);`. `defaultHandlerExpand` keeps calling `characterData` (an expansion is source text and is counted there).

(f) Head/script/style depth: at the top of `startElement` (after `self` is obtained, before any return) add `if (VisibleText::isNonVisibleTag(name)) self->nonVisibleTextDepth_++;` and at the top of `endElementBody` add `if (VisibleText::isNonVisibleTag(name) && self->nonVisibleTextDepth_ > 0) self->nonVisibleTextDepth_--;`.

(g) The page's first element:
```cpp
void ChapterHtmlSlimParser::noteElementOnPage(const uint32_t offset) {
  if (currentPageVisibleOffsetSet_) return;
  currentPageVisibleOffset_ = offset;
  currentPageVisibleOffsetSet_ = true;
}
```
Call it immediately before each of the eight `currentPage->elements.push_back(...)`: in `addLineToPage` with `lineVisibleOffset`; in `flushTableFragment` with `packer.firstVisibleOffset != UINT32_MAX ? packer.firstVisibleOffset : visibleTextOffset_` (and set `packer.firstVisibleOffset = UINT32_MAX;` after the push); at the other six with `visibleTextOffset_` (prefix `self->` where the site is in a static callback). At the data-row push (line ~4276, `t.packer.rows.push_back(std::move(tr));`) add before it:
```cpp
  if (t.packer.firstVisibleOffset == UINT32_MAX) {
    for (const auto& cell : t.pendingRow.cells) {
      if (cell.text) {
        if (const auto first = cell.text->firstWordVisibleOffset()) {
          t.packer.firstVisibleOffset = std::min(t.packer.firstVisibleOffset, *first);
        }
      }
    }
  }
```

(h) `emitPage` (line 987) becomes
```cpp
void ChapterHtmlSlimParser::emitPage() {
  // A page with no noted element (nothing but spacing) starts where the parse is.
  const uint32_t start = currentPageVisibleOffsetSet_ ? currentPageVisibleOffset_ : visibleTextOffset_;
  paragraphLutPerPage.push_back({start, xpathParagraphIndex, xpathListItemIndex});
  currentPageVisibleOffsetSet_ = false;
```
followed by the old body. Every `emitPage(lastBodyChildByteOffset)` (run `grep -n "emitPage(" lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`) becomes `emitPage()`; delete `self->lastBodyChildByteOffset = self->saxParser_.byteOffset();` (line ~2339).

(i) `addLineToPage` gains the parameter `const uint32_t lineVisibleOffset` and the three lambdas pass it: at 969 and 4072 `addLineToPage(std::move(textBlock), lineEndsWithHyphenatedWord, suppressHyphenationRetry, currentTextBlock->lastLineVisibleOffset())`; at 4691 `addLineToPage(std::move(tb), lineEndsWithHyphen, suppressRetry, text->lastLineVisibleOffset())`.

- [ ] **Step 6: Run the LUT tests**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -5; ./epub_pipeline/EpubPipelineTest.exe --gtest_filter='*LutFixture*' 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]|Failure" | head -20`
Expected: `[  PASSED  ] 93 tests.` (30 books × 3 + 3 synthetic). A failure in `StartsAreInPageOrderAndWithinTheChapter` with a start above the total means a counted re-dispatch (check step (e)); a start below where it should be means a skipped-byte path missing its `off +=`.

- [ ] **Step 7: Run the whole suite: goldens must not move**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . -j 8 -- -k 0 2>&1 | grep -E "error:" | grep -v dlfcn; ctest -j 8 2>&1 | grep -E "tests passed|tests failed"`
Expected: `100% tests passed out of 1565`. If a golden moved, a layout decision changed: diff the dump and find the edit that touched layout rather than counting.

- [ ] **Step 8: Docs**

`docs/file-formats.md` (the ImHex comment, lines ~239-245):
```
    // One entry per page: visible-text offset of the page's first element, 1-based <p> sibling
    // index, running <li> count.
    // visibleTextOffset is the number of visible bytes (lib/Epub/Epub/VisibleText.h: non-whitespace
    // bytes of character data inside <body>, outside head/script/style) of the chapter's source
    // text before the page's first element. KOReader sync pushes it and resolves pulled
    // positions to a page with it. Two consecutive pages may share one offset (an image or rule
    // page followed by the text at that offset); a lookup answers the first of them.
    // paragraphIndex is 1-based, matching KOReader XPath p[N] convention; listItemIndex likewise
    // for li[N].
    struct ParagraphLutEntry { u32 visibleTextOffset; u16 paragraphIndex; u16 listItemIndex; };
```
and add to the version notes list: `Version 80 replaces the paragraph LUT entry's unused XHTML byte offset with the page's visible-text offset. Same size; caches rebuild on the version bump.`

`docs/contributing/section-indexing.md` (lines ~88-95): the entry becomes `{ u32 visibleTextOffset, u16 paragraphIndex, u16 listItemIndex } × count` and the paragraph:
```
One 8-byte entry per page. `visibleTextOffset` is the number of visible bytes of the chapter's source text before the page's first element, counted by the layout parser with the rule in `lib/Epub/Epub/VisibleText.h`, the same rule the KOReader XPath mappers count with; it is what a KOReader sync pushes for the page and how a pulled position becomes a page (`Section::getPageForVisibleTextOffset`). `paragraphIndex` is the 1-based `<p>` sibling count; `listItemIndex` is the running `<li>` count. They let incoming KOReader XPath strings (`p[N]`, `li[N]`) snap to a page when no offset could be resolved, and (`paragraphIndex` of a page and of the page before it) tell the sync whether a remote paragraph opens on the current page.
```

- [ ] **Step 9: Format and commit**

```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp lib/Epub/Epub/Section.h lib/Epub/Epub/Section.cpp test/epub_pipeline/VisibleTextOffsetLutTest.cpp
git add lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp lib/Epub/Epub/Section.h lib/Epub/Epub/Section.cpp test/epub_pipeline/VisibleTextOffsetLutTest.cpp test/epub_pipeline/CMakeLists.txt docs/file-formats.md docs/contributing/section-indexing.md
git commit -m "feat(epub): the section cache records each page's visible-text offset

Co-Authored-By: Claude <noreply@anthropic.com>"
```

- [ ] **Step 10: Device measurement (manual, X3)**

Build and flash; repeat Task 0 step 5's chapter builds on the same two books (the version bump rebuilds them) and record the same numbers in the baseline file under "After Task 3". If the body-child book's minimum free heap during the build is more than 2 KB below the baseline, stop and report before Task 4: the 16-bit-delta fallback from the spec is the next step, not more of this plan.

---

### Task 4: Stream the inflate into the SAX parser, stopping at the target

**Files:**
- Create: `lib/KOReaderSync/SaxFeedSink.h`
- Modify: `lib/ZipFile/ZipFile.h:102`, `lib/ZipFile/ZipFile.cpp:617-705`
- Modify: `lib/Epub/Epub.h:274,388`, `lib/Epub/Epub.cpp:1879-1945`
- Modify: `lib/KOReaderSync/ChapterXPathIndexerInternal.h`, `lib/KOReaderSync/ChapterXPathIndexerInternal.cpp:304-366,453-461`
- Modify: `lib/KOReaderSync/ChapterXPathForwardMapper.cpp:117-191`, `lib/KOReaderSync/ChapterXPathReverseMapper.cpp:229-294`
- Modify: `test/epub_pipeline/VisibleTextOffsetLutTest.cpp` (the count call), `test/zip_entry_reader/HalStorage.h` is untouched
- Test: `test/epub_pipeline/StreamedParseTest.cpp`, `test/epub_pipeline/CMakeLists.txt`

**Interfaces:**
- Produces: `class SaxFeedSink final : public Print` with `explicit SaxFeedSink(SaxParser&)`, `const bool* stopFlag() const`, `bool failed() const`; `bool ChapterXPathIndexerInternal::streamSpine(const std::shared_ptr<Epub>&, int spineIndex, SaxParser&)`; `size_t ChapterXPathIndexerInternal::countTotalTextBytes(const std::shared_ptr<Epub>&, int spineIndex)`; `bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize, const bool* stop = nullptr) const`; `bool ZipFile::readFileToStream(const char* filename, Print& out, size_t chunkSize, const bool* stop = nullptr)`.
- Removes: `decompressToTempFile`, `runParse`.

- [ ] **Step 1: Write the failing test**

`test/epub_pipeline/StreamedParseTest.cpp`:
```cpp
// The KOReader XPath mappers read a chapter straight from the inflate into the SAX parser: no
// temp file on the SD card, and when the parser has what it needs the inflate stops.
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "Epub.h"
#include "KOReaderSync/ChapterXPathIndexerInternal.h"
#include "KOReaderSync/SaxFeedSink.h"
#include "SaxParser/SaxParser.h"

namespace fs = std::filesystem;

namespace {

const std::string kBook = std::string(CORPUS_DIR) + "/test_spine_toc_edges.epub";
constexpr int kAppendixSpine = 12;  // 14 KB of XHTML

struct CountingState {
  size_t elements = 0;
  size_t stopAfter = 0;
  SaxParser* parser = nullptr;
};

void onStart(void* ud, const char*, const char**) {
  auto* s = static_cast<CountingState*>(ud);
  if (++s->elements >= s->stopAfter && s->stopAfter > 0) s->parser->stop();
}
void onEnd(void*, const char*) {}

struct StreamedParseFixture : testing::Test {
  fs::path work;
  std::shared_ptr<Epub> epub;

  void SetUp() override {
    work = fs::temp_directory_path() / "streamed_parse";
    fs::remove_all(work);
    fs::create_directories(work);
    epub = std::make_shared<Epub>(kBook, (work / "cache").string());
    ASSERT_TRUE(epub->load(true));
    epub->setupCacheDir();
  }
  void TearDown() override {
    epub.reset();
    fs::remove_all(work);
  }
};

}  // namespace

TEST_F(StreamedParseFixture, AWholeChapterStreamsThroughWithoutATempFile) {
  CountingState state;
  SaxParser parser;
  ASSERT_TRUE(parser.init(&state, onStart, onEnd));
  state.parser = &parser;
  EXPECT_TRUE(ChapterXPathIndexerInternal::streamSpine(epub, kAppendixSpine, parser));
  EXPECT_GT(state.elements, 20u);
  EXPECT_FALSE(fs::exists(fs::path(epub->getCachePath()) / ".tmp_kox_12.html"));
}

TEST_F(StreamedParseFixture, AStoppedParserStopsTheInflate) {
  CountingState state;
  state.stopAfter = 3;
  SaxParser parser;
  ASSERT_TRUE(parser.init(&state, onStart, onEnd));
  state.parser = &parser;
  // Count the bytes the sink was handed: a sink that stops is not fed the whole chapter.
  struct CountingSink final : Print {
    SaxFeedSink inner;
    size_t bytes = 0;
    explicit CountingSink(SaxParser& p) : inner(p) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* buf, size_t n) override {
      bytes += n;
      return inner.write(buf, n);
    }
  } sink(parser);
  const auto href = epub->getSpineItem(kAppendixSpine).href;
  EXPECT_TRUE(epub->readItemContentsToStream(href, sink, 1024, sink.inner.stopFlag()));
  EXPECT_TRUE(parser.isStopped());
  EXPECT_EQ(state.elements, 3u);
  EXPECT_LT(sink.bytes, 4096u) << "the inflate ran on after the parser stopped";
}

TEST_F(StreamedParseFixture, CountingStreamsToo) {
  EXPECT_GT(ChapterXPathIndexerInternal::countTotalTextBytes(epub, kAppendixSpine), 10000u);
  EXPECT_EQ(ChapterXPathIndexerInternal::countTotalTextBytes(epub, 99), 0u);
}
```
Add `StreamedParseTest.cpp` to the `EpubPipelineTest` sources.

- [ ] **Step 2: Run it to verify it fails**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake .. > /dev/null && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3`
Expected: compile error, `KOReaderSync/SaxFeedSink.h: No such file or directory`.

- [ ] **Step 3: The sink**

`lib/KOReaderSync/SaxFeedSink.h`:
```cpp
#pragma once

#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <cstddef>
#include <cstdint>

// Hands inflate output straight to a SaxParser. The XPath mappers used to inflate a chapter to a
// temp file on the SD card and parse that; this is the same parse without the file, and it tells
// the inflate to stop as soon as the parser does (the forward mapper stops at its target).
//
// Stack object, no allocation: a reference, two flags. The parser's own state is the caller's.
class SaxFeedSink final : public Print {
 public:
  explicit SaxFeedSink(SaxParser& parser) : parser_(parser) {}

  size_t write(const uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, const size_t n) override {
    if (stopped_) {
      return n;  // drained, not parsed: the stream loop honours stopFlag() and ends
    }
    if (!parser_.feed(buf, n)) {
      failed_ = !parser_.isStopped();
      stopped_ = true;
    } else if (parser_.isStopped()) {
      stopped_ = true;
    }
    return n;
  }

  // For Epub::readItemContentsToStream: set once the parser stopped or failed.
  const bool* stopFlag() const { return &stopped_; }
  bool failed() const { return failed_; }

 private:
  SaxParser& parser_;
  bool stopped_ = false;
  bool failed_ = false;
};
```

- [ ] **Step 4: The stop flag through the two inflate loops**

`lib/ZipFile/ZipFile.h:102`: `bool readFileToStream(const char* filename, Print& out, size_t chunkSize, const bool* stop = nullptr);`. In `ZipFile.cpp`, the signature gains `const bool* stop`; in the STORED loop after `remaining -= dataRead;` add `if (stop && *stop) break;` (the `free(buffer); return true;` after the loop handles it); in the DEFLATED loop, right after the `out.write(outputBuffer, produced)` block's success path, add
```cpp
        if (stop && *stop) {
          success = true;
          break;
        }
```
(check the loop's exit convention just below it: `success` is what the function returns after freeing the buffers; keep it so.)

`lib/Epub/Epub.h`: `bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize, const bool* stop = nullptr) const;` and `bool readItemContentsToStreamWithArena(const std::string& itemHref, Print& out, BuildArena* arena, const bool* stop) const;`. In `Epub.cpp` pass `stop` through (`readItemContentsToStreamWithArena(itemHref, out, loadScratch_, stop)` and `zip.readFileToStream(path.c_str(), out, chunkSize, stop)`), and in the arena loop after the write add `if (stop && *stop) return true;`.

- [ ] **Step 5: The mappers stream**

`lib/KOReaderSync/ChapterXPathIndexerInternal.h`: replace the `decompressToTempFile` and `runParse` declarations with
```cpp
// Inflate the spine item straight into `saxParser`. True when the parse completed or the parser
// stopped itself; false on a missing item or a parse error.
bool streamSpine(const std::shared_ptr<Epub>& epub, int spineIndex, SaxParser& saxParser);
```
and `size_t countTotalTextBytes(const std::string& tmpPath);` with `size_t countTotalTextBytes(const std::shared_ptr<Epub>& epub, int spineIndex);`.

`ChapterXPathIndexerInternal.cpp`: delete `decompressToTempFile`, the anonymous `pumpSaxParserFromFile` and `runParse`; add `#include "SaxFeedSink.h"` and
```cpp
bool streamSpine(const std::shared_ptr<Epub>& epub, const int spineIndex, SaxParser& saxParser) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return false;
  }
  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return false;
  }
  SaxFeedSink sink(saxParser);
  if (!epub->readItemContentsToStream(href, sink, 1024, sink.stopFlag())) {
    LOG_ERR("KOX", "Failed to stream spine=%d", spineIndex);
    return false;
  }
  if (sink.failed()) {
    return false;
  }
  return saxParser.isStopped() || saxParser.finalize();
}
```
`countTotalTextBytes` becomes
```cpp
size_t countTotalTextBytes(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  ByteCounter state;
  SaxParser saxParser;
  if (!saxParser.init(&state, bcStart, bcEnd, bcChar, bcDefault)) {
    return 0;
  }
  return streamSpine(epub, spineIndex, saxParser) ? state.totalTextBytes : 0;
}
```
`ChapterXPathForwardMapper.cpp`: `getTotalTextBytesCached(epub, spineIndex)` drops the `tmpPath` parameter and calls `countTotalTextBytes(epub, spineIndex)`; `findXPathForProgressInternal` loses the `decompressToTempFile`/`Storage.remove` lines and replaces `runParse(saxParser, tmpPath)` with `streamSpine(epub, spineIndex, saxParser)`; `<HalStorage.h>` is no longer needed there. `ChapterXPathReverseMapper.cpp` likewise: `findProgressForXPathInternal` drops the temp file, `const bool parseOk = streamSpine(epub, spineIndex, saxParser);`, and the two `Storage.remove` calls go.

`test/epub_pipeline/VisibleTextOffsetLutTest.cpp`: replace the three temp-file lines with `const size_t total = ChapterXPathIndexerInternal::countTotalTextBytes(epub, spine);`.

- [ ] **Step 6: Run the tests**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -5; ./epub_pipeline/EpubPipelineTest.exe --gtest_filter='StreamedParse*:ProgressMapperFixture.*:Corpus/*:*LutFixture*' 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]"`
Expected: all pass (the corpus round trip's allowances still hold; nothing about placement changed yet).

- [ ] **Step 7: Full suite, format, commit**

Run the full suite as in Task 3 step 7 (expected 1568 passing), then:
```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i lib/KOReaderSync/SaxFeedSink.h lib/ZipFile/ZipFile.h lib/ZipFile/ZipFile.cpp lib/Epub/Epub.h lib/Epub/Epub.cpp lib/KOReaderSync/ChapterXPathIndexerInternal.h lib/KOReaderSync/ChapterXPathIndexerInternal.cpp lib/KOReaderSync/ChapterXPathForwardMapper.cpp lib/KOReaderSync/ChapterXPathReverseMapper.cpp test/epub_pipeline/StreamedParseTest.cpp test/epub_pipeline/VisibleTextOffsetLutTest.cpp
git add -A lib/KOReaderSync lib/ZipFile lib/Epub/Epub.h lib/Epub/Epub.cpp test/epub_pipeline/StreamedParseTest.cpp test/epub_pipeline/VisibleTextOffsetLutTest.cpp test/epub_pipeline/CMakeLists.txt
git commit -m "perf(kosync): stream a chapter into the XPath mappers and stop at the target

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Push by offset, with text points

**Files:**
- Modify: `lib/KOReaderSync/CrossPointPosition.h`
- Modify: `lib/KOReaderSync/ChapterXPathIndexerState.h:16-21,41-79` (`StackNode`, push/pop)
- Modify: `lib/KOReaderSync/ChapterXPathForwardMapper.h`, `lib/KOReaderSync/ChapterXPathForwardMapper.cpp:41-115,152-191`
- Modify: `lib/KOReaderSync/ChapterXPathIndexer.h`, `lib/KOReaderSync/ChapterXPathIndexer.cpp`
- Modify: `lib/KOReaderSync/ProgressMapper.cpp` (`toKOReader`)
- Modify: `src/activities/reader/EpubReaderAutoSync.cpp:45-60` (`currentKoPosition`)
- Test: `test/epub_pipeline/ProgressMapperTest.cpp`

**Interfaces:**
- Consumes: `Section::getVisibleTextOffsetForPage` (Task 3), `streamSpine` (Task 4).
- Produces: `CrossPointPosition::visibleTextOffset` (`uint32_t`) and `hasVisibleTextOffset` (`bool`); `std::string ChapterXPathIndexer::findXPathForVisibleOffset(const std::shared_ptr<Epub>&, int spineIndex, uint32_t visibleOffset)`. Task 6 reads the position fields; Task 7 fills them for the sync screen.

- [ ] **Step 1: Write the failing tests**

Append to `test/epub_pipeline/ProgressMapperTest.cpp`, before `}  // namespace`, with `#include "Epub/Section.h"` and `#include "GfxRenderer.h"` added at the top:
```cpp
// --- Push by content offset -----------------------------------------------------------------------
//
// With the chapter laid out, a page's start is a visible-text offset in the section cache. The
// push names the text at that offset to the character, in the shape KOReader itself emits.

struct OffsetPushFixture : ProgressMapperFixture {
  GfxRenderer renderer;

  std::unique_ptr<Section> build(const int spine) {
    Section::BuildParams p;
    p.viewportWidth = 480;
    p.viewportHeight = 800;
    p.fontSizeNormalization = false;
    auto section = std::make_unique<Section>(epub, spine, renderer);
    EXPECT_TRUE(section->createSectionFile(p, {}, true));
    EXPECT_TRUE(section->loadSectionFile(p));
    return section;
  }

  CrossPointPosition pageWithOffset(const Section& section, const int spine, const int page) const {
    CrossPointPosition pos{};
    pos.spineIndex = spine;
    pos.pageNumber = page;
    pos.totalPages = section.pageCount;
    if (const auto off = section.getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) {
      pos.visibleTextOffset = *off;
      pos.hasVisibleTextOffset = true;
    }
    return pos;
  }
};

TEST_F(OffsetPushFixture, AMidParagraphPageStartPushesAsATextPointAndPullsBackToItsOffset) {
  const auto section = build(kSectionSpine);
  ASSERT_GT(section->pageCount, 3);
  // 60 equal paragraphs over ~40 pages: page 2 starts inside a paragraph, not at its first word.
  const auto pos = pageWithOffset(*section, kSectionSpine, 2);
  ASSERT_TRUE(pos.hasVisibleTextOffset);

  const auto ko = ProgressMapper::toKOReader(epub, pos);
  EXPECT_NE(ko.xpath.find("/section[1]/p["), std::string::npos) << ko.xpath;
  EXPECT_NE(ko.xpath.find("/text()[1]."), std::string::npos) << ko.xpath;

  const auto back = ProgressMapper::toCrossPoint(epub, ko, kSectionSpine, section->pageCount);
  ASSERT_TRUE(back.hasVisibleTextOffset);
  EXPECT_EQ(back.visibleTextOffset, pos.visibleTextOffset);
}

TEST_F(OffsetPushFixture, TheChapterStartPushesAsOffsetZeroOfItsFirstText) {
  const auto section = build(kSectionSpine);
  const auto ko = ProgressMapper::toKOReader(epub, pageWithOffset(*section, kSectionSpine, 0));
  // The heading's text sits inside inline elements (span/a/span): the element path, as today.
  EXPECT_EQ(ko.xpath, "/body/DocFragment[2]/body/section[1]/header[1]/hgroup[1]/h1[1]/span[1]/a[1]/span[1]");
}

TEST_F(OffsetPushFixture, WithoutAnOffsetThePushUsesTheFraction) {
  CrossPointPosition pos{};
  pos.spineIndex = kSectionSpine;
  pos.pageNumber = 20;
  pos.totalPages = 40;
  const auto ko = ProgressMapper::toKOReader(epub, pos);
  EXPECT_EQ(ko.xpath.rfind("/body/DocFragment[2]/body/section[1]/p[", 0), 0u) << ko.xpath;
  EXPECT_EQ(ko.xpath.find("/text()"), std::string::npos) << ko.xpath;
}

```
For the entity case to mean anything, change the fixture's `ch12.xhtml` body to `paragraphs("Earlier", 40)` with the first paragraph ending in `&mdash;`: in `paragraphs()` add a parameter `const char* tail = ""` appended before `</p>` for `i == 1`, and build `ch12` with `paragraphs("Earlier", 40, " &mdash;")`. Then the entity test pushes page 1 of the built flat spine and asserts the pulled offset equals the pushed one (add it to the block above):
```cpp
TEST_F(OffsetPushFixture, EntityAtThePageStartRoundTrips) {
  const auto section = build(kFlatSpine);
  ASSERT_GT(section->pageCount, 1);
  const auto pos = pageWithOffset(*section, kFlatSpine, 1);
  ASSERT_TRUE(pos.hasVisibleTextOffset);
  const auto ko = ProgressMapper::toKOReader(epub, pos);
  const auto back = ProgressMapper::toCrossPoint(epub, ko, kFlatSpine, section->pageCount);
  ASSERT_TRUE(back.hasVisibleTextOffset);
  EXPECT_EQ(back.visibleTextOffset, pos.visibleTextOffset);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3`
Expected: compile error, `'struct CrossPointPosition' has no member named 'visibleTextOffset'`.

- [ ] **Step 3: The position fields and the forward mapper**

`lib/KOReaderSync/CrossPointPosition.h`, after `hasListItemIndex`:
```cpp
  // The page's start as a visible-text offset in the chapter (Section::getVisibleTextOffsetForPage;
  // VisibleText.h's rule). Exact: a push names the text at this offset to the character, a pull
  // resolves to the page this offset falls on. Absent when the chapter is not laid out or the
  // match was inexact.
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
```

`lib/KOReaderSync/ChapterXPathIndexerState.h`: `StackNode` gains
```cpp
  // Text-node bookkeeping for the element: how many text nodes (runs of character data between
  // child element boundaries) it has had, and the codepoints into the current one. The forward
  // mapper emits /text()[N].M from these; the reverse mapper counts the same way.
  int textNodeCount = 0;
  size_t codepointsInTextNode = 0;
  bool inTextNode = false;
```
In `pushElement`, before `StackNode& node = stack.emplace_back();` add `if (!stack.empty()) stack.back().inTextNode = false;`; in `popElement`, after `stack.pop_back();` add `if (!stack.empty()) stack.back().inTextNode = false;`.

`lib/KOReaderSync/ChapterXPathForwardMapper.h`: add
```cpp
// The XPath of the text at `visibleOffset` (VisibleText.h's count from the chapter's start).
// Text that is a direct child of a block element is named to the codepoint, /text()[N].M; text
// inside an inline element is named by the element. Empty when the offset is past the chapter's
// text or the parse fails.
std::string findXPathForVisibleOffsetInternal(const std::shared_ptr<Epub>& epub, int spineIndex,
                                              uint32_t visibleOffset);
```
`ChapterXPathForwardMapper.cpp`: replace the `ForwardState` struct and its header comment with
```cpp
// Forward mapper: the XPath of the text at a visible-byte offset.
//
// Where the cursor lands in text that is a direct child of a BLOCK element (p, li, headings,
// div, td, ...) the result is .../block[K]/text()[N].M: N the text node within the block, M the
// codepoint within the node. That is the shape KOReader emits itself, and the shape our reverse
// mapper resolves at its text-node-exact tier. Inside an inline element (em, span, a, ...) the
// element path is emitted: crengine merges and renumbers inline runs, and the deep text-point
// forms of 1.43 did not survive it (see git history of this file).
namespace {

bool isBlockTag(const std::string& tag) {
  static constexpr const char* kBlocks[] = {"p",   "li",         "h1",  "h2",      "h3",      "h4",  "h5",   "h6",
                                            "div", "td",         "th",  "blockquote", "dd",   "dt",  "figcaption",
                                            "pre", "section",    "article", "body"};
  for (const char* b : kBlocks) {
    if (tag == b) return true;
  }
  return false;
}

struct ForwardState : StackState {
  int spineIndex;
  size_t targetOffset;
  // The fraction path's target can equal the chapter's total (intra 1.0) and must name the chunk
  // that ENDS there; a page's start is a byte of text and must name the chunk that CONTAINS it.
  bool inclusive = true;
  std::string result;
  bool found = false;
  SaxParser* saxParser = nullptr;

  ForwardState(const int spineIndex, const size_t targetOffset) : spineIndex(spineIndex), targetOffset(targetOffset) {}

  void onStartElement(const char* rawName) { pushElement(rawName); }
  void onEndElement() { popElement(); }

  void onCharData(const char* text, const int len) {
    if (shouldSkipText(len) || found || stack.empty()) {
      return;
    }
    StackNode& parent = stack.back();
    if (!parent.inTextNode) {
      parent.inTextNode = true;
      parent.textNodeCount++;
      parent.codepointsInTextNode = 0;
    }
    if (isWhitespaceOnly(text, len)) {
      parent.codepointsInTextNode += countUtf8Codepoints(text, len);
      return;
    }
    const size_t visible = countVisibleBytes(text, len);
    const bool reached = inclusive ? totalTextBytes + visible >= targetOffset : totalTextBytes + visible > targetOffset;
    if (reached) {
      // The target is inside this chunk (totalTextBytes <= target < totalTextBytes + visible).
      if (isBlockTag(parent.tag)) {
        const size_t targetVisibleByteInChunk = targetOffset - totalTextBytes;
        const size_t cpInChunk = codepointAtVisibleByte(text, len, targetVisibleByteInChunk);
        result = currentXPath(spineIndex) + "/text()[" + std::to_string(parent.textNodeCount) + "]." +
                 std::to_string(parent.codepointsInTextNode + cpInChunk);
      } else {
        result = currentXPath(spineIndex);
      }
      found = true;
      if (saxParser) saxParser->stop();
      return;
    }
    totalTextBytes += visible;
    parent.codepointsInTextNode += countUtf8Codepoints(text, len);
  }
};
```
Add the new entry point after `findXPathForProgressInternal`:
```cpp
std::string findXPathForVisibleOffsetInternal(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                              const uint32_t visibleOffset) {
  ForwardState state(spineIndex, visibleOffset);
  state.inclusive = false;
  SaxParser saxParser;
  if (!saxParser.init(&state, parserStartCb<ForwardState>, parserEndCb<ForwardState>, parserCharCb<ForwardState>,
                      parserDefaultCb<ForwardState>)) {
    return "";
  }
  state.saxParser = &saxParser;
  streamSpine(epub, spineIndex, saxParser);
  LOG_DBG("KOX", "Forward: spine=%d offset=%u -> %s", spineIndex, visibleOffset,
          state.result.empty() ? "(not found)" : state.result.c_str());
  return state.result;
}
```
and in `findXPathForProgressInternal` set `state.inclusive = true;` after constructing the state. The `bodyTextNodeCount`/`codepointsInBodyTextNode`/`inBodyTextNode` members and their handling go: the per-element bookkeeping replaces them, and for body-level text the parent IS `body`, a block tag, so the emitted form is unchanged.

`ChapterXPathIndexer.h/.cpp`: add
```cpp
  /**
   * The XPath of the text at a visible-text offset (Section::getVisibleTextOffsetForPage). Text
   * that is a direct child of a block element is named to the codepoint (/text()[N].M); text
   * inside an inline element by its element. Empty on failure.
   */
  static std::string findXPathForVisibleOffset(const std::shared_ptr<Epub>& epub, int spineIndex,
                                               uint32_t visibleOffset);
```
delegating to `findXPathForVisibleOffsetInternal`.

`ProgressMapper.cpp`, `toKOReader`: replace the `result.xpath = ChapterXPathIndexer::findXPathForProgress(...)` block with
```cpp
  // The page's content offset names the text to the character; the byte fraction is the fallback
  // for a position without one (the finished-book sentinel, a chapter not laid out yet).
  if (pos.hasVisibleTextOffset) {
    result.xpath = ChapterXPathIndexer::findXPathForVisibleOffset(epub, pos.spineIndex, pos.visibleTextOffset);
  }
  if (result.xpath.empty()) {
    result.xpath = ChapterXPathIndexer::findXPathForProgress(epub, pos.spineIndex, intraSpineProgress);
  }
  if (result.xpath.empty()) {
    result.xpath = generateXPath(pos.spineIndex);
  }
```
and extend the DBG line with `off=%u/%d` (`pos.visibleTextOffset`, `pos.hasVisibleTextOffset`).

`src/activities/reader/EpubReaderAutoSync.cpp`, `currentKoPosition`: inside `if (section)`, before the paragraph lookup, add
```cpp
    if (const auto off = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) {
      pos.visibleTextOffset = *off;
      pos.hasVisibleTextOffset = true;
    }
```

- [ ] **Step 4: Run the tests**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -5; ./epub_pipeline/EpubPipelineTest.exe --gtest_filter='OffsetPushFixture.*:ProgressMapperFixture.*:XPath*' 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]|Failure|xpath"`
Expected: all pass. `AMidParagraphPageStart...` needs Task 6's pull-side offset to assert `back.hasVisibleTextOffset`; until then expect exactly that assertion to fail and the others to pass, and carry it as the RED of Task 6. (The reverse mapper's text-node-exact tier must already resolve the emitted `/text()[1].M` form; if the XPath shape assertion fails, the forward emission is wrong, not the pull.)

- [ ] **Step 5: Full suite, format, commit**

Full suite as before (the one carried-over failure is expected until Task 6; everything else green), then:
```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i lib/KOReaderSync/CrossPointPosition.h lib/KOReaderSync/ChapterXPathIndexerState.h lib/KOReaderSync/ChapterXPathForwardMapper.h lib/KOReaderSync/ChapterXPathForwardMapper.cpp lib/KOReaderSync/ChapterXPathIndexer.h lib/KOReaderSync/ChapterXPathIndexer.cpp lib/KOReaderSync/ProgressMapper.cpp src/activities/reader/EpubReaderAutoSync.cpp test/epub_pipeline/ProgressMapperTest.cpp
git add lib/KOReaderSync src/activities/reader/EpubReaderAutoSync.cpp test/epub_pipeline/ProgressMapperTest.cpp
git commit -m "feat(kosync): push the page's content offset as a text point

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5b: The text-node counter follows crengine's DOM

**Why this task exists.** Task 5's push emits `/p[K]/text()[N].M`, and the pull resolves the same form. Both counted N and M the way our own SAX stream looks, not the way KOReader's crengine builds its DOM. Verified in `koreader/crengine` source (`crengine/src/lvtinydom.cpp`, `lvxml.cpp`) on 2026-10-06:

- **R1** `ldomElementWriter::onText`: the first whitespace-only text run of a non-`pre` block element (`_isBlock && childCount==0 && IsEmptySpace`) is dropped at parse time. `<p>\n  <em>x</em> rest</p>` has ONE text node in crengine: `" rest"` is `text()[1]`. We numbered it `text()[2]`. 293 of 3,733 `<p>` in the test corpus (moby-dick: 287 of 2,774) hit this.
- **R2** `PreProcessXmlString` (non-pre): CR, LF and TAB become spaces, and a run of spaces is stored as ONE space (the first one, even at node start; the EPUB writer sets no TXTFLG_TRIM, so nothing is trimmed). `.M` indexes this stored, collapsed text in codepoints. `<p>Hello\n      world</p>` stores `"Hello world"`: `w` is at M=6, not 12. 3,211 of 3,733 corpus `<p>` are hard-wrapped.
- **R3** `ldomNode::removeStandaloneWhitespaceTextChildrenInMixedContent`: when an element has BOTH block children and inline content, its whitespace-only text nodes are deleted unless they sit between two inline-ish siblings (an inline element or a text node). Elements with only inline content (an ordinary `<p>`) are untouched.
- **R4** `createXPointerV1/V2`: a `text()[N]` that does not exist yields a null XPointer, and `LVDocView::getBookmarkPage(null)` returns 0. A push with N one too high sends the KOReader user to the FIRST PAGE OF THE BOOK.
- **R5** `<pre>` (TXTFLG_PRE, inherited by descendants): R1 and R2 do not apply; text is stored raw.
- **R6** (accepted gap) a comment splits a text node in crengine; our yxml SAX reports no comment event. Not handled.
- Whitespace for R1-R3 is crengine's `IsEmptySpace` set: space, CR, LF, TAB only. NBSP (U+00A0, also from `&nbsp;`) is a visible character to crengine and to us.

One counter, in `StackState`, used by both mappers, replaces the two private copies (forward `StackNode` fields, reverse `inParentTextNode`/`currentTextNodeCount`/`codepointsInCurrentTextNode`). Host tests pin the rules above and our own round trip; only a push to a live KOReader can pin crengine itself, so the PR checklist (Task 8) gets a device step: push from a pretty-printed book (moby-dick) and confirm KOReader lands mid-paragraph.

**Files:**
- Modify: `lib/KOReaderSync/ChapterXPathIndexerState.h` (`StackNode`, `StackState::pushElement/popElement`, new `onTextChunk`/`closeTextRun`)
- Modify: `lib/KOReaderSync/ChapterXPathIndexerInternal.h`, `lib/KOReaderSync/ChapterXPathIndexerInternal.cpp` (collapsed-codepoint helpers, `isBlockTag` moves here)
- Modify: `lib/KOReaderSync/ChapterXPathForwardMapper.cpp` (`ForwardState` uses the shared counter; header comment states the rules)
- Modify: `lib/KOReaderSync/ChapterXPathReverseMapper.cpp` (`ReverseState` uses the shared counter; its three private fields go)
- Create: `test/epub_pipeline/SyntheticBook.h` (the one-chapter STORED book builder, extracted from `VisibleTextOffsetLutTest.cpp`'s `book()`)
- Modify: `test/epub_pipeline/VisibleTextOffsetLutTest.cpp` (`book()` calls the extracted builder)
- Create: `test/epub_pipeline/TextNodeRulesTest.cpp` (registered the same way `VisibleTextOffsetLutTest.cpp` is in the test CMake list)
- Modify: `test/epub_pipeline/ProgressMapperRoundTripTest.cpp` (text-point exactness invariant)
- Modify: `test/epub_pipeline/ProgressMapperTest.cpp` (Task 5's three expected `.M` values, recomputed under R2 and justified by hand in the commit message)

**Interfaces:**
- Consumes: `isWhitespaceOnly`, `countVisibleBytes`, `isEntityRef`, `lookupHtmlEntity`, `VisibleText::isSpace` (Task 1); `ChapterXPathIndexer::findXPathForVisibleOffset` (Task 5); `ProgressMapper::toCrossPoint` filling `CrossPointPosition::visibleTextOffset` for exact matches (Task 6).
- Produces: `StackState::onTextChunk`, `StackState::closeTextRun`, `isBlockTag` (shared), `collapsedCodepoints`, `collapsedCodepointAtVisibleByte`, `visibleBytesBeforeCollapsedCodepoint`. Deletes `countUtf8Codepoints`, `codepointAtVisibleByte`, `visibleBytesBeforeCodepoint` once nothing calls them (grep first; only the two mappers do today).

**Heap discipline:** `StackNode` grows by five `bool`s, laid out together after the integers (no padding waste); the stack is under 40 deep. `isBlockTag` is a `static constexpr` table. No new allocations; the per-chunk `std::string` work that exists today in the reverse mapper's text-node branch is out of scope (note it in the report if you see it).

- [ ] **Step 1: Extract the synthetic book builder**

Create `test/epub_pipeline/SyntheticBook.h`:
```cpp
#pragma once
// A one-chapter EPUB built from a <body> fragment: STORED zip at work/book<index>.epub, loaded,
// cache directory set up. One file per book: the cache is keyed on the path.
#include <Epub/Epub.h>

#include <filesystem>
#include <memory>
#include <string>

#include "StoredZipWriter.h"

inline std::shared_ptr<Epub> syntheticBook(const std::filesystem::path& work, const int index,
                                           const std::string& body) {
  // body of VisibleTextOffsetLutTest's book(), verbatim: mimetype, container.xml, content.opf,
  // chapter.xhtml = "<?xml ...?><html ...><head><title>C</title></head><body>\n" + body + "\n</body></html>\n"
  // then zip.write(path); Epub(path, work/"cache"); load(true); setupCacheDir(); return it.
}
```
Move the body of `VisibleTextOffsetLutTest.cpp`'s `book()` into it unchanged (same OPF, same chapter wrapper, same `EXPECT_TRUE(epub->load(true))`), and make `book()` a one-line call: `return syntheticBook(work, bookCount++, body);`. Run `EpubPipelineTest --gtest_filter='*LutFixture*'`: unchanged results.

- [ ] **Step 2: Write the failing rule tests**

Create `test/epub_pipeline/TextNodeRulesTest.cpp`:
```cpp
// The text() numbering and .M offsets we push and pull follow crengine's DOM, not our SAX stream.
// Rules R1-R5 and the crengine functions they come from are in ChapterXPathForwardMapper.cpp's
// header comment. Offsets below are visible bytes before the target word (Task 1's rule: every
// non-whitespace byte counts; NBSP from &nbsp; is two).
#include <KOReaderSync/ChapterXPathIndexer.h>
#include <KOReaderSync/ProgressMapper.h>
#include <gtest/gtest.h>

#include <filesystem>

#include "SyntheticBook.h"

namespace {
namespace fs = std::filesystem;

struct TextNodeRules : testing::Test {
  fs::path work;
  int bookCount = 0;
  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("text_node_rules_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::string push(const std::string& body, const uint32_t offset) {
    return ChapterXPathIndexer::findXPathForVisibleOffset(syntheticBook(work, bookCount++, body), 0, offset);
  }
  // The visible offset the pull lands on for a KOReader text point, or UINT32_MAX when inexact.
  uint32_t pull(const std::string& body, const std::string& xpath) {
    const auto pos = ProgressMapper::toCrossPoint(syntheticBook(work, bookCount++, body), KOReaderPosition{xpath, 0.5f});
    return pos.hasVisibleTextOffset ? pos.visibleTextOffset : UINT32_MAX;
  }
};

const std::string kP = "/body/DocFragment[1]/body/";

// R1: the leading whitespace of a block is not a text node.
TEST_F(TextNodeRules, LeadingWhitespaceOfABlockIsNotANode) {
  EXPECT_EQ(push("<p>\n  <em>x</em> rest of it</p>", 1), kP + "p[1]/text()[1].1");
}

// R2: a whitespace run is one codepoint.
TEST_F(TextNodeRules, HardWrappedParagraphCountsCollapsedCodepoints) {
  EXPECT_EQ(push("<p>Hello\n      world\n      again</p>", 10), kP + "p[1]/text()[1].12");
}

// R2 across SAX chunks: a run longer than any parser buffer is still one codepoint.
TEST_F(TextNodeRules, AWhitespaceRunSplitAcrossChunksIsStillOneCodepoint) {
  const std::string run(3000, ' ');
  EXPECT_EQ(push("<p>Hello" + run + "world</p>", 5), kP + "p[1]/text()[1].6");
  EXPECT_EQ(push("<p>" + run + "<em>x</em> rest</p>", 1), kP + "p[1]/text()[1].1");
}

// R5: pre keeps everything.
TEST_F(TextNodeRules, PreKeepsRawWhitespaceAndItsLeadingRun) {
  EXPECT_EQ(push("<pre>Hello\n      world</pre>", 5), kP + "pre[1]/text()[1].12");
  EXPECT_EQ(push("<pre>\n  <b>x</b> y</pre>", 1), kP + "pre[1]/text()[2].1");
}

// R3: whitespace next to a block sibling is gone in mixed content; between inlines it stays.
TEST_F(TextNodeRules, MixedContentDropsWhitespaceNextToBlocks) {
  EXPECT_EQ(push("<div>\n<p>a</p>\n<span>s</span>\nloose text</div>", 2), kP + "div[1]/text()[1].1");
  EXPECT_EQ(push("<div>\n<span>s</span>\nloose <em>e</em> text\n<p>later</p></div>", 7), kP + "div[1]/text()[2].1");
}

// NBSP is a character, not whitespace, on both sides.
TEST_F(TextNodeRules, NbspIsACharacter) {
  EXPECT_EQ(push("<p>a&nbsp;&nbsp;b</p>", 5), kP + "p[1]/text()[1].3");
}

// The pull resolves the same numbering, so a KOReader text point lands where crengine meant.
TEST_F(TextNodeRules, ThePullUsesTheSameRules) {
  EXPECT_EQ(pull("<p>\n  <em>x</em> rest of it</p>", kP + "p[1]/text()[1].1"), 1u);
  EXPECT_EQ(pull("<p>Hello\n      world\n      again</p>", kP + "p[1]/text()[1].12"), 10u);
  EXPECT_EQ(pull("<pre>Hello\n      world</pre>", kP + "pre[1]/text()[1].12"), 5u);
  EXPECT_EQ(pull("<div>\n<p>a</p>\n<span>s</span>\nloose text</div>", kP + "div[1]/text()[1].1"), 2u);
}
}  // namespace
```
If `toCrossPoint` only fills `visibleTextOffset` with a laid-out section present (check Task 6's `ProgressMapper.cpp`), build one in `pull()` the way `VisibleTextOffsetLutTest::build` does, with a `GfxRenderer` member, before mapping.

Append to `test/epub_pipeline/ProgressMapperRoundTripTest.cpp`, in the `RoundTripFixture` suite:
```cpp
// A page start that pushes as a text point comes back on exactly that offset; one that pushes as
// an element path comes back on or before it. This is the counting agreement between the parser's
// LUT and the two mappers, with no allowance.
TEST_P(RoundTripFixture, EveryTextPointComesBackExactly) {
  const int spineCount = epub->getSpineItemsCount();
  for (int spine = 0; spine < spineCount; ++spine) {
    auto section = loadSection(spine);  // whatever the fixture already uses to get a laid-out Section
    const int pages = section->getPageCount();
    for (int page = 0; page < pages; ++page) {
      const auto start = section->getVisibleTextOffsetForPage(page);
      if (!start) continue;
      CrossPointPosition pos;  // fill spineIndex, pageNumber, totalPages, visibleTextOffset/has as the existing round-trip test does
      const auto ko = ProgressMapper::toKOReader(epub, pos);
      const auto back = ProgressMapper::toCrossPoint(epub, ko);
      ASSERT_TRUE(back.hasVisibleTextOffset) << "spine " << spine << " page " << page << " via " << ko.xpath;
      if (ko.xpath.find("/text()[") != std::string::npos) {
        EXPECT_EQ(back.visibleTextOffset, *start) << "spine " << spine << " page " << page << " via " << ko.xpath;
      } else {
        EXPECT_LE(back.visibleTextOffset, *start) << "spine " << spine << " page " << page << " via " << ko.xpath;
      }
    }
  }
}
```
Adapt the fixture plumbing (how it obtains the Section and fills `CrossPointPosition`) from the existing `EveryPageComesBackOnOrShortlyBeforeItself`; the assertions above are the requirement.

- [ ] **Step 3: Run them to see them fail**

Build `test/build` and run `EpubPipelineTest --gtest_filter='TextNodeRules.*:Corpus/RoundTripFixture.EveryTextPointComesBackExactly/*'`. Expected: `LeadingWhitespaceOfABlockIsNotANode` fails with `text()[2]`, `HardWrapped...` with `.24`, the pre and mixed tests fail, `ThePullUsesTheSameRules` fails; the corpus exactness test fails on hard-wrapped books.

- [ ] **Step 4: The helpers**

In `ChapterXPathIndexerInternal.h` add, and implement in the `.cpp` beside `countVisibleBytes`:
```cpp
// Block-level tags, as crengine renders them (CSS display above inline). One list for all three
// uses: R1's parent, R3's siblings, and which text gets a text point.
bool isBlockTag(const std::string& tag);

// Codepoints of one character-data chunk as crengine stores them (R2): with `collapse`, a run of
// space/CR/LF/TAB is ONE codepoint, and a run continuing from the previous chunk (`lastWasSpace`
// in) adds none; without it every codepoint counts. Entity references count by their expansion.
// `lastWasSpace` leaves holding the chunk's final state.
size_t collapsedCodepoints(const char* text, int len, bool collapse, bool& lastWasSpace);
// The collapsed codepoint index, within the chunk, of the codepoint holding the chunk's
// targetVisibleByte-th (0-based) visible byte. The chunk must hold that byte.
size_t collapsedCodepointAtVisibleByte(const char* text, int len, size_t targetVisibleByte, bool collapse,
                                       bool lastWasSpace);
// Visible bytes of the chunk before its k-th collapsed codepoint (k may equal the chunk's count).
size_t visibleBytesBeforeCollapsedCodepoint(const char* text, int len, size_t k, bool collapse,
                                            bool lastWasSpace);
```
Implement all three over one private walker: resolve an entity chunk to its expansion exactly as `codepointAtVisibleByte` does today, then step codepoints with `utf8NextCodepoint`; for each codepoint, `ws = collapse && (c == ' ' || c == '\r' || c == '\n' || c == '\t')`; a `ws` codepoint contributes 1 collapsed codepoint only when `!lastWasSpace`, then sets it; any other codepoint contributes 1 and clears it; its visible bytes are its bytes for which `!VisibleText::isSpace`. `isBlockTag`'s table: `p li h1 h2 h3 h4 h5 h6 div td th blockquote dd dt figcaption pre section article body ul ol dl table thead tbody tfoot tr caption hr figure nav aside header footer main address fieldset form details summary center`. Move it out of `ChapterXPathForwardMapper.cpp`'s anonymous namespace.

- [ ] **Step 5: The counter in StackState**

In `ChapterXPathIndexerState.h`, `StackNode` becomes (bools together, after the integers):
```cpp
struct StackNode {
  std::string tag;
  int index = 1;
  // Text-node bookkeeping that mirrors crengine's DOM (rules R1-R5 in ChapterXPathForwardMapper.cpp).
  int textNodeCount = 0;            // text nodes materialised so far under this element
  size_t codepointsInTextNode = 0;  // collapsed codepoints of the open (or provisional) run so far
  bool hasText = false;             // reverse mapper: the element has had visible text
  bool inTextNode = false;          // a node is open: visible text since the last child boundary
  bool pendingWhitespace = false;   // a whitespace-only run is open and not yet a node
  bool lastWasSpace = false;        // the open run ends in whitespace (R2 carry across chunks)
  bool childSeen = false;           // crengine childCount != 0: an element or a materialised node
  bool blockChildSeen = false;      // a block child has started: R3 applies from here on
  bool prevSiblingIsBlock = false;  // the last child boundary was a block element's end
};
```
`StackState` gains `int preDepth = 0;` and:
```cpp
  struct TextRun {
    bool counts;              // false: whitespace-only and still provisional, no node yet
    int nodeIndex;            // 1-based text() index of the node the chunk belongs to
    size_t codepointsBefore;  // collapsed codepoints of that node before this chunk
    bool spaceBefore;         // the node's text before this chunk ends in whitespace
  };

  // Account for one character-data chunk under stack.back(). Called for EVERY chunk inside <body>
  // that shouldSkipText() lets through, whitespace-only ones included.
  TextRun onTextChunk(const char* text, const int len) {
    StackNode& n = stack.back();
    const bool collapse = preDepth == 0;
    if (!n.inTextNode) {
      if (isWhitespaceOnly(text, len)) {
        if (!n.pendingWhitespace) {
          n.pendingWhitespace = true;
          n.codepointsInTextNode = 0;
          n.lastWasSpace = false;
        }
        n.codepointsInTextNode += collapsedCodepoints(text, len, collapse, n.lastWasSpace);
        return {false, n.textNodeCount + 1, 0, false};
      }
      // Visible text: the run is a node, and a provisional whitespace prefix is part of it.
      n.inTextNode = true;
      n.textNodeCount++;
      n.childSeen = true;
      if (!n.pendingWhitespace) {
        n.codepointsInTextNode = 0;
        n.lastWasSpace = false;
      }
      n.pendingWhitespace = false;
    }
    const TextRun run{true, n.textNodeCount, n.codepointsInTextNode, n.lastWasSpace};
    n.codepointsInTextNode += collapsedCodepoints(text, len, collapse, n.lastWasSpace);
    return run;
  }

  // A child boundary under `parent`: a child element starts (`nextIsBlock` says which kind) or the
  // parent itself ends. Settles a provisional whitespace-only run the way crengine's DOM does.
  void closeTextRun(StackNode& parent, const bool nextIsBlock, const bool parentEnds) {
    if (parent.pendingWhitespace) {
      // R1 (ldomElementWriter::onText): the first whitespace-only run of a non-pre block is dropped.
      const bool firstOfBlock = !parent.childSeen && isBlockTag(parent.tag) && preDepth == 0;
      // R3 (removeStandaloneWhitespaceTextChildrenInMixedContent): in mixed content a whitespace-only
      // node survives only between two inline-ish siblings.
      const bool mixed = parent.blockChildSeen || nextIsBlock;
      const bool betweenInlines = parent.childSeen && !parent.prevSiblingIsBlock && !nextIsBlock && !parentEnds;
      if (!firstOfBlock && !(mixed && !betweenInlines)) {
        parent.textNodeCount++;
        parent.childSeen = true;
      }
      parent.pendingWhitespace = false;
    }
    parent.inTextNode = false;
    parent.lastWasSpace = false;
  }
```
`pushElement`: delete the `if (!stack.empty()) stack.back().inTextNode = false;` line. After the tag is lowercased into `node.tag`, add:
```cpp
    const bool block = isBlockTag(node.tag);
    if (depth > 0) {
      StackNode& parent = stack[depth - 1];  // after emplace_back: the vector may have moved
      closeTextRun(parent, block, false);
      if (block) parent.blockChildSeen = true;
    }
    if (node.tag == "pre") preDepth++;
```
`popElement`: delete its `stack.back().inTextNode = false;` line. Before `stack.pop_back()`:
```cpp
    closeTextRun(stack.back(), false, true);
    const bool block = isBlockTag(stack.back().tag);
    if (stack.back().tag == "pre") preDepth--;
```
and after it:
```cpp
    if (!stack.empty()) {
      StackNode& parent = stack.back();
      parent.childSeen = true;
      parent.prevSiblingIsBlock = block;
      parent.inTextNode = false;
      parent.pendingWhitespace = false;
      parent.lastWasSpace = false;
    }
```
`ChapterXPathIndexerState.h` needs `isBlockTag`, `isWhitespaceOnly`, `collapsedCodepoints` visible: include `ChapterXPathIndexerInternal.h` there if it is not already, or forward-declare the three.

- [ ] **Step 6: The forward mapper on the counter**

`ForwardState::onCharData` becomes:
```cpp
  void onCharData(const char* text, const int len) {
    if (shouldSkipText(len) || found || stack.empty()) {
      return;
    }
    StackNode& parent = stack.back();
    const TextRun run = onTextChunk(text, len);  // every chunk, before any early return
    const size_t visible = countVisibleBytes(text, len);
    if (!run.counts || visible == 0) {
      return;
    }
    // Fraction path: the target can equal the chapter total and names the chunk that ENDS there.
    // Offset path: a page's start is a byte of text and names the chunk that CONTAINS it.
    const bool reached = inclusive ? totalTextBytes + visible >= targetOffset : totalTextBytes + visible > targetOffset;
    if (reached) {
      if (textPointsInBlocks ? isBlockTag(parent.tag) : parent.tag == "body") {
        const size_t targetVisibleByteInChunk = targetOffset - totalTextBytes;
        const size_t cpInChunk =
            collapsedCodepointAtVisibleByte(text, len, targetVisibleByteInChunk, preDepth == 0, run.spaceBefore);
        result = currentXPath(spineIndex) + "/text()[" + std::to_string(run.nodeIndex) + "]." +
                 std::to_string(run.codepointsBefore + cpInChunk);
      } else {
        result = currentXPath(spineIndex);
      }
      found = true;
      if (saxParser) saxParser->stop();
      return;
    }
    totalTextBytes += visible;
  }
```
(On the fraction path `targetVisibleByteInChunk` can equal `visible` when the target is the chapter total; keep today's behaviour for that case: `collapsedCodepointAtVisibleByte` returns the chunk's collapsed count when the byte is one past the end.) Delete the local `isBlockTag`. Replace the header comment's text-point paragraph with rules R1-R6 above, naming the crengine functions, so the next reader does not rediscover them.

- [ ] **Step 7: The reverse mapper on the counter**

In `ReverseState`: delete `inParentTextNode`, `codepointsInCurrentTextNode`, `currentTextNodeCount`, and the two lines that reset `inParentTextNode` in `onStartElement`/`onEndElement`. `onCharData` becomes:
```cpp
  void onCharData(const char* text, const int len) {
    if (shouldSkipText(len) || stack.empty()) {
      return;
    }
    const TextRun run = onTextChunk(text, len);  // every chunk, before any early return
    const size_t visible = countVisibleBytes(text, len);

    if (targetTextNodeIndex > 0 && run.counts) {
      const std::string xpath = normalizeXPath(currentXPath(spineIndex));
      if (xpath == targetNorm) {
        stack.back().hasText = true;
        if (run.nodeIndex == targetTextNodeIndex && bestTier < MatchTier::EXACT) {
          bool carry = run.spaceBefore;
          const size_t codepoints = collapsedCodepoints(text, len, preDepth == 0, carry);
          const size_t charOff = static_cast<size_t>(targetCharOffset);
          if (charOff >= run.codepointsBefore && charOff <= run.codepointsBefore + codepoints) {
            const size_t pos = totalTextBytes + visibleBytesBeforeCollapsedCodepoint(
                                                   text, len, charOff - run.codepointsBefore, preDepth == 0, run.spaceBefore);
            bestTier = MatchTier::EXACT;
            bestDepth = pathDepth(xpath);
            bestOffset = pos;
            bestExact = true;
            bestTierName = "text-node-exact";
            bestLiIndex = liCount;
          }
        }
        totalTextBytes += visible;
        return;
      }
    }

    if (visible == 0) {
      return;
    }
    if (!stack.empty() && !stack.back().hasText) {
      stack.back().hasText = true;
      checkMatch();
    }
    totalTextBytes += visible;
  }
```
Update the comment near the top of the file (`M is treated as codepoint offset`) to say collapsed codepoints as crengine stores them. Delete `countUtf8Codepoints`, `codepointAtVisibleByte`, `visibleBytesBeforeCodepoint` if `grep -rn` across `lib/ src/ test/` shows no other caller.

- [ ] **Step 8: Run the suite**

`EpubPipelineTest` whole, then `ctest -j 8` in `test/build` (the `dlfcn.h` build failure of `epub_build_inventory` is known and ignored). Task 5's three expectations in `ProgressMapperTest.cpp` (`p[16]/text()[1].50`, `p[33]/text()[1].0`, `p[17]/text()[1].51`) may change under R2: recompute each by hand from the fixture XHTML (collapsed codepoints before the page's first word) and put the arithmetic for at least one in the commit message. `ProgressMapperRoundTripTest`'s `allowedDrift` table must NOT be widened: if any book's drift grows, stop and report DONE_WITH_CONCERNS with the book, spine and page.

- [ ] **Step 9: Firmware compile**

From PowerShell at the repo root, with `PLATFORMIO_CORE_DIR` set to `C:\pio`, run `pio run -e default` (pio's full path is in the memory file `dev-tool-locations.md`; it is not on PATH). Report RAM/Flash from the summary line. Run it from the repo root, never from `test/build`.

- [ ] **Step 10: Format and commit**

Run clang-format (at `C:\Program Files\LLVM\bin\clang-format.exe`, not on PATH) with `-i` on every touched `.h/.cpp`. One commit:
```
fix(kosync): number text nodes and count codepoints the way crengine's DOM does

<the rules, one line each, and the hand arithmetic for one updated expectation>
```

### Task 6: Pull by offset; the reader lands on the exact page

**Files:**
- Modify: `lib/KOReaderSync/ChapterXPathReverseMapper.h`, `lib/KOReaderSync/ChapterXPathReverseMapper.cpp:229-294`
- Modify: `lib/KOReaderSync/ChapterXPathIndexer.h`, `lib/KOReaderSync/ChapterXPathIndexer.cpp` (`findProgressForXPath`)
- Modify: `lib/KOReaderSync/ProgressMapper.cpp` (`toCrossPoint`, the INF line)
- Modify: `src/activities/ActivityResult.h:69-76` (`SyncResult`)
- Modify: `src/CrossPointState.h` (result fields + `clear()`), `src/JsonSettingsIO.cpp`
- Modify: `src/activities/reader/EpubReaderActivity.h:80-175` (`NavigationTarget`), `src/activities/reader/EpubReaderActivity.cpp:3034-3140` (`resolveInto`)
- Modify: `src/activities/reader/EpubReaderSync.cpp:218-244` (`applyPendingSyncSession`), `src/activities/reader/EpubReaderAutoSync.cpp` (`silentApplyRemote`), `src/activities/reader/ReaderActivity.cpp:811-816`
- Modify: `src/activities/reader/KOReaderSyncActivity.cpp` (`applyRemoteAndFinish`, `resumeReader`, the Apply branch in `loop`)
- Test: `test/epub_pipeline/ProgressMapperTest.cpp`

**Interfaces:**
- Consumes: `CrossPointPosition::visibleTextOffset/hasVisibleTextOffset` (Task 5); `Section::getPageForVisibleTextOffset` (Task 3).
- Produces: `bool ChapterXPathIndexer::findProgressForXPath(const std::shared_ptr<Epub>&, int spineIndex, const std::string& xpath, float& outIntraSpineProgress, bool& outExactMatch, uint16_t* outListItemIndex = nullptr, uint32_t* outVisibleOffset = nullptr)`; `NavigationTarget::Kind::VisibleOffset` and `static NavigationTarget makeVisibleOffset(uint32_t offset, int fallback)`; `SyncResult::visibleOffset/hasVisibleOffset`; `KOReaderSyncSessionState::resultVisibleOffset/resultHasVisibleOffset`.

- [ ] **Step 1: Write the failing tests**

Append to `ProgressMapperTest.cpp` (inside the namespace, after the fallback section):
```cpp
// --- Pull by content offset ---------------------------------------------------------------------

TEST_F(ProgressMapperFixture, AnExactMatchCarriesItsOffset) {
  const auto pos = map("/body/DocFragment[2]/body/section/p[30]/text().0", percentageInto(kSectionSpine, 0.5f));
  EXPECT_TRUE(pos.hasVisibleTextOffset);
  EXPECT_GT(pos.visibleTextOffset, 0u);
  // p[30] of 60 equal paragraphs: a little under half the chapter's text.
  const auto total = ChapterXPathIndexerInternal::countTotalTextBytes(epub, kSectionSpine);
  EXPECT_NEAR(static_cast<double>(pos.visibleTextOffset) / static_cast<double>(total), 0.48, 0.02);
}

TEST_F(ProgressMapperFixture, TheChapterHeadingCarriesOffsetZero) {
  const auto pos = map("/body/DocFragment[2]/body/section/header/hgroup/h1/span[1]/a/span/text().0",
                       percentageInto(kSectionSpine, 0.3f));
  EXPECT_TRUE(pos.hasVisibleTextOffset);
  EXPECT_EQ(pos.visibleTextOffset, 0u);
}

TEST_F(ProgressMapperFixture, AnInexactMatchCarriesNoOffset) {
  EXPECT_FALSE(map("/body/DocFragment[2]/body/section/div[4]/p[2]/text().0", percentageInto(kSectionSpine, 0.4f))
                   .hasVisibleTextOffset);
}

TEST_F(ProgressMapperFixture, AFailedResolveCarriesNoOffset) {
  EXPECT_FALSE(map("/body/DocFragment[3]/body/p[30]/text().0", percentageInto(kBrokenSpine, 0.4f), kBrokenSpine)
                   .hasVisibleTextOffset);
}
```
with `#include "KOReaderSync/ChapterXPathIndexerInternal.h"` at the top.

- [ ] **Step 2: Run them to verify they fail**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3; ./epub_pipeline/EpubPipelineTest.exe --gtest_filter='ProgressMapperFixture.*Offset*:ProgressMapperFixture.*Carries*' 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]|Failure"`
Expected: `AnExactMatchCarriesItsOffset` and `TheChapterHeadingCarriesOffsetZero` fail (`hasVisibleTextOffset` false); the two negative cases pass by default.

- [ ] **Step 3: The reverse mapper reports the offset**

`ChapterXPathReverseMapper.h`: `findProgressForXPathInternal(..., uint16_t* outListItemIndex, uint32_t* outVisibleOffset);`. In the `.cpp`, add the parameter, set `if (outVisibleOffset) *outVisibleOffset = 0;` at the top, and after `outExactMatch = state.bestExact;` add
```cpp
  if (outVisibleOffset) {
    *outVisibleOffset = static_cast<uint32_t>(state.bestOffset);
  }
```
`ChapterXPathIndexer.h/.cpp`: `findProgressForXPath` gains `uint32_t* outVisibleOffset = nullptr` and passes it through.

`ProgressMapper.cpp`, `toCrossPoint`: declare `uint32_t offsetFromXPath = 0;` next to `liIndexFromXPath`, pass `&offsetFromXPath` as the last argument of `findProgressForXPath`, and inside the success block, after the `liIndexFromXPath` handling:
```cpp
      // Only an exact match is a position; an ancestor or index-insensitive match is a stand-in
      // whose offset (a wrapper's end, a sibling's start) must not be mistaken for one.
      if (xpathExactMatch) {
        result.visibleTextOffset = offsetFromXPath;
        result.hasVisibleTextOffset = true;
      }
```
The INF line gains `off=%u/%d` with `result.visibleTextOffset, result.hasVisibleTextOffset`.

- [ ] **Step 4: Run the mapper tests**

Run: `./epub_pipeline/EpubPipelineTest.exe --gtest_filter='ProgressMapperFixture.*:OffsetPushFixture.*' 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]|Failure"` (after building).
Expected: all pass, including Task 5's carried-over `AMidParagraphPageStartPushesAsATextPointAndPullsBackToItsOffset`.

- [ ] **Step 5: The reader's target**

`src/activities/reader/EpubReaderActivity.h`: add `VisibleOffset,  // KOReader content offset (Section::getPageForVisibleTextOffset)` to `Kind`; add `uint32_t visibleOffset;  // Kind::VisibleOffset` to the union; add
```cpp
    static NavigationTarget makeVisibleOffset(uint32_t offset, int fallback = 0) {
      NavigationTarget t;
      t.kind = Kind::VisibleOffset;
      t.visibleOffset = offset;
      t.fallbackPage = fallback;
      return t;
    }
```
`EpubReaderActivity.cpp`, `resolveInto`, a new case before `Kind::Paragraph`:
```cpp
    case Kind::VisibleOffset: {
      if (const auto p = sec.getPageForVisibleTextOffset(visibleOffset)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved offset %u -> page %d", visibleOffset, *p);
      } else {
        LOG_DBG("ERS", "No offset LUT for offset %u; using fallback page %d", visibleOffset, fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }
```
Check `anchorNavTargetToCurrentPage()` and any `switch (navTarget.kind)` elsewhere in the file (`grep -n "Kind::ListItem" src/activities/reader/EpubReaderActivity.cpp`): wherever `Kind::ListItem` is listed as "a LUT-anchored kind", list `Kind::VisibleOffset` beside it.

`src/activities/ActivityResult.h`, `SyncResult`: add `uint32_t visibleOffset = 0; bool hasVisibleOffset = false;  // content offset in the spine (exact page via the section LUT)`.

`src/CrossPointState.h`: add `uint32_t resultVisibleOffset = 0; bool resultHasVisibleOffset = false;` after `resultHasListItemIndex`, and zero both in `clear()`. `src/JsonSettingsIO.cpp`: serialise `resultVisibleOffset` and `resultHasVisibleOffset` next to the list-item result fields, reading them with `| (uint32_t)0` and `| false`. `src/activities/reader/ReaderActivity.cpp` (the AUTO_PULL handoff) zeroes both after `sync.resultHasListItemIndex = false;`.

`KOReaderSyncActivity.cpp`: `applyRemoteAndFinish` copies `sync.resultVisibleOffset = remotePosition.visibleTextOffset; sync.resultHasVisibleOffset = remotePosition.hasVisibleTextOffset;`; `resumeReader` copies `appliedResult->visibleOffset/hasVisibleOffset` and zeroes them in the else branch; the Apply branch in `loop` adds the two fields to its `SyncResult` initialiser (`remotePosition.visibleTextOffset, remotePosition.hasVisibleTextOffset`).

`EpubReaderSync.cpp`, `applyPendingSyncSession`: before the `if (sync.resultHasListItemIndex)` chain add
```cpp
    if (sync.resultHasVisibleOffset) {
      restoreTarget = NavigationTarget::makeVisibleOffset(sync.resultVisibleOffset, restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d offset=%u", restoreSpineIndex, restorePage,
              sync.resultVisibleOffset);
    } else if (sync.resultHasListItemIndex) {
```
`EpubReaderAutoSync.cpp`, `silentApplyRemote`: the same preference, `if (remotePos.hasVisibleTextOffset) target = NavigationTarget::makeVisibleOffset(remotePos.visibleTextOffset, remotePos.pageNumber); else if (...)`.

- [ ] **Step 6: Full suite, firmware build, format, commit**

Full host suite as before. Firmware: from PowerShell at the repo root, `$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & "$HOME\.platformio\penv\Scripts\pio.exe" run -e default` must succeed (the reader-side code is device-only). Then:
```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i lib/KOReaderSync/ChapterXPathReverseMapper.h lib/KOReaderSync/ChapterXPathReverseMapper.cpp lib/KOReaderSync/ChapterXPathIndexer.h lib/KOReaderSync/ChapterXPathIndexer.cpp lib/KOReaderSync/ProgressMapper.cpp src/activities/ActivityResult.h src/CrossPointState.h src/JsonSettingsIO.cpp src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp src/activities/reader/EpubReaderSync.cpp src/activities/reader/EpubReaderAutoSync.cpp src/activities/reader/ReaderActivity.cpp src/activities/reader/KOReaderSyncActivity.cpp test/epub_pipeline/ProgressMapperTest.cpp
git add -A lib/KOReaderSync src test/epub_pipeline/ProgressMapperTest.cpp
git commit -m "feat(kosync): a pulled position lands on the exact page by its content offset

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: The sync screen compares offsets; one struct for its position

**Files:**
- Modify: `lib/KOReaderSync/ProgressComparison.h`, `lib/KOReaderSync/ProgressComparison.cpp`
- Modify: `src/activities/reader/KOReaderSyncActivity.h` (constructor, members), `src/activities/reader/KOReaderSyncActivity.cpp` (`localReadingPosition`, `computeLocalProgressAndChapter`)
- Modify: `src/CrossPointState.h`, `src/JsonSettingsIO.cpp`, `src/activities/ActivityManager.cpp:535-547`, `src/activities/reader/EpubReaderSync.cpp:67-106`, `src/activities/reader/ReaderActivity.cpp:798-820`, `src/activities/reader/EpubReaderAutoSync.cpp` (`localReadingPosition`)
- Test: `test/progress_comparison/ProgressComparisonTest.cpp`

**Interfaces:**
- Consumes: `Section::getVisibleTextOffsetForPage` (Task 3); `CrossPointPosition::visibleTextOffset/hasVisibleTextOffset` (Task 5/6).
- Produces: `LocalReadingPosition::visibleOffsetAtPage` (`uint32_t`), `visibleOffsetAtNextPage` (`uint32_t`, `UINT32_MAX` on the last page), `hasVisibleOffset` (`bool`); `struct SyncLocalPosition { int spineIndex; int page; int totalPages; uint16_t paragraphIndex; bool hasParagraphIndex; uint16_t paragraphIndexBefore; uint32_t visibleOffsetAtPage; uint32_t visibleOffsetAtNextPage; bool hasVisibleOffset; }` declared in `KOReaderSyncActivity.h`; `KOReaderSyncActivity(GfxRenderer&, MappedInputManager&, const std::string& epubPath, const SyncLocalPosition& local, KOReaderSyncIntentState intent)`; `KOReaderSyncSessionState::visibleOffsetAtPage/visibleOffsetAtNextPage/hasVisibleOffset`.

- [ ] **Step 1: Write the failing tests**

Append to `test/progress_comparison/ProgressComparisonTest.cpp` before the `SelectRemoteRecord` section:
```cpp
// --- content offsets within the spine -------------------------------------------------------------
//
// With the chapter laid out, the reader knows its page's start and the next page's start as
// visible-text offsets, and a mapped record carries the offset its XPath resolved to. The offset
// decides before the paragraph LUT does: it is exact for every book, wrapped ones included.

LocalReadingPosition readerAtOffsets(const int spine, const int page, const uint32_t start, const uint32_t next) {
  LocalReadingPosition local = readerWithoutLut(spine, page);
  local.visibleOffsetAtPage = start;
  local.visibleOffsetAtNextPage = next;
  local.hasVisibleOffset = true;
  return local;
}

CrossPointPosition recordAtOffset(const int spine, const uint32_t offset) {
  CrossPointPosition remote = record(spine, 0);
  remote.visibleTextOffset = offset;
  remote.hasVisibleTextOffset = true;
  return remote;
}

TEST(CompareProgress, RemoteOffsetInsideTheLocalPageIsSynchronized) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.30f, recordAtOffset(2, 1000), 0.33f),
            ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.30f, recordAtOffset(2, 1399), 0.33f),
            ProgressComparison::Synchronized);
}

TEST(CompareProgress, RemoteOffsetBeforeTheLocalPageIsLocalAhead) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.30f, recordAtOffset(2, 999), 0.33f),
            ProgressComparison::LocalAhead);
}

TEST(CompareProgress, RemoteOffsetAfterTheLocalPageIsRemoteAhead) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.33f, recordAtOffset(2, 1400), 0.30f),
            ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, TheLastPageRunsToTheEndOfTheChapter) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 9, 8000, UINT32_MAX), 0.9f, recordAtOffset(2, 123456), 0.95f),
            ProgressComparison::Synchronized);
}

TEST(CompareProgress, OffsetsOutrankTheParagraphTier) {
  // The paragraph tier would say "same page" (p[12] opens on page 5); the offsets know better.
  LocalReadingPosition local = reader(2, 5, 11, 14);
  local.visibleOffsetAtPage = 1000;
  local.visibleOffsetAtNextPage = 1400;
  local.hasVisibleOffset = true;
  CrossPointPosition remote = record(2, 12);
  remote.visibleTextOffset = 1500;
  remote.hasVisibleTextOffset = true;
  EXPECT_EQ(compareProgress(local, 0.3f, remote, 0.3f), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, OneSideWithoutAnOffsetFallsThrough) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.30f, record(2, 0), 0.33f),
            ProgressComparison::RemoteAhead);  // percentage decided
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), 0.30f, recordAtOffset(2, 999), 0.33f),
            ProgressComparison::RemoteAhead);  // no local offset, no remote paragraph: percentage
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target ProgressComparisonTest -j 8 2>&1 | grep -E "error" | head -3`
Expected: compile error, `'struct LocalReadingPosition' has no member named 'visibleOffsetAtPage'`.

- [ ] **Step 3: The comparator**

`ProgressComparison.h`: extend the header comment's list with a tier between 1 and 2:
```
//   1b. within the same spine, the content offsets when both sides have them: the remote's offset
//      falls on the local page exactly when start(p) <= offset < start(p+1) -- exact for every
//      book, laid out or not on the other side;
```
and `LocalReadingPosition` gains
```cpp
  // The page's start and the next page's start as visible-text offsets (Section's LUT);
  // visibleOffsetAtNextPage is UINT32_MAX on the last page. hasVisibleOffset false when the chapter
  // has no LUT, and the paragraph tier takes over.
  uint32_t visibleOffsetAtPage = 0;
  uint32_t visibleOffsetAtNextPage = UINT32_MAX;
  bool hasVisibleOffset = false;
```
`ProgressComparison.cpp`, in `compareProgress` inside the same-spine branch, before the paragraph tier:
```cpp
    if (local.hasVisibleOffset && remote.hasVisibleTextOffset) {
      if (remote.visibleTextOffset < local.visibleOffsetAtPage) return ProgressComparison::LocalAhead;
      if (remote.visibleTextOffset < local.visibleOffsetAtNextPage) return ProgressComparison::Synchronized;
      return ProgressComparison::RemoteAhead;
    }
```

- [ ] **Step 4: Run the comparator tests**

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target ProgressComparisonTest -j 8 2>&1 | grep -E "error" | head -3; ./progress_comparison/ProgressComparisonTest.exe 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]"`
Expected: `[  PASSED  ] 23 tests.`

- [ ] **Step 5: The handoff and the struct**

`src/CrossPointState.h`, after `paragraphIndexBefore`:
```cpp
  // The page's start and the next page's start as content offsets (Section::getVisibleTextOffsetForPage),
  // for the sync screen to push the page exactly and to compare a mapped record's offset against
  // [visibleOffsetAtPage, visibleOffsetAtNextPage). hasVisibleOffset false when the chapter has no LUT.
  uint32_t visibleOffsetAtPage = 0;
  uint32_t visibleOffsetAtNextPage = UINT32_MAX;
  bool hasVisibleOffset = false;
```
zeroed in `clear()` (`visibleOffsetAtNextPage = UINT32_MAX`). `JsonSettingsIO.cpp`: serialise the three next to `paragraphIndexBefore` (`| (uint32_t)0`, `| (uint32_t)UINT32_MAX`, `| false`).

`EpubReaderSync.cpp`, `launchKOReaderSync`, in the `if (section && !positionOverride)` block after the paragraph lookups:
```cpp
    if (const auto start = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      sync.visibleOffsetAtPage = *start;
      sync.visibleOffsetAtNextPage =
          section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage + 1)).value_or(UINT32_MAX);
      sync.hasVisibleOffset = true;
    }
```
with the three fields reset (`0`, `UINT32_MAX`, `false`) alongside `sync.paragraphIndexBefore = 0;` above it. `ReaderActivity.cpp`'s AUTO_PULL handoff sets the same three defaults.

`KOReaderSyncActivity.h`: before the class,
```cpp
// Where the reader was when it handed off to the sync screen, as ActivityManager reads it out of
// APP_STATE.koReaderSyncSession. One struct rather than nine positional ints.
struct SyncLocalPosition {
  int spineIndex = 0;
  int page = 0;
  int totalPages = 0;
  uint16_t paragraphIndex = 0;
  bool hasParagraphIndex = false;
  uint16_t paragraphIndexBefore = 0;
  uint32_t visibleOffsetAtPage = 0;
  uint32_t visibleOffsetAtNextPage = UINT32_MAX;
  bool hasVisibleOffset = false;
};
```
the constructor becomes
```cpp
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                const SyncLocalPosition& local,
                                KOReaderSyncIntentState syncIntent = KOReaderSyncIntentState::COMPARE)
      : Activity("KOReaderSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(local.spineIndex),
        currentPage(local.page),
        totalPagesInSpine(local.totalPages),
        localParagraphIndex(local.paragraphIndex),
        hasLocalParagraphIndex(local.hasParagraphIndex),
        localParagraphIndexBefore(local.paragraphIndexBefore),
        localVisibleOffsetAtPage(local.visibleOffsetAtPage),
        localVisibleOffsetAtNextPage(local.visibleOffsetAtNextPage),
        hasLocalVisibleOffset(local.hasVisibleOffset),
        syncIntent(syncIntent),
```
with members `uint32_t localVisibleOffsetAtPage; uint32_t localVisibleOffsetAtNextPage; bool hasLocalVisibleOffset;` after `localParagraphIndexBefore`.

`ActivityManager.cpp`:
```cpp
  SyncLocalPosition local;
  local.spineIndex = sync.spineIndex;
  local.page = sync.page;
  local.totalPages = sync.totalPagesInSpine;
  local.paragraphIndex = sync.paragraphIndex;
  local.hasParagraphIndex = sync.hasParagraphIndex;
  local.paragraphIndexBefore = sync.paragraphIndexBefore;
  local.visibleOffsetAtPage = sync.visibleOffsetAtPage;
  local.visibleOffsetAtNextPage = sync.visibleOffsetAtNextPage;
  local.hasVisibleOffset = sync.hasVisibleOffset;
  replaceActivity(std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, sync.epubPath, local, sync.intent));
```

`KOReaderSyncActivity.cpp`: `localReadingPosition()` adds `local.visibleOffsetAtPage = localVisibleOffsetAtPage; local.visibleOffsetAtNextPage = localVisibleOffsetAtNextPage; local.hasVisibleOffset = hasLocalVisibleOffset;`; `computeLocalProgressAndChapter` fills the position before `toKOReader`:
```cpp
  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine, localParagraphIndex,
                                 hasLocalParagraphIndex};
  localPos.visibleTextOffset = localVisibleOffsetAtPage;
  localPos.hasVisibleTextOffset = hasLocalVisibleOffset;
```
`EpubReaderAutoSync.cpp`, the free `localReadingPosition(section, spine, page)`: add
```cpp
  if (const auto start = section.getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) {
    local.visibleOffsetAtPage = *start;
    local.visibleOffsetAtNextPage =
        section.getVisibleTextOffsetForPage(static_cast<uint16_t>(page + 1)).value_or(UINT32_MAX);
    local.hasVisibleOffset = true;
  }
```
(the wake pull's remote is a peek without an offset, so this tier does not fire there; it is filled for completeness and for `silentApplyRemote`'s future).

- [ ] **Step 6: Full suite, firmware build, format, commit**

Host suite and `pio run -e default` as in Task 6 step 6, then:
```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i lib/KOReaderSync/ProgressComparison.h lib/KOReaderSync/ProgressComparison.cpp src/activities/reader/KOReaderSyncActivity.h src/activities/reader/KOReaderSyncActivity.cpp src/CrossPointState.h src/JsonSettingsIO.cpp src/activities/ActivityManager.cpp src/activities/reader/EpubReaderSync.cpp src/activities/reader/ReaderActivity.cpp src/activities/reader/EpubReaderAutoSync.cpp test/progress_comparison/ProgressComparisonTest.cpp
git add -A lib/KOReaderSync src test/progress_comparison
git commit -m "feat(kosync): the sync screen pushes and compares by content offset

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Zero drift, docs, release note, device checks, PR

**Files:**
- Modify: `test/epub_pipeline/ProgressMapperRoundTripTest.cpp`
- Modify: `docs/contributing/koreader-sync-xpath-mapping.md`, `docs/contributing/koreader-synchronization.md`, `RELEASE_NOTES.md`
- Modify: `docs/superpowers/plans/2026-10-06-kosync-content-offset-baseline.md` (the "after" numbers)

**Interfaces:** consumes everything above.

- [ ] **Step 1: Tighten the round trip (RED first)**

In `ProgressMapperRoundTripTest.cpp`: delete `struct Allowance`, `allowedDrift()` and their comment; the header comment's "Two books are allowed more" sentence goes and point 2 becomes "It lands on the same page: the push names the page's first text to the character, the pull resolves that text to its offset, and the offset is the page's own start." `snappedPage` prefers the offset:
```cpp
int snappedPage(const CrossPointPosition& pos, const Section& section) {
  if (pos.hasVisibleTextOffset) {
    if (const auto p = section.getPageForVisibleTextOffset(pos.visibleTextOffset)) return *p;
  }
  if (pos.hasListItemIndex) {
```
The push supplies the offset: in both loops replace `ProgressMapper::toKOReader(epub, {spine, page, pages})` with
```cpp
      CrossPointPosition local{};
      local.spineIndex = spine;
      local.pageNumber = page;
      local.totalPages = pages;
      if (const auto off = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) {
        local.visibleTextOffset = *off;
        local.hasVisibleTextOffset = true;
      }
      const auto ko = ProgressMapper::toKOReader(epub, local);
```
The assertions become `EXPECT_EQ(landed, page) << ...` (one assertion, the "ahead"/"behind" split and the `RecordProperty` lines go); `LastPageDoesNotFallToTheChapterStart` becomes `LastPageComesBackAsTheLastPage` with `EXPECT_EQ(snappedPage(back, *section), pages - 1)`. `ChapterStartStaysOnFirstPageWhateverThePercentageSays` pushes page 0 with its offset the same way.

Run: `cd /c/_development/witchhunt-reader/test/build && cmake --build . --target EpubPipelineTest -j 8 2>&1 | grep -E "error" | head -3; ./epub_pipeline/EpubPipelineTest.exe --gtest_filter='Corpus/RoundTripFixture.*' 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]|came back" | head -20`
Expected: `[  PASSED  ] 90 tests.` If a book still drifts, the log line names the page and the XPath: a page whose pull lands one page late is a tie the lookup did not resolve to the run's first page (Task 3), a page whose push has no `/text()` is a block tag missing from `isBlockTag` (Task 5).

- [ ] **Step 2: Docs**

`docs/contributing/koreader-sync-xpath-mapping.md`: rewrite "Current Strategy" so CrossPoint → KOReader reads: (1) percentage from chapter/page as before; (2) the page's content offset from the section cache (`Section::getVisibleTextOffsetForPage`) → `ChapterXPathIndexer::findXPathForVisibleOffset`, one streamed pass stopped at the offset, `…/block[K]/text()[N].M` for text that is a direct child of a block element, the element path inside inline elements; (3) the byte-fraction scan only without an offset; (4) the synthetic chapter path last. KOReader → CrossPoint: (1)–(2) as before; add "(2b) an exact match carries the resolved offset (`CrossPointPosition::visibleTextOffset`); the reader lands on the page by `Section::getPageForVisibleTextOffset`, the paragraph LUT snap is the fallback for inexact matches and failed resolves". Replace the "ChapterXPathIndexer Design" paragraph that mentions the temporary file with: "The module streams one spine XHTML from the inflate straight into the SAX parser (`SaxFeedSink`); nothing touches the SD card, and the forward pass stops the inflate at its target." Add to "Known Limitations": "A pushed position inside an inline element is named by the element, not the character."

`docs/contributing/koreader-synchronization.md`: "Core Logic" forward becomes: 1. the page's content offset from the section LUT; 2. stream parse, stop at the offset; 3. emit `/text()[N].M` for block-child text, the element path inside inline elements; 4. without an offset, count and scan by fraction (streamed). Reverse: 1. stream parse; 2. tiers as before; 3. an exact match yields the offset; the reader resolves it to a page by LUT. Add "content offset" to the "Which side is further" order as tier 1b.

`RELEASE_NOTES.md`, in "### Fixes" after the "decides which side is further" entry:
```
- **KOReader sync now lands on the exact page, in both directions, for every book.** The reader records where each page starts in the chapter's text and syncs that: a position pushed from the reader names the text at the top of the page to the character, and a position pulled from KOReader opens on the page that text is on. Before, both directions estimated the page from how far into the chapter's text the position was, which on chapters with headings could land a page ahead (skipping text) and on dense ones a page or two behind, and books converted by Calibre could not be pinned to a page at all. Chapters are laid out again once after the update. Pushing is also quicker: the reader no longer writes the chapter to the card and reads it back twice.
```

- [ ] **Step 3: Full suite, goldens, firmware**

Full host suite: expected `100% tests passed out of 1582` (count it; it is the Task 7 count plus nothing new). Goldens fingerprint as in Task 0 step 4: must equal the baseline hash. Firmware `pio run -e default`: note `RAM:` and `Flash:` and `firmware.bin`'s size against the baseline (6,223,456 B at #397's HEAD in this checkout).

- [ ] **Step 4: Device checks (manual, X3)**

Flash; repeat Task 0 step 5's builds and sync on the same books; record under "After Task 8" in the baseline file. Then the sync checklist with KOReader on the other side, smart sync on: (1) KOReader at the start of chapter N, reader on the last page of N−1: the reader applies and opens chapter N; (2) both on the same page: already in sync; (3) reader ten pages past a KOReader chapter-start push, closes with sync-on-close: the upload happens; (4) sync on wake, nothing changed: "remote matches local; nothing to do", no chapter inflated; (5) push from a page that starts mid-paragraph, pull on KOReader: KOReader lands mid-paragraph on that text; pull it back on the reader: the same page.

- [ ] **Step 5: Commit, push, PR**

```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i test/epub_pipeline/ProgressMapperRoundTripTest.cpp
git add test/epub_pipeline/ProgressMapperRoundTripTest.cpp docs/contributing/koreader-sync-xpath-mapping.md docs/contributing/koreader-synchronization.md RELEASE_NOTES.md docs/superpowers/plans/2026-10-06-kosync-content-offset-baseline.md
git commit -m "test(kosync): every corpus page round-trips to itself

Co-Authored-By: Claude <noreply@anthropic.com>"
git push -u origin feat/kosync-content-offset
```
Open the PR against `master` (or against `feat/kosync-progress-comparison` while #397 is unmerged) with `gh pr create`, following `.github/PULL_REQUEST_TEMPLATE.md` and the style of #397's body: goal, cause, one line per commit, memory (per stage, from the spec, with the measured device numbers), flash delta against 6,223,456 B, verification (host counts, goldens unchanged, the device checklist results), and "AI Usage: YES".

---

## Self-review

- **Spec coverage:** invariant and shared helper (Task 1), `ParsedText` (Task 2), parser counting, page start, LUT entry, `Section` accessors, version bump, format docs (Task 3), streaming with early stop (Task 4), push with text points and the block-tag rule, `currentKoPosition` (Task 5), pull offset for exact matches, `NavigationTarget::VisibleOffset`, `resolveInto`, apply paths, session result fields (Task 6), handoff fields, `SyncLocalPosition`, comparator tier, `computeLocalProgressAndChapter` (Task 7), round-trip tightening, docs, release note, device measurement before/after (Tasks 0, 3, 8). The spec's "Memory" section is enforced by Task 2's gate arithmetic, Task 4's stack-only sink and Tasks 3/8's device measurements.
- **Type consistency:** `getVisibleTextOffsetForPage(uint16_t) -> std::optional<uint32_t>` and `getPageForVisibleTextOffset(uint32_t) -> std::optional<uint16_t>` are used with those types in Tasks 5–8; `addWord`'s sixth parameter is `uint32_t visibleOffset` everywhere; `streamSpine(epub, spineIndex, saxParser)` and `countTotalTextBytes(epub, spineIndex)` match between Task 4 and the tests; `findProgressForXPath`'s `uint32_t* outVisibleOffset` is last, after `outListItemIndex`.
- **Review Focus:** 1 → Task 3 `TiesResolveToTheFirstPageOfTheRun` and Task 8's corpus; 2 → Task 3 synthetic-word offsets and Task 8's corpus; 3 → Task 5 `EntityAtThePageStartRoundTrips`; 4 → Task 3 `PageBeyondTheLutHasNoOffset`, Task 5 `WithoutAnOffsetThePushUsesTheFraction`; 5 → Task 3 `StyleTextInsideBodyIsNotCounted`.
