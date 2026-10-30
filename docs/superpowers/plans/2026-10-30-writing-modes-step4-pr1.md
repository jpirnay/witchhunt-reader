# Writing modes step 4, PR 1: widows, orphans and lone images on vertical pages — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A vertical page fills its columns (no widow or orphan rule), and a page holding only a small image shows it
centred across the page instead of at the right edge.

**Architecture:** Both fixes live in the parser, in the turned frame where a vertical section is laid out.
`keepTogetherBreak` returns false for a vertical section after its heading branch, so headings still keep with what
follows. `emitPage` centres an image-only vertical page's images along the block axis (the content frame's y), where
it already stamps the page's block extent. The reader's render-time offset (`imageOnlyPageYOffset`) stays 0 for a
vertical page. Section version 105.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3), host tests with GoogleTest/CMake (`test/`), the serial test
API (`tools/devctl.py`, `docs/serial-test-api.md`) on the X4.

**Spec:** `docs/superpowers/specs/2026-10-30-writing-modes-step4-design.md`, "PR 1: small fixes the step 3 PR named".

## Global Constraints

- No change to a horizontal page: every horizontal golden in `test/epub_pipeline/goldens` stays byte-identical.
- No new resident RAM; nothing allocates in the draw path.
- Section version 105 (master is 104); one bump for this PR, in Task 1.
- A heading's keep-with-next stays in force on vertical pages.
- Comments and commit messages follow the repo's style: plain sentences, conventional prefixes (`fix(epub):`,
  `test(epub):`, `docs:`), no Co-Authored-By trailer, never skip hooks.

## Review Focus

1. A vertical heading at a page's foot still moves with the first lines of its text (Task 1's third test).
2. An image beside text on a vertical page keeps its flow position; only an image-only page moves (Task 2's second
   test).
3. An image-only vertical page whose images already span the block extent (the acceptance book's full-page maps)
   does not move (Task 2's third test).
4. A paragraph longer than a vertical page still splits and nothing loops (Task 1 reruns
   `ParagraphLongerThanAPageStillSplits` with `vertical = true`).
5. A cached v104 section is rebuilt, not misread, after the update (the version check; Task 3 checks it on the X4).

---

### Task 1: no widows or orphans in a vertical section

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (`keepTogetherBreak`, ~:1543-1581)
- Modify: `lib/Epub/Epub/Section.cpp:36` (`SECTION_FILE_VERSION`)
- Test: `test/epub_pipeline/KeepTogetherTest.cpp`
- Goldens: the four vertical ones in `test/epub_pipeline/goldens` may change (below)

**Interfaces:**
- Consumes: `ChapterHtmlSlimParser::vertical_` (set at `<body>` by step 3's `decideFrame`).
- Produces: `SECTION_FILE_VERSION = 105`; Task 2 extends its comment and does not bump again.

- [ ] **Step 1: Let the fixture build a vertical section**

In `KeepTogetherLayout`, add a member `bool vertical = false;`. In `layout()`, when it is set, use
`params.verticalText = VerticalText::On`, `params.viewportWidth = 240`, `params.viewportHeight = 100`: the parser
swaps them, so the content frame is the same 100 px column by 240 px block extent as the horizontal tests, and every
count in the file's header comment holds. The words (`aaaaNNN`) are Latin, set sideways with their horizontal
advance, so each still fills a column of its own.

- [ ] **Step 2: Write the failing tests** (parameterised over heap and lent arena, like the others)

```cpp
TEST_P(KeepTogetherFixture, AVerticalSectionLeavesAnOrphan) {
  vertical = true;
  std::string first;
  const auto l = layout(fill(9) + para(4, &first));
  EXPECT_EQ(l.pageOf.at(first), 0) << "the column fits: a vertical page fills it";
  EXPECT_EQ(l.linesOn.at(0), 10);
}

TEST_P(KeepTogetherFixture, AVerticalSectionLeavesAWidow) {
  vertical = true;
  std::string first, last;
  const auto l = layout(fill(7) + para(4, &first, &last));
  EXPECT_EQ(l.linesOn.at(0), 10);
  EXPECT_EQ(l.linesOn.at(1), 1);
  EXPECT_EQ(l.pageOf.at(last), 1);
}

TEST_P(KeepTogetherFixture, AVerticalHeadingStillMovesWithWhatFollows) {
  vertical = true;
  std::string first;
  const auto l = layout(fill(8) + "<h2>Hdg</h2>" + para(3, &first));
  EXPECT_EQ(l.pageOf.at("Hdg"), 1);
  EXPECT_EQ(l.pageOf.at(first), 1);
}

TEST_P(KeepTogetherFixture, AVerticalParagraphLongerThanAPageStillSplits) {
  vertical = true;
  std::string first, last;
  const auto l = layout(fill(9) + para(21, &first, &last));
  EXPECT_EQ(l.linesOn.at(0), 10);
  EXPECT_EQ(l.pageOf.at(first), 0);
  EXPECT_EQ(l.pageOf.at(last), 2);
}
```

Derivations, for the record in each test's comment: orphan, 9 lines then 1 fits of 4 (the horizontal rule would move
it); widow, 7 then 3 of 4 fit (the horizontal rule stops at 2); heading, the h2 is 34 px at 192..226 with no room
for two lines below (keep-with-next, unchanged); long paragraph, 1 + 10 + 10 lines of 21 on pages 0-2.

- [ ] **Step 3: Run them and see the first, second and fourth fail**

Run: `cmake --build build/test --target EpubPipelineTest -j 6 && ctest --test-dir build/test -R "KeepTogether" -j 8`
Expected: `AVerticalSectionLeavesAnOrphan`, `AVerticalSectionLeavesAWidow` and
`AVerticalParagraphLongerThanAPageStillSplits` fail (the rule moves lines); the heading test passes already.

- [ ] **Step 4: Implement**

In `keepTogetherBreak`, after the `if (style.keepWithNext) { ... }` block, return false for a vertical section, with
a comment: the rule was written for Latin text; a vertical page holds about 9 columns where a horizontal one holds
14 lines, and ending it early to keep two columns together cost the acceptance book pages that stopped after 5 of 9
columns (#577). The heading branch above stays.

Bump `SECTION_FILE_VERSION` to 105 in `Section.cpp`. Comment: `// v105: a vertical section keeps no widows or
orphans` (Task 2 adds its part).

- [ ] **Step 5: Run the keep-together tests and the pipeline goldens**

Run: `ctest --test-dir build/test -R "KeepTogether|EpubPipeline|Vertical" -j 8`
Expected: the four new tests pass; every horizontal golden matches. A vertical golden that differs is regenerated
with `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`, and its diff (`git diff --stat` and a read of
the hunks) may only move lines between pages and change page counts; report each changed golden with its page
counts before and after.

- [ ] **Step 6: Commit**

```bash
git add lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp lib/Epub/Epub/Section.cpp test/epub_pipeline/KeepTogetherTest.cpp test/epub_pipeline/goldens
git commit -m "fix(epub): a vertical page keeps no widows or orphans; a heading still keeps with its text (section v105)"
```

### Task 2: an image-only vertical page centres its images across the page

**Files:**
- Modify: `lib/Epub/Epub/Page.h` (a predicate beside `getImageBoundingBox`, ~:246)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (`emitPage`, ~:1405-1407)
- Modify: `src/activities/reader/ImageOnlyPage.h` (use the predicate; comment)
- Modify: `lib/Epub/Epub/Section.cpp:36` (the version comment only)
- Test: `test/epub_pipeline/VerticalLayoutTest.cpp`, `test/epub_pipeline/ImageOnlyPageTest.cpp`

**Interfaces:**
- Consumes: `Page::getImageBoundingBox(int16_t&, int16_t&, int16_t&, int16_t&) const`, `PageElement::yPos`,
  `Page::verticalBlockExtent`.
- Produces: `bool Page::isImageOnly() const` — true when the page has elements and every one is a `TAG_PageImage`.

- [ ] **Step 1: Write the failing tests** in `VerticalLayoutTest.cpp`, beside `APortraitImageIsSizedTurnedIntoTheColumns`

```cpp
// The 200 x 400 image is a 400 x 200 box in the turned frame; alone on a page 460 wide it moves to (460 - 200) / 2.
TEST_F(VerticalLayout, ALoneImageIsCentredAcrossAVerticalPage) {
  const std::string on = dump("", document(kImageBody), profileOf(VerticalText::On), "", png(200, 400));
  EXPECT_NE(std::string::npos, on.find("IMG y=130 x=180 w=400 h=200 ")) << on;
}

// Beside text an image keeps its place in the flow: the same block position as on the horizontal page.
TEST_F(VerticalLayout, AnImageBesideTextKeepsItsPlace) {
  const std::string body = std::string("<p>本文</p>") + kImageBody;
  const int on = firstImageY(dump("", document(body), profileOf(VerticalText::On), "", png(200, 400)));
  const int off = firstImageY(dump("", document(body), profileOf(VerticalText::Off), "", png(200, 400)));
  EXPECT_GT(on, 0);
  EXPECT_EQ(off, on);
}

// An image as wide as the block extent stays where it is (the acceptance book's full-page maps).
TEST_F(VerticalLayout, AnImageSpanningThePageIsNotMoved) {
  const std::string on = dump("", document(kImageBody), profileOf(VerticalText::On), "", png(460, 400));
  EXPECT_EQ(0, firstImageY(on)) << on;
}
```

`firstImageY(const std::string& dump)` is a test helper in the file's anonymous namespace: the `y=` of the first
`  IMG ` record, -1 when there is none. Check `png(460, 400)` against the sizing: if the parser scales the turned
image so its block extent is not 460, use a width that gives exactly the viewport's 460 and say so in the comment.

In `ImageOnlyPageTest.cpp`, add `TEST(ImageOnlyPage, ThePredicateNeedsOnlyImages)`: a page with one `PageImage` is
`isImageOnly()`; the same page with a `PageHR` added is not; an empty page is not.

- [ ] **Step 2: Run them and see them fail**

Run: `cmake --build build/test -j 6 -- -k 0 && ctest --test-dir build/test -R "VerticalLayout|ImageOnlyPage" -j 8`
Expected: `ALoneImageIsCentredAcrossAVerticalPage` fails (y=0); the predicate test fails to compile until Step 3.

- [ ] **Step 3: Implement**

- `Page::isImageOnly()` in `Page.h`; `ReaderPageLayout::imageOnlyPageYOffset` uses it instead of its own
  `std::all_of`. Its comment says a vertical page is centred by the parser instead (`emitPage`).
- In `emitPage`, where `verticalBlockExtent` is stamped: when the page `isImageOnly()` and its bounding box's height
  is less than `viewportHeight` (the block extent in the turned frame), add `(viewportHeight - h) / 2 - y` to every
  element's `yPos` when that is positive. A comment says why it is the parser's job here: the block extent is known at
  build, and the reader's offset moves along the column, not across it.
- The version comment in `Section.cpp` becomes `// v105: a vertical section keeps no widows or orphans, and an
  image-only vertical page is centred across its block extent`. No second bump.

- [ ] **Step 4: Run the tests and the goldens**

Run: `ctest --test-dir build/test -R "VerticalLayout|ImageOnlyPage|EpubPipeline|Vertical" -j 8`
Expected: all pass; no golden changes in this task (the fixtures have no image-only vertical page; a change is a
finding to report).

- [ ] **Step 5: Commit**

```bash
git add lib/Epub/Epub/Page.h lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp src/activities/reader/ImageOnlyPage.h lib/Epub/Epub/Section.cpp test/epub_pipeline/VerticalLayoutTest.cpp test/epub_pipeline/ImageOnlyPageTest.cpp
git commit -m "fix(epub): an image-only vertical page centres its images across the page"
```

### Task 3: suite, firmware, the X4, docs

**Files:**
- Modify: `docs/future_work/writing-modes.md` ("What step 4 inherits from step 3": drop the lone-image bullet;
  roadmap row 4's state: `in progress (PR 1 #<this PR>)`)

- [ ] **Step 1: The whole host suite**

Run: `cmake --build build/test -j 6 -- -k 0`, then `ctest --test-dir build/test -j 8 -E HeapPeakRegression`, then
`ctest --test-dir build/test -R HeapPeakRegression --timeout 3000`.
Expected: only the known Windows failures (FirstChapter x2, LongChapterPageTables x2, KeptReadingState x1;
`epub_build_inventory` does not build).

- [ ] **Step 2: clang-format 21**

Run: `PATH="/c/tmp/cf21/clang_format/data/bin:$PATH" bash bin/clang-format-fix`, then `--dry-run --Werror` on each
touched C++ file. Expected: clean.

- [ ] **Step 3: Firmware sizes**

`gh_release` then `default` (`export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1`, one pio run at a time), against
master's `default` and `gh_release`. Record Flash and RAM.

- [ ] **Step 4: The X4** (COM5; flash the `default` `firmware.factory.bin` with esptool `write-flash 0x0`)

Back up with `cmd:GET:*`; NotoSansJP 14 pt, portrait, as the step 3 run. The acceptance book (in `/test` on the X4):
- spine 22 under Automatic: the pages around the inline image (step 3's pages 32-34) now fill; screenshots of them,
  and the spine's page count (step 3: 65);
- spines 4, 9 and 39 under Automatic: page counts against step 3's 12, 78 and 139 (equal or fewer);
- the same four spines under Off: unchanged from step 3 (8, 69, 61, 129);
- spines 5 and 6 (full-page maps): unchanged, screenshots.
A section cached by the step 3 build is rebuilt (the log shows the version mismatch), not misread. Restore every
setting from the backup and verify with `GET`; leave the X4 on this build.

- [ ] **Step 5: Docs and commit**

```bash
git add docs/future_work/writing-modes.md
git commit -m "docs(writing-modes): step 4 PR 1"
```
