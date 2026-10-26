# Writing modes step 0: the book's writing mode, and CSS boxes read along it — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An element whose CSS sets `writing-mode` (or `-epub-writing-mode`, `-webkit-writing-mode`, `-ms-writing-mode`,
legacy values included), or inherits it, has its margins, paddings and image width and height read along that writing
mode: in a vertical-rl or vertical-lr element, top is inline-start, right and left are block-start and block-end, and
width is the block size. Books are still set horizontally; a book written for vertical text keeps its author's spacing
along the right axes. Books without `writing-mode` lay out byte-identically.

**Architecture:** `CssStyle` gains `writingMode` (`CssWritingMode`: HorizontalTb, VerticalRl, VerticalLr) with a
defined bit, parsed by `CssParser`, carried by the three style codecs (`applyOver`, the rule cache's payload, the arena's
compressed records) and merged like any property. `ChapterHtmlSlimParser` keeps the inherited writing mode in a small
fixed stack keyed by element depth. Right after an element's style is resolved (cascade, then the `style` attribute), it
calls `CssStyle::mapBoxToLogical(mode)`, which rewrites the physical box fields into the names layout reads them by.
Every later reader (`BlockStyle::fromCssStyle`, span indents, tables, lists, image sizing) is unchanged. The image path
resolves its own style and gets the same call.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3), host tests with GoogleTest/CMake (`test/`), Python fixture
generators (`test/epubs/`), the serial test API (`tools/devctl.py`, `docs/serial-test-api.md`) on the X3.

**Spec:** `docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md`, section 5.2 (the paragraph "CSS box
properties are physical; layout is logical") and 10.1 (the reference run that found it); the roadmap is
`docs/future_work/writing-modes.md`. Every expected value below is derived by hand from the host stubs' metrics
(`test/epub_pipeline/GfxRenderer.h`: ascender 18, line height 24) and the code at master `0625ac40a`, and each test's
comment carries its derivation. A value that does not reproduce is a finding to report, not a number to adjust silently.

## Maintainer decisions (2026-10-26)

1. The CSS box rotation goes into step 0.
2. Writing modes follow the book, with a global setting and a per-book override (spec section 9). Step 0 adds no
   setting: it sets nothing vertically, and what it changes follows the book's CSS alone.

## Global Constraints

From the spec, verbatim (5.2): "The fix is one rotation where `CssStyle` becomes `BlockStyle` (and in image sizing),
chosen by the writing mode **the CSS was written for**, the book's declared mode, not the rendered one".

Decided for this plan (reasons in the tasks and in the report):
- **Step 0 is re-scoped to what step 0 uses** (to confirm with the maintainer before Task 1). The roadmap's step 0 also
  listed `dir`, `direction`, `text-align: start/end`, OPF `page-progression-direction`, page progression in the reader
  and `text-combine-upright`. Nothing reads any of them until the text is set right to left (step 1) or vertically
  (steps 3 and 4), and a page progression that follows the book while the text is still set horizontally turns pages
  the wrong way. They move to the steps that first read them: `dir`, `direction`, `start/end`, ppd and page progression
  to step 1 (page progression reused by step 3), `text-combine-upright` to step 4. The roadmap tables are updated with
  this plan.
- **Per element, not per book.** The rotation uses each element's computed writing mode (its own, else its parent's),
  which is what "the writing mode the CSS was written for" means where a book mixes modes (`変な家２` has
  `horizontal-tb` and `vertical-lr` blocks inside a `vertical-rl` body).
- **What rotates:** the four margins, the four paddings, and `width`/`height` (stored as `imageWidth`/`imageHeight`,
  read only by image sizing). **What does not:** `text-align: left/right` and `float: left/right` are line-relative in
  CSS Writing Modes (line-left is the top of a vertical-rl line), and `text-indent` is logical already. There are no
  border properties in `CssStyle`.
- **Values:** `horizontal-tb`, `lr`, `lr-tb`, `rl`, `rl-tb` read as HorizontalTb; `vertical-rl`, `tb`, `tb-rl`,
  `sideways-rl` as VerticalRl; `vertical-lr`, `tb-lr`, `sideways-lr` as VerticalLr (the SVG 1.1 and IE values CSS Writing
  Modes 3 maps, and the sideways values by the block direction they share). `initial` is HorizontalTb, defined.
  `inherit`, `unset`, `revert` and anything unknown leave the property unset, so the element takes its parent's mode.
- **Storage:** `writingMode` takes a padding byte of `CssStyle` (after `visibilityHidden`, offset 114), so `sizeof
  (CssStyle)` stays 124 (measured on the host; every member is 4-byte aligned or smaller, so the C3 lays it out the
  same). The rule cache's payload stays 77 bytes: the small-caps byte (`CssParser.cpp:1918`, read at `:1817-1823`)
  carries the writing mode in bits 2-3 and its defined bit in bit 4. The arena record's "extra" byte (`compressStyle`)
  carries it in bits 5-6 and its defined bit in bit 4. `CSS_CACHE_VERSION` 30 (old caches have rules whose
  `writing-mode` was dropped at compile time and must be compiled again). `SECTION_FILE_VERSION` 101.
- **Parser state:** a fixed array of 8 `{depth, mode}` entries in `ChapterHtmlSlimParser`, no heap. The parser object
  lives across a sliced build's steps (`Section.cpp:844`), so no saved state changes.
- **Logging:** one `LOG_INF("EHP", ...)` per section build when an element switches mode, naming the mode. A ninth nested
  switch is dropped with one `LOG_ERR`.
- **Out of scope:** `CompiledContent` (`lib/Epub/Epub/content/`, not wired) has its own `CssStyle` codec; it is not
  touched, and its own plan decides whether it stores logical styles.
- Books without `writing-mode` lay out exactly as before: every golden in `test/epub_pipeline/goldens` must stay
  byte-identical (no fixture uses `writing-mode`, checked 2026-10-26). The one new golden is the new fixture's.
- No upstream code is ported (upstream has no `writing-mode` support); koreader-tategumi and crengine are GPL and are not
  sources.
- Comments say what the code does and why; no history narration, no "step 0".
- clang-format 21 is what CI runs: `bin/clang-format-fix -g`, and `clang-format-21 -style=file -i` for new files.
- Host tests (Linux): `cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release`, then
  `cmake --build build/test --target <T> -j 8` and `ctest --test-dir build/test -R <pattern> --output-on-failure`.
  Targets: `CssParserTest`, `EpubCssCacheTest`, `EpubCssCascadeTest`, `EpubPipelineTest`. In a worktree beside other
  worktrees, run ctest without `-j` (shared `/tmp` fixtures).
- Firmware: `/home/jens/.platformio/penv/bin/pio run -e default` and `-e gh_release`, from a worktree (the main
  checkout's `platformio.local.ini` drops the serial test API).

## Review Focus

1. **An element closed through a skip path** (`display: none`, `hidden`, an unreadably small or transparent run): its
   stack entry must not outlive it. The stack is trimmed by depth at every start tag, so a sibling or anything after it
   never sees a stale mode. Pinned in Task 4: `WritingModeBoxes.AHiddenElementsModeDoesNotLeak`.
2. **The style memo** (`cssStyleCache_`, keyed tag|class): one entry can serve elements under different modes, so the
   rotation runs on the element's copy after the memo, never on the memo's entry. Pinned in Task 4:
   `WritingModeBoxes.OneClassUnderTwoModes`.
3. **Shorthands and partial declarations:** `margin: 1em 0` sets four sides, `margin-right` one; the defined bits must
   move with the values, or an undefined side would overwrite a defined one in a later `applyOver`. The rotation runs
   after the cascade, so no rotated style is ever merged again. Pinned in Task 3: `CssWritingMode.DefinedBitsTurnWithTheirValues`.
4. **Percentages:** a vertical book's `height: 50%` on an image is half its column, the inline size. After the rotation
   it is `imageWidth`, resolved against the container width, which is the inline size of a horizontal page. Pinned in
   Task 4: `WritingModeBoxes.AVerticalImageHeightIsAHorizontalWidth`.
5. **The 4 em inset cap** (`BlockStyle::MAX_HORIZONTAL_INSET_EM`): a vertical book's large `margin-top` (an indent down
   the column) becomes a left inset and is capped like any other. Intended; noted in the report.

---

### Task 0: Confirm the re-scope

- [ ] **Step 1:** Ask the maintainer to confirm the re-scope under Global Constraints. If declined, stop and revise the
  plan: the moved items each need their own task and tests.

### Task 1: `writing-mode` in `CssStyle` and the parser

**Files:**
- Modify: `lib/Epub/Epub/css/CssStyle.h` (the enum after `CssFloat` at :76; the flag after `fontFamily : 1` at :106, its
  constructor, `anySet`, `clearAll`; the field after `visibilityHidden` at :211; `kCascadeProperties` at :219;
  `applyOver`; `hasWritingMode`; `reset`)
- Modify: `lib/Epub/Epub/css/CssParser.cpp` (the declaration dispatch, beside `text-align` at :757), `CssParser.h`
  (`interpretWritingMode`)
- Test: `test/epub_css/CssParserTest.cpp`

**Interfaces:**
- Produces:
  - `enum class CssWritingMode : uint8_t { HorizontalTb = 0, VerticalRl = 1, VerticalLr = 2 };` with a comment saying
    it is the writing mode a box's CSS was written for, and that sideways modes read as the vertical mode with the same
    block direction;
  - `CssStyle::writingMode` (default HorizontalTb), `CssPropertyFlags::writingMode`, `CssStyle::hasWritingMode()`;
  - `static bool CssParser::interpretWritingMode(std::string_view value, CssWritingMode& out);` false for `inherit`,
    `unset`, `revert` and unknown values;
  - `static_assert(sizeof(CssStyle) == 124, ...)` under the struct, naming the padding byte the field took.
- `applyOver` merges `writingMode` at rank index 28, keyed on the defined bit; `kCascadeProperties` becomes 29.

- [ ] **Step 1: Write the failing tests** in `CssParserTest.cpp`:
```cpp
// Every spelling a book uses: the unprefixed property, the EPUB 3.0 and WebKit prefixes (a calibre conversion keeps
// only -webkit-), and IE's, whose legacy values CSS Writing Modes 3 maps.
TEST(CssParserDeclarations, WritingModeSpellings) {
  const auto mode = [](const char* decl) {
    const CssStyle s = CssParser::parseInlineStyle(decl);
    return s.hasWritingMode() ? static_cast<int>(s.writingMode) : -1;
  };
  EXPECT_EQ(1, mode("writing-mode: vertical-rl"));
  EXPECT_EQ(1, mode("-webkit-writing-mode: vertical-rl"));
  EXPECT_EQ(2, mode("-epub-writing-mode: vertical-lr"));
  EXPECT_EQ(1, mode("-ms-writing-mode: tb-rl"));
  EXPECT_EQ(2, mode("-ms-writing-mode: tb-lr"));
  EXPECT_EQ(0, mode("writing-mode: horizontal-tb"));
  EXPECT_EQ(0, mode("-ms-writing-mode: lr-tb"));
  EXPECT_EQ(0, mode("writing-mode: rl"));
  EXPECT_EQ(1, mode("writing-mode: sideways-rl"));
  EXPECT_EQ(2, mode("writing-mode: sideways-lr"));
  EXPECT_EQ(0, mode("writing-mode: initial"));
  EXPECT_EQ(1, mode("WRITING-MODE : VERTICAL-RL !important"));
  EXPECT_EQ(-1, mode("writing-mode: inherit"));  // the parent's mode: unset
  EXPECT_EQ(-1, mode("writing-mode: unset"));
  EXPECT_EQ(-1, mode("writing-mode: diagonal"));
}

// The last declaration wins across spellings, as CSS has it: they are one property.
TEST(CssParserDeclarations, WritingModeLastSpellingWins) {
  const CssStyle s = CssParser::parseInlineStyle("-webkit-writing-mode: vertical-rl; writing-mode: horizontal-tb");
  ASSERT_TRUE(s.hasWritingMode());
  EXPECT_EQ(CssWritingMode::HorizontalTb, s.writingMode);
}

// A more specific rule's mode beats a less specific one's, through applyOver's ranks.
TEST(CssParserDeclarations, WritingModeCascades) {
  CssStyle low = CssParser::parseInlineStyle("writing-mode: vertical-rl");
  CssStyle merged;
  uint32_t ranks[CssStyle::kCascadeProperties] = {};
  merged.applyOver(CssParser::parseInlineStyle("writing-mode: horizontal-tb"), 2, ranks);
  merged.applyOver(low, 1, ranks);
  EXPECT_EQ(CssWritingMode::HorizontalTb, merged.writingMode);
}
```
- [ ] **Step 2: Run them; they fail to compile** (`hasWritingMode` does not exist).
  `cmake --build build/test --target CssParserTest -j 8`
- [ ] **Step 3: Implement.** The four property names in the dispatch, each `interpretWritingMode` and on success
  `style.writingMode = ...; style.defined.writingMode = 1;`. The flag in the constructor, `anySet` (a rule that sets only
  `writing-mode`, the EBPAJ `html.vrtl` rule, must be kept) and `clearAll`; the field in `reset`; `applyOver` at index 28.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R CssParserDeclarations --output-on-failure`: all pass, and
  `EpubCssCascadeTest` still passes.
- [ ] **Step 5: Commit** `feat(css): parse writing-mode and its prefixed and legacy spellings`.

### Task 2: The two persisted codecs

**Files:**
- Modify: `lib/Epub/Epub/css/CssParser.cpp` (`encodeCssStylePayload`'s small-caps byte at :1918, `readCssStylePayload`
  at :1817-1823; `compressStyle`'s `extra` byte at :2296-2304 and `decompressStyle`; the payload layout comment at
  :101-105 and the `compressStyle` comment at :2262-2271)
- Modify: `lib/Epub/Epub/css/CssParser.h` (`CSS_CACHE_VERSION` 30 at :119, with a `v30` line in the history comment above
  it: "`writing-mode` is parsed and persisted; a v29 compile dropped it, so a vertical book's rules lost their mode")
- Test: `test/epub_css/CssParserCacheTest.cpp` (`definedEqual` at :465, `stylesEqual` at :478,
  `ResidentPreservesAllStyleFields` at :695)

**Interfaces:**
- The payload's small-caps byte: bit 0 small-caps value, bit 1 its defined bit (as now), bits 2-3 `writingMode`, bit 4
  its defined bit. `CSS_FIXED_STYLE_BYTES` stays 77; its `static_assert` stays.
- The arena record's `extra` byte: bit 4 `writingMode` defined, bits 5-6 its value; `mask` bit 31 announces the byte as
  now. `kMaxCompressedStyle` is unchanged (the byte is already counted). `packDefinedMask` is unchanged (its 32 bits are
  full); the dedup `memcmp` covers the extra byte, so two records differing only in mode stay apart.

- [ ] **Step 1: Write the failing tests.** Add `writingMode` to `definedEqual` and `stylesEqual`. In
  `ResidentPreservesAllStyleFields` add two rules and probes:
```cpp
      ".f { -webkit-writing-mode: vertical-lr; font-variant: small-caps; }\n"  // shares the small-caps byte
      ".g { writing-mode: vertical-rl; }\n";  // a rule with nothing else is kept
  // ...
      {"div", "f"}, {"html", "g"}};
  // ...
  EXPECT_EQ(CssWritingMode::VerticalLr, heapStyles[7].writingMode) << "writing-mode did not survive the disk cache";
  EXPECT_TRUE(heapStyles[7].smallCaps && heapStyles[7].defined.smallCaps) << "the shared byte lost small caps";
  EXPECT_TRUE(heapStyles[8].defined.writingMode);
  EXPECT_EQ(CssWritingMode::VerticalRl, heapStyles[8].writingMode);
```
  The existing loop then compares the resident (arena) path with the heap path for every probe, the new ones included.
- [ ] **Step 2: Run; the new expectations fail** (the cache drops the mode).
  `cmake --build build/test --target EpubCssCacheTest -j 8 && ctest --test-dir build/test -R CssParserArena --output-on-failure`
- [ ] **Step 3: Implement** both codecs and the version bump.
- [ ] **Step 4: Run** all of `EpubCssCacheTest`: pass.
- [ ] **Step 5: Commit** `feat(css): keep writing-mode in the rule cache and the arena (CSS cache v30)`.

### Task 3: `CssStyle::mapBoxToLogical`

**Files:**
- Modify: `lib/Epub/Epub/css/CssStyle.h`
- Test: `test/epub_css/CssParserTest.cpp`

**Interfaces:**
- Produces `void CssStyle::mapBoxToLogical(CssWritingMode authoredIn);` documented as: rewrites the box's physical
  properties into the names layout reads them by; afterwards top, right, bottom and left are block-start, inline-end,
  block-end and inline-start, and `imageWidth`/`imageHeight` the inline and block size, as horizontal-tb names them.
  `authoredIn` is the element's computed writing mode. Line-relative properties need nothing.
- The mapping (CSS Writing Modes 3, 6.2):

  | after the call | vertical-rl | vertical-lr |
  |---|---|---|
  | top (block-start) | right | left |
  | right (inline-end) | bottom | bottom |
  | bottom (block-end) | left | right |
  | left (inline-start) | top | top |
  | imageWidth (inline size) | height | height |
  | imageHeight (block size) | width | width |

  Values and defined bits move together; HorizontalTb is a no-op. Sketch (the names are the builder's):
```cpp
  void mapBoxToLogical(const CssWritingMode authoredIn) {
    if (authoredIn == CssWritingMode::HorizontalTb) return;
    const bool rl = authoredIn == CssWritingMode::VerticalRl;  // block-start is the right side, else the left
    struct Side {
      CssLength length;
      bool defined;
    };
    const auto turn = [rl](const Side top, const Side right, const Side bottom, const Side left, Side (&out)[4]) {
      out[0] = rl ? right : left;  // block-start
      out[1] = bottom;             // inline-end
      out[2] = rl ? left : right;  // block-end
      out[3] = top;                // inline-start
    };
    Side m[4];
    turn({marginTop, defined.marginTop != 0}, {marginRight, defined.marginRight != 0},
         {marginBottom, defined.marginBottom != 0}, {marginLeft, defined.marginLeft != 0}, m);
    marginTop = m[0].length;    defined.marginTop = m[0].defined;
    marginRight = m[1].length;  defined.marginRight = m[1].defined;
    marginBottom = m[2].length; defined.marginBottom = m[2].defined;
    marginLeft = m[3].length;   defined.marginLeft = m[3].defined;
    // the same for the paddings, then width and height swap with their defined bits
  }
```

- [ ] **Step 1: Write the failing tests:**
```cpp
// margin: 1px 2px 3px 4px is top 1, right 2, bottom 3, left 4.
TEST(CssWritingMode, VerticalRlTurnsTheBox) {
  CssStyle s = CssParser::parseInlineStyle("margin: 1px 2px 3px 4px; padding: 5px 6px 7px 8px; width: 9px; height: 10px");
  s.mapBoxToLogical(CssWritingMode::VerticalRl);
  EXPECT_FLOAT_EQ(2, s.marginTop.value);     // block-start: right
  EXPECT_FLOAT_EQ(3, s.marginRight.value);   // inline-end: bottom
  EXPECT_FLOAT_EQ(4, s.marginBottom.value);  // block-end: left
  EXPECT_FLOAT_EQ(1, s.marginLeft.value);    // inline-start: top
  EXPECT_FLOAT_EQ(6, s.paddingTop.value);
  EXPECT_FLOAT_EQ(7, s.paddingRight.value);
  EXPECT_FLOAT_EQ(8, s.paddingBottom.value);
  EXPECT_FLOAT_EQ(5, s.paddingLeft.value);
  EXPECT_FLOAT_EQ(10, s.imageWidth.value);   // inline size: height
  EXPECT_FLOAT_EQ(9, s.imageHeight.value);   // block size: width
}

TEST(CssWritingMode, VerticalLrTurnsTheBox) {
  CssStyle s = CssParser::parseInlineStyle("margin: 1px 2px 3px 4px");
  s.mapBoxToLogical(CssWritingMode::VerticalLr);
  EXPECT_FLOAT_EQ(4, s.marginTop.value);     // block-start: left
  EXPECT_FLOAT_EQ(3, s.marginRight.value);   // inline-end: bottom
  EXPECT_FLOAT_EQ(2, s.marginBottom.value);  // block-end: right
  EXPECT_FLOAT_EQ(1, s.marginLeft.value);    // inline-start: top
}

TEST(CssWritingMode, DefinedBitsTurnWithTheirValues) {
  CssStyle s = CssParser::parseInlineStyle("margin-right: 2em; height: auto");
  s.mapBoxToLogical(CssWritingMode::VerticalRl);
  EXPECT_TRUE(s.hasMarginTop());
  EXPECT_EQ(CssUnit::Em, s.marginTop.unit);
  EXPECT_FALSE(s.hasMarginRight());
  EXPECT_FALSE(s.hasMarginBottom());
  EXPECT_FALSE(s.hasMarginLeft());
  EXPECT_TRUE(s.defined.imageWidth);  // the explicit auto travels with its axis
  EXPECT_FALSE(s.hasImageWidth());
  EXPECT_FALSE(s.defined.imageHeight);
}

TEST(CssWritingMode, HorizontalIsANoOp) {
  CssStyle s = CssParser::parseInlineStyle("margin: 1px 2px 3px 4px; width: 9px");
  s.mapBoxToLogical(CssWritingMode::HorizontalTb);
  EXPECT_FLOAT_EQ(1, s.marginTop.value);
  EXPECT_FLOAT_EQ(4, s.marginLeft.value);
  EXPECT_FLOAT_EQ(9, s.imageWidth.value);
}
```
- [ ] **Step 2: Run; they fail to compile.** `cmake --build build/test --target CssParserTest -j 8`
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/test -R CssWritingMode --output-on-failure`: pass.
- [ ] **Step 5: Commit** `feat(css): map a box's physical sides to logical ones by writing mode`.

### Task 4: The parser follows the inherited writing mode

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` (the stack beside `cssAncestorMasks_`;
  `enterWritingMode`)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (after the `style` attribute merge at :2170; the img branch
  at :2580-2582)
- Modify: `lib/Epub/Epub/Section.cpp` (`SECTION_FILE_VERSION` 101 at :36, its comment: "v101: margins, paddings and image
  sizes are read along the CSS writing mode")
- Create: `test/epub_pipeline/WritingModeBoxTest.cpp`; add it to `EpubPipelineTest` in
  `test/epub_pipeline/CMakeLists.txt`

**Interfaces:**
- In the parser:
```cpp
  // The writing mode open elements' CSS was written for, where it changes: writing-mode is inherited, so an element takes
  // the innermost entry above it. Trimmed by depth at every start tag, so an element closed through a skip path leaves
  // nothing behind. A book nests a mode change a level or two deep; an entry past the eighth is dropped.
  struct WritingModeEntry {
    int16_t depth;
    CssWritingMode mode;
  };
  static constexpr uint8_t kMaxWritingModes = 8;
  WritingModeEntry writingModes_[kMaxWritingModes] = {};
  uint8_t writingModeCount_ = 0;
  CssWritingMode elementWritingMode_ = CssWritingMode::HorizontalTb;  // the element startElement is resolving
  bool writingModeLogged_ = false;
  // Trims the stack to the element's ancestors, pushes the element's own mode when it sets a different one, and
  // returns the element's computed mode. `depth` is the element's own (startElement increments it on the way out).
  CssWritingMode enterWritingMode(const CssStyle& style);
```
- In `startElement`, inside `if (self->cssParser)`, right after `cssStyle.applyOver(self->inlineStyleFor(styleAttr))`:
```cpp
    self->elementWritingMode_ = self->enterWritingMode(cssStyle);
    cssStyle.mapBoxToLogical(self->elementWritingMode_);
```
  and in the img branch, after `imgStyle.applyOver(...)` at :2582: `imgStyle.mapBoxToLogical(self->elementWritingMode_);`.
  The display:none and hidden exits come after the call, and an element they skip is trimmed by the next start tag at
  its depth.

**The tests.** The fixture builds one-chapter books (CSS plus a body, or a whole document for an `<html class>`) with
`test_zip::StoredZipWriter`, as `HeadingBlockLayoutTest` does, and dumps them with `pipeline_harness::runAndDump` under
the default profile (460 × 760, embedded style on, no extra paragraph spacing). Each CSS starts with
`body { margin: 0; padding: 0 } p { margin: 0; text-indent: 0 }`. The "twin" tests need no arithmetic: a book written
vertically must dump exactly like its horizontal twin, whose CSS names the same logical sides physically.

- [ ] **Step 1: Write the failing tests:**
  - `AVerticalBookLaysOutAsItsHorizontalTwin`: body class `v` with `.v { -webkit-writing-mode: vertical-rl }`, CSS
    `p { margin: 1em 0 } .ind { padding-top: 2em } .gap { margin-right: 1em; margin-left: 2em }`, against a horizontal
    book with `p { margin: 0 1em } .ind { padding-left: 2em } .gap { margin-top: 1em; margin-bottom: 2em }`; body
    `<p>Alpha</p><p class="ind">Bravo</p><p class="gap">Charlie</p><p>Delta</p>`. The dumps are equal.
  - `AVerticalLrBookLaysOutAsItsHorizontalTwin`: `writing-mode: vertical-lr` with
    `.gap { margin-left: 1em; margin-right: 2em }` against `.gap { margin-top: 1em; margin-bottom: 2em }`. Equal.
  - `AVerticalBooksPositions`, the absolute values for the first twin's first three paragraphs, from the dump's
    `LINE y=… x=…` lines (derivation in the test's comment): rotated, `p` has margin-left and margin-right 18 (1 em = the
    stub's ascender) and no vertical margins, `.ind` adds padding-left 36; so Alpha `y=0 x=18`, Bravo `y=24 x=54`
    (one 24 px line later, inset 18 + 36), Charlie `y=66 x=18` (Bravo's line ends at 48; Charlie's margin-top, from
    `margin-right: 1em`, is 18, and Bravo left no margin-bottom to collapse with, `ChapterHtmlSlimParser.cpp:4838-4841`).
    The horizontal control (no `writing-mode`, the same vertical CSS) gives Alpha `y=18 x=0` and Bravo `y=96 x=0`
    (Alpha's 18 + 24 + its margin-bottom 18 = 60, Bravo's margin-top 18 collapses into it, then padding-top 36).
  - `AVerticalImageHeightIsAHorizontalWidth`: a 100 × 200 PNG (`pngHeader` as in `ImageWrapperWidthTest`),
    `<img src="tall.png" style="height: 50%"/>` in a vertical-rl body against `style="width: 50%"` in a horizontal one.
    Equal dumps.
  - `AVerticalSpanIndentIsAHorizontalOne`: `span.i { margin-top: 2em }` in a vertical-rl body against
    `span.i { margin-left: 2em }` (the poem-indent path, `effectiveInlineMarginLeft`). Equal dumps.
  - `TheModeComesFromHtmlOrBody`: an EBPAJ document `<html class="vrtl">` with `html.vrtl { writing-mode: vertical-rl }`,
    and a calibre one `<body class="calibre">` with `.calibre { -webkit-writing-mode: vertical-rl }`: both dump like the
    first twin's horizontal book.
  - `ANestedHorizontalBlockIsNotTurned`: in a vertical-rl body,
    `<p>Alpha</p><div style="writing-mode: horizontal-tb"><p>Bravo</p></div><p>Charlie</p>` with `p { margin: 1em 0 }`:
    Alpha and Charlie at `x=18` with no vertical margins, Bravo at `x=0` with its 18 px above and below.
  - `OneClassUnderTwoModes`: a horizontal body,
    `<div class="v"><p class="x">Alpha</p></div><p class="x">Bravo</p>` with `.v { writing-mode: vertical-rl }` and
    `.x { margin: 1em 0 }`: Alpha `x=18`, Bravo `x=0` (the memo served Bravo the unrotated style).
  - `AHiddenElementsModeDoesNotLeak`: in a horizontal body, `<div style="display: none; writing-mode: vertical-rl">
    <p>Gone</p></div><p class="x">Alpha</p>` with `.x { margin: 1em 0 }`: Alpha `y=18 x=0`.
- [ ] **Step 2: Run; they fail** (the twins differ; the vertical book lays out like the control).
  `cmake --build build/test --target EpubPipelineTest -j 8 && ctest --test-dir build/test -R WritingModeBoxes --output-on-failure`
- [ ] **Step 3: Implement** the stack, `enterWritingMode` (with the one `LOG_INF` per section and the one `LOG_ERR` on a
  ninth entry), the two calls, and the section version.
- [ ] **Step 4: Run** the new tests, then all of `EpubPipelineTest`: every existing golden unchanged.
- [ ] **Step 5: Commit** `feat(epub): read margins, paddings and image sizes along the CSS writing mode`.

### Task 5: The fixture book

**Files:**
- Create: `test/epubs/make_test_writing_modes.py` and the `test/epubs/test_writing_modes.epub` it writes
- Create: `test/epub_pipeline/goldens/test_writing_modes.golden.txt` (via `UPDATE_GOLDENS=1`)

The book is a small stand-in for the commercial acceptance book, whose markup cannot go in the repository. Its docstring
says what each part pins, in the style of `make_test_cjk_block_seams.py`. Language `ja`, no ppd (not read yet).
- **Spine 1, calibre style:** `<body class="class">` with `.class { -webkit-writing-mode: vertical-rl; display: block }`;
  a contents list of `<p class="toc"><a href="#c1">１</a> 章題</p>` entries with `.toc { margin: 1em 0 }`, a heading
  with `padding-top: 2em; margin: 0.83em 0`, two prose paragraphs with `text-indent: 1em`, and a
  `<div style="writing-mode: horizontal-tb">` holding one paragraph.
- **Spine 2, EBPAJ style:** `<html class="vrtl">` with `html.vrtl { writing-mode: vertical-rl }` and
  `-ms-writing-mode: tb-rl` alongside, the same contents list.
- **Spine 3, a control:** the same markup without any `writing-mode`.

- [ ] **Step 1:** Write the generator and run it: `python3 test/epubs/make_test_writing_modes.py`.
- [ ] **Step 2:** `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`; then `git status` must show exactly one
  new golden and no other golden changed.
- [ ] **Step 3: Check the golden by hand**, and say so in the commit: spine 1's and 2's contents entries sit on
  consecutive lines (24 px apart) inset by one em (`x=18`); spine 3's are 42 px apart (a line plus the collapsed 18 px
  margin) and not inset; the horizontal-tb paragraph in spine 1 has its margins above and below.
- [ ] **Step 4: Commit** `test(epub): a writing-mode fixture book and its golden`.

### Task 6: Documents

**Files:**
- Modify: `docs/file-formats.md` (`CSS_CACHE_VERSION (29)` at :57, :300 and :316 → 30; the payload's small-caps byte
  in the record layout below :316)
- Modify: `docs/future_work/writing-modes.md` (step 0 shipped; the "CSS box properties" item under "Left from the spike"
  removed)

- [ ] **Step 1:** Make the edits. The roadmap tables were re-scoped with this plan; check they still match what shipped.
- [ ] **Step 2: Commit** `docs: writing-mode in the CSS cache format and the roadmap`.

### Task 7: Firmware builds and the X3

- [ ] **Step 1:** `pio run -e default` and `-e gh_release` from the worktree. Record flash and static RAM against master
  0625ac40a (6,097,497 B flash and 57,408 B RAM for `default`, measured 2026-10-26).
- [ ] **Step 2: Back up the X3** before anything opens a book, per the device memories: `GET:*` to
  `settings_before.txt`, and `/.crosspoint`'s files over USB Transfer to
  `~/.local/state/witchhunt-device-backups/<date>-x3-writing-modes-step0/` with a sha256 list, mode 700. The scratch
  scripts that did this on 2026-10-26 are described in the spec's section 10.1 run; write them again if the scratchpad
  is gone. The X3 auto-sleeps after 15 minutes without a button: a DOWN/UP on Home first.
- [ ] **Step 3: Flash the branch** (`pio run -e default -t upload --upload-port /dev/ttyACM0`, nothing else holding the
  port). Font NotoSansJP 14 pt (`CMD:SET:fontFamily=NotoSansJP`).
- [ ] **Step 4: The acceptance book** (`/test/annas-arch-91dbd462043f.epub`, already on the card): open spine 4 (the
  contents) and spine 9 (the prologue) at page 1 with `CMD:ACT:READER:...@4:1` and `@9:1`, screenshot both, read the
  page count from `CMD:STATE`. Master set the contents in 9 pages, each entry a line plus a collapsed one-em margin;
  with the entries' `margin: 1em 0` read as a one-em inset, each takes one line, so expect roughly two thirds of that
  (the exact ratio depends on NotoSansJP's line height against its em). The prologue's page count may move a little
  (its heading's `padding-top: 2em` becomes a left inset).
- [ ] **Step 5: The control.** Upload `.idea/A Game Of Thrones Book 1 -- Martin, George R_ R_ -- 2011 -- chenjin5.epub`
  to `/test` and open three spine items under master and under the branch: identical page counts (no `writing-mode`,
  160 margin rules).
- [ ] **Step 6: Restore** the font setting and every changed `/.crosspoint` file from the backup, verify by readback,
  and flash master back unless the maintainer says otherwise.
- [ ] **Step 7:** Report the numbers and screenshots in the PR. The PR body stays short (2-4 KB): what changed, the
  re-scope, the review focus, the device result.
