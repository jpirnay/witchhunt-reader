# Writing modes step 1, PR 2: layout and drawing of right-to-left text Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A paragraph that needs bidi is laid out by one UAX #9 pass per line and stored in visual order with each
word's logical text, its display form and the line's logical order. The renderer puts niqqud on its letter, lists
and tables start on the right, and CSS and HTML bidi markup becomes control characters. Hebrew paragraphs read right;
pages still turn left to right.

**Architecture:**
- `BidiUtils` gains the entry points the line pass needs and a workspace a build can lend (deviation 11, new).
- A new pure unit, `lib/Epub/Epub/BidiLine`, turns one line's tokens into pieces in visual order:
  - isolates are ordered on their own (decision 8);
  - a piece is a run of one token's characters (decision 5);
  - it gives the controls open at the line's start, and past 128 code points upstream's fallback.
- `ParsedText` decides per paragraph whether it needs bidi and its base direction (decision 6). It measures tokens by
  their own visual form, breaks lines as today, and positions a bidi line's pieces in visual order. Its own arrays
  stay logical.
- `TextBlock` stores two optional regions, flagged by the word count's top bit; `SECTION_FILE_VERSION` 108.
- The parser keeps literal controls and inserts CSS controls into the logical text. Visible offsets never count them.

**Tech Stack:** C++20 (`-fno-exceptions`), minibidi (C), GoogleTest host tests (CMake, Git Bash), Python 3 fixture
generator, PlatformIO firmware (ESP32-C3).

**Spec:** `docs/superpowers/specs/2026-11-02-writing-modes-step1-design.md` (approved 2026-11-02), "The pieces"
item 2. PR 1 (`docs/superpowers/plans/2026-11-02-writing-modes-step1-pr1.md`, #599) built `lib/MiniBidi` and
`lib/TextShaping`. This plan implements:
- decisions 1, 5, 6 and 8;
- deviations 1 (the workspace), 5, 6, 7 and 8, and the new deviation 11;
- "How a paragraph is laid out", "Storage and drawing";
- from "The reader": the profile tags, lists and tables.

## Calls this plan makes (the spec leaves them open)

- **One PR, not 2a/2b.** The parts share one fixture, one rule checker and one format bump. A split would bump
  `SECTION_FILE_VERSION` twice and refresh the RTL goldens twice. Stacked PRs have bitten this repo twice (#398,
  #584).
- **"Needs bidi" means a right-to-left base, or a code point of class R, AL, RLE, RLO, RLI or FSI** (minibidi's
  `is_rtl_class`; RLM and ALM are R and AL).
  - LRE, LRO, LRI, PDF, PDI and LRM alone cannot reorder a left-to-right paragraph. They are dropped like the marks.
  - So an English book with `<span dir="ltr">` or a stray LRM lays out byte for byte as today. The spec's "a
    paragraph that does not need bidi still drops them" then has content.
  - `<bdo dir="rtl">` on Latin text still works (RLO), as deviation 8 wants.
- **Decision 6, both halves:**
  - in a right-to-left book, the base is RTL iff the paragraph's first pass has an R or AL code point;
  - in any other book, iff it has R or AL and no L;
  - a paragraph with no letter of either direction takes the book's direction (the profile's default direction),
    as the spec's "How a paragraph is laid out" step 1 says. So `* * *` or `1948` is right to left in a Hebrew book
    and left to right in a Latin one, and no Latin golden moves. (Controller's ruling, 2026-11-02. The literal
    "unless it has no left-to-right letter" would turn every `* * *` of every Latin book RTL.)
- **`dir="auto"` and block `unicode-bidi: plaintext`:** the first strong letter of the first pass's tokens, else the
  book's direction.
- **The workspace is lent per call:**
  - On a build with a lent arena (the borrowed framebuffer), each line pass and each width pass reserves a block for
    the 1.5 KB workspace and the pass's 4.5 KB scratch, binds it and releases it before the line is materialised.
    LIFO always holds.
  - Otherwise the workspace is allocated on first use and released when the build ends. The scratch is a
    `ParsedText` heap member, dropped with the layout scratch.
- **The TextBlock header grows only on lines with the region.**
  - The on-disk word count's bit 15 (counts are capped at 10,000) says a `u16` region size follows.
  - In RAM, `arenaOwned_` moves into the bitfield so `bidiBytes_` fits. `TextBlock` stays 52 B, and every line
    without the region keeps its bytes.
- **Literal controls follow one rule:**
  - a control never sets a word's visible offset;
  - a word made only of controls is held and joins the next word of the block;
  - held controls are dropped at a block's end.
  - The same goes for U+2066-2069, which master kept as ordinary text (drawn as missing glyphs).
- **Split offsets** (`addWord`'s CJK split, hyphenation) count no bidi control. They then equal master's, which
  stripped the controls before splitting.
- **The hanging-indent clamp** uses the side the indent is drawn on (the base direction), not the parser's
  direction. The list padding uses the parser's.
- **A table's direction** is its computed direction, else the book's, as for lists. Cells are stored in visual order
  (reversed at layout), so the page format and `PageTableFragment::render` do not change. Upstream #3891 mirrors
  the column x instead.
- **Bidi lines carry no annotations** (ruby in right-to-left text is out of scope) and skip CJK push-in. A bidi line
  that comes out LTR, identical and unsplit takes the existing path untouched.
- **The renderer gates** `isTransparentMark` to U+0591-08FF and U+FB1E, so Latin and CJK glyphs pay no class
  lookup.
  - `EpdFont::getTextDimensions` keeps `utf8IsCombiningMark` only, as upstream does.
  - It gets the anchored placement for Latin marks, which is the same arithmetic.

## Global Constraints

- **Upstream baseline:** crosspoint-reader `develop` @ `e8299b5f`; read with
  `git -C C:/_development/crosspoint-reader show e8299b5f:<path>`. The PRs ported here:
  - #1700 `f5bc554a` (Uri Tauber);
  - #2541 `932a4728` (Husam Younis): the reordered loop, the marks and the niqqud anchors;
  - #3198 `ce6c6cbc`;
  - #3891 `0305aaa6` (Justin Mitchell): the RTL table.
- **Attribution:**
  - Task 4's commit, whose anchors are #2541's code, gets `Co-authored-by: Husam Younis <youhusam@gmail.com>`. No
    other commit gets a co-author trailer. No Claude trailer anywhere.
  - Commit bodies name the upstream PR and commit they follow.
  - Every line changed in `lib/MiniBidi` carries `// witchhunt: deviation N`.
- **Every Latin and CJK golden stays byte-identical.** Only `test_rtl_markup*.golden.txt` and the new
  `test_bidi_text*.golden.txt` change. A paragraph that does not need bidi keeps its layout and its section bytes.
- **`SECTION_FILE_VERSION` 107 → 108** (`lib/Epub/Epub/Section.cpp:36`).
- **Nothrow:**
  - no bare `new`, no exceptions, no `try`;
  - a failed workspace or scratch lays the paragraph out without bidi, with `LOG_ERR("BIDI", ...)`.
- **No resident memory:**
  - the workspace lives during a section build only;
  - the scratch is released with `ParsedText::releaseLayoutScratch`;
  - `TextBlock` stays 52 B on the firmware (`static_assert`, `TextBlock.cpp:22`) and 88 B on the host
    (`VerticalTextBlock.TheCentredMarksBitDoesNotGrowTheBlock`).
- **`BIDI_MAX_LINE` stays 128.** Past it a line takes upstream's result, `computeVisualWordOrder` with per-word forms.
- **Every deviation is named in the spec:** Task 1 adds deviation 11 and extends deviation 1's text.
- **Never declare** a local `std::string word(...)` with parentheses: Arduino's `word()` macro breaks firmware builds.
  Brace-init or another name.
- **Host tests from Git Bash:**
  - build: `cmake --build build/test -j 6 --target <T>`;
  - run: `ctest --test-dir build/test -R <pattern> --output-on-failure`;
  - the whole suite: `cmake --build build/test -j 6 -- -k 0` and `ctest --test-dir build/test -j 8 -E HeapPeakRegression`;
  - the only expected failures are FirstChapter ×2, LongChapterPageTables ×2 and KeptReadingState ×1 (Windows-only);
  - a missing `build/test` is configured as `docs/superpowers/plans/2026-10-28-writing-modes-step3.md` "Global
    Constraints" says.
- **Goldens:**
  - refresh with `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`;
  - `git status` then lists only the goldens the task names, each checked by hand;
  - goldens are LF.
- **Formatting:** clang-format 21 (`PATH="/c/tmp/cf21/bin:$PATH" bin/clang-format-fix -g`) on every touched C/C++
  file.
- **Source files** are written with the Write/Edit tools, never a shell heredoc. Before each commit, check that no
  touched file holds a NUL byte.
- **The PR targets master:**
  - if #599 is not merged when it opens, the body says its first commits are #599's;
  - after #599 merges, merge master and check `git merge-base`.

## Review Focus

1. **An isolate or override that wraps onto the next line** (an English phrase in a Hebrew paragraph, a long `<bdi>`).
   The next line must keep the isolate's order. Pinned in Task 2 (`AnIsolateOpenAtTheLineStartIsOrderedOnItsOwn`,
   `AnOverrideOpenAtTheLineStartStillHolds`) and Task 9 (`AnIsolateAcrossALineBreakKeepsItsOrder`).
2. **A build on the lent framebuffer arena:**
   - the line pass borrows from it between a page's lines;
   - a release out of order, or a block left open across `beforeLine_`, corrupts the page.
   Pinned in Task 6 (`ALentArenaBuildLaysOutAsAHeapBuild`, at 48 KB and at 16 KB, where the borrow falls back to the
   heap).
3. **A paragraph past the 96-token flush.** Its base direction and its open controls must hold for every pass. Pinned
   in Task 2 (`TheOpenControlsFollowTheText`) and Task 7 (`TheDirectionHoldsAcrossTheLongBlockSplit`).
4. **Vocalised Hebrew in landscape:** every mark is a code point, so lines pass 128 code points and take the
   fallback. Pinned in Task 6 (`ALineOverTheBidiLimitStillRunsRightToLeft`) and Task 11 (the rules again at 1,600 px).
5. **KOSync with literal LRM/RLM, and with inserted controls.** The page offsets must equal master's. Pinned in Task 8
   (`KeptBidiMarksLeaveTheContentOffsetsAsTheyWere`, `ABidiControlCountsNothingWhereAWordIsSplit`) and Task 9
   (`InsertedControlsCountNowhere`).

---

### Task 1: BidiUtils, the line pass's entry points and a lendable workspace

**Files:**
- Modify: `lib/MiniBidi/BidiUtils.h`, `lib/MiniBidi/BidiUtils.cpp`
- Modify: `test/minibidi/BidiUtilsTest.cpp`
- Modify: `docs/superpowers/specs/2026-11-02-writing-modes-step1-design.md` (deviations 1 and 11)

**Interfaces:**
- Consumes: PR 1's `BidiUtils` (shared workspace under `bidiMutex`, `getWorkspace()`, `releaseWorkspace()`).
- Produces (namespace `BidiUtils`, each declaration marked `// witchhunt: deviation 11` or `1`):
  ```cpp
  inline constexpr size_t kMaxLineCodepoints = 128;  // BIDI_MAX_LINE, for callers without minibidi.h
  inline constexpr size_t kWorkspaceBytes = 1536;    // BIDI_MAX_LINE bidi_chars; static_assert in the .cpp
  // Orders cps[0..count) (logical) at paragraphLevel (0 or 1; no autodetection) with one do_bidi in the shared
  // workspace. On true: visual[v] is the logical index drawn at visual position v and drawn[v] the code point drawn
  // there (mirrored by L4); each run of transparent marks follows its base, as applyBidiVisual emits it. False, the
  // outputs untouched, for count 0, count > kMaxLineCodepoints, or no workspace.
  bool orderLine(const uint32_t* cps, size_t count, int paragraphLevel, uint16_t* visual, uint32_t* drawn);
  // Whether a code point of utf8[0..len) has bidi class R, AL, RLE, RLO, RLI or FSI (is_rtl_class). A byte scan
  // first; only candidates are decoded: lead bytes D6-DF, E0 A0-A3, E2 80 8F/AB/AE, E2 81 A7/A8, EF AC-BB.
  bool hasRtlClass(const char* utf8, size_t len);
  // 1 for class R or AL, 0 for L, -1 otherwise.
  int strongLevel(uint32_t cp);
  // While bound, `buffer` (kWorkspaceBytes, 4-aligned) is the shared workspace and nothing is allocated; nullptr
  // unbinds. A heap workspace allocated earlier stays until releaseWorkspace().
  void bindWorkspace(void* buffer);
  // Whether a heap workspace is allocated (tests: none survives a build).
  bool heapWorkspaceHeld();
  ```
- `releaseWorkspace()` also unbinds. Upstream's five functions keep their bodies. `orderLine` re-states the
  marks-after-base rule over indices rather than refactoring `applyBidiVisual`'s loop, and a test pins that the two
  agree.

- [ ] **Step 1: Write the failing tests** in `test/minibidi/BidiUtilsTest.cpp`:
  - `OrderLineReversesARightToLeftRun`: {05E9, 05DC, 05D5, 05DD} at level 1 → visual {3, 2, 1, 0}, drawn
    {05DD, 05D5, 05DC, 05E9}.
  - `OrderLineKeepsALeftToRightLine`: "abc" at 0 → visual {0, 1, 2}.
  - `OrderLineMirrorsBracketsInARightToLeftRun`: {0028, 05D0, 0029} at 1 → visual {2, 1, 0}, drawn
    {0028, 05D0, 0029}.
  - `OrderLinePutsMarksAfterTheirBase`: {05E9, 05B8, 05C1, 05DC, 05D5, 05B9, 05DD} at 1 → visual
    {6, 4, 5, 3, 0, 1, 2}, drawn {05DD, 05D5, 05B9, 05DC, 05E9, 05B8, 05C1}.
  - `OrderLineMatchesApplyBidiVisual`: for "He said שלום.", "שלום (world) 1998.", "שָׁלוֹם עוֹלָם" and
    "abc(שלום)def", each at levels 0 and 1, the UTF-8 of `drawn` equals `applyBidiVisual(s, out, level)`'s `out`.
  - `OrderLineRefusesPastTheLimit`: 129 × U+05D0 → false; `visual[0]` and `drawn[0]` keep their presets 0xBEEF.
  - `ABoundWorkspaceIsUsedAndNoHeapOneMade`:
    - `releaseWorkspace()`, then `bindWorkspace(buf)` (a 4-aligned 1536 B array), `orderLine` on שלום → true and
      `heapWorkspaceHeld()` false;
    - `bindWorkspace(nullptr)`, `orderLine` again → true and `heapWorkspaceHeld()` true;
    - `releaseWorkspace()` → false.
  - `HasRtlClassFindsRightToLeftLettersAndControls`:
    - true for "שלום", "\u202Eabc" (RLO), "\u2067abc" (RLI), "\u2068abc" (FSI), "\u200F", "\u061C" and "\uFB20";
    - false for "abc", "日本", "\u2066abc" (LRI), "\u200Eabc" (LRM), "“q”" and "，".
  - `StrongLevelOfLettersAndNeutrals`: 'a' 0, 05D0 1, 0627 1, 200E 0, 200F 1, 4E00 0, '1' -1, ' ' -1.
- [ ] **Step 2: Run them:** `cmake --build build/test -j 6 --target BidiUtilsTest` fails to compile (the names are
  undeclared).
- [ ] **Step 3: Implement** them in `BidiUtils.cpp`. `getWorkspace()` returns the bound buffer when one is bound,
  else the heap one (allocated on first use, as now). Add `static_assert(sizeof(BidiWorkspace) == kWorkspaceBytes)`.
- [ ] **Step 4: Run** `BidiUtilsTest`, `BidiClassTest`, `MiniBidiArabicTest` and `TextShapingTest`: all pass.
- [ ] **Step 5: The spec.**
  - Add to "The deviations, all of them":
    `11. **The line pass's entry points.** BidiUtils gains orderLine (one do_bidi over a caller's code points at a given level: the visual order and the drawn, mirrored code points, each run of marks after its base as applyBidiVisual emits them), hasRtlClass and strongLevel for the layout's tests, bindWorkspace and heapWorkspaceHeld. Upstream's functions are unchanged.`
  - Extend deviation 1 with
    `During a build with a lent arena the line pass borrows the workspace from it for each call (bindWorkspace); otherwise the workspace is allocated on first use.`
- [ ] **Step 6: Commit** `feat(bidi): orderLine and a workspace a section build can lend (deviation 11)`.

### Task 2: BidiLine, one line's tokens to pieces in visual order

**Files:**
- Create: `lib/Epub/Epub/BidiLine.h`, `lib/Epub/Epub/BidiLine.cpp`
- Create: `test/bidi_line/CMakeLists.txt`, `test/bidi_line/BidiLineTest.cpp`
- Modify: `test/CMakeLists.txt`:
  - `add_subdirectory(bidi_line)`;
  - `BIDI_LAYOUT_SOURCES`: `lib/MiniBidi/minibidi.c`, `BidiUtils.cpp`, `lib/TextShaping/TextShaping.cpp`,
    `HebrewShaper.cpp`, `lib/Epub/Epub/BidiLine.cpp`;
  - `BIDI_LAYOUT_INCLUDES`: `lib/MiniBidi`, `lib/TextShaping`;
  - both set before the subdirectories, for Task 6's targets too.

**Interfaces:**
- Consumes: Task 1; `TextShaping::shape` and `Unit` (PR 1); `BidiUtils::applyBidiVisual`, `detectParagraphLevel`,
  `computeVisualWordOrder`, `isTransparentMark`; `BuildArena` (`lib/Memory`).
- Produces (namespace `BidiLine`):
  ```cpp
  inline constexpr size_t kMaxCodepoints = BidiUtils::kMaxLineCodepoints;
  inline constexpr uint8_t kMaxOpenControls = 8;
  bool isControl(uint32_t cp);      // U+200E, 200F, 061C, 202A-202E, 2066-2069
  bool dropControls(std::string& text);  // removes them; true when it removed any
  struct OpenControls {             // the embeddings, overrides and isolates open, outermost first
    uint32_t cps[kMaxOpenControls] = {};
    uint8_t count = 0;
    uint8_t overflow = 0;           // openers past kMaxOpenControls; their closers consume these first
    // LRE, RLE, LRO, RLO, LRI, RLI, FSI push. PDF pops the innermost entry if it is not an isolate. PDI pops up to
    // and including the innermost isolate, and does nothing when none is open (UAX #9 X6a, X7).
    void advance(std::string_view text);
  };
  struct Token { std::string_view text; bool spaceBefore; };  // spaceBefore: TokenBoundary::hasSpace
  enum class Gap : uint8_t { None, Glued, Space };
  struct Piece {
    uint16_t token;                   // index into the line's tokens
    uint16_t begin, end;              // bytes of that token's text; a token's pieces, in logical order, cover it exactly
    uint16_t displayBegin, displayLength;  // the drawn form in Line::display
    Gap gapBefore;                    // from the piece drawn left of it; None for the first
    uint16_t spaceToken;              // Gap::Space: the token whose word space it is
    uint8_t readingIndex;             // its place in reading order
  };
  struct Line {
    std::vector<Piece> pieces;        // visual order, left to right
    std::string display;              // drawn forms: visual order, mirrored, marks after base, no controls or U+00AD
    bool reordered = false;           // a readingIndex differs from its visual index
    bool fallback = false;            // over kMaxCodepoints
    void clear();                     // keeps capacity
  };
  struct Scratch {  // plain arrays of kMaxCodepoints entries: cps, owning token, byte offset, visual, drawn,
                    // shaped units, an isolate's outer pass, isolate spans; about 4.5 KB
  };
  // False (out cleared) only when BidiUtils has no workspace: the caller lays the line out without bidi.
  bool layoutLine(const Token* tokens, size_t count, const OpenControls& open, uint8_t paragraphLevel,
                  Scratch& scratch, Line& out);
  // A token's own drawn form, which breaking measures: TextShaping::shape, then applyBidiVisual at
  // detectParagraphLevel(token, paragraphLevel) with open's controls prepended, then dropControls and U+00AD
  // dropped. False, out untouched, when that is the token's text byte for byte.
  bool tokenForm(std::string_view token, const OpenControls& open, uint8_t paragraphLevel, Scratch& scratch,
                 std::string& out);
  // Lends a block of `arena` holding the workspace and a Scratch for one scope; unbinds and releases it on
  // destruction. scratch() is null with no arena or no room (the caller then uses its own heap Scratch).
  class ArenaWorkspace {
   public:
    explicit ArenaWorkspace(BuildArena* arena);
    ~ArenaWorkspace();
    Scratch* scratch() const;
  };
  ```

**The pass** (`layoutLine`):
1. **The logical sequence.** `open`'s code points, then each token's units, with one U+0020 separator before each
   token after the first that has `spaceBefore`.
   - A token's units are `TextShaping::shape`'s when it returns true; else its code points, stepping like
     `utf8NextCodepoint`.
   - Each unit records its token and its cluster byte offset; a separator records the token it precedes.
   - Glued tokens and CJK gaps get no separator. Over `kMaxCodepoints`, go to 5.
2. **Isolates (decision 8).** An initiator (U+2066-2068) spans to its matching PDI, or to the line's end. Spans are
   handled innermost first. For a span, and last for the line:
   - build its outer sequence: its direct characters, with one U+FFFC per child span;
   - `orderLine` it at the span's level: LRI 0, RLI 1, FSI the first strong letter (`strongLevel`) of its content
     outside nested spans, else 0; the line's own level is `paragraphLevel`;
   - expand: each U+FFFC becomes the child's order, initiator first and PDI last.
   - `drawn` is kept per logical index: each character is ordered and mirrored exactly once.
3. **Pieces.** Walk the visual order:
   - controls and U+00AD join no run;
   - a separator ends the run, and sets the next piece's `gapBefore = Space` with its token;
   - a change of token ends the run with `Glued`;
   - each piece's display is its units' drawn code points in visual order.
   - Then for each token, sort its pieces by first byte: the first begins at 0, each ends where the next begins,
     and the last ends at the token's length.
   - A token with no drawn character gives no piece. Two separators with no piece between them are one gap (the
     later's token).
   - If a shaped cluster's units land in two pieces, go to 5 (it cannot happen in step 1).
4. **Reading order.** `readingIndex` ranks pieces by (token, begin). `reordered` is set when any differs from its
   visual index.
5. **Fallback** (`fallback = true`, upstream's result):
   - one piece per token, its display `tokenForm` (or the token's text);
   - the order is `computeVisualWordOrder(tokens, paragraphLevel == 1)`; when that returns false, reversed for level
     1 and identity for 0;
   - visual neighbours that are logical neighbours keep the later one's glue (`Glued` when it is glued, else
     `Space`); any other pair is `Space`, with the right-hand piece's token.

- [ ] **Step 1: Write the failing tests** in `test/bidi_line/BidiLineTest.cpp`.
  - A helper renders a `Line` as `t<token>:<display>` items joined by ` S<n> ` for `Space(n)` and ` G ` for `Glued`.
    Expected values were checked against minibidi on 2026-11-02.
  - `AMixedLineInARightToLeftParagraph`: {"שלום", "(world)", "1998."} at 1 → `t2:.1998 S2 t1:(world) S1 t0:םולש`;
    readings {2, 1, 0}; reordered.
  - `AMixedLineInALeftToRightParagraph`: {"He", "said", "שלום."} at 0 → `t0:He S1 t1:said S2 t2:םולש.`; not
    reordered; the third piece covers bytes [0, 9).
  - `BracketsAroundHebrewInEnglish`: {"The", "word", "(שלום)", "means"} at 0 → `... S2 t2:(םולש) S3 t3:means`.
  - `TheMarkupParagraphOfTestRtlMarkup`: {"מילה", "\u2068\u202Dbdo", "text\u202C\u2069", "ו\u2068bdi",
    "text\u2069", "ו\u2066Latin", "123\u2069"} at 1 →
    `t5:Latin S6 t6:123 G t5:ו S5 t3:bdi S4 t4:text G t3:ו S3 t1:bdo S2 t2:text S1 t0:הלימ`.
    - readings {7, 8, 6, 4, 5, 3, 1, 2, 0};
    - token 5's pieces are bytes [0, 5) ("ו" and its LRI) and [5, 10).
  - `AnIsolateKeepsItsNeighboursOutside`: {"\u2067שלום\u2069:", "3", "posts"} at 0 →
    `t0:םולש: S1 t1:3 S2 t2:posts`.
  - `AnIsolateOpenAtTheLineStartIsOrderedOnItsOwn`: {"שלום\u2069", "1998"} at 0 with open {RLI} →
    `t0:םולש S1 t1:1998`. With open {} it is `t1:1998 S1 t0:םולש`.
  - `AnOverrideOpenAtTheLineStartStillHolds`: {"ghi\u202C", "jkl"} at 0 with open {RLO} → `t0:ihg S1 t1:jkl`.
  - `ALeftToRightMarkKeepsPlusSignsWithTheirLetter`: {"C++\u200E", "שלום"} at 1 → `t1:םולש S1 t0:C++`. Without the
    LRM, `t1:םולש S1 t0:++C`.
  - `ALoneControlTokenGivesNoPiece`: {"abc", "\u2066", "def"} at 0 → `t0:abc S2 t2:def`.
  - `NiqqudFollowsItsLetter`: {"שָׁלוֹם"} at 1 → display {05DD, 05D5, 05B9, 05DC, 05E9, 05B8, 05C1}.
  - `TheDivineNameIsDrawnAsHeAndGeresh`: {"יהוה"} at 1 → one piece, bytes [0, 8), display {05F3, 05D4}.
  - `ALineOverTheLimitFallsBackToUpstreamsOrder`: 26 × "אבגד" at 1 (129 code points) → fallback; pieces tokens
    25…0; each display "דגבא"; every gap `Space`.
  - `ALongLeftToRightLineKeepsItsOrder`: 13 × {"abcd", "אבגד"} at 0 → fallback; identity order; the Hebrew pieces
    display "דגבא".
  - `TheOpenControlsFollowTheText`: from {}:
    - "\u2067abc" → {RLI}; "def\u2069" → {};
    - "\u202Bx\u2066y" → {RLE, LRI}; then "\u202C" → unchanged; "\u2069" → {RLE}; "\u202C" → {};
    - "\u2069" on {} → {};
    - 10 × RLE then 2 × PDF → count 8, overflow 0; then 8 × PDF → count 0.
  - `ATokensOwnFormIsMadeAtItsOwnDirection` (`tokenForm`):
    - "abc(שלום)" at 1 → "abc(םולש)"; "שלום" at 0 → "םולש";
    - "abc" at 1 → false; "יהוה" at 1 → {05F3, 05D4};
    - "abc" with open {RLO} at 0 → "cba"; "C++\u200E" at 1 → "C++".
  - `DropControlsLeavesTheText`: "a\u200Eb\u2067c\u202Cd" → "abcd", true; "abc" → false.
  - `AnArenaLendsTheWorkspaceAndScratch`:
    - a 16 KB `BuildArena` → `scratch()` non-null; `heapWorkspaceHeld()` stays false through a `layoutLine`;
    - after the scope the arena's `used()` is back where it was;
    - a 1 KB arena → `scratch()` null and `used()` unchanged.
- [ ] **Step 2: Run them:** `cmake --build build/test -j 6 --target BidiLineTest` fails (no `BidiLine.h`).
- [ ] **Step 3: Implement `BidiLine.h/.cpp`** as the pass above says. `ArenaWorkspace` reserves one block,
  allocates `kWorkspaceBytes` (align 4) and `sizeof(Scratch)` (its alignment), binds, and on destruction unbinds
  then releases. When the second allocation fails, it releases the block and binds nothing.
- [ ] **Step 4: Run** `BidiLineTest`: all pass.
- [ ] **Step 5: Commit** `feat(epub): the bidi line pass, pieces in visual order with isolates on their own`. The
  body names decisions 5 and 8 and upstream #2541's fallback.

### Task 3: TextBlock, display forms and logical order (v108)

**Files:**
- Modify: `lib/Epub/Epub/blocks/TextBlock.h` (layout comment 30-44, members 133-172, `WordRange` 219-233, accessors)
- Modify: `lib/Epub/Epub/blocks/TextBlock.cpp` (`arenaSize` 82-85, range ctor 189-364, `wordBox` 467-512, `render`
  514-775, `serialize` 777-814, `deserialize` 816-914)
- Modify: `lib/Epub/Epub/Section.cpp:36` (108 and its comment line)
- Modify: `test/epub_pipeline/PipelineRunner.cpp` (`dumpWords`, `dumpTextLine`, the table cell lines)
- Modify: `docs/file-formats.md` (the two TextBlock bullets, lines 261-262)
- Create: `test/text_block_range/TextBlockBidiTest.cpp` (added to `TextBlockRangeTest`)

**Interfaces:**
- Produces:
  - `WordRange::displayForms` (`const std::vector<std::string>*`, indexed like `words`; null or an empty string:
    none);
  - `WordRange::logicalOrder` (`const std::vector<uint8_t>*`, indexed from the line's first word; null or empty:
    reading order is stored order);
  - `const char* wordDrawText(uint16_t i) const` (the display form, else the text; NUL-terminated);
  - `bool hasDisplayForm(uint16_t i) const`;
  - `uint8_t logicalPosition(uint16_t i) const` (i when there is no order);
  - `bool hasLogicalOrder() const`.
- **The bidi region** follows the annotation region, read with memcpy:
  - `u8 parts`: bit 0 display forms, bit 1 logical order;
  - [display: `u16 offset[wordCount]` into the texts that follow, `0xFFFF` for none; then the forms, each
    NUL-terminated];
  - [order: `u8 position[wordCount]`].
  - `uint16_t bidiBytes_` is its size, 0 for none. `arenaOwned_` becomes a bitfield beside `vertical_` so the struct
    stays 52 B / 88 B.
- **Construction:**
  - a form is stored only when it differs from the word's stored text (soft hyphens stripped);
  - an order only when it is not the identity;
  - an order that is not a permutation of [0, count), or that has more than 255 words, makes the block invalid
    (`LOG_ERR`).
- **On disk:** the header is `u16 numWords | (bidiBytes_ != 0 ? 0x8000 : 0)`, `u8 flags`, `u16 textBytes`,
  `[u16 annotationBytes]`, then `[u16 bidiBytes]`.
- **`deserialize` rejects:**
  - `parts` not in 1-3;
  - sizes that do not add up (1 + 2·wc + texts + wc);
  - an offset that is neither 0xFFFF nor inside the texts, a form not NUL-terminated inside them, a display part
    with no form;
  - an order that is not a permutation, or wc > 255 with an order.
- **The dump:**
  - after a word's `W` (and `WF`) record: `<indent> WD t=<form>`, when it has one;
  - after the line's words and annotations: `<indent>O order=<p0>,<p1>,...`, when it has an order.
  - Neither matches the word-stream pattern `\bW x=\S+ s=\S+ z=\S+ t=(.*)`.

- [ ] **Step 1: Write the failing tests** in `TextBlockBidiTest.cpp`, with the shim's recorder and
  `HalFile::forReadWrite`:
  - `AFormIsKeptOnlyWhereItDiffers`: words {"abc", "שלום"}, forms {"abc", "םולש"} → `hasDisplayForm(0)` false,
    `(1)` true, `wordDrawText(1) == "םולש"`, `wordText(1) == "שלום"`.
  - `ASoftHyphenatedWordDrawnAsItsTextHasNoForm`: "ab\xC2\xADc" with form "abc" → none.
  - `TheLogicalOrderIsKeptOnlyWhenItReorders`: {1, 0} → kept, positions 1 and 0; {0, 1} → `hasLogicalOrder()` false.
  - `ALineWithoutTheRegionKeepsItsBytes`: words {"abc", "def"} with forms {"abc", "def"} and order {0, 1} serialise to
    the same bytes as with both null; bit 15 of the word count is clear.
  - `TheRegionSurvivesTheCache`: forms and order round-trip; `bytes[1] & 0x80` is set.
  - `ACorruptRegionFailsTheRead`: the order rewritten to {0, 0}, and an offset rewritten to 0x7000, each make
    `deserialize` return nullptr.
  - `RenderDrawsTheForm`: `drawCalls[1].text == "םולש"`.
  - `AWordBoxMeasuresTheForm`: "abc\u200E" with form "abc" → the width of three glyphs.
- [ ] **Step 2: Run them:** they fail to compile (`displayForms` unknown).
- [ ] **Step 3: Implement** the region, the accessors and the header bit. `render` and `wordBox` read
  `wordDrawText(i)` wherever they read `wordText(i)` today, except the emphasis loop (no bidi line has emphasis).
  - Update the dump.
  - Set `SECTION_FILE_VERSION = 108` with
    `// v108: right-to-left lines: a word's display form and the line's logical order (the TextBlock bidi region, flagged by the word count's top bit)`.
  - Update `docs/file-formats.md`: the word count's top bit, and the region's layout as above.
- [ ] **Step 4: Run** `TextBlockRangeTest`, `VerticalTextTest`, `EpubPipelineTest`: pass. No golden moves (nothing
  writes the region yet).
- [ ] **Step 5: Commit** `feat(epub): TextBlock keeps a word's display form and the line's logical order (v108)`.

### Task 4: The renderer, right-to-left marks over their letter

**Files:**
- Modify: `lib/EpdFont/EpdFontData.h:33-62` (`combiningMark`: upstream's `Anchor`, `anchorFor`, `anchorShift`,
  `anchorOver`, `anchorOverRotated90CW`, `raiseAboveBase(anchor, …)`, verbatim from `e8299b5f`)
- Modify: `lib/EpdFont/EpdFont.cpp:60,69` (`anchorFor(cp)` with the anchored helpers)
- Modify: `lib/GfxRenderer/GfxRenderer.cpp`:
  - `drawText` 1476-1484;
  - `scaledTextAdvanceX` 3182;
  - `getTextAdvanceX` 3256;
  - `drawTextRotated90CW` 3360-3368.
- Test: `test/differential_rounding/DifferentialRoundingTest.cpp`

**Interfaces:**
- Consumes: `BidiUtils::isTransparentMark` (PR 1).
- Produces:
  - `combiningMark::Anchor`, `anchorFor(uint32_t)`, `anchorOver(Anchor, …)`, `anchorOverRotated90CW(Anchor, …)`,
    `raiseAboveBase(Anchor, markTop, markHeight, baseTop)`, with upstream's signatures;
  - in `GfxRenderer.cpp`, `static bool overlaysItsBase(uint32_t cp)`:
    `utf8IsCombiningMark(cp) || (cp >= 0x0591 && cp <= 0x08FF ? BidiUtils::isTransparentMark(cp) : cp == 0xFB1E)`.
    It replaces `utf8IsCombiningMark` at the four sites, which then place a mark with
    `anchorOver(anchorFor(cp), …)` / `anchorOverRotated90CW` and `raiseAboveBase(anchorFor(cp), …)`.
  - A face without the mark's glyph skips it (the existing `if (!combiningGlyph) continue;`).
  - Upstream's renderer bidi hooks stay out (deviation 1): `resolveVisualText`, `appendShapedRtlTokens`, and the
    `BidiBaseDir` parameters.

- [ ] **Step 1: Write the failing tests** (`CombiningMark.*`):
  - `anchorFor`: 05BC → CenterNative, 05BA → CenterNative, 05C1 → RightNative, 05B9 → LeftNative,
    05C2 → LeftNative, 0301 → CenterRaised, 05B8 → CenterRaised.
  - `anchorOver(CenterRaised, 100, 1, 10, 2, 4)` == 102, the old `centerOver`'s value;
    `anchorOver(RightNative, 100, 1, 10, 2, 4)` == 105; `anchorOver(LeftNative, 100, 1, 10, 2, 4)` == 99.
  - `raiseAboveBase(CenterNative, 20, 4, 18)` == 0; `raiseAboveBase(CenterRaised, 20, 4, 18)` == 3, as the old
    three-argument function gives.
- [ ] **Step 2: Run** `DifferentialRoundingTest`: fails to compile.
- [ ] **Step 3: Port the anchors** from upstream's `EpdFontData.h`, keeping their comment. Rewire `EpdFont.cpp` and
  the four renderer sites. Add `#include <BidiUtils.h>` to `GfxRenderer.cpp`.
- [ ] **Step 4: Run** `DifferentialRoundingTest` and every target that compiles `EpdFont.cpp` (the full host build):
  pass. Build the firmware `default` env once (it is the only build of `GfxRenderer.cpp`): it links.
- [ ] **Step 5: Commit** `feat(gfx): Hebrew and Arabic marks sit on their letter, with the niqqud anchors`. The body
  says "ported from crosspoint-reader #2541 (932a4728)"; trailer `Co-authored-by: Husam Younis <youhusam@gmail.com>`.

### Task 5: Directions reach the paragraph

**Files:**
- Modify: `lib/Epub/Epub/TypographyProfile.h` (the `Direction` comment), `TypographyProfile.cpp` (`forLanguage`)
- Modify: `lib/Epub/Epub/blocks/BlockStyle.h` (`direction`, `getCombinedBlockStyle`)
- Modify: `lib/Epub/Epub/css/CssStyle.h:466-470` (`resolveLogical`)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`:
  - every BlockStyle built for an element: `userAlignmentBlockStyle` ~3136, `headerBlockStyle` ~3215;
  - the `<br>` block's style ~3280, which takes its block's direction;
  - the table cell's text, ~2472, on its `getBlockStyle()`.
- Modify: `lib/Epub/Epub/ParsedText.h/.cpp` (`resolveAlignment`, called at the top of `layoutAndExtractLines`)
- Test: `test/typography/TypographyTest.cpp`, `test/epub_css/CssParserTest.cpp:409-423`,
  `test/epub_pipeline/WritingModeLayoutTest.cpp`

**Interfaces:**
- Produces:
  - `TypographyProfile::forLanguage`: primary tags `he`, `iw`, `yi`, `ar`, `fa`, `ur` → `direction = RightToLeft`,
    `hyphenator = nullptr`.
  - `BlockStyle::direction` (`CssDirection`, default Unset): the paragraph's computed direction. It is `Ltr` or
    `Rtl` from dir or CSS; `Auto` for `dir="auto"` or `unicode-bidi: plaintext` on the block; Unset when nothing sets
    one. It is parse-time only, never serialised. `getCombinedBlockStyle` takes the child's when set, else the
    parent's.
  - `ChapterHtmlSlimParser::blockDirection(const CssStyle&) const`: `Auto` when `cssStyle.unicodeBidi` is
    Plaintext, else `elementFlow_.direction`.
  - `resolveLogical` resolves `text-align: start/end` only for a direction of Ltr or Rtl. Unset and Auto keep Start
    and End for the layout.
  - `void ParsedText::resolveAlignment(bool rtl)` (private): Start → (rtl ? Right : Left), End → (rtl ? Left :
    Right). Idempotent; until Task 6 it is called with `false`.

- [ ] **Step 1: Write the failing tests:**
  - `TypographyProfile.HebrewArabicPersianUrduAndYiddishRunRightToLeft`:
    - "he", "he-IL", "HE", "iw", "yi", "ar", "ar-EG", "fa", "ur" → RightToLeft, hyphenator null, not
      `verticalAllowed`, not `cjkRepertoire`;
    - "en", "de", "ja", "hy" → LeftToRight.
  - `CssLogical.StartAndEndFollowTheDirection` updated: Unset and Auto keep Start (and End); Ltr and Rtl as before.
  - `BlockStyle.TheChildsDirectionWinsElseTheParents` (in `WritingModeLayoutTest.cpp`): a parent Rtl with a child
    Unset → Rtl; a child Ltr → Ltr.
  - `WritingModeLayout.TextAlignStartStillIndentsALatinParagraph` (book alignment):
    `.s { text-align: start; text-indent: 1em }` with `<p class="s">Alpha bravo</p>` → `align=1`, first word
    `x=18`.
- [ ] **Step 2: Run** `TypographyTest`, `CssParserTest`, `EpubPipelineTest`: the new expectations fail.
- [ ] **Step 3: Implement.** Set `direction` from `blockDirection(cssStyle)` at the listed sites.
- [ ] **Step 4: Run** the three targets and `CjkLayoutTest`, `WordSizeLayoutTest`: pass. Every golden is unchanged.
- [ ] **Step 5: Commit** `feat(epub): Hebrew, Arabic, Persian, Urdu and Yiddish books run right to left; a paragraph keeps its direction for layout`.

### Task 6: ParsedText lays a paragraph that needs bidi out by the line pass

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.h` (members below), `lib/Epub/Epub/ParsedText.cpp`:
  - `layoutAndExtractLines` 477-798 (decision, `resolveAlignment`, `preserveSource` 494-511/788-797);
  - the indent at 593-599 and 1374-1375;
  - `calculateWordWidths` 921-936;
  - `extractLine` 1827-2182;
  - `reset` 906-919 and `releaseLayoutScratch` 865-904.
- Modify: `lib/Epub/Epub/Section.cpp` (`BuildState::~BuildState` ~993: `BidiUtils::releaseWorkspace()`)
- Modify: `test/epub_pipeline/CMakeLists.txt` (`PIPELINE_SOURCES`/`PIPELINE_INCLUDES`),
  `test/cjk_layout/CMakeLists.txt`, `test/word_size_layout/CMakeLists.txt` (`BIDI_LAYOUT_SOURCES`/`_INCLUDES`)
- Modify: `test/epub_pipeline/WritingModeLayoutTest.cpp`:
  - `writeBook`/`dump` take a trailing `lang = "en"`;
  - `parseLines` keeps each word's `x`, `t` and `WD` form and the line's `O` order;
  - a "---- Right to left ----" section.
- Goldens: `test_rtl_markup.golden.txt`, `test_rtl_markup_bookalign.golden.txt`

**Interfaces:**
- Consumes: Tasks 1-3 and 5.
- Produces (private to `ParsedText`):
  - `bool bidi_` (the block needs bidi; sticky across passes), `uint8_t level_`, `bool levelDecided_`;
  - `BidiLine::OpenControls openControls_` (at the next line's start); `std::unique_ptr<BidiLine::Scratch> bidiScratch_`;
  - `BidiLine::Line bidiLine_`;
  - the per-line piece vectors `pieceWords_`, `pieceForms_`, `pieceStyles_`, `pieceSizes_`, `pieceBoundaries_`,
    `pieceOrder_`.
  - `reset()` clears the flags and the controls; `releaseLayoutScratch()` frees the scratch and the piece vectors;
    `preserveSource` saves and restores `openControls_`.
  - `bool rtl() const { return bidi_ && level_ == 1; }`.

**The paragraph** (at the top of `layoutAndExtractLines`, before the indent):
- The pass needs bidi when `blockStyle.direction == Rtl` or a token `hasRtlClass`. The flag is sticky: once set, the
  block keeps it.
- A pass that does not need bidi `dropControls` its tokens and erases any token left empty (all parallel vectors).
- Until Task 7 the level is 1 for `Rtl`, else 0, decided once (`levelDecided_`).
- Then `resolveAlignment(rtl())`, and in a right-to-left block `Left` with `!textAlignDefined` becomes `Right`.
- The indent conditions read "Justify or the start side": `(rtl() ? Right : Left)`. The hanging clamp is
  `-(rtl() ? rightInset() : leftInset())`.

**Widths** (`calculateWordWidths`): in a bidi block, inside one `ArenaWorkspace(lineArena_)`, walk a copy of
`openControls_` over the tokens. Each token whose `tokenForm` returns true is measured by that form. Hyphenation
needs no change: `isAlphabetic` is Latin and Cyrillic only, so the patterns offer no break inside a Hebrew word, and
an overlong one still takes the fallback split.

**A line** (`extractLine`), in a bidi block:
- Run `layoutLine` over the line's tokens with `openControls_`, inside an `ArenaWorkspace(lineArena_)` scope that
  ends before `beforeLine_`. The scratch is the scope's, else `bidiScratch_` (made nothrow on first use; on failure
  log and lay the line out without bidi).
- When the line is not reordered, has one piece per token with no form different from its text, and `!rtl()`: the
  existing path, unchanged.
- Otherwise position the pieces in visual order:
  - widths `measureWordWidth` of each piece's form in its token's font, style and scale;
  - a `Space` gap is `spaceBefore(j) × wordScale(spaceSource(j))` for `j = lastBreakAt + spaceToken`, and is
    justifiable;
  - a `Glued` gap is the kerning between the two forms' facing code points in the left piece's font when both share
    it, times its scale;
  - a piece whose form is " " is a justifiable gap, as today;
  - `extra = min(spare / gaps, effectivePageWidth / 8)` when `Justify`, not the last line, and gaps ≥ 1;
  - the start x when `rtl()`:
    - Right or Justify: `gridInset_ + effectivePageWidth − content`;
    - Center: `gridInset_ + (effectivePageWidth − content) / 2`;
    - Left: `gridInset_`.
  - Left to right, the start x is as today.
  - The pieces' texts are substrings of their tokens; styles and `WordSize` are their token's; `continues` is
    `gapBefore != Space`. The forms are `display` slices, the order is `readingIndex` when `reordered`, and there
    are no annotations.
- After `processLine` returns Accepted, `openControls_` steps over the line's tokens (a retried line steps once).
  `lastLineVisibleOffset_` is the line's first logical token's, as today.

- [ ] **Step 1: Write the failing tests** (book alignment unless said; widths from the stub: שלום 32, עולם 32,
  "(world)" 66, ".1998" 36, He 17, said 35, space 6):
  - `BidiLayout.ARightToLeftLineRunsFromTheRightEdge`: `<p dir="rtl">שלום עולם</p>` → words
    (390, "עולם", WD "םלוע") and (428, "שלום", WD "םולש"); `O order=1,0`.
  - `BidiLayout.AMixedLineInARightToLeftParagraph`: `<p dir="rtl">שלום (world) 1998.</p>` →
    (314, "1998.", WD ".1998"), (356, "(world)", no WD), (428, "שלום", WD "םולש"); `O order=2,1,0`.
  - `BidiLayout.AMixedLineInALeftToRightParagraph`: `<p dir="ltr">He said שלום.</p>` → (0, "He"), (23, "said"),
    (64, "שלום.", WD "םולש."); no O record.
  - `BidiLayout.TheFirstLineIndentGoesOnTheRight`: `<p dir="rtl" style="text-indent: 2em">שלום עולם</p>` →
    x 354 and 392.
  - `BidiLayout.AHangingIndentHangsIntoTheRightInset`:
    `<p dir="rtl" style="margin-right: 2em; text-indent: -1em">שלום עולם</p>` → LINE x=0, words at 372 and 410.
  - `BidiLayout.ARightToLeftParagraphTurnsTheUsersLeftAlignmentRight`:
    - profile `paragraphAlignment = 1` with `<p dir="rtl">שלום</p>` → align=3, x 428;
    - book alignment with `<p dir="rtl" style="text-align: left">שלום</p>` → align=1, x 0.
  - `BidiLayout.AJustifiedRightToLeftLineFillsTheMeasure`: viewport 472, `<p dir="rtl">` with 20 × "שלום", default
    profile →
    - line 1: 12 words, extra 2 per gap, its leftmost at 0, its rightmost ending at 472;
    - line 2: 8 words, its rightmost ending at 472, its leftmost at 174.
    - At 460 the remainder of `spare / gaps` would stay undistributed at the left, as it stays at the right today.
  - `BidiLayout.ALineOverTheBidiLimitStillRunsRightToLeft`: viewport 1400, `<p dir="rtl">` with 30 × "שלום" →
    one line; word k at x = 266 + 38k, each WD "םולש"; `O order=29,28,…,0`.
  - `BidiLayout.ALentArenaBuildLaysOutAsAHeapBuild`: the corpus `test_rtl_markup.epub` and the mixed-line book above,
    dumped with `lentArenaBytes` 0, 49152 and 16384 → three identical dumps each.
  - `BidiLayout.TheBidiWorkspaceIsGoneWhenTheBuildEnds`: after dumping the mixed-line book,
    `BidiUtils::heapWorkspaceHeld()` is false.
- [ ] **Step 2: Run** `EpubPipelineTest` (`-R "BidiLayout|WritingModeLayout"`): they fail.
- [ ] **Step 3: Implement** as above, and wire `BIDI_LAYOUT_SOURCES` into the three targets.
- [ ] **Step 4: Run** all of `EpubPipelineTest`, `CjkLayoutTest`, `WordSizeLayoutTest`, `TextBlockRangeTest`.
  - Only the two `test_rtl_markup` goldens move; refresh them.
  - Check them by hand and say so in the commit: every Hebrew word has a WD with its letters reversed; each line has
    the O order its words read in; the bookalign start lines end at 460.
  - The `dir="auto"` and `<bdo>`/`<bdi>` paragraphs are still as before (Tasks 7 and 9).
- [ ] **Step 5: Commit** `feat(epub): a paragraph that needs bidi is laid out one UAX #9 pass per line`. The body
  names decision 5, upstream #1700/#2541 (the reordered loop, the default alignment flip) and the deviation 1
  mechanism.

### Task 7: ParsedText, the paragraph's direction when the markup does not give one

**Files:**
- Modify: `lib/Epub/Epub/ParsedText.cpp` (the level decision in `layoutAndExtractLines`)
- Modify: `test/epub_pipeline/WritingModeLayoutTest.cpp` (`DirAutoReadsAsLtrForNow` becomes
  `DirAutoTakesTheFirstStrongLetter`)
- Goldens: the two `test_rtl_markup` goldens (the `dir="auto"` paragraph)

**Interfaces:**
- Consumes: `typography_.direction` (Task 5), `BidiUtils::strongLevel`.
- The level, decided on the block's first pass (`!isContinuation_`), whether or not that pass needs bidi, and kept:
  - Ltr 0, Rtl 1;
  - Auto: the first `strongLevel ≥ 0` over the pass's tokens in order, else the book's direction;
  - Unset in a right-to-left book: 1 iff the pass has an R/AL code point;
  - Unset otherwise: 1 iff it has R/AL and no L.
  - For Unset, a pass with no `hasRtlClass` token decides 0 without scanning for L. A later pass that first needs
    bidi uses the level decided on the first.
  - Task 6's needs-bidi test now reads `level_ == 1` where it read `direction == Rtl`, so an Auto paragraph that falls
    back to a right-to-left book's direction is laid out right to left.
  - `resolveAlignment` then reads it.

- [ ] **Step 1: Write the failing tests** (lang as given; book alignment):
  - `BidiLayout.AHebrewBooksParagraphRunsRightToLeftWhenItHasAHebrewLetter` (he): `<p>Wiki שלום</p>` →
    (393, "שלום") and (431, "Wiki"); `O order=1,0`.
  - `BidiLayout.AHebrewBooksParagraphWithoutAHebrewLetterRunsLeftToRight` (he): `<p>Only English, 2024.</p>` →
    first word x 0; no WD, no O.
  - `BidiLayout.AnEnglishBookQuotingHebrewStaysLeftToRight` (en): `<p>He said שלום.</p>` → x 0, 23, 64.
  - `BidiLayout.AHebrewOnlyParagraphInAnEnglishBookTurnsRightToLeft` (en): `<p>שלום עולם</p>` → x 390, 428.
  - `WritingModeLayout.DirAutoTakesTheFirstStrongLetter`: `<p class="s" dir="auto">שלום</p>` → align=3;
    `<p class="s" dir="auto">abc שלום</p>` → align=1.
  - `BidiLayout.StartAndEndFollowTheDecidedDirection` (he): `.s`/`.e` on "שלום" → 3 and 1;
    `.s` on "Only English" → 1.
  - `BidiLayout.PlaintextOnABlockTakesTheFirstStrongLetter` (en): `.pt { unicode-bidi: plaintext }` with
    `<p class="pt s">שלום</p>` → align=3.
  - `BidiLayout.TheDirectionHoldsAcrossTheLongBlockSplit`: `<p>Wiki` followed by 120 × " שלום" (121 tokens, past the
    96-token flush), default profile.
    - he: every line's rightmost word ends at 460;
    - en: every line's leftmost word is at 0.
    - Widths come from the stub `GfxRenderer::getTextAdvanceX` on the WD form or the text.
- [ ] **Step 2: Run** `EpubPipelineTest -R "BidiLayout|WritingModeLayout"`: the new ones fail.
- [ ] **Step 3: Implement** the decision.
- [ ] **Step 4: Run** all of `EpubPipelineTest`.
  - Only the two `test_rtl_markup` goldens move: "טקסט אוטומטי" (dir=auto) is now right to left, WD-reversed and
    O-ordered, at align=3 in bookalign. Check by hand.
- [ ] **Step 5: Commit** `feat(epub): a paragraph without dir takes the book language's direction unless its letters say otherwise`.

### Task 8: The parser keeps literal bidi controls where a paragraph needs them

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`:
  - the char loop 3706-3905 (a bidi control is kept, not dropped by `defaultIgnorableLen`);
  - the three `partWordVisibleOffset_` sites (3729, 3809/3830, 3903);
  - `flushPartWordBuffer` ~1188;
  - `startNewTextBlock` ~1974.
- Modify: `ChapterHtmlSlimParser.h` (`bool partWordControlsOnly_`)
- Modify: `lib/Epub/Epub/ParsedText.cpp:403` and `:1779-1784`: offsets via a file-local
  `visibleBytesOutsideControls(const char*, int)`, `VisibleText::visibleBytes` minus every bidi control's bytes
- Test: `WritingModeLayoutTest.cpp`, `test/epub_pipeline/VisibleTextOffsetLutTest.cpp`,
  `test/epub_pipeline/ParsedTextOffsetTest.cpp`

**Interfaces:**
- Consumes: Task 6's drop of controls from passes that do not need bidi.
- Rules:
  - A bidi control (`BidiLine::isControl`) in character data goes into the word buffer and advances `off` as today.
  - A buffer holding only controls sets `partWordControlsOnly_`. The first other byte sets `partWordVisibleOffset_`
    and clears it.
  - `flushPartWordBuffer` with a controls-only buffer adds no word and keeps the bytes for the next word.
  - `startNewTextBlock` drops a controls-only buffer.

- [ ] **Step 1: Write the failing tests:**
  - `BidiLayout.AMarkInAnEnglishParagraphIsDroppedAsBefore` (en): `<p>abc&#x200E; def</p>` → words "abc" and
    "def", byte for byte; no WD.
  - `BidiLayout.AMarkInAHebrewParagraphIsKeptAndNotDrawn`: `<p dir="rtl">C++&#x200E; שלום</p>` →
    (398, "שלום", WD "םולש") and (436, "C++\u200E", WD "C++").
  - `BidiLayout.AWordOfMarksJoinsTheNextWord`: `<p dir="rtl">שלום &#x200F; עולם</p>` → two words, "שלום" and
    "\u200Fעולם".
  - `BidiLayout.AMarkEndingAParagraphLeavesTheNextOneClean`: `<p dir="rtl">שלום &#x200F;</p><p dir="rtl">עולם</p>` →
    the second paragraph's word is "עולם".
  - `SyntheticLutFixture.KeptBidiMarksLeaveTheContentOffsetsAsTheyWere`:
    - books A (120 × `<p>&#x200F;שלום עולם טוב.</p>`) and B (the same without the RLM);
    - equal page counts;
    - for every page, `A(p) == B(p) + 3 × (1 + B(p) / 23)` (23 visible bytes a paragraph, each page starting on a
      paragraph), which is master's count.
  - `ParsedTextOffset.ABidiControlCountsNothingWhereAWordIsSplit`: `addWord("日\u200E本", …, visibleOffset 100)`
    laid out 20 px wide → line offsets {100, 103}.
- [ ] **Step 2: Run** `EpubPipelineTest`: they fail.
- [ ] **Step 3: Implement** the rules and the offset helper.
- [ ] **Step 4: Run** all of `EpubPipelineTest`: pass; no golden moves (no fixture holds a literal control).
- [ ] **Step 5: Commit** `feat(epub): literal bidi controls are kept in paragraphs that need bidi and count nowhere`.

### Task 9: CSS and HTML bidi markup becomes control characters (deviation 5)

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h/.cpp`:
  - `startElement` after `enterFlow`/`resolveLogical` (~2270), for an element whose content is laid out;
  - `endElement` ~3924;
  - a stack `struct BidiEdge { int16_t depth; uint32_t closers[2]; uint8_t count; } bidiEdges_[kMaxFlowEntries]`
    with its count. An element nested past `kMaxFlowEntries` inserts nothing, neither opener nor closer.
- Goldens: the two `test_rtl_markup` goldens (the `<bdo>`/`<bdi>`/`<span dir>` paragraph)

**Interfaces:**
- Consumes: Task 8's buffer rules.
- Produces: `void ChapterHtmlSlimParser::appendBidiControl(uint32_t cp)`. It puts 3 UTF-8 bytes in the word buffer
  (flushing a full one first), never advances `off`, and sets `partWordControlsOnly_` on an empty buffer.
- The controls, by `unicode-bidi` and `elementFlow_.direction` (CSS Writing Modes 3, 2.4.2), at an inline element's
  start and end (the parser opens no block for it):

  | unicode-bidi | direction | start | end |
  |---|---|---|---|
  | embed | rtl / other | RLE 202B / LRE 202A | PDF 202C |
  | isolate | ltr / rtl / auto or unset | LRI 2066 / RLI 2067 / FSI 2068 | PDI 2069 |
  | bidi-override | rtl / other | RLO 202E / LRO 202D | PDF |
  | isolate-override | rtl / other | FSI RLO / FSI LRO | PDF PDI |
  | plaintext | any | FSI | PDI |

  A block element takes only the override of `bidi-override` and `isolate-override` (LRO/RLO … PDF). Its other values
  set the paragraph's direction (Task 5). The closers go at the matching `endElement`, popped by depth.

- [ ] **Step 1: Write the failing tests** (widths: מילה 37, Latin 51, 123 21, סוף 22, abc 25, fed 27, ghi 29,
  "םולש:" 40, 3 8, posts 46):
  - `BidiLayout.AnInlineDirSpanBecomesAnIsolate`: `<p dir="rtl">מילה <span dir="ltr">Latin 123</span> סוף</p>` →
    (311, "סוף"), (339, "\u2066Latin", WD "Latin"), (396, "123\u2069", WD "123"), (423, "מילה");
    `O order=3,1,2,0`.
  - `BidiLayout.AnIsolateKeepsANumberAfterItOutside`: `<p dir="rtl">שלום <span dir="ltr">abc</span> 123</p>` →
    (370, "123"), (397, "\u2066abc\u2069", WD "abc"), (428, "שלום"); `O order=2,1,0`.
  - `BidiLayout.ABdoOverridesLatinText`: `<p dir="ltr">abc <bdo dir="rtl">def</bdo> ghi</p>` → (0, "abc"),
    (31, "\u2068\u202Edef\u202C\u2069", WD "fed"), (64, "ghi").
  - `BidiLayout.ABdiKeepsAUserNameApart`: `<p dir="ltr"><bdi>שלום</bdi>: 3 posts</p>` →
    (0, "\u2068שלום\u2069:", WD "םולש:"), (46, "3"), (60, "posts").
  - `BidiLayout.UnicodeBidiInCss`: `.o { unicode-bidi: bidi-override; direction: rtl }` and
    `.e { unicode-bidi: embed; direction: rtl }` on spans around "def" → texts "\u202Edef\u202C" (WD "fed") and
    "\u202Bdef\u202C" (WD "def").
  - `BidiLayout.AnLtrSpanInAnEnglishBookChangesNothing` (en): `<p>abc <span dir="ltr">def</span> ghi</p>` → words
    "abc", "def", "ghi" at 0, 31, 64; no WD.
  - `BidiLayout.AnIsolateAcrossALineBreakKeepsItsOrder`: viewport 200,
    `<p dir="rtl">שלום <span dir="ltr">one two three four five six seven</span> עולם</p>`. On the line holding
    "עולם", the span's words have x increasing in their source order, and "עולם" is left of all of them.
  - `SyntheticLutFixture.InsertedControlsCountNowhere`: 120 × `<p><span dir="rtl">שלום</span> עולם טוב.</p>` against
    120 × `<p>שלום עולם טוב.</p>` → equal page starts.
- [ ] **Step 2: Run** `EpubPipelineTest`: they fail.
- [ ] **Step 3: Implement** the table and the stack. An element skipped as hidden inserts nothing.
- [ ] **Step 4: Run** all of `EpubPipelineTest`.
  - Only the two `test_rtl_markup` goldens move, the markup paragraph now as `TheMarkupParagraphOfTestRtlMarkup`
    (Task 2) orders it: "Latin", "123", "ו", "bdi", "text", "ו", "bdo", "text", "מילה". Check by hand.
- [ ] **Step 5: Commit** `feat(epub): dir, bdo, bdi and unicode-bidi become UAX #9 controls in the text (deviation 5)`.

### Task 10: Lists and tables on the start side

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h/.cpp`:
  - `startSideIsRight()`;
  - the list padding at 3138-3141;
  - `BufferedTable::rtl`, set at ~2383;
  - `layoutTableRow` 5461-5572, which reverses `out.cells` after the padding loop when `rtl`.

**Interfaces:**
- Produces: `bool ChapterHtmlSlimParser::startSideIsRight() const`: `elementFlow_.direction == Rtl`, or Unset/Auto
  with `typography_.direction == RightToLeft`.
- A list without its own start-side padding (`hasPaddingRight()` when right, else `hasPaddingLeft()`) gets
  `LIST_DEFAULT_PADDING_EM` on that side.

- [ ] **Step 1: Write the failing tests** (book alignment; widths • 7, ".1" 16; an `li` takes the default 1em
  first-line indent, as in `test_lists`, so its line ends at 460 − 27 − 18 = 415):
  - `BidiLayout.AListIsIndentedOnTheRight`: `<html dir="rtl">` with `<ul><li>שלום</li></ul>` → LINE x=0, (370,
    "שלום"), (408, "•"). With `<ol><li>שלום</li></ol>` → (361, "שלום"), (399, "1.", WD ".1").
  - `BidiLayout.AListInAHebrewBookWithoutDirIsIndentedOnTheRight` (he): `<ul><li>שלום</li></ul>` → LINE x=0.
  - `BidiLayout.ARightToLeftTableRunsItsColumnsRightToLeft` (he): `<table><tr><td>א</td><td>ב</td><td>ג</td></tr></table>`
    → CELL 0 "ג", CELL 1 "ב", CELL 2 "א".
    - `<tr><td colspan="2">אב</td><td>ג</td></tr>` → CELL 0 "ג" span=1, CELL 1 "אב" span=2;
    - `<table dir="ltr">` → CELL 0 "א".
- [ ] **Step 2: Run** `EpubPipelineTest`: they fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all of `EpubPipelineTest`: pass; no golden moves (`test_rtl_markup` has no list or table).
- [ ] **Step 5: Commit** `feat(epub): lists and tables start on the right in right-to-left text`. The body names
  upstream #3891 (0305aaa6) for the table.

### Task 11: The fixture book, its goldens and the rule checker

**Files:**
- Modify: `test/epubs/make_test_writing_modes.py`:
  - a third book, `test_bidi_text.epub`;
  - the docstring says what each part pins;
  - the other two books regenerate byte for byte.
- Modify: `test/epub_pipeline/EpubPipelineTest.cpp` (`kBookAlignmentBooks` gains `test_bidi_text`)
- Create: `test/epub_pipeline/BidiRulesTest.cpp` (added to `EpubPipelineTest`)
- Goldens: `test_bidi_text.golden.txt`, `test_bidi_text_bookalign.golden.txt`

**The book:**
- Language `he`, no page progression, CSS `body { margin: 0 } p { margin: 0; text-indent: 1em }`, plus `.left`,
  `.start` and `.hang { margin-right: 2em; text-indent: -1em }`.
- **Spine 1**, `<html lang="he">` with no `dir`, so decision 6 decides. In order:
  - an `h1` "פרק ראשון: בדיקה";
  - "הוא אמר (hello world) בשנת 1998, ואז הלך הביתה.";
  - "בספר The Book of Ruth יש ארבעה פרקים בלבד.";
  - "המחיר עלה מ־25 ל־3.50 שקלים, כלומר ב־14%.";
  - Genesis 1:1-3 vocalised, from "בְּרֵאשִׁית בָּרָא" to "וַיְהִי־אוֹר׃";
  - "וַיְדַבֵּר יְהוָה אֶל־מֹשֶׁה לֵּאמֹר׃ שִׁירוּ לַיהוָה שִׁיר חָדָשׁ׃";
  - "השפה C++&#x200E; פופולרית, וגם C#&#x200E; היא שפה. &#x200F;100 אחוז.";
  - "המשתמש <bdi>User_123</bdi> כתב 3 הודעות, והמשתמש <bdi>מנהל</bdi> כתב 5.";
  - "<bdo dir="rtl">reversed</bdo> ו<span dir="ltr">Latin 1-2-3</span>.";
  - "הערה ראשונה<a epub:type="noteref" href="ch3.xhtml#n1">1</a> והערה שנייה<a epub:type="noteref" href="ch3.xhtml#n2">2</a> באותה שורה.";
  - "This paragraph is English only, in a Hebrew book, 2024.";
  - "* * *";
  - `<ol>` with "פריט ראשון" and "פריט שני עם English"; `<ul>` with "נקודה";
  - a 2 × 3 table, א1 ב1 ג1 / א2 ב2 ג2;
  - `.left` "יישור לשמאל"; `.start` "יישור להתחלה";
  - `.hang` "פסקה עם הזחה תלויה שנמשכת יותר משורה אחת כדי לראות את השורה השנייה.";
  - "Wikipedia" followed by 10 × "היא אנציקלופדיה חופשית שכל אחד יכול לערוך, והיא נכתבת בשיתוף פעולה." (111 tokens).
- **Spine 2**, `<html lang="en" dir="ltr">`, English quoting Hebrew:
  - "He said שלום. and left.";
  - "The word (שלום) means peace, she said in 1998.";
  - "Hebrew: שלום עולם, and more.";
  - `<p dir="rtl">שלום עולם בתוך פסקה באנגלית.</p>`;
  - "<bdi>שלום</bdi>: 3 posts".
- **Spine 3**, the notes: two `<aside epub:type="footnote">`, ids n1 and n2.
- Counted on 2026-11-02 at word level: 207 Hebrew words (no Latin letter or digit, not the divine name), 47 words
  without a Hebrew letter, 102 points.

**The rules** (`BidiRules`). Parameterised over `test_bidi_text` (460 px), `test_bidi_text` at 1,600 px (the
fallback lines) and `test_rtl_markup`, through `forEachPage`. Every rule walks every `PageLine` and table cell line.
Each book's floors are in its parameter, as `VerticalRules`' are, listed as 460 px / 1,600 px / `test_rtl_markup`.
- `EveryWordIsDrawnInsideThePage`: `PageLine.xPos + x ≥ 0`, and `+ width(wordDrawText) ≤` the viewport.
- `GluedWordsTouchAndSpacedWordsKeepASpace`: in stored order, a word with `wordContinues` starts where the one before
  ends; any other starts at least 6 px after it. Floors, lines with `hasLogicalOrder()`: 25 / 8 / 3.
- `EveryMarkStaysWithItsLetter`: for every word, the clusters of its draw text (a base and the `isTransparentMark`
  code points after it) equal those of its text, as multisets; no draw text starts with a mark. Floors, marks:
  80 / 80 / 0.
- `HebrewWordsAreDrawnRightToLeft`: for every word with a Hebrew letter and no ASCII letter or digit, whose letters
  are not the divine name, its draw text's Hebrew letters are its text's reversed. Floors, words: 160 / 160 / 10.
- `LatinAndDigitsKeepTheirOrder`: for every word with no R or AL letter and no LRO/RLO in its text, its draw text's
  ASCII letters and digits equal its text's in order. Floors, words: 35 / 35 / 5.
- `TwoMarkersOnALineAreReadInOrder` (one `TEST`, `test_bidi_text`, `inlineFootnotePreviews = false`): on the line
  holding the words "1" and "2", `logicalPosition` of "1" < that of "2", and x of "1" > that of "2".

- [ ] **Step 1:** Extend the generator and run `python -I test/epubs/make_test_writing_modes.py`. `git status` shows
  only `test_bidi_text.epub` new.
- [ ] **Step 2:** Write `BidiRulesTest.cpp`, modelled on `VerticalRulesTest.cpp` (per-test work directories). Log
  each count. Investigate a floor that is not met before touching it: the fixture is the suspect.
- [ ] **Step 3:** Refresh: `git status` shows exactly the two new goldens. Check by hand, and say so in the commit:
  - every Hebrew line ends at the right margin, less its indent;
  - the list markers and the table are on the right;
  - the English paragraph starts at x 0;
  - "* * *" has no letter, so it takes the book's direction: it ends at the right margin, less its indent;
  - spine 2 is left to right with reversed Hebrew pieces, except its `dir="rtl"` paragraph;
  - "יְהוָה" has WD "׳ה".
- [ ] **Step 4: Run** all of `EpubPipelineTest` without `UPDATE_GOLDENS`: pass (the corpus-wide LUT and determinism
  tests take the new book too).
- [ ] **Step 5: Commit** `test(epub): a Hebrew fixture, its goldens, and the bidi rule checker`.

### Task 12: Sizes, the devices, the roadmap and the PR

**Files:**
- Modify: `docs/future_work/writing-modes.md` (row 1: "in progress: PR 1 #599, PR 2 #<n>"; "Next step.": PR 3)

The device runs are the controller's to schedule; this task names what they must show. A Hebrew-capable face exists
only for the run: upstream's `lib/EpdFont/builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf` (66,156 B,
full Basic Latin, U+05F3 and 46 presentation forms). Fetch it with `git -C C:/_development/crosspoint-reader show
e8299b5f:<path>` into the scratchpad, never the repository.

- [ ] **Step 1: The whole host suite** on the final tree: only the five known Windows-only failures. clang-format 21
  on every touched file; no NUL bytes.
- [ ] **Step 2: Sizes.**
  - Check no other pio build runs. `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1`.
  - Build `default`, then `gh_release`, in the background.
  - Record `Flash:` and `RAM:` against the base: `default` 6,150,595 / 57,776, `gh_release` 6,038,001 / 57,272.
  - From `.pio/build/default/firmware.map`, record `.text` + `.rodata` of `minibidi.c.o`, `BidiUtils.cpp.o`,
    `BidiLine.cpp.o`, `TextShaping.cpp.o` and `HebrewShaper.cpp.o`.
  - The build log's `Web bundle:` line names the `web-<hash>.bin` each flash is followed by, copied to the card's
    root.
- [ ] **Step 3: The X4 (COM4)**, then **the X3 (COM6)**, by the controller's device rules. For each:
  - back up with `cmd:GET:*`; flash `default`; copy the web bundle;
  - upload `test_bidi_text.epub` and `test_rtl_markup.epub` to `/test`, and the face to
    `/.crosspoint/fonts/NotoSansHebrew/`; `cmd:SET:fontFamily=NotoSansHebrew`.
  - Then show, with a screenshot each:
    - spine 1's pages: lines start on the right; "(hello world)", 1998 and the trailing period sit as UAX #9 puts
      them;
    - Genesis' niqqud on its letters (dagesh inside, shin and sin dots on their arms, holam left);
    - the divine name as ה׳; lists numbered and the table run on the right; the hanging indent on the right;
    - spine 2's English with reversed Hebrew pieces.
  - Set landscape (`cmd:SET:orientation=1`) at the smallest font size, and show spine 1's long paragraph (fallback
    lines) still reading right.
  - From the log: `createSectionFile spine=… arena: cap=… highWater=` and the `HEAP` lines (build heap); no
    `BIDI Failed to allocate`.
  - `heap` from STATE after five page turns.
  - An English book's page counts at spine items 5, 20 and 40 equal the base build's.
  - Restore the settings from the backup, and flash the base back unless told otherwise.
- [ ] **Step 4: Say in the PR what the runs cannot show yet** (PR 3): pages turning right to left; the dictionary,
  footnote overlay and QR text in reading order (they read stored order until then); the Font Manager's Hebrew
  families; Hebrew in the built-in faces; bionic on Hebrew.
- [ ] **Step 5: The PR** (2-4 KB), titled `feat(writing-modes): right-to-left lines, bidi and Hebrew layout (step 1 PR 2)`.
  It holds: what changed; deviation 11; the calls above; the review focus; the sizes; the device table (build heap,
  heap after a turn, page counts); the screenshots (orphan assets branch).
- [ ] **Step 6: Commit** the roadmap edit: `docs(writing-modes): step 1 PR 2 is #<n>`.
