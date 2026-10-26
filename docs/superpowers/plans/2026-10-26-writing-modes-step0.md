# Writing modes step 0: every markup trigger parsed, CSS boxes and start/end read along them — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every piece of markup that says how text runs is parsed, cascaded, cached and tested: CSS `writing-mode`,
`text-orientation`, `text-combine-upright`, `direction`, `unicode-bidi` and `text-align: start/end` in every spelling
books use; the HTML `dir` attribute with `<bdo>` and `<bdi>`; the OPF spine's `page-progression-direction`. Two of them
take effect now: margins, paddings and image sizes are read along each element's writing mode, and `start`/`end` along
its direction. Books are still set horizontally and left to right; books without any of this markup lay out
byte-identically.

**Architecture:** `CssStyle` gains five one-byte enums (`writingMode`, `textOrientation`, `textCombine`, `direction`,
`unicodeBidi`), each with an `Unset` value of 0 instead of a defined bit (the flag word holds 28 of 32 bits, and five
more would overflow it), and `CssTextAlign` gains `Start` and `End`. `CssParser` parses them; the three style codecs
carry them in one 16-bit flow word. An HTML layer (`HtmlDirectionality.h`) turns `dir`, `<bdo>` and `<bdi>` into the
same properties under the author's CSS. `ChapterHtmlSlimParser` keeps the inherited writing mode and direction in a small
fixed stack keyed by element depth, and right after an element's style is resolved it calls
`CssStyle::resolveLogical(writingMode, direction)`, which rewrites the physical box fields and `start`/`end` into what
layout reads. Every later reader is unchanged. The OPF parser records `page-progression-direction` in `book.bin`.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3), host tests with GoogleTest/CMake (`test/`), Python fixture
generators (`test/epubs/`), the serial test API (`tools/devctl.py`, `docs/serial-test-api.md`) on the X3.

**Spec:** `docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md`, sections 5.1 (who decides what),
5.2 (the paragraph "CSS box properties are physical; layout is logical"), 5.3 (base direction) and 10.1 (the reference
run that found the box rule); the roadmap is `docs/future_work/writing-modes.md`. Every expected value below is derived
by hand from the host stubs' metrics (`test/epub_pipeline/GfxRenderer.h`: ascender 18, line height 24) and the code at
master `0625ac40a`, and each test's comment carries its derivation. A value that does not reproduce is a finding to
report, not a number to adjust silently.

## Maintainer decisions (2026-10-26)

1. The CSS box rotation goes into step 0.
2. Step 0 covers every parser trigger, including those first acted on in later steps.
3. Writing modes follow the book, with a global setting and a per-book override (spec section 9); none of that is in
   step 0, which sets nothing vertically.

## The triggers and who reads them

| Trigger | Spellings | Parsed into | Read in step 0 by | First read later |
|---|---|---|---|---|
| `writing-mode` | unprefixed, `-epub-`, `-webkit-`, `-ms-` (with `tb-rl`, `lr-tb` ...) | `CssStyle::writingMode` | the box rotation | step 3: the vertical decision |
| `text-orientation` | unprefixed, `-epub-`, `-webkit-` | `CssStyle::textOrientation` | | steps 3 and 4: upright or sideways |
| `text-combine-upright` | unprefixed, `-webkit-text-combine`, `-epub-text-combine`, `-ms-text-combine-horizontal` | `CssStyle::textCombine` | | step 4: tate-chu-yoko |
| `direction` | | `CssStyle::direction` | `start`/`end` | step 1: base direction |
| `dir`, `<bdo>`, `<bdi>` | `ltr`, `rtl`, `auto` | `direction` and `unicodeBidi`, by HTML's rules | `start`/`end` | step 1 |
| `unicode-bidi` | values with `-webkit-`/`-moz-` prefixes | `CssStyle::unicodeBidi` | | step 1: embeddings and isolates |
| `text-align: start/end` | | `CssTextAlign::Start`/`End` | the block's alignment | |
| OPF `page-progression-direction` | `ltr`, `rtl`, `default` | `BookMetadata::pageProgression`, `Epub::pageProgression()` | the golden dump | step 1: page turns |

**Not in step 0, because they are behaviour rather than parsing:** FlowContext and the reader's page-turn direction
(steps 1 and 3; following the book's `rtl` while its text is still set left to right would turn the pages backwards),
the bidi controls that `unicode-bidi`, `<bdo>` and `<bdi>` become (step 1), and the per-word tate-chu-yoko flag
(step 4). `lang` attributes stay out, as `docs/future_work/cjk.md` decided.

## Global Constraints

From the spec, verbatim (5.2): "The fix is one rotation where `CssStyle` becomes `BlockStyle` (and in image sizing),
chosen by the writing mode **the CSS was written for**: each element's computed writing mode (its own, else inherited),
not the mode the page is set in."

Decided for this plan (reasons in the tasks and in the report):
- **Values.**
  - `writing-mode`: `horizontal-tb`, `lr`, `lr-tb`, `rl`, `rl-tb` → HorizontalTb; `vertical-rl`, `tb`, `tb-rl`,
    `sideways-rl` → VerticalRl; `vertical-lr`, `tb-lr`, `sideways-lr` → VerticalLr (the SVG 1.1 and IE values CSS
    Writing Modes 3 maps; the sideways values by the block direction they share).
  - `text-orientation`: `mixed`, `upright`, `sideways`; `sideways-right` → Sideways; `use-glyph-orientation` → Mixed.
  - `text-combine-upright`: `none`, `all`, `digits` (2), `digits 2`-`digits 4`. `-webkit-text-combine: horizontal` and
    `-epub-text-combine: horizontal` → All, `-epub-text-combine: horizontal 3` → Digits3; `-ms-text-combine-horizontal`
    takes the unprefixed values.
  - `direction`: `ltr`, `rtl`. `Auto` exists only for the `dir` attribute.
  - `unicode-bidi`: `normal`, `embed`, `isolate`, `bidi-override`, `isolate-override`, `plaintext`, and `-webkit-` and
    `-moz-` prefixed `isolate`, `isolate-override` and `plaintext`.
  - `initial` is each property's initial value (HorizontalTb, Mixed, None, Ltr, Normal); `inherit`, `unset`, `revert`
    and anything unknown leave the property Unset. `text-align: start` and `end` become Start and End (they were Left
    and Right).
- **HTML's rules for `dir`** (HTML, Rendering, "Bidirectional text"), applied as user-agent style under the author's
  CSS: an element with `dir="ltr"`, `"rtl"` or `"auto"` gets that direction and `unicode-bidi: isolate`; `<bdo>` gets
  `isolate-override`; `<bdi>` gets `isolate` and, without a `dir`, direction Auto. Values are ASCII case-insensitive;
  any other value is as if absent. A property the author's CSS set is left alone. The attribute applies whether or not
  the reader uses the book's CSS: it is markup, not style.
- **What rotates:** the four margins, the four paddings, and `width`/`height` (stored as `imageWidth`/`imageHeight`,
  read only by image sizing). `text-align: left/right` and `float: left/right` are line-relative in CSS Writing Modes
  and need nothing; `text-indent` is logical already. There are no border properties in `CssStyle`.
- **`start`/`end`** resolve against the element's computed direction: Start is Left and End Right for Ltr and Auto, the
  other way round for Rtl. Auto resolves as Ltr until step 1 resolves it from the text (UAX #9 P2/P3). An inherited
  alignment comes through `BlockStyle`'s merge already resolved by the ancestor that set it; CSS would resolve it again
  for a child of another direction. Rare (a direction change inside a `text-align: start` container); noted in the
  report.
- **Storage.**
  - `CssStyle`: the five enums take padding bytes (after `pageBreakAfter`: offsets 97-99; after `visibilityHidden`:
    114-115), so `sizeof(CssStyle)` stays 124. Checked on the host on 2026-10-26 with the five fields inserted; every
    member is 4-byte aligned or smaller, so the C3 lays it out the same. `CssPropertyFlags` is unchanged.
  - The flow word, the same 16 bits in both codecs: bits 0-1 `writingMode`, 2-3 `direction`, 4-6 `textCombine`, 7-8
    `textOrientation`, 9-11 `unicodeBidi`; each 0 when Unset.
  - The rule cache's payload appends it: `CSS_FIXED_STYLE_BYTES` 77 → 79, `CSS_CACHE_VERSION` 30.
  - The arena record: bit 4 of the `extra` byte announces it, written right after that byte; `kMaxCompressedStyle`
    74 → 76. Records without any of these properties are unchanged.
  - `CssTextAlign` Start = 6 and End = 7 fit the payload's byte and the arena's 3 bits.
  - `book.bin`: one byte after the description, `BOOK_CACHE_VERSION` 18. Every book is indexed again once, behind the
    first-open popup, on its next open; its section caches stay (a version mismatch rebuilds `book.bin` only,
    `BookMetadataCache::load`). The largest test book took 4.9 s behind that popup after #533; ordinary books well
    under a second.
  - Section files: `SECTION_FILE_VERSION` 101.
- **Parser state:** a fixed array of 8 `{depth, writingMode, direction}` entries in `ChapterHtmlSlimParser`, no heap.
  The parser object lives across a sliced build's steps (`Section.cpp:844`), so no saved state changes.
- **`anyDefined()`:** `CssStyle::anyDefined()` is `defined.anySet()` or any of the five enums set. It replaces
  `defined.anySet()` where a whole style is asked whether it says anything: `CssParser.cpp:1094` (a rule is kept),
  `:2842` (a cascade result), `ChapterHtmlSlimParser.cpp:2164` (the style memo), and `CssCensusTest.cpp:97`. A rule
  with only `writing-mode` (the EBPAJ template's `html.vrtl`) must be kept.
- **Logging:** one `LOG_INF("EHP", ...)` per section build when an element switches writing mode or direction, naming
  it. A ninth nested switch is dropped with one `LOG_ERR`.
- **Out of scope:** `CompiledContent` (`lib/Epub/Epub/content/`, not wired) has its own `CssStyle` codec; it is not
  touched, and its own plan decides whether it stores logical styles.
- Books without this markup lay out exactly as before: every golden in `test/epub_pipeline/goldens` stays
  byte-identical (no fixture declares any trigger, checked 2026-10-26), and the dump prints the new BOOK field only when
  the OPF declares it. The new goldens are the two new fixtures'.
- No upstream code is ported (upstream parses `dir` and `direction` for its RTL, but its parser is not ours and nothing
  else is there); koreader-tategumi and crengine are GPL and are not sources.
- No settings, no UI, no strings.
- Comments say what the code does and why; no history narration, no "step 0".
- clang-format 21 is what CI runs: `bin/clang-format-fix -g`, and `clang-format-21 -style=file -i` for new files.
- Host tests (Linux): `cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release`, then
  `cmake --build build/test --target <T> -j 8` and `ctest --test-dir build/test -R <pattern> --output-on-failure`.
  Targets: `CssParserTest`, `EpubCssCacheTest`, `EpubCssCascadeTest`, `ContentOpfParserTest`, `BookMetadataCacheTest`,
  `EpubPipelineTest`. In a worktree beside other worktrees, run ctest without `-j` (shared `/tmp` fixtures).
- Firmware: `/home/jens/.platformio/penv/bin/pio run -e default` and `-e gh_release`, from a worktree (the main
  checkout's `platformio.local.ini` drops the serial test API).

## Review Focus

1. **An element closed through a skip path** (`display: none`, `hidden`, an unreadably small or transparent run): its
   stack entry must not outlive it. The stack is trimmed by depth at every start tag. Pinned in Task 5:
   `WritingModeLayout.AHiddenElementsModeDoesNotLeak`.
2. **The style memo** (`cssStyleCache_`, keyed tag|class): one entry can serve elements under different modes and
   directions, so `resolveLogical` runs on the element's copy after the memo. Pinned in Task 5:
   `WritingModeLayout.OneClassUnderTwoModes`.
3. **Shorthands and partial declarations:** the defined bits move with the values. The rotation runs after the cascade,
   so no rotated style is merged again. Pinned in Task 3: `CssLogical.DefinedBitsTurnWithTheirValues`.
4. **Percentages:** a vertical book's `height: 50%` on an image is half its column; rotated it is `imageWidth`,
   resolved against the container width. Pinned in Task 5: `WritingModeLayout.AVerticalImageHeightIsAHorizontalWidth`.
5. **Precedence of `dir`:** the author's CSS beats the attribute, the attribute beats inheritance, and the attribute
   holds with the book's CSS off. Pinned in Task 4 (`HtmlDirectionality.AuthorCssBeatsTheAttribute`) and Task 5
   (`WritingModeLayout.DirHoldsWithoutTheBooksCss`).
6. **The 4 em inset cap** (`BlockStyle::MAX_HORIZONTAL_INSET_EM`): a vertical book's large `margin-top` becomes a left
   inset and is capped like any other. Intended; noted in the report.

---

### Task 1: The CSS properties

**Files:**
- Modify: `lib/Epub/Epub/css/CssStyle.h` (`CssTextAlign` at :9; the five enums after `CssFloat` at :76; the fields at
  :187 and :211; `kCascadeProperties` at :219; `applyOver`; the `has*()` accessors; `anyDefined()`; `reset`; a
  `static_assert(sizeof(CssStyle) == 124, ...)` naming the padding the fields took)
- Modify: `lib/Epub/Epub/css/CssParser.cpp` (the declaration dispatch beside `text-align` at :757; `interpretAlignment`
  at :566-574; `anySet` → `anyDefined` at :1094 and :2842), `CssParser.h` (the `interpret*` declarations)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (`anySet` → `anyDefined` at :2164),
  `test/epub_css/CssCensusTest.cpp` (:97)
- Test: `test/epub_css/CssParserTest.cpp`

**Interfaces:**
```cpp
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4, Inherit = 5, Start = 6, End = 7 };
// How text runs, as the element's CSS says it. 0 is Unset in each: these properties have no defined bit (the flag word
// is nearly full), so Unset is how "not declared" is told apart from a declared initial value.
enum class CssWritingMode : uint8_t { Unset = 0, HorizontalTb, VerticalRl, VerticalLr };
enum class CssTextOrientation : uint8_t { Unset = 0, Mixed, Upright, Sideways };
enum class CssTextCombine : uint8_t { Unset = 0, None, All, Digits2, Digits3, Digits4 };
enum class CssDirection : uint8_t { Unset = 0, Ltr, Rtl, Auto };  // Auto: only from the HTML dir attribute
enum class CssUnicodeBidi : uint8_t { Unset = 0, Normal, Embed, Isolate, BidiOverride, IsolateOverride, Plaintext };
```
- `CssStyle` fields `writingMode`, `textOrientation`, `textCombine` (after `pageBreakAfter`), `direction`,
  `unicodeBidi` (after `visibilityHidden`), each Unset by default; `hasWritingMode()` and so on test `!= Unset`.
- `applyOver` merges each at rank indices 28-32 when the base's value is set; `kCascadeProperties` becomes 33.
- `bool CssStyle::anyDefined() const;`
- `static bool CssParser::interpretWritingMode(std::string_view, CssWritingMode&)` and the same for the other four;
  false leaves the property Unset. The `-epub-text-combine` and `-ms-text-combine-horizontal` digit forms parse the
  integer after the keyword.

- [ ] **Step 1: Write the failing tests** in `CssParserTest.cpp`, one per property in the style below, covering every
  spelling and value in Global Constraints, `initial`, `inherit`, an unknown value, case and `!important`:
```cpp
TEST(CssParserDeclarations, WritingModeSpellings) {
  const auto mode = [](const char* decl) { return CssParser::parseInlineStyle(decl).writingMode; };
  EXPECT_EQ(CssWritingMode::VerticalRl, mode("writing-mode: vertical-rl"));
  EXPECT_EQ(CssWritingMode::VerticalRl, mode("-webkit-writing-mode: vertical-rl"));  // a calibre book's only spelling
  EXPECT_EQ(CssWritingMode::VerticalLr, mode("-epub-writing-mode: vertical-lr"));
  EXPECT_EQ(CssWritingMode::VerticalRl, mode("-ms-writing-mode: tb-rl"));
  EXPECT_EQ(CssWritingMode::VerticalLr, mode("-ms-writing-mode: tb-lr"));
  EXPECT_EQ(CssWritingMode::HorizontalTb, mode("-ms-writing-mode: lr-tb"));
  EXPECT_EQ(CssWritingMode::HorizontalTb, mode("writing-mode: rl"));
  EXPECT_EQ(CssWritingMode::VerticalRl, mode("writing-mode: sideways-rl"));
  EXPECT_EQ(CssWritingMode::VerticalLr, mode("writing-mode: sideways-lr"));
  EXPECT_EQ(CssWritingMode::HorizontalTb, mode("writing-mode: initial"));
  EXPECT_EQ(CssWritingMode::VerticalRl, mode("WRITING-MODE : VERTICAL-RL !important"));
  EXPECT_EQ(CssWritingMode::Unset, mode("writing-mode: inherit"));
  EXPECT_EQ(CssWritingMode::Unset, mode("writing-mode: diagonal"));
}

TEST(CssParserDeclarations, TextCombineSpellings) {
  const auto tcy = [](const char* decl) { return CssParser::parseInlineStyle(decl).textCombine; };
  EXPECT_EQ(CssTextCombine::All, tcy("text-combine-upright: all"));
  EXPECT_EQ(CssTextCombine::Digits2, tcy("text-combine-upright: digits"));
  EXPECT_EQ(CssTextCombine::Digits4, tcy("text-combine-upright: digits 4"));
  EXPECT_EQ(CssTextCombine::All, tcy("-webkit-text-combine: horizontal"));
  EXPECT_EQ(CssTextCombine::Digits3, tcy("-epub-text-combine: horizontal 3"));
  EXPECT_EQ(CssTextCombine::Digits2, tcy("-ms-text-combine-horizontal: digits 2"));
  EXPECT_EQ(CssTextCombine::None, tcy("-webkit-text-combine: none"));
  EXPECT_EQ(CssTextCombine::Unset, tcy("text-combine-upright: digits 7"));  // outside 2-4: invalid
}

TEST(CssParserDeclarations, StartAndEndStayLogical) {
  EXPECT_EQ(CssTextAlign::Start, CssParser::parseInlineStyle("text-align: start").textAlign);
  EXPECT_EQ(CssTextAlign::End, CssParser::parseInlineStyle("text-align: end").textAlign);
}

// The last declaration wins across spellings, as CSS has it: they are one property.
TEST(CssParserDeclarations, TheLastSpellingWins) {
  EXPECT_EQ(CssWritingMode::HorizontalTb,
            CssParser::parseInlineStyle("-webkit-writing-mode: vertical-rl; writing-mode: horizontal-tb").writingMode);
  EXPECT_EQ(CssTextCombine::All,
            CssParser::parseInlineStyle("text-combine-upright: none; -webkit-text-combine: horizontal").textCombine);
}

// A more specific rule's value beats a less specific one's, through applyOver's ranks.
TEST(CssParserDeclarations, TheFlowPropertiesCascade) {
  CssStyle merged;
  uint32_t ranks[CssStyle::kCascadeProperties] = {};
  merged.applyOver(CssParser::parseInlineStyle("direction: ltr; unicode-bidi: isolate"), 2, ranks);
  merged.applyOver(CssParser::parseInlineStyle("direction: rtl; unicode-bidi: plaintext"), 1, ranks);
  EXPECT_EQ(CssDirection::Ltr, merged.direction);
  EXPECT_EQ(CssUnicodeBidi::Isolate, merged.unicodeBidi);
}

TEST(CssParserDeclarations, AStyleWithOnlyAFlowPropertySaysSomething) {
  EXPECT_TRUE(CssParser::parseInlineStyle("writing-mode: vertical-rl").anyDefined());
  EXPECT_TRUE(CssParser::parseInlineStyle("unicode-bidi: isolate").anyDefined());
  EXPECT_FALSE(CssParser::parseInlineStyle("writing-mode: inherit").anyDefined());
}
```
- [ ] **Step 2: Run them; they fail to compile.** `cmake --build build/test --target CssParserTest -j 8`
- [ ] **Step 3: Implement.** Any `switch` over `CssTextAlign` gains Start and End; none exists today outside the
  reader's own alignment setting, which never holds them.
- [ ] **Step 4: Run** `ctest --test-dir build/test -R "CssParser|EpubCssCascade" --output-on-failure`: pass.
- [ ] **Step 5: Commit** `feat(css): parse writing-mode, text-orientation, text-combine-upright, direction and unicode-bidi`.

### Task 2: The two persisted codecs

**Files:**
- Modify: `lib/Epub/Epub/css/CssParser.cpp` (`CSS_FIXED_STYLE_BYTES` and its layout comment at :99-111;
  `encodeCssStylePayload` after the font-family hash; `readCssStylePayload`; `kMaxCompressedStyle` and the comment at
  :2262-2271; `compressStyle`'s `extra` byte at :2296-2304; `decompressStyle`)
- Modify: `lib/Epub/Epub/css/CssParser.h` (`CSS_CACHE_VERSION` 30 at :119; a `v30` line above it: "`writing-mode`,
  `text-orientation`, `text-combine-upright`, `direction` and `unicode-bidi` are persisted in a flow word, and
  `text-align: start/end` as themselves; a v29 compile dropped the five and stored start/end as left/right")
- Test: `test/epub_css/CssParserCacheTest.cpp` (`definedEqual` at :465, `stylesEqual` at :478,
  `ResidentPreservesAllStyleFields` at :695)

- [ ] **Step 1: Write the failing tests.** Add the five fields to `stylesEqual`. In `ResidentPreservesAllStyleFields`
  add rules and probes that set every field at a non-trivial value, one of them with nothing else:
```cpp
      ".f { -webkit-writing-mode: vertical-lr; -webkit-text-orientation: upright; text-combine-upright: digits 3; "
      "direction: rtl; unicode-bidi: isolate-override; text-align: end; }\n"
      ".g { writing-mode: vertical-rl; }\n";
  // ...
      {"div", "f"}, {"html", "g"}};
  // ...
  const CssStyle& f = heapStyles[7];
  EXPECT_EQ(CssWritingMode::VerticalLr, f.writingMode) << "the flow word did not survive the disk cache";
  EXPECT_EQ(CssTextOrientation::Upright, f.textOrientation);
  EXPECT_EQ(CssTextCombine::Digits3, f.textCombine);
  EXPECT_EQ(CssDirection::Rtl, f.direction);
  EXPECT_EQ(CssUnicodeBidi::IsolateOverride, f.unicodeBidi);
  EXPECT_EQ(CssTextAlign::End, f.textAlign);
  EXPECT_EQ(CssWritingMode::VerticalRl, heapStyles[8].writingMode) << "a rule with only writing-mode was dropped";
```
  The existing loop then compares the resident (arena) path with the heap path for every probe.
- [ ] **Step 2: Run; the new expectations fail.**
  `cmake --build build/test --target EpubCssCacheTest -j 8 && ctest --test-dir build/test -R CssParserArena --output-on-failure`
- [ ] **Step 3: Implement** both codecs and the version bump.
- [ ] **Step 4: Run** all of `EpubCssCacheTest`: pass.
- [ ] **Step 5: Commit** `feat(css): keep the flow properties in the rule cache and the arena (CSS cache v30)`.

### Task 3: `CssStyle::resolveLogical`

**Files:**
- Modify: `lib/Epub/Epub/css/CssStyle.h`
- Test: `test/epub_css/CssParserTest.cpp`

**Interfaces:**
- `void CssStyle::resolveLogical(CssWritingMode writingMode, CssDirection direction);` documented as: rewrites what
  layout reads physically into what the element means logically. Afterwards top, right, bottom and left are
  block-start, inline-end, block-end and inline-start, and `imageWidth`/`imageHeight` the inline and block size, as
  horizontal-tb names them; `text-align: start`/`end` are left or right. Both arguments are the element's computed
  values (Unset reads as HorizontalTb and Ltr).
- The box mapping (CSS Writing Modes 3, 6.2):

  | after the call | vertical-rl | vertical-lr |
  |---|---|---|
  | top (block-start) | right | left |
  | right (inline-end) | bottom | bottom |
  | bottom (block-end) | left | right |
  | left (inline-start) | top | top |
  | imageWidth (inline size) | height | height |
  | imageHeight (block size) | width | width |

  Values and defined bits move together. Sketch (the names are the builder's):
```cpp
  void resolveLogical(const CssWritingMode writingMode, const CssDirection direction) {
    if (textAlign == CssTextAlign::Start || textAlign == CssTextAlign::End) {
      const bool rtl = direction == CssDirection::Rtl;  // Auto reads as Ltr until the text decides
      textAlign = (textAlign == CssTextAlign::Start) != rtl ? CssTextAlign::Left : CssTextAlign::Right;
    }
    if (writingMode != CssWritingMode::VerticalRl && writingMode != CssWritingMode::VerticalLr) return;
    const bool rl = writingMode == CssWritingMode::VerticalRl;  // block-start is the right side, else the left
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
    // margins, then paddings, through turn(); then width and height swap with their defined bits
  }
```

- [ ] **Step 1: Write the failing tests:**
```cpp
// margin: 1px 2px 3px 4px is top 1, right 2, bottom 3, left 4.
TEST(CssLogical, VerticalRlTurnsTheBox) {
  CssStyle s = CssParser::parseInlineStyle("margin: 1px 2px 3px 4px; padding: 5px 6px 7px 8px; width: 9px; height: 10px");
  s.resolveLogical(CssWritingMode::VerticalRl, CssDirection::Unset);
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

TEST(CssLogical, VerticalLrTurnsTheBox) {
  CssStyle s = CssParser::parseInlineStyle("margin: 1px 2px 3px 4px");
  s.resolveLogical(CssWritingMode::VerticalLr, CssDirection::Unset);
  EXPECT_FLOAT_EQ(4, s.marginTop.value);     // block-start: left
  EXPECT_FLOAT_EQ(3, s.marginRight.value);   // inline-end: bottom
  EXPECT_FLOAT_EQ(2, s.marginBottom.value);  // block-end: right
  EXPECT_FLOAT_EQ(1, s.marginLeft.value);    // inline-start: top
}

TEST(CssLogical, DefinedBitsTurnWithTheirValues) {
  CssStyle s = CssParser::parseInlineStyle("margin-right: 2em; height: auto");
  s.resolveLogical(CssWritingMode::VerticalRl, CssDirection::Unset);
  EXPECT_TRUE(s.hasMarginTop());
  EXPECT_EQ(CssUnit::Em, s.marginTop.unit);
  EXPECT_FALSE(s.hasMarginRight());
  EXPECT_FALSE(s.hasMarginBottom());
  EXPECT_FALSE(s.hasMarginLeft());
  EXPECT_TRUE(s.defined.imageWidth);  // the explicit auto travels with its axis
  EXPECT_FALSE(s.hasImageWidth());
  EXPECT_FALSE(s.defined.imageHeight);
}

TEST(CssLogical, HorizontalLeavesTheBox) {
  CssStyle s = CssParser::parseInlineStyle("margin: 1px 2px 3px 4px; width: 9px");
  s.resolveLogical(CssWritingMode::HorizontalTb, CssDirection::Rtl);
  EXPECT_FLOAT_EQ(1, s.marginTop.value);
  EXPECT_FLOAT_EQ(4, s.marginLeft.value);  // margins are physical in CSS: direction does not mirror them
  EXPECT_FLOAT_EQ(9, s.imageWidth.value);
}

TEST(CssLogical, StartAndEndFollowTheDirection) {
  const auto align = [](const char* decl, const CssDirection d) {
    CssStyle s = CssParser::parseInlineStyle(decl);
    s.resolveLogical(CssWritingMode::Unset, d);
    return s.textAlign;
  };
  EXPECT_EQ(CssTextAlign::Left, align("text-align: start", CssDirection::Unset));
  EXPECT_EQ(CssTextAlign::Right, align("text-align: start", CssDirection::Rtl));
  EXPECT_EQ(CssTextAlign::Left, align("text-align: end", CssDirection::Rtl));
  EXPECT_EQ(CssTextAlign::Left, align("text-align: start", CssDirection::Auto));  // until the text decides
  EXPECT_EQ(CssTextAlign::Right, align("text-align: end", CssDirection::Ltr));
  EXPECT_EQ(CssTextAlign::Center, align("text-align: center", CssDirection::Rtl));
}
```
- [ ] **Step 2: Run; they fail to compile.** `cmake --build build/test --target CssParserTest -j 8`
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/test -R CssLogical --output-on-failure`: pass.
- [ ] **Step 5: Commit** `feat(css): resolve a box's sides and start/end along writing mode and direction`.

### Task 4: HTML's `dir`, `<bdo>` and `<bdi>`

**Files:**
- Create: `lib/Epub/Epub/parsers/HtmlDirectionality.h` (header-only, no parser state)
- Test: `test/epub_css/CssParserTest.cpp` (it already builds the CSS headers; the new header needs nothing else)

**Interfaces:**
- `void applyHtmlDirectionality(CssStyle& style, std::string_view tagName, std::string_view dirAttr);` — HTML's
  user-agent rules for directionality (Global Constraints), under whatever the author's CSS already set in `style`.
  An empty `dirAttr` means the element has no `dir` attribute.

- [ ] **Step 1: Write the failing tests:**
```cpp
TEST(HtmlDirectionality, DirSetsDirectionAndIsolates) {
  CssStyle s;
  applyHtmlDirectionality(s, "p", "rtl");
  EXPECT_EQ(CssDirection::Rtl, s.direction);
  EXPECT_EQ(CssUnicodeBidi::Isolate, s.unicodeBidi);
}

TEST(HtmlDirectionality, ValuesAreCaseInsensitiveAndAnythingElseIsAbsent) {
  CssStyle a, b;
  applyHtmlDirectionality(a, "div", "AUTO");
  applyHtmlDirectionality(b, "div", "sideways");
  EXPECT_EQ(CssDirection::Auto, a.direction);
  EXPECT_EQ(CssDirection::Unset, b.direction);
  EXPECT_EQ(CssUnicodeBidi::Unset, b.unicodeBidi);
}

TEST(HtmlDirectionality, BdoOverridesAndBdiIsolatesWithAutoDirection) {
  CssStyle bdo, bdi;
  applyHtmlDirectionality(bdo, "bdo", "ltr");
  applyHtmlDirectionality(bdi, "bdi", "");
  EXPECT_EQ(CssUnicodeBidi::IsolateOverride, bdo.unicodeBidi);
  EXPECT_EQ(CssDirection::Ltr, bdo.direction);
  EXPECT_EQ(CssUnicodeBidi::Isolate, bdi.unicodeBidi);
  EXPECT_EQ(CssDirection::Auto, bdi.direction);
}

TEST(HtmlDirectionality, AuthorCssBeatsTheAttribute) {
  CssStyle s = CssParser::parseInlineStyle("direction: ltr; unicode-bidi: plaintext");
  applyHtmlDirectionality(s, "p", "rtl");
  EXPECT_EQ(CssDirection::Ltr, s.direction);
  EXPECT_EQ(CssUnicodeBidi::Plaintext, s.unicodeBidi);
}

TEST(HtmlDirectionality, NoAttributeChangesNothing) {
  CssStyle s;
  applyHtmlDirectionality(s, "span", "");
  EXPECT_FALSE(s.anyDefined());
}
```
- [ ] **Step 2: Run; they fail to compile.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/test -R HtmlDirectionality --output-on-failure`: pass.
- [ ] **Step 5: Commit** `feat(epub): HTML's directionality rules for dir, bdo and bdi`.

### Task 5: The parser follows writing mode and direction

**Files:**
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` (the stack beside `cssAncestorMasks_`; `enterFlow`)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (the attribute loop at :2036: `dir`; after the
  `if (self->cssParser)` block that ends at :2171; the img branch at :2580-2582)
- Modify: `lib/Epub/Epub/Section.cpp` (`SECTION_FILE_VERSION` 101 at :36, its comment: "v101: margins, paddings and
  image sizes are read along the CSS writing mode, and text-align start/end along the direction")
- Create: `test/epub_pipeline/WritingModeLayoutTest.cpp`; add it to `EpubPipelineTest` in
  `test/epub_pipeline/CMakeLists.txt`

**Interfaces:**
- In the parser:
```cpp
  // The writing mode and direction open elements run in, where they change: both are inherited, so an element takes
  // the innermost entry above it. Trimmed by depth at every start tag, so an element closed through a skip path leaves
  // nothing behind. A book nests a change a level or two deep; an entry past the eighth is dropped.
  struct FlowEntry {
    int16_t depth;
    CssWritingMode writingMode;
    CssDirection direction;
  };
  static constexpr uint8_t kMaxFlowEntries = 8;
  FlowEntry flows_[kMaxFlowEntries] = {};
  uint8_t flowCount_ = 0;
  FlowEntry elementFlow_ = {};  // the element startElement is resolving, its values computed
  bool flowLogged_ = false;
  // Trims the stack to the element's ancestors, pushes the element when it sets a writing mode or direction other than
  // its parent's, and returns its computed values. `depth` is the element's own (startElement increments it last).
  FlowEntry enterFlow(const CssStyle& style);
```
- In `startElement`, after the `if (self->cssParser)` block (so `dir` holds with the book's CSS off, when `cssStyle` is
  empty):
```cpp
  applyHtmlDirectionality(cssStyle, name, dirAttr);
  self->elementFlow_ = self->enterFlow(cssStyle);
  cssStyle.resolveLogical(self->elementFlow_.writingMode, self->elementFlow_.direction);
```
  and in the img branch, after `imgStyle.applyOver(...)` at :2582:
  `imgStyle.resolveLogical(self->elementFlow_.writingMode, self->elementFlow_.direction);`. The display:none and
  hidden exits come after the call; an element they skip is trimmed by the next start tag at its depth.

**The tests.** The fixture builds one-chapter books (CSS plus a body, or a whole document for `<html class>` and
`<html dir>`) with `test_zip::StoredZipWriter`, as `HeadingBlockLayoutTest` does, and dumps them with
`pipeline_harness::runAndDump`. The writing-mode tests use the default profile (460 × 760, embedded style on, no extra
paragraph spacing); the alignment tests set `paragraphAlignment = 4` (`CssTextAlign::None`, the book's own alignment),
since the default profile's justify overrides the book's. Each CSS starts with
`body { margin: 0; padding: 0 } p { margin: 0; text-indent: 0 }`. A "twin" test needs no arithmetic: a book written
vertically must dump exactly like its horizontal twin, whose CSS names the same logical sides physically.

- [ ] **Step 1: Write the failing tests** (`WritingModeLayout.*`):
  - `AVerticalBookLaysOutAsItsHorizontalTwin`: body class `v` with `.v { -webkit-writing-mode: vertical-rl }`, CSS
    `p { margin: 1em 0 } .ind { padding-top: 2em } .gap { margin-right: 1em; margin-left: 2em }`, against a horizontal
    book with `p { margin: 0 1em } .ind { padding-left: 2em } .gap { margin-top: 1em; margin-bottom: 2em }`; body
    `<p>Alpha</p><p class="ind">Bravo</p><p class="gap">Charlie</p><p>Delta</p>`. The dumps are equal.
  - `AVerticalLrBookLaysOutAsItsHorizontalTwin`: `writing-mode: vertical-lr` with
    `.gap { margin-left: 1em; margin-right: 2em }` against `.gap { margin-top: 1em; margin-bottom: 2em }`. Equal.
  - `AVerticalBooksPositions`, the first twin's first three paragraphs from the dump's `LINE y=… x=…` lines: rotated,
    `p` has margin-left and margin-right 18 (1 em = the stub's ascender) and no vertical margins, and `.ind` adds
    padding-left 36. So Alpha `y=0 x=18`; Bravo `y=24 x=54` (one 24 px line later, inset 18 + 36); Charlie `y=66 x=18`
    (Bravo's line ends at 48; Charlie's margin-top, from `margin-right: 1em`, is 18, and Bravo left no margin-bottom to
    collapse with, `ChapterHtmlSlimParser.cpp:4838-4841`). The horizontal control (the same vertical CSS, no
    `writing-mode`) gives Alpha `y=18 x=0` and Bravo `y=96 x=0` (Alpha's 18 + 24 + its margin-bottom 18 = 60, Bravo's
    margin-top 18 collapses into it, then padding-top 36).
  - `AVerticalImageHeightIsAHorizontalWidth`: a 100 × 200 PNG (`pngHeader` as in `ImageWrapperWidthTest`),
    `<img src="tall.png" style="height: 50%"/>` in a vertical-rl body against `style="width: 50%"` in a horizontal
    one. Equal dumps.
  - `AVerticalSpanIndentIsAHorizontalOne`: `span.i { margin-top: 2em }` in a vertical-rl body against
    `span.i { margin-left: 2em }` (the poem-indent path, `effectiveInlineMarginLeft`). Equal dumps.
  - `TheModeComesFromHtmlOrBody`: an EBPAJ document `<html class="vrtl">` with `html.vrtl { writing-mode: vertical-rl }`,
    and a calibre one `<body class="calibre">` with `.calibre { -webkit-writing-mode: vertical-rl }`: both dump like
    the first twin's horizontal book.
  - `ANestedHorizontalBlockIsNotTurned`: in a vertical-rl body,
    `<p>Alpha</p><div style="writing-mode: horizontal-tb"><p>Bravo</p></div><p>Charlie</p>` with `p { margin: 1em 0 }`:
    Alpha and Charlie at `x=18` with no vertical margins, Bravo at `x=0` with its 18 px above and below.
  - `OneClassUnderTwoModes`: a horizontal body, `<div class="v"><p class="x">Alpha</p></div><p class="x">Bravo</p>`
    with `.v { writing-mode: vertical-rl }` and `.x { margin: 1em 0 }`: Alpha `x=18`, Bravo `x=0`.
  - `AHiddenElementsModeDoesNotLeak`: a horizontal body,
    `<div style="display: none; writing-mode: vertical-rl"><p>Gone</p></div><p class="x">Alpha</p>` with
    `.x { margin: 1em 0 }`: Alpha `y=18 x=0`.
  - `StartAndEndFollowDir` (alignment profile): `<html dir="rtl">` with `.s { text-align: start } .e { text-align: end }`
    and `<p class="s">Alpha</p><p class="e">Bravo</p><div dir="ltr"><p class="s">Charlie</p></div>`: Alpha `align=3`
    (Right), Bravo `align=1` (Left), Charlie `align=1`. The same body with no `dir` and no `direction`: Alpha `align=1`,
    Bravo `align=3`, as master sets them today.
  - `StartAndEndFollowCssDirection` (alignment profile): `.r { direction: rtl }` on a `div` around `<p class="s">`:
    `align=3`. `<p class="s" dir="rtl" style="direction: ltr">`: `align=1` (the author's CSS beats the attribute).
  - `DirAutoReadsAsLtrForNow` (alignment profile): `<p class="s" dir="auto">` with Hebrew text: `align=1`.
  - `DirHoldsWithoutTheBooksCss`: `<html dir="rtl">` and `<p style="text-align: start">` with `embeddedStyle = false`
    and the alignment profile: the inline style is not applied, so the paragraph takes the reader's alignment, and the
    test pins only that the build succeeds and the `dir` path ran (a `LogObserver` on the one `EHP` line).
  - `AnInlineBdoLeavesTheBlockAlone` (alignment profile): `<p class="s">abc <bdo dir="rtl">def</bdo> ghi</p>`:
    `align=1`.
- [ ] **Step 2: Run; they fail** (the twins differ; start and end do not follow `dir`).
  `cmake --build build/test --target EpubPipelineTest -j 8 && ctest --test-dir build/test -R WritingModeLayout --output-on-failure`
- [ ] **Step 3: Implement** the attribute, the stack, `enterFlow` (with the one `LOG_INF` and the one `LOG_ERR`), the
  calls, and the section version.
- [ ] **Step 4: Run** the new tests, then all of `EpubPipelineTest`: every existing golden unchanged.
- [ ] **Step 5: Commit** `feat(epub): read boxes along the writing mode and start/end along the direction`.

### Task 6: The OPF's page progression

**Files:**
- Modify: `lib/Epub/Epub/parsers/ContentOpfParser.cpp` (the `<spine>` start tag at :606), `ContentOpfParser.h` (a
  `pageProgression` member)
- Modify: `lib/Epub/Epub/BookMetadataCache.h` (`BookMetadata::pageProgression`), `BookMetadataCache.cpp`
  (`BOOK_CACHE_VERSION` 18 at :28 with a `v18` line in the comment above it: "the spine's page-progression-direction;
  a v17 cache does not have it"; the size sum at :327, the writer after the description at :368, the reader after
  :758)
- Modify: `lib/Epub/Epub.cpp` (copy it at :209), `lib/Epub/Epub.h` (`PageProgression Epub::pageProgression() const`)
- Modify: `test/epub_pipeline/PipelineRunner.cpp` (the BOOK line at :149: ` ppd=ltr` or ` ppd=rtl` when declared)
- Test: `test/epub_opf/ContentOpfParserTest.cpp`, `test/book_metadata_cache/BookMetadataCacheTest.cpp`

**Interfaces:**
- `enum class PageProgression : uint8_t { Default = 0, Ltr, Rtl };` in `BookMetadataCache.h`: Default when the spine
  says nothing or `default`.

- [ ] **Step 1: Write the failing tests:** `ContentOpfParser.ReadsThePageProgressionDirection` (`rtl`, `ltr`,
  `default`, absent, `RTL` uppercase read as Rtl, an `opf:spine` prefixed tag) and
  `BookMetadataCacheTest.ThePageProgressionSurvivesBookBin` (build a book whose spine says `rtl`, load `book.bin`
  again, read Rtl).
- [ ] **Step 2: Run; they fail to compile.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** both suites and `EpubPipelineTest`: pass, goldens unchanged (no fixture declares it).
- [ ] **Step 5: Commit** `feat(epub): record the spine's page-progression-direction (book cache v18)`.

### Task 7: The fixture books

**Files:**
- Create: `test/epubs/make_test_writing_modes.py`, writing `test_writing_modes.epub` and `test_rtl_markup.epub`
- Create: their goldens in `test/epub_pipeline/goldens/` (via `UPDATE_GOLDENS=1`)

Both books stand in for real ones whose markup cannot go in the repository. The generator's docstring says what each
part pins, in the style of `make_test_cjk_block_seams.py`.
- **`test_writing_modes.epub`**, language `ja`, spine `page-progression-direction="rtl"`:
  - spine 1, calibre style: `<body class="class">` with `.class { -webkit-writing-mode: vertical-rl; display: block }`;
    a contents list of `<p class="toc"><a href="#c1"><span class="tcy">１２</span></a> 章題</p>` entries with
    `.toc { margin: 1em 0 }` and `.tcy { -webkit-text-combine: horizontal; text-combine-upright: all }`, a heading with
    `padding-top: 2em; margin: 0.83em 0`, two prose paragraphs with `text-indent: 1em`, a
    `<span style="-epub-text-orientation: upright">ABC</span>`, and a `<div style="writing-mode: horizontal-tb">` holding
    one paragraph;
  - spine 2, EBPAJ style: `<html class="vrtl">` with `html.vrtl { writing-mode: vertical-rl; -ms-writing-mode: tb-rl }`,
    the same contents list;
  - spine 3, a control: the same markup with no flow property at all.
- **`test_rtl_markup.epub`**, language `he`, no page progression: `<html dir="rtl">`; paragraphs with
  `text-align: start` and `end`; a `div` with `direction: ltr` around a start-aligned paragraph; `<bdo dir="ltr">` and
  `<bdi>` inline; a `<span dir="ltr">` with Latin and digits; a paragraph with `dir="auto"`; a rule with
  `unicode-bidi: plaintext`.

- [ ] **Step 1:** Write the generator and run it: `python3 test/epubs/make_test_writing_modes.py`.
- [ ] **Step 2:** `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`; `git status` must show exactly two
  new goldens and no other golden changed.
- [ ] **Step 3: Check both goldens by hand**, and say so in the commit:
  - `test_writing_modes`: the BOOK line ends ` ppd=rtl`; spines 1 and 2 have their contents entries on consecutive
    lines (24 px apart) inset by one em (`x=18`); spine 3's are 42 px apart (a line plus the collapsed 18 px margin)
    and not inset; the horizontal-tb paragraph has its margins above and below.
  - `test_rtl_markup`: no `ppd`; the start-aligned paragraphs under `dir="rtl"` at `align=3`, the end-aligned at
    `align=1`, the one inside the `direction: ltr` div at `align=1`, the `dir="auto"` one at `align=1`.
- [ ] **Step 4: Commit** `test(epub): writing-mode and right-to-left markup fixtures and their goldens`.

### Task 8: Documents

**Files:**
- Modify: `docs/file-formats.md` (`CSS_CACHE_VERSION (29)` at :57, :300 and :316 → 30, and the payload's flow word in
  the record layout below :316; `book.bin`'s page-progression byte and its version)
- Modify: `docs/future_work/writing-modes.md` (step 0 shipped; the "CSS box properties" item under "Left from the
  spike" removed; the triggers table above, as "parsed in step 0, first read in step N")

- [ ] **Step 1:** Make the edits.
- [ ] **Step 2: Commit** `docs: the flow properties in the cache formats, and the roadmap`.

### Task 9: Firmware builds and the X3

- [ ] **Step 1:** `pio run -e default` and `-e gh_release` from the worktree. Record flash and static RAM against master
  0625ac40a (6,097,497 B flash and 57,408 B RAM for `default`, measured 2026-10-26).
- [ ] **Step 2: Back up the X3** before anything opens a book, per the device memories: `GET:*` to
  `settings_before.txt`, and `/.crosspoint`'s files over USB Transfer to
  `~/.local/state/witchhunt-device-backups/<date>-x3-writing-modes-step0/` with a sha256 list, mode 700. The X3
  auto-sleeps after 15 minutes without a button: a DOWN/UP on Home first.
- [ ] **Step 3: Flash the branch** (`pio run -e default -t upload --upload-port /dev/ttyACM0`, nothing else holding the
  port). Font NotoSansJP 14 pt (`CMD:SET:fontFamily=NotoSansJP`). Upload `test_rtl_markup.epub` and
  `test_writing_modes.epub` to `/test`.
- [ ] **Step 4: The re-index cost.** The first open of `/test/annas-arch-91dbd462043f.epub` after the flash rebuilds its
  `book.bin` (v18): record the popup's time from the log (`buildBookBin completed in`, and the first page).
- [ ] **Step 5: The acceptance book** (already on the card): open spine 4 (the contents) and spine 9 (the prologue) at
  page 1 with `CMD:ACT:READER:...@4:1` and `@9:1`, screenshot both, read the page count from `CMD:STATE`. Master set the
  contents in 9 pages, each entry a line plus a collapsed one-em margin; read as a one-em inset with no space between
  entries, each takes one line, so expect roughly two thirds of that (the exact ratio depends on NotoSansJP's line
  height against its em). The prologue's count may move a little (its heading's `padding-top: 2em` becomes a left
  inset).
- [ ] **Step 6: The fixtures:** screenshot `test_rtl_markup` (the start-aligned paragraphs sit right) and
  `test_writing_modes` spine 1.
- [ ] **Step 7: The control.** Upload `.idea/A Game Of Thrones Book 1 -- Martin, George R_ R_ -- 2011 -- chenjin5.epub`
  to `/test` and open three spine items under master and under the branch: identical page counts.
- [ ] **Step 8: Restore** the font setting and every changed `/.crosspoint` file from the backup, verify by readback,
  and flash master back unless the maintainer says otherwise.
- [ ] **Step 9:** Report the numbers and screenshots in the PR. The PR body stays short (2-4 KB): what changed, the
  triggers table, the review focus, the device result.
