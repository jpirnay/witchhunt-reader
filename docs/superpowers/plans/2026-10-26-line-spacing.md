# Line spacing: a CJK scale, two wider steps, the book's own, per book — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Chinese, Japanese and Korean books get a line pitch from the CJK layout requirements (1.5 to 2.2 em, Normal
1.7 em) instead of a factor on the CJK font's tight natural line. The setting gains Extra wide, Widest and Book's
style, which reads each block's CSS `line-height`, floored at Tight. A per-book override sits in Quick overrides. The
three copies of the step-to-factor mapping become one.

**Architecture:** A header-only `LineSpacing.h` in `lib/Epub/Epub` maps a step, a scale (font factor, built-in Noto
Sans factor, CJK pitch) and the body font's point size and line height to a `LineSpacing {compression, bookFloor,
emPx}`. The reader, the sleep screen's page draw and the dictionary call it. The parser's flow stack, which already
carries the inherited writing mode and direction, also carries the inherited `line-height`, so every block knows its
computed value; `effectiveLineHeight` reads it when Book's style is on. Two new build params enter the property hash
only when Book's style is on, so no existing cache is invalidated by the format.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3), host tests with GoogleTest/CMake (`test/`), the serial test API
(`tools/devctl.py`, `docs/serial-test-api.md`) on the X3.

**Spec:** `docs/superpowers/specs/2026-10-26-line-spacing-design.md`. The study behind the values:
`docs/future_work/cjk.md`, "Line spacing", and the comparison page (https://claude.ai/artifact/4KSjeuQCXNoHWMtarCZTai).
Expected values below are derived from the host stubs' metrics (`test/epub_pipeline/GfxRenderer.h`: ascender 18, line
height 24) and the code at master `e978c0f2c`; each test's comment carries its derivation. A value that does not
reproduce is a finding to report, not a number to adjust silently.

## Maintainer decisions (2026-10-26)

1. One CJK scale for zh, ja and ko: Tight 1.5, Normal 1.7, Wide 1.85, Extra wide 2.0, Widest 2.2 em.
2. Other books keep factors on the font's line height: 0.95, 1.0, 1.1, then 1.2 and 1.3.
3. Book's style honours the book's `line-height`, floored at Tight; a block without one gets Normal.
4. A per-book override (Quick overrides) and one shared mapping. The TXT and Markdown readers stay as they are.

## Global Constraints

- **Step numbering.** `CrossPointSettings::LINE_COMPRESSION`: `TIGHT = 0`, `NORMAL = 1`, `WIDE = 2` unchanged,
  `EXTRA_WIDE = 3` (upstream's number, crosspoint-reader #3037), `WIDEST = 4`, `FROM_BOOK = 5`. Saved settings and
  `recent.json` stay valid. `LineSpacing.h` carries the same order as `enum class LineSpacingStep : uint8_t`; the
  settings header asserts each value matches.
- **The values.**

  | Step | CJK pitch (em) | Font factor | Built-in Noto Sans factor |
  |---|---|---|---|
  | Tight | 1.5 | 0.95 | 0.90 |
  | Normal | 1.7 | 1.0 | 0.95 |
  | Wide | 1.85 | 1.1 | 1.0 |
  | Extra wide | 2.0 | 1.2 | 1.1 |
  | Widest | 2.2 | 1.3 | 1.2 |
  | Book's style | Normal, floored at Tight | Normal, floored at Tight | Normal, floored at Tight |

  CJK compression = `pitchEm × emPx / fontLineHeightPx`; `emPx = round(pointSize × 150 / 72)` (every font kind is
  converted or rasterised at 150 dpi: `fontconvert.py` `set_char_size(size << 6, size << 6, 150, 150)`,
  `ttf::ppemFromPoints`). A font line height of 0 (no font) falls back to compression 1.0.
- **Which scale.** CJK when `TypographyProfile::forLanguage(epub->getLanguage()).cjkRepertoire`; else built-in Noto
  Sans when the body font id equals `getBuiltinReaderFontIdForPoints(NOTOSANS, pointSize)` (today's test); else font.
  The dictionary has no book: font or Noto Sans scale, Book's style as Normal.
- **Book's style in the layout.** `BlockStyle::lineHeightEm` (float, 0 = none). The flow entry gains `lineHeightEm`,
  inherited like writing mode; pushed when any of the three differs from the parent's. After `enterFlow`, startElement
  sets the element's `cssStyle.lineHeightEm` to the computed value (and its defined bit when non-zero), so
  `BlockStyle::fromCssStyle` copies it at every call. A block's compression is `max(lineHeightEm × bookPerEm,
  bookFloor)` when `bookFloor > 0` and `lineHeightEm > 0`, else `lineCompression`; `bookPerEm = emPx /
  renderer.getLineHeight(fontId)`, computed once in the parser's constructor (0 when the line height is 0, which turns
  the book's value off). Only `effectiveLineHeight` reads it; the `<br>` separator gap, `<hr>` margins, table rows and
  keep-with-next's body lines keep `lineCompression`.
- **Caches.** `Section::BuildParams` gains `float bookLineHeightFloor = 0` and `uint16_t lineHeightEmPx = 0`, appended
  to the property hash only when the floor is non-zero. Not in the section header (like `fontSizeNormalization`). No
  `SECTION_FILE_VERSION` bump, no `CSS_CACHE_VERSION` bump: Book's style off lays out byte-identically, and the CSS
  cache has carried `line-height` since #560.
- **Heap.** No allocation. The flow entry grows by a float (8 entries: +32 B in the parser object, which lives in the
  build's arena or heap as today). `LineSpacing` is three scalars on the stack.
- **Strings.** `STR_EXTRA_WIDE` ("Extra Wide", upstream's key and text) and `STR_WIDEST` ("Widest") in
  `english.yaml` only; the release round translates. `STR_BOOK_S_STYLE` exists. An `STR_` key must be used in code in
  the same commit that adds it (the strip-unused step drops it otherwise).
- **No upstream code is ported.** Upstream's #3037 only adds the enum value and one factor per scale; its key name and
  enum number are reused for compatibility, nothing else.
- Comments say what the code does and why; no history narration.
- clang-format 21 is what CI runs: `bin/clang-format-fix -g`, and `clang-format-21 -style=file -i` for new files.
- Host tests (Linux): `cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/home/jens/_development/witchhunt-reader/test/build/_deps/googletest-src`, then
  `cmake --build build/test --target <T> -j 8` and `ctest --test-dir build/test -R <pattern> --output-on-failure`. In a
  worktree beside other worktrees, run ctest without `-j` (shared `/tmp` fixtures). The whole suite before the PR.
- Firmware: `/home/jens/.platformio/penv/bin/pio run -e default` and `-e gh_release`, from this worktree.

## Review Focus

1. **Inheritance through the flow stack.** An element closed through a skip path must not leave its `line-height`
   behind (the stack is trimmed by depth at every start tag); a ninth nested change is dropped with the existing
   one-time error. Pinned in Task 3: `LineSpacingLayout.AHiddenElementsValueDoesNotLeak`.
2. **The style memo** serves one tag|class entry to elements under different ancestors: the computed value is written
   into the element's copy after the memo, never into the memo. Pinned in Task 3:
   `LineSpacingLayout.OneClassUnderTwoAncestors`.
3. **Byte-identical with Book's style off**, including for books that declare `line-height`: every golden unchanged,
   and the property hash of a default `BuildParams` unchanged. Pinned in Task 3: `SectionPropertyHash.
   BookStyleOffKeepsTheKey`, and the golden run.
4. **CJK pitch is absolute.** The same CJK book at Normal gets 1.7 em whatever the font's own line height. Pinned in
   Task 1: `LineSpacing.CjkPitchIsTheSameEmWhateverTheFont`.
5. **The sleep screen's page** must build under the same key as the reader, or the sleep path makes a duplicate section
   variant (the lesson of 402cf5d67). Both go through one helper; pinned by review, and checked on the X3 (Task 6).

---

### Task 1: `LineSpacing.h`

**Files:**
- Create: `lib/Epub/Epub/LineSpacing.h` (header-only, `constexpr` where it can be)
- Create: `test/typography/LineSpacingTest.cpp`; Modify: `test/typography/CMakeLists.txt` (add the source)

- [ ] **Step 1: Write the tests first.**
  - `LineSpacing.FontFactorsPerStep`: font scale, steps 0-4 give 0.95, 1.0, 1.1, 1.2, 1.3; `bookFloor` 0.
  - `LineSpacing.BuiltinNotoSansFactorsPerStep`: 0.90, 0.95, 1.0, 1.1, 1.2.
  - `LineSpacing.CjkPitchPerStep`: 14 pt (emPx 29), line height 42: compression = pitch × 29 / 42 (to 1e-5) for 1.5,
    1.7, 1.85, 2.0, 2.2, and the parser's pitch `int(42 × compression + 0.5)` = 49, 54, 58, 64 for the last four (the
    study's 49/58 px at 1.7/2.0). Tight is 43.5 px, a half pixel the float product may put on either side: assert its
    compression only.
  - `LineSpacing.CjkPitchIsTheSameEmWhateverTheFont`: Normal at line height 42 and at line height 35, same emPx: pitch
    `round(lineHeight × compression)` is 49 both times.
  - `LineSpacing.EmIsPointsAt150Dpi`: emPx for 12, 14, 18 pt = 25, 29, 38, in integer arithmetic
    `(points × 150 + 36) / 72` as `ttf::ppemFromPoints` rounds.
  - `LineSpacing.BookStyleIsNormalFlooredAtTight`: step 5, each scale: compression = Normal's, `bookFloor` = Tight's,
    `emPx` set.
  - `LineSpacing.OutOfRangeStepIsNormal`: step 9 → Normal, `bookFloor` 0.
  - `LineSpacing.NoFontLineHeightFallsBack`: CJK scale, line height 0 → compression 1.0, no division.
- [ ] **Step 2:** Run, see them fail to compile. **Step 3:** Write `LineSpacing.h`: `LineSpacingStep`,
  `LineSpacingScale {Font, BuiltinNotoSans, CjkPitch}`, `LineSpacing`, `lineSpacingEmPx(pointSize)`,
  `lineSpacingFor(step, scale, pointSize, fontLineHeightPx)`, each with a short comment on where its values come from
  (JLREQ/CLREQ range, the study; no history).
- [ ] **Step 4:** Run `TypographyTest`, green. **Step 5: Commit** `feat(epub): one line-spacing mapping, with a CJK
  pitch scale`.

### Task 2: The setting and its two existing readers

**Files:**
- Modify: `src/CrossPointSettings.h` (:273 enum; asserts against `LineSpacingStep`), `src/CrossPointSettings.cpp`
  (`getReaderLineCompression` → `lineSpacingFor`, font or Noto Sans scale; Book's style reads as Normal there)
- Modify: `src/SettingsList.h:389` (six labels), `lib/I18n/translations/english.yaml` (two keys beside `STR_WIDE`)
- Modify: `src/util/DictHtmlPages.cpp:87` only if the signature changes (it should not)
- Modify: `test/setting_info_options` if it pins the option count (check)

- [ ] **Step 1:** Check how `lineSpacing` is read back from JSON: the generic enum clamp must follow the option count
  (`JsonSettingsIO`); a stored 3-5 from a newer file must survive, an out-of-range value fall back to Normal. Add a test
  to the settings tests that already cover enum clamping, if one exists; else note it in the report.
- [ ] **Step 2:** Make the edits. `getReaderLineCompression` keeps its name and callers; it now returns
  `lineSpacingFor(...).compression`.
- [ ] **Step 3:** Build `-e default` (the strings compile in). **Step 4: Commit** `feat(settings): Extra wide, Widest
  and Book's style line spacing`.

### Task 3: Book's style in the parser

**Files:**
- Modify: `lib/Epub/Epub/blocks/BlockStyle.h` (`lineHeightEm`; `fromCssStyle` copies it when defined;
  `getCombinedBlockStyle` takes the child's when set, else the parent's, as `fontSizeMultiplier` does)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h` (`FlowEntry::lineHeightEm` and its comment; the constructor's
  `bookLineHeightFloor` and `lineHeightEmPx` after `lineCompression`; `bookPerEm_`, `bookFloor_`)
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (`enterFlow` :894, the log only for mode/direction
  changes; after `enterFlow` at :2210 write the computed value into `cssStyle`; `effectiveLineHeight` :4545)
- Modify: `lib/Epub/Epub/Section.h` (`BuildParams`), `lib/Epub/Epub/Section.cpp` (hash :361 with its static_assert,
  the parser construction :1203)
- Modify: `test/epub_pipeline/PipelineRunner.h/.cpp` (`Profile` gains the two fields, passed to `BuildParams`)
- Create: `test/epub_pipeline/LineSpacingLayoutTest.cpp`; Modify: `test/epub_pipeline/CMakeLists.txt`

- [ ] **Step 1: Write the tests first** (synthetic books as in `WritingModeLayoutTest.cpp`; profile `lineCompression`
  1.0, `bookLineHeightFloor` 0.95, `lineHeightEmPx` 20, so `bookPerEm` = 20 / 24; line pitch = `int(24 × c + 0.5)`):
  - `AParagraphsOwnValue`: `p { line-height: 1.75 }`: c = 1.75 × 20/24 = 1.4583, pitch 35 (`y` of line 2 minus line 1).
  - `ATightValueIsFloored`: `line-height: 1.0`: 0.833 < 0.95, pitch `int(22.8 + 0.5)` = 23.
  - `NoValueIsNormal`: no `line-height`: pitch 24.
  - `InheritedFromHtml`: `html { line-height: 1.75 }`, `p` without: pitch 35.
  - `AChildOverridesItsParent`: `body { line-height: 2 }`, `p.n { line-height: 1.5 }`: `p.n` pitch 30 (1.25 × 24),
    a plain `p` pitch 40.
  - `AHiddenElementsValueDoesNotLeak`: `<div style="display:none; line-height: 3">…</div><p>…</p>`: the `p` pitch 24.
  - `OneClassUnderTwoAncestors`: `.a { line-height: 2 }`, `<div class="a"><p class="x">…</p></div><p class="x">…</p>`:
    40 then 24.
  - `BookStyleOffIgnoresTheValue`: floor 0, `p { line-height: 1.75 }`: pitch 24.
  - `WithoutTheBooksCss`: `embeddedStyle` false, floor 0.95: pitch 24.
  - `SectionPropertyHash.BookStyleOffKeepsTheKey` (in the existing Section hash tests if there are any, else here): the
    hash of a default `BuildParams` equals the value computed before this change (pin the literal, measured on master
    `e978c0f2c` before editing `Section.cpp`); with a floor it differs, and two emPx values differ.
- [ ] **Step 2:** Run, see them fail. **Step 3:** Implement. Keep the flow stack's comment accurate: it now carries the
  inherited values layout reads (writing mode, direction, line-height).
- [ ] **Step 4:** Run `EpubPipelineTest` and the new target: every golden byte-identical (`UPDATE_GOLDENS` unset), the
  new tests green. Run `WritingModeLayoutTest` (the flow stack's other user).
- [ ] **Step 5: Commit** `feat(epub): Book's style line spacing reads the book's line-height`.

### Task 4: The reader's effective line spacing

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h/.cpp`: one helper (a static member or a file-local function)
  `readerLineSpacing(uint8_t step, const Epub&, int fontId, uint8_t pointSize, const GfxRenderer&)` that picks the
  scale and calls `lineSpacingFor`. `getEffectiveReaderLineCompression` (:3292) becomes `getEffectiveLineSpacing`
  returning the struct; `makeSectionBuildParams` (:1386) fills `lineCompression`, `bookLineHeightFloor`,
  `lineHeightEmPx`; the lambda in `drawCurrentPageToBuffer` (:7356) goes, and that path calls the same helper with the
  book's override applied.
- Grep for every `BuildParams` built in `src/` (background build, in-place gate, page index) and confirm each comes from
  `makeSectionBuildParams` or the sleep path.

- [ ] **Step 1:** Make the edits. **Step 2:** `pio run -e default`. **Step 3: Commit** `refactor(reader): one line
  spacing for the reader and the sleep screen's page`.

### Task 5: The per-book override

**Files** (the paragraph-alignment override is the template; follow its every site):
- Modify: `src/RecentBooksStore.h/.cpp` (`int8_t lineSpacingOverride = -1`; carried in `addBook`'s copy at :73 and in
  the full `setReaderOverrides` overload :304; the shorter overloads pass `it->lineSpacingOverride`)
- Modify: `src/JsonSettingsIO.cpp` (:798 write, :875 read with `clampInt8(..., -1, LINE_COMPRESSION_COUNT - 1, -1)`)
- Modify: `src/activities/ActivityResult.h`, `src/activities/reader/QuickOverridesActivity.h/.cpp`,
  `src/activities/reader/EpubReaderMenuActivity.h/.cpp` (an entry with Default plus the six labels, Default first as
  for alignment), `src/activities/reader/EpubReaderActivity.h/.cpp` (`bookLineSpacingOverride`, loaded at :750, applied
  where the other overrides are applied :3124-3171, compared for the rebuild decision at :3148, passed on at
  :7440-7498 and :7686-7717)

- [ ] **Step 1:** Make the edits. A changed override rebuilds the section exactly as a changed alignment does.
- [ ] **Step 2:** Extend the `recent.json` round-trip test if one exists (grep `paragraphAlignmentOverride` in `test/`);
  else add `lineSpacingOverride` to the nearest store test.
- [ ] **Step 3:** `pio run -e default`. **Step 4: Commit** `feat(reader): a per-book line spacing override`.

### Task 6: Documents, builds and the X3

**Files:**
- Modify: `USER_GUIDE.md:392` (the six choices; CJK books' em pitch; Book's style; the override)
- Modify: `docs/future_work/cjk.md` (step 10 done, by this PR), `docs/future_work/writing-modes.md` (the step 0 row:
  shipped, #560)

- [ ] **Step 1:** Make the edits. **Commit** `docs: line spacing in the guide and the roadmaps`.
- [ ] **Step 2:** `pio run -e default` and `-e gh_release`. Record flash and static RAM against master `e978c0f2c`.
- [ ] **Step 3: Back up the X3** before anything opens a book: `GET:*` to `settings_before.txt`, and `/.crosspoint`'s
  `recent.json`, `state.json` and stats over USB Transfer to
  `~/.local/state/witchhunt-device-backups/2026-10-26-x3-line-spacing/` with a sha256 list, mode 700. A DOWN/UP on
  Home first (the X3 auto-sleeps after 15 minutes).
- [ ] **Step 4: Flash the branch.** Font Noto Sans CJK 14 pt, as in the study.
- [ ] **Step 5: CJK at Normal.** The study's three pages (ja prologue, zh essay, ko excerpt; page 2 each) at Normal:
  screenshots match the study's 1.7 em screens, page counts 60/24/4 (the study's 1.7 em counts; today's Normal was
  52/21/4).
- [ ] **Step 6: Latin unchanged.** The English control (A Game of Thrones, three spine items) at Normal: page counts as
  on master (6/39/41 in the step 0 check), and the section cache files of a Latin book opened on master are reused
  (no rebuild in the log).
- [ ] **Step 7: Book's style.** `変な家２` (EBPAJ, 1.75): pitch 1.75 em (51 px at 29 px em); the Hayakawa book
  (calibre's 1.2): floored at 1.5 em (43 or 44 px, the half pixel above).
- [ ] **Step 8: The override.** Set Extra wide from Quick overrides on one book: it rebuilds at 2.0 em (58 px); another
  book stays at the global step; back to Default.
- [ ] **Step 9: The sleep screen's page** (only while attended, never `CMD:SLEEP` unattended): skip if unattended; else
  one sleep with the reader open, and the log shows no second section build.
- [ ] **Step 10: Restore** the settings and `/.crosspoint` files from the backup, verify by readback, flash master back
  unless the maintainer says otherwise.
- [ ] **Step 11:** Report in the PR, 2-4 KB: what changed, the table of values, the review focus, the device result.
