# CJK Dictionary Lookups Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On a Han or kana character, one Confirm looks up the longest dictionary word that starts there and shows it with up to two shorter matches, in the reader's own font. Before that, two standalone PRs: the prewarm log noise goes (L1), and the word screen frees its arrays while a definition is open (H1).

**Architecture:** Three PRs, in the spec's order (§9, decision 6).
- **PR 1 (L1)** changes two log levels in `SdCardFont::prewarm`.
- **PR 2 (H1)** frees the word screen's arrays and snapshot while a definition is open, and rebuilds them on return without moving the cursor.
- **PR 3** adds three things:
  - a pure run builder (`CjkLookupRun`);
  - a longest-prefix search and a multi-entry reader in `Dictionary`: one descent plus short probes, over the session buffers that exist today (§5.2);
  - the wiring into the word screen (speculation, Confirm, span highlight, tap), the serial `ACT:DICT` and the definition screen's font.

**Tech Stack:**
- C++20 firmware (ESP32-C3/S3, PlatformIO, `-fno-exceptions`).
- GoogleTest host tests under `test/` (CMake, Ninja, MSYS2 UCRT64).
- `tools/devctl.py` over the serial test API, on the X4.
- PyGlossary on the host, to make the StarDict test dictionaries.

**Spec:** `docs/superpowers/specs/2026-10-21-cjk-dictionary-design.md` (decided 2026-10-22). It is roadmap step 9 in `docs/future_work/cjk.md`. Task 9 deletes the spec once its lasting parts are in `docs/dictionary.md`, as the spec asks.

## Global Constraints

**From the spec, verbatim:**
- §2: "No steady-state RAM. Transient RAM is no larger than the existing lookup session, plus a few bytes."
- §4: "Anything the lookup adds must live in the existing session buffers or be a few bytes."
- §2: "No error-level log line on a path that recovers."
- §2: "Latin-script lookups behave exactly as today, and the existing host tests stay green."
- §5.1: "When C applies. It depends on the selected word's first code point, not on the book's language". Han or kana (`utf8IsUnspacedCjk`): "the run is built across words". Hangul: "the run is the token itself". "Anything else takes today's path, unchanged."
- §5.1, the run:
  - "in reading order, across a line end";
  - "up to 12 code points";
  - "stopping at the first code point that is not unspaced CJK, or that `cleanWord` would strip as punctuation (。、「」（）…)";
  - "Kept as letters: 々 U+3005, 〆 U+3006 and ー U+30FC, which `EDGE_PUNCTUATION` leaves alone, and kana";
  - "Each code point remembers which word it came from, so a match length maps back to a span of words."
- §5.1: "A tap on any character inside the highlighted span looks the span up. A tap elsewhere moves the cursor, as today."
- §5.2: "The stem, lowercase, elision and hyphen fallbacks of `resolve()` are skipped for a CJK run."
- §5.2: "Between probes the search checks `mappedInput.hasPendingInput()`. If input is waiting, it gives up and leaves the speculation 'unknown'".
- §5.2: "A Latin dictionary on a CJK page costs one search and reports 'Missing', as today."
- §5.2, step 4: "With a `.syn`, do the same over `.syn` and `.sidx` and take the longer result (ties go to `.idx`)."
- §5.3: "The definition holds the longest match, then up to two shorter ones from the same search (≤ 3 entries, longest first, within `MAX_DEFINITION_BYTES`)."
- §5.3: "Plain dictionaries: each entry is `headword` on its own line, then its text." "HTML dictionaries: each entry is a `<p><b>headword</b></p>` (escaped) followed by its HTML." "the buffer is sized once and filled in place, with no growth."
- §5.3, the font: "A lookup that started on CJK text passes the reader's effective font id ... to the definition, instead of `DICTIONARY_FONT_ID`." "The headword moves into the body's first line, and the header keeps the dictionary name and page counter." "Latin lookups keep Noto Sans and its IPA."
- §4: "A span drawn on the differential path must fit 2 runs and 2,048 bytes (16,384 px). Larger spans take the full repaint". "A grown span is the same second repaint, not a new one."
- §5.6, the H1 stop rule: "if H1 does not get the gate to pass on the X3 dense page (section 7), take H4 and leave the gates alone."
- §5.7, L1: "The first miss logs at debug level; `prewarm()` logs one line after the retry: info with the glyph count kept and dropped, error only when nothing is resident".
- §7 row 2: "median within 1.5× today's exact lookup on the same dictionary (target to agree)".

**The maintainer's answers (2026-10-24), which override the spec where they differ:**
- **Every entry of a headword is shown.** A CJK lookup shows every entry of the longest match first: CC-CEDICT's 行 xíng and háng, both. The shorter matches come after them (up to two, as planned) while the buffer has room under `MAX_DEFINITION_BYTES`. This replaces §5.3's "≤ 3 entries". A shorter match brings all of its entries too (Ruling 8).
- **The exact lookup does not change.** `resolve()` and `lookup()`, which every Latin word takes, still read one entry.
- **The speed target is a firm gate:** at most 1.5× today's exact lookup, as the median over ~50 words per book, on the X4. It is no longer "target to agree".
- **Devices:** the X4 only, with `CMD:HEAP:HOLD` ballast standing in for the X3. There is no X3 run and no touch-board run. Spec row 12 is recorded as not run.
- **The maintainer's decisions:**
  - the selection grows to the longest match with "no extra press";
  - there is "no shrink gesture";
  - "No Japanese or Korean deinflection this round";
  - "StarDict only ... no dictionaries shipped and no conversion in the web page".
- **The §2 anti-goals:**
  - no word segmenter on the device;
  - no format other than StarDict;
  - no dictionary in flash;
  - no online lookups;
  - no "CJK mode" setting.

**Project rules:**
- **Execution.** Subagent-driven: one implementer and one reviewer per task. Device steps run one at a time: there is one X4, on COM6, and one process per port.
  - The CJK UI text fallback plan (roadmap step 6) uses the same X4 and flashes its own builds. A device session of this plan starts by flashing this branch's build, unless the ledger shows that build is still on the device, and sets the settings it needs (`fontFamily`, the dictionary) itself.
- **Devices.**
  - Validate on the X4 only, with `tools/devctl.py` (`docs/serial-test-api.md`).
  - There is no X3 run: `CMD:HEAP:HOLD` ballast stands in for its heap (the maintainer, 2026-10-24).
  - There is no touch-board run (X4 Pro, T5S3). Spec §7 row 12 is recorded as not run, and the reviewer reads the tap code instead.
- **Host tests.** Run from Git Bash, never from PowerShell. Configure each worktree once, from its root:
  `cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/cc.exe -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/c++.exe -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1`
- **Before each PR:** the PR gate below, including the whole host suite. The known Windows-only failures are FirstChapter ×2, LongChapterPageTables ×2 and KeptReadingState.
- **Firmware:** build `env:default` and `env:gh_release` before each PR. Leave the S3 environments to CI. Format with clang-format 21.
- **Text:**
  - User-facing strings go through `tr(STR_*)`, with the key added to `lib/I18n/translations/english.yaml` as `docs/i18n.md` says. Never hand-run `scripts/gen_i18n.py`.
  - This plan adds no user-facing string. A step that finds it needs one follows `docs/i18n.md`.
- **Layering.**
  - `Dictionary` stays free of `MappedInputManager`. The input check is injected as a plain function pointer: the word screen passes `CooperativeAbort::shouldAbortLongTask`, whose predicate `main.cpp:1567` already sets to `mappedInputManager.hasPendingInput()`.
  - No `std::function` anywhere.
- **Heap:**
  - no bare `new`;
  - `makeUniqueNoThrow` for fallible allocations;
  - `heapHasBlockFor` or `ESP.getMaxAllocHeap()` before container growth;
  - a size note on every new allocation.
- **Versions.** No section or cache version changes.
  - Master is at section 99 (ruby B1, #554). In flight: the DP line-indent fix (section 100) and emphasis dots B2 (section 101, `CSS_CACHE_VERSION` 30).
  - If a step finds it needs a bump, it takes the next free number: section 102, or 103 if the UI text fallback plan has taken 102; CSS cache 31. The number is renumbered to master's + 1 at merge.
- **Parallel work this plan must not touch:**
  - Emphasis dots B2 (in flight) owns `TextBlock`, `ParsedText`, `ChapterHtmlSlimParser`, `Section` and `CssParser`. Ruby B1 has merged (#554).
  - Roadmap step 6 (CJK UI text fallback, `docs/superpowers/plans/2026-10-23-cjk-ui-text-fallback.md`) owns `FallbackFaceProvider`, `CjkUiFonts`, the UI faces and `SdCardFontManager`.
  - This plan draws CJK definitions in the reader's font through one function, `DictionaryWordSelectActivity::definitionFontId()`. Step 6 does not touch the dictionary. Once both have merged, a follow-up moves the headword back into the header, adopting step 6's `BookTextScope`, `CjkUiFonts::readerFontAttached()` and `prefetchBookText()`. This plan builds no UI fallback of its own.
  - Both plans add `STATE` rows to `docs/serial-test-api.md`. This plan's rows go directly after the `sel`, `label` row; step 6's go after `heap`, `maxblock`. The two then merge cleanly in either order.
  - `lib/EpdFont/SdCardFont.cpp`: PR 1 changes `prewarm()` and `prewarmStyle()`, and step 6 moves `packInkMask()` out of the same file. The changes are 30 lines apart and merge cleanly in either order.

## Review Focus

1. **Confirm in the definition with two dictionaries installed, after H1 freed the arrays.**
   - Expected: the next dictionary's definition, or its "not found", opens. The word screen never reads `words[selected]` before the busy repaint has rebuilt the array.
   - Pinned by Task 2 Step 8 (the exact-lookup path) and Task 7 Step 7 (the prefix search).
2. **A span wider than the 2,048-byte snapshot or over three rows** (the largest reader size, or a long unpunctuated run across two line ends).
   - Expected: the whole highlight is drawn through the full repaint, and the next cursor move leaves no inverted pixels behind.
   - Pinned by Task 8 Step 6.
3. **Presses arriving while a speculative search runs** (Right pressed repeatedly across a line, on a CC-CEDICT-sized index).
   - Expected: the cursor keeps up. An abandoned search draws no span or outline on a word the cursor has left, and the word the cursor stops on is searched once it rests.
   - Pinned by the Task 4 test `PrefixSearchGivesUpBetweenProbesWhenAsked` and Task 7 Step 8.
4. **A Latin dictionary chosen while reading CJK, and a Latin word with a CJK dictionary.**
   - Expected: the CJK run reports Missing after one probe per index file. The Latin word takes today's exact path, byte for byte.
   - Pinned by the Task 4 test `LatinDictionaryAnswersACjkRunInOneProbePerFile` and Task 9 Step 9.
5. **Same-headword entries** (CC-CEDICT's 行 xíng and háng), **including a run of them that crosses a sample boundary, or that a `.syn` alias points into the middle of.**
   - Expected: every entry of the longest match appears, in `.idx` order and starting from the run's first entry, before the shorter matches. Each definition appears once.
   - Pinned by the Task 4 tests `PrefixSearchCountsEveryEntryOfADuplicateHeadword`, `PrefixSearchFindsADuplicateRunAcrossASample`, `PrefixSearchListsTheWholeRunBehindASynonym` and `PrefixSearchListsEachDefinitionOnce`, and the Task 5 tests `PrefixDefinitionsListEveryEntryOfTheLongestMatchFirst` and `PrefixDefinitionsFillTheBufferBeforeTheShorterMatches`.

## Rulings on the spec

Where the spec left a choice open or disagreed with the project's rules, this plan decided as follows. A reviewer holds each task to these.

1. **One plan for three PRs.** §9 says "Each is its own plan and PR". This document keeps one part per PR, each with its own branch and gate.
2. **H1's stop rule is measured on the X4,** as the maintainer confirmed on 2026-10-24. The spec names the X3, but `CMD:HEAP:HOLD` ballast stands in for the room the X3 lacks (Task 2 Step 3). If the X3 still falls short, it falls back to plain text, which is H4: no crash.
3. **`.idx` and `.syn` hits are merged by length.** On a tie the `.idx` hit wins, and each definition is listed once. This reads "take the longer result" so that the shorter matches from the other file survive (大學生 → 大学 through `.syn`, then 大).
4. **No probe cap before the first hit; at most 3 probes per file after it.** The spec says "usually one or two". A cap before the first hit could report a false Missing. The loop is still bounded: the target shortens on every probe, so there are at most 12, plus one `locateRun` per hit whose run may begin before its window (Ruling 8). The firm 1.5× gate (Task 9 Step 4) checks what this costs.
5. **The abort is opt-in per call.** Speculation passes the input check. Confirm and the serial lookup do not, because the reader is waiting for them. `Dictionary` gets the check as a function pointer and never sees `MappedInputManager`.
6. **The test dictionaries are built in `DictionaryTest.cpp`** with its `writeDictionary` helper, not with `make_fixture.py` (§6). That helper is the existing pattern for rule tests. Regenerating the fixture could also rewrite the checked-in `.dict.dz`, whose bytes depend on the zlib version.
7. **The CJK definition's header has no title.** The dictionary name and the page counter stay as they are today, so the name appears only when two or more dictionaries are installed.
8. **Every entry of a listed headword is shown** (the maintainer, 2026-10-24, reversing this plan's first draft). This applies to the longest match first, then to each shorter one.
   - **Contiguity.** StarDict sorts by `asciiCaseCmp` with a `strcmp` tiebreak, so the entries of one headword (byte-for-byte equal) sit next to each other in the `.idx`. A hit therefore records where its run starts (`idxStart`) and how long it is (`entryCount`, at most 8). The reader reads the run again from there.
   - **Where a run can be cut.** The run is cut only when the headword is the window's first entry (`w0`), because then it may begin in the window before. One extra probe ("locate the run": the last sample strictly below the headword, then a scan forward to its first entry) finds the start.
   - **Synonyms.** A `.syn` hit resolves to one entry, which may lie in the middle of its run, so it takes the same probe.
   - **The exact lookup is unchanged:** it still reads one entry.
9. **Speculation is re-enabled once an index build succeeds**, for Latin too. Without it, a dictionary indexed in this session would never grow a CJK span until the screen is opened again.
10. **L2 is not taken.** Asking `heapHasBlockFor` first has no visible effect, and the arena path has no heap block to ask about.

---

## PRs, branches and the PR gate

| Part | PR title | Branch | Worktree | Tasks |
|---|---|---|---|---|
| A | `fix(fonts): no error line when the prewarm arena retry recovers` | `fix/sdfont-prewarm-log-level` from `origin/master` | `C:/tmp/wt-dict-l1` | 1 |
| B | `feat(dictionary): free the word arrays while a definition is open` | `feat/dict-release-word-arrays` from `origin/master` | `C:/tmp/wt-dict-h1` | 2 |
| C | `feat(dictionary): look CJK text up by its longest dictionary word` | `feat/cjk-dictionary` (this branch, which holds the plan) | `C:/tmp/wt-dict` | 3-9 |

Creating the worktrees for parts A and B (Part C uses this one):

```bash
git -C C:/tmp/wt-dict fetch origin
git -C C:/tmp/wt-dict worktree add C:/tmp/wt-dict-l1 -b fix/sdfont-prewarm-log-level origin/master
git -C C:/tmp/wt-dict-l1 submodule update --init freeink-sdk
```

Do the same for `C:/tmp/wt-dict-h1` and `feat/dict-release-word-arrays`.

**How the parts depend on each other:**
- Parts A and B are independent of each other.
- In Part C, Tasks 3-6 do not touch the word screen and can start at once.
- Before Task 7, merge Part B into this branch: `git merge --no-ff feat/dict-release-word-arrays`, or `origin/master` if PR 2 has already merged.
- Open PR 3 against `master` once PR 2 has merged. If PR 2 is still open then, open PR 3 against `feat/dict-release-word-arrays`, and retarget it to `master` when PR 2 merges.
- If Task 2's stop rule drops H1, PR 2 is not opened and Part B is not merged. Task 7 then starts from this branch as it is, and its references to Task 2's rebuild do not apply.

**The PR gate** runs from the part's worktree, in Git Bash.

1. Merge `origin/master` into the branch, then run the whole suite:
   ```bash
   cmake --build build/test -j 8 -- -k 0
   ctest --test-dir build/test -j 8 2>&1 | tail -25
   ```
   Expected: the only failures are the five known Windows-only ones (two FirstChapter, two LongChapterPageTables, KeptReadingState). `epub_build_inventory` may fail to build on Windows (`dlfcn.h`); `-k 0` keeps the rest building.
2. Build the firmware, in the background (a cold build exceeds 600 s):
   ```bash
   export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
   "$HOME/.platformio/penv/Scripts/pio.exe" run -e default    > C:/tmp/cjkdict/<part>-default.log 2>&1
   "$HOME/.platformio/penv/Scripts/pio.exe" run -e gh_release > C:/tmp/cjkdict/<part>-release.log 2>&1
   ```
   Expected: both logs end in `[SUCCESS]`.
3. Format with clang-format 21. If `/c/tmp/cf21` is gone, reinstall it with `python -m pip install --target C:/tmp/cf21 "clang-format>=21,<22"`.
   ```bash
   git diff --name-only --diff-filter=d origin/master... -- '*.cpp' '*.h' | xargs -r /c/tmp/cf21/clang_format/data/bin/clang-format.exe -i
   git status --short
   ```
   Expected: no changes, or one `style: clang-format 21` commit.
4. Push and open the PR. The body carries a summary, the test results, and the device results with their numbers:
   ```bash
   git push -u origin <branch>
   gh pr create --base master --title "<title above>" --body-file C:/tmp/cjkdict/<part>-pr.md
   ```

**Device session rules**, for every `devctl.py` and `serial_transfer.py` call:
- Prefix each call with `MSYS_NO_PATHCONV=1`, and pass `--port COM6`.
- Before a long session, run `cmd:GET:sleepTimeoutMinutes`, then `cmd:SET:sleepTimeoutMinutes=30`. Restore the value at the end.
- The X4's UI is German: "Nicht gefunden" means not found, and the dictionary row is "Wörterbuch".
- On this X4 the POWER key opens the word screen. Check `cmd:GET:btnShortPower` first. If it is bound to something else, open the screen from the reader menu: `cmd:BTN:CONFIRM`, `"select:(Dictionary|Wörterbuch)"`, `cmd:BTN:CONFIRM`.
- Never send `SLEEP` or `BTN:POWER:LONG`.
- Keep runs sent over serial to 12 characters or fewer, so a `CMD:` line stays short.
- Logs and test files go under `C:/tmp/cjkdict/` (`mkdir -p /c/tmp/cjkdict` once). Each devctl run that a step greps passes `--log C:/tmp/cjkdict/<name>.log`.

## Ruby B1 (merged in #554)

B1 is on master (`82756a9c8`, section 99), and this branch has it merged. **No code in this plan changes.** Checked against that tree:
- B1 keeps readings out of the token stream: they are `TextBlock` annotations (flags bit 1). `RubyLayout.GroupRubySpansItsWholeBase` pins that a ruby line's words are its base tokens only. `extractWords` and `CjkLookupRun` therefore see only the bases, and the inline （わがはい） that stopped a run before B1 is gone.
- A group's follower tokens are glued (`continues`, `ParsedText::addRubyGroup`). The word screen therefore makes a whole base group one `Word`:
  - the cursor sits on 吾輩, never on 輩 alone, and the run starts at the group's first character;
  - a match that ends inside a group maps to the whole group (`wordsCovering`, pinned by `MatchLengthsMapBackToWords` with 生活), so its highlight can be wider than the match, as with kinsoku glue.
- `TextBlock::wordBox` puts each box on the base, below the ruby band (`CjkRubyTest`'s `WordBoxSitsBelowTheBand`). The highlight boxes come from `wordBox`, at the font's line height, so they cover the base and are no taller on a ruby line. The snapshot limits hold.
- Task 9 Step 5 checks this on the device, on `/test/test_cjk_ja_soseki.epub`.
- **After merging B2 (section 101, CSS cache 30) into this branch:** rerun the PR gate's suite, and Task 9 Step 5.

## File map

Line numbers in this plan are master's at `82756a9c8` (checked 2026-10-24: #553 and #554 changed none of the code files cited here, only `docs/future_work/cjk.md`). Once an earlier task has changed a file, find the function by its name.

| File | Part | What |
|---|---|---|
| `lib/EpdFont/SdCardFont.cpp` | A | prewarm log levels |
| `test/sd_font_arena/SdFontArenaTest.cpp`, `test/sd_font_arena/CMakeLists.txt` | A | log-capturing tests |
| `src/activities/reader/DictionaryWordSelectActivity.{h,cpp}` | B, C | H1; CJK speculation, lookup, span highlight, tap, STATE |
| `docs/serial-test-api.md` | B, C | STATE fields of the word screen; `ACT:DICT` CJK reply |
| `src/util/DictionaryPunctuation.h` (new) | C | `EDGE_PUNCTUATION`, shared by `cleanWord` and the run builder |
| `src/util/CjkLookupRun.{h,cpp}` (new), `test/dictionary/CjkLookupRunTest.cpp` (new) | C | the run of CJK text a lookup starts on |
| `src/util/Dictionary.{h,cpp}`, `test/dictionary/DictionaryTest.cpp`, `test/dictionary/CMakeLists.txt` | C | prefix search, multi-entry reader |
| `src/SerialTestApi.cpp` | C | `ACT:DICT` reports the prefix search |
| `src/activities/reader/DictionaryDefinitionActivity.{h,cpp}`, `src/util/DictHtmlPages.h` (comment) | C | font id parameter, no header title for CJK |
| `docs/dictionary.md`, `docs/future_work/cjk.md`; the spec deleted | C | user docs, roadmap |

---

## Part A: PR 1, the prewarm log noise (L1)

### Task 1: The arena retry logs no error when it recovers

**Files:**
- Modify: `lib/EpdFont/SdCardFont.cpp:1693-1701` (`prewarmStyle`'s failed arena allocation), `:1210-1242` (`prewarm`'s retry)
- Modify: `test/sd_font_arena/CMakeLists.txt`
- Test: `test/sd_font_arena/SdFontArenaTest.cpp`

**Interfaces:**
- Consumes: the existing `setLogObserver(LogObserver)` hook in `test/epub_pipeline/LogObserver.h` and `test/epub_pipeline/LoggingImpl.cpp`.
- Produces three log lines, with this exact text (the tests match it):
  - DBG `SDCF` `Failed to allocate mini bitmap (%u bytes) for style %u`: today's text, at a lower level.
  - INF `SDCF` `Prewarm style %u kept %u of %u glyphs after the arena retry`: the style index, the glyphs kept (`fit`) and the glyphs asked for (`cpCount`).
  - ERR `SDCF` `Prewarm style %u kept no glyphs: no block for any of %u`: only when the retry leaves nothing resident.

- [ ] **Step 1: Create the worktree, and configure the host tests in it** (see the PR table and Global Constraints).

- [ ] **Step 2: Turn logging on for `SdFontArenaTest`**

In `test/sd_font_arena/CMakeLists.txt`:
- replace `${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp` with `${REPO_ROOT}/test/epub_pipeline/LoggingImpl.cpp`;
- add `target_compile_definitions(SdFontArenaTest PRIVATE ENABLE_SERIAL_LOG=1 LOG_LEVEL=2)`.

- [ ] **Step 3: Write the failing tests** in `SdFontArenaTest.cpp`, after `PrewarmRetrySizesFromTheArenaRoom`

```cpp
#include "../epub_pipeline/LogObserver.h"  // with the other includes

std::vector<std::string> sdcfLines;  // "<level> <message>" of every SDCF line while observed
void keepSdcfLine(const char* level, const char* origin, const char* message) {
  if (std::strcmp(origin, "SDCF") == 0) sdcfLines.push_back(std::string(level) + " " + message);
}
int countLines(const char* level, const char* fragment = "") {
  int n = 0;
  for (const auto& line : sdcfLines) n += line.rfind(level, 0) == 0 && line.find(fragment) != std::string::npos;
  return n;
}

TEST(SdFontArena, ARetryThatKeepsGlyphsLogsNoError) {
  // PrewarmRetrySizesFromTheArenaRoom's arena: the first attempt fails, the retry keeps 1 of the 5 glyphs.
  // ... same font, probe arena and BuildArena(fullUse - GLYPHS * kBitmap + 7500) as that test ...
  sdcfLines.clear();
  setLogObserver(keepSdcfLine);
  ESP.setMaxAllocHeap(0);
  font.prewarm("ABCD", 0x01);
  ESP.setMaxAllocHeap(100 * 1024);
  setLogObserver(nullptr);
  EXPECT_EQ(0, countLines("ERR"));
  EXPECT_EQ(1, countLines("INF", "kept 1 of 5 glyphs"));
}

TEST(SdFontArena, ARetryThatKeepsNothingLogsOneError) {
  // 3,000 bytes of room is under the retry's 4,096-byte headroom: not one glyph fits.
  // ... BuildArena(fullUse - GLYPHS * kBitmap + 3000), otherwise as above ...
  EXPECT_EQ(1, countLines("ERR"));
  EXPECT_EQ(1, countLines("ERR", "kept no glyphs"));
}
```

Factor the two arenas' shared setup (`buildFont(2000, false)`, `writeTemp`, the probe arena's `fullUse`) into a helper in the anonymous namespace, so the new tests do not copy it.

- [ ] **Step 4: Run them to see them fail**

Run: `cmake --build build/test --target SdFontArenaTest && ./build/test/sd_font_arena/SdFontArenaTest.exe --gtest_filter='SdFontArena.ARetry*'`
Expected: FAIL. In `ARetryThatKeepsGlyphsLogsNoError`, `countLines("ERR")` is 1 (the "Failed to allocate mini bitmap" line), and the INF count is 0.

- [ ] **Step 5: Change the log lines in `SdCardFont.cpp`**

- Make the `Failed to allocate mini bitmap` line (`:1695`) a `LOG_DBG`.
- After the retry loop (`:1232-1239`), log one line per style that was retried: the INF line when `missedForStyle != PREWARM_ARENA_TOO_LARGE`, the ERR line otherwise.
- Do not take the spec's L2 (asking `heapHasBlockFor` before the allocation). It changes nothing a reader sees, and the arena path has no heap block to ask about.
- Upstream #4112 changes the same failure path. Note it in the PR body.

- [ ] **Step 6: Run the whole binary**

Run: `cmake --build build/test --target SdFontArenaTest && ./build/test/sd_font_arena/SdFontArenaTest.exe`
Expected: PASS, every test, the two new ones included.

- [ ] **Step 7: Check on the X4** (only if a CJK `.cpfont` family is on the card)

Run: `MSYS_NO_PATHCONV=1 python tools/devctl.py --port COM6 cmd:FS:LS:/.crosspoint/fonts`

If a folder holds a CJK `.cpfont` family (its name says SC, TC, JP, KR or CJK):
1. Flash this branch: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6`.
2. In one devctl run with `--log C:/tmp/cjkdict/l1.log`: open a dense CJK page in that family; open the word screen (`cmd:BTN:POWER`); press `cmd:BTN:RIGHT` twice, with `idle:5000:SCREEN` after each press; then press `cmd:BTN:BACK` twice.
3. Count the errors: `grep -c "\[ERR\] \[SDCF\]" C:/tmp/cjkdict/l1.log`.

Expected: 0, and `kept N of M glyphs` INF lines where master logged errors.

If no such family is on the card, skip this step and say so in the PR body: the host tests carry the change.

- [ ] **Step 8: Commit**

```bash
git add lib/EpdFont/SdCardFont.cpp test/sd_font_arena/SdFontArenaTest.cpp test/sd_font_arena/CMakeLists.txt
git commit -m "fix(fonts): the prewarm arena retry logs an error only when it keeps no glyph"
```

- [ ] **Step 9: Run the PR gate, then open PR 1**

---

## Part B: PR 2, the word arrays released while a definition is open (H1)

### Task 2: Free the arrays for the definition, rebuild them on return

**Files:**
- Modify: `src/activities/reader/DictionaryWordSelectActivity.h:56-59`, the Fragment comment ("stays alive until the definition has been read" now ends there), and `:164-240` (members)
- Modify: `src/activities/reader/DictionaryWordSelectActivity.cpp:93-99` (`onEnter`), `:138-316` (`extractWords`), `:529-649` (`performLookup`), `:651-724` (`loop`)
- Modify: `docs/serial-test-api.md:197-210` (the STATE table)

**Interfaces:**
- Produces:
  - `void releaseWordArrays();` empties `fragments`, `words` and `drawStyles` by swapping each with an empty vector. It also resets `snapshot`, sets `snapshotIdx = -1`, `snapshotRunCount = 0` and `wordsExtracted = false`, and keeps `selected` and the speculation fields.
  - `bool wordsBuiltOnce = false;` is set by the first successful `extractWords()`.
  - On a rebuild, `extractWords()` keeps `selected`: it is clamped to the last word, and the speculation is reset only when the clamp moved it. It logs DBG `DICT` `Rebuilt %u words in %lu ms`.
  - The snapshot is taken in `extractWords()` after the two arrays, when `snapshot` is null, no longer in `onEnter()`.
  - `void appendDebugState(std::string& out) const override;`, inside `#if SERIAL_TEST_API` as in `HomeActivity.h:141-146`. It writes `sel=<selected>` and `label=<the selected word's text>` through `SerialCommandGrammar::appendField`, and nothing while `!wordsExtracted || words.empty()`.

- [ ] **Step 1: Make the CJK test dictionary** (once for the whole plan; Tasks 6-9 reuse it)

Keep the download and the tool in separate directories:

```bash
mkdir -p /c/tmp/cjkdict/src /c/tmp/cjkdict/cedict
curl -L -o /c/tmp/cjkdict/src/cedict.txt.gz https://www.mdbg.net/chinese/export/cedict/cedict_1_0_ts_utf-8_mdbg.txt.gz
gzip -dk /c/tmp/cjkdict/src/cedict.txt.gz
python -m venv C:/tmp/cjkdict/venv && C:/tmp/cjkdict/venv/Scripts/python -m pip install pyglossary jieba
C:/tmp/cjkdict/venv/Scripts/python -m pyglossary --help | grep -i -E "read-options|write-options"
C:/tmp/cjkdict/venv/Scripts/python -m pyglossary C:/tmp/cjkdict/src/cedict.txt C:/tmp/cjkdict/cedict/cedict.ifo \
  --read-format=EDICT2 --write-format=Stardict --read-options="colorize_tones=False" --write-options="dictzip=False"
grep -E "wordcount|synwordcount|sametypesequence" /c/tmp/cjkdict/cedict/cedict.ifo; ls -l /c/tmp/cjkdict/cedict
```

Expected:
- `cedict.ifo`, `cedict.idx` and a plain `cedict.dict` (no `.dz`), and a `cedict.syn` if PyGlossary writes the traditional forms as synonyms;
- `sametypesequence=h`.

If a format name or option is refused, find the right one in `--help` and in PyGlossary's plugin pages (`doc/p/edict2.md`, `doc/p/stardict.md`), and write down the command that worked: Task 9 documents it.

- [ ] **Step 2: Put it on the X4 and choose it**

```bash
export MSYS_NO_PATHCONV=1
python tools/devctl.py --port COM6 cmd:ACT:USB_TRANSFER
python tools/serial_transfer.py --port COM6 mkdir /dictionaries/cedict
for f in cedict.ifo cedict.idx cedict.dict cedict.syn; do
  [ -f /c/tmp/cjkdict/cedict/$f ] && python tools/serial_transfer.py --port COM6 put C:/tmp/cjkdict/cedict/$f /dictionaries/cedict/$f
done
python tools/devctl.py --port COM6 cmd:ACT:HOME idle:30000 cmd:ACT:SETTINGS:READER idle:20000 \
  "select:(Dictionary|Wörterbuch)" cmd:BTN:CONFIRM idle:20000:SCREEN "select:cedict" cmd:BTN:CONFIRM idle:20000 \
  "cmd:ACT:DICT:大学"
```

Expected: `DICT:result=found dict=cedict word=大学 headword=大学 bytes=<n>`. The first lookup builds the index first, which takes seconds. The `en-de` dictionary already on the card stays installed for Step 8.

- [ ] **Step 3: Measure the failing case on master, before any change**

1. Build and flash this worktree, which is still `origin/master`, with `pio run -e default -t upload --upload-port COM6`.
2. Find a dense Chinese page. The X3's reference is Hong Lou Meng at 14 pt: use it if `cmd:FS:LS:/books` or `cmd:FS:LS:/test` lists it, otherwise the densest `test_cjk_zh*` book. Check that STATE's `font=` is a CJK family.
3. Run:

```bash
python tools/devctl.py --port COM6 --log C:/tmp/cjkdict/h1-master.log \
  "cmd:ACT:READER:<book>@<spine>:<page>" idle:60000:SCREEN state \
  cmd:BTN:POWER idle:10000:SCREEN state check:activity=DictionaryWordSelect wait:1 \
  cmd:BTN:CONFIRM idle:15000:SCREEN state check:activity=DictionaryDefinition \
  cmd:BTN:BACK idle:10000:SCREEN cmd:BTN:BACK idle:10000:SCREEN
grep DHTML C:/tmp/cjkdict/h1-master.log
```

Expected: `[ERR] [DHTML] Low heap for styled definition (<free> free, <block> max block)`.

- If `check:activity=DictionaryDefinition` fails (the cursor was on a character with no entry), add one `cmd:BTN:RIGHT idle:5000:SCREEN wait:1` before the CONFIRM.
- If the log shows `Styled layout starting` instead, the X4 has the room the X3 lacks. Insert `cmd:HEAP:HOLD=8192,4` before `cmd:BTN:POWER` and `cmd:HEAP:FREE` at the end. Raise the 8192 by 4096 per run until the `Low heap` line appears.
- Write down the free and block numbers, and the ballast (`BALLAST`, 0 if none).

- [ ] **Step 4: Implement H1** in `DictionaryWordSelectActivity`

1. **`releaseWordArrays()`.** Add it as in Interfaces. Call it in `performLookup` right after `dict.releaseCaches()` (`:589`), before `startActivityForResult`, under `RenderLock lock(*this);`: `performLookup` runs in `loop()`, and the render task reads the same arrays (`render`, `appendDebugState`).
2. **The rebuild.**
   - In `extractWords()`, keep `selected` on a rebuild instead of recentring it (`:310-315`).
   - Take the snapshot after the arrays.
   - Time the rebuild for the DBG line.
   - Remove the snapshot allocation from `onEnter()`.
3. **`loop()`.** Treat `!wordsExtracted` like `words.empty()`: until the rebuild has been drawn, Confirm, the moves and speculation do nothing, and Back still finishes. `wordsExtracted` is the last thing `extractWords()` sets, so on the render task it is the safer flag to read than `words.empty()`.
4. **`performLookup()`.** Right after `requestUpdateAndWait()` and the font-cache release (`:541-549`), the busy repaint has run `extractWords()` again. If `words` is still empty, the rebuild was refused and `extractWords()` set the error popup. Then: if `popup` is still `Busy`, set it to `None`; call `requestUpdate()`; return. That path is the dictionary switch (`switchDictionary` → `performLookup`), the only caller that can arrive with the arrays released.
5. **`appendDebugState()`.**
6. **`docs/serial-test-api.md`.** The STATE table's `sel`, `label` row now reads "menus, lists and the dictionary's word screen (the word under the cursor)".

Both arrays are rebuilt from the same page with the same metrics, so the rebuild returns the same indices. It allocates nothing that the first build did not.

- [ ] **Step 5: Build and flash**

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6`
Expected: `[SUCCESS]`.

- [ ] **Step 6: Measure again, with the same page and ballast**

Run Step 3's sequence, logging to `h1-branch.log`, with three changes:
- add `state` before the CONFIRM;
- add `state check:activity=DictionaryWordSelect` after the first BACK;
- add `cmd:HEAP:HOLD=<BALLAST>,4` if Step 3 needed it.

Expected:
- `grep DHTML` shows `Styled layout starting (... free=<f> contig=<c>)` and no `Low heap for styled definition`;
- the two STATE lines around the definition show the same `sel=` and `label=`;
- `grep "Rebuilt .* words in" h1-branch.log` gives one line. Record its ms: that is spec row 6.

- [ ] **Step 7: Apply the stop rule**

If Step 6 still logs `Low heap for styled definition` at `BALLAST`, H1 does not reach the gate. Then take H4:
- do not open PR 2;
- record both measurements for Task 9;
- continue Part C without merging this branch.

Otherwise H1 stays. The PR body records:
- free and contig at the definition's entry, before and after;
- the ballast;
- the rebuild time.

- [ ] **Step 8: Check a dictionary switch** (Review Focus 1)

The X4 has `en-de` and `cedict`, so the definition's Confirm switches between them.

```bash
python tools/devctl.py --port COM6 --log C:/tmp/cjkdict/h1-switch.log \
  "cmd:ACT:READER:<book>@<spine>:<page>" idle:60000:SCREEN \
  cmd:BTN:POWER idle:10000:SCREEN wait:1 state cmd:BTN:CONFIRM idle:15000:SCREEN state check:activity=DictionaryDefinition \
  cmd:BTN:CONFIRM idle:15000:SCREEN state cmd:BTN:BACK idle:10000:SCREEN state cmd:BTN:BACK idle:10000:SCREEN
```

Expected:
- the `state` after the second CONFIRM shows `activity=DictionaryDefinition`, or `activity=DictionaryWordSelect` with `popup=` (the other dictionary has no entry);
- every `state` answers, and no boot banner appears in the log;
- the last `state` shows the same `sel=` and `label=` as the first.

Afterwards, choose `cedict` again (Step 2's settings sequence).

- [ ] **Step 9: Commit**

```bash
git add src/activities/reader/DictionaryWordSelectActivity.h src/activities/reader/DictionaryWordSelectActivity.cpp docs/serial-test-api.md
git commit -m "feat(dictionary): free the word arrays while a definition is open, rebuild them on return"
```

- [ ] **Step 10: Run the PR gate, then open PR 2** (unless Step 7 dropped it)

---

## Part C: PR 3, the CJK lookup

Tasks 3-6 run on this branch now. Before Task 7, merge Part B (see the PR table).

### Task 3: The run of CJK text a lookup starts on

**Files:**
- Create: `src/util/DictionaryPunctuation.h`. Move in `EDGE_PUNCTUATION`, its `CodepointRange` struct and its comment and attribution, from `src/util/Dictionary.cpp:65-90`.
- Modify: `src/util/Dictionary.cpp:92-105`. `edgePunctuationLen` calls `isDictionaryEdgePunctuation`.
- Create: `src/util/CjkLookupRun.h`, `src/util/CjkLookupRun.cpp`
- Modify: `test/dictionary/CMakeLists.txt`. Add a `CjkLookupRunTest` executable: `CjkLookupRunTest.cpp`, `src/util/CjkLookupRun.cpp` and `lib/Utf8/Utf8.cpp`; include dirs `src/util` and `lib/Utf8`; `crosspoint_test_common`, `GTest::gtest_main`, `gtest_discover_tests`.
- Test: `test/dictionary/CjkLookupRunTest.cpp`

**Interfaces:**
- Produces:

```cpp
// DictionaryPunctuation.h -- inline, flash-resident table
// True for a codepoint cleanWord() strips from a word's edge: EDGE_PUNCTUATION or utf8IsDefaultIgnorable().
bool isDictionaryEdgePunctuation(uint32_t cp);

// CjkLookupRun.h -- ~80 bytes, a stack local of its callers; no heap
class CjkLookupRun {
 public:
  enum class Script : uint8_t { Other, Unspaced, Hangul };
  static constexpr uint8_t MAX_CODEPOINTS = 12;
  // The script of the first codepoint of `text` that cleanWord() keeps (leading ASCII
  // non-alphanumerics and edge punctuation skipped): Unspaced (utf8IsUnspacedCjk), Hangul
  // (utf8IsHangul) or Other, also when nothing is kept.
  static Script scriptOf(const char* text, size_t length);
  // Feeds the page's next word, in reading order. The first word fixes the script and may start
  // with edge punctuation, which is skipped. Unspaced: takes codepoints while they are unspaced CJK
  // and not edge punctuation, across words. Hangul: takes the first word's leading Hangul
  // codepoints and closes. Returns true while the run is open (feed the next word), false once a
  // stop codepoint, the 12-codepoint cap or the script closed it; the word that closes it may
  // still have contributed codepoints.
  bool append(const char* text, size_t length);
  Script script() const;
  const char* text() const;   // NUL-terminated
  uint8_t bytes() const;
  uint8_t codepoints() const;
  // Words (counted from the first appended, 1-based count) the first `runBytes` bytes of the run
  // come from; 0 when runBytes is 0 or not the end of a codepoint.
  uint8_t wordsCovering(uint8_t runBytes) const;
};
```

- [ ] **Step 1: Write the failing tests** in `test/dictionary/CjkLookupRunTest.cpp`

```cpp
using Script = CjkLookupRun::Script;
Script scriptOf(const std::string& s) { return CjkLookupRun::scriptOf(s.data(), s.size()); }
CjkLookupRun runOf(const std::vector<std::string>& words) {
  CjkLookupRun run;
  for (const auto& w : words) if (!run.append(w.data(), w.size())) break;
  return run;
}

TEST(CjkLookupRun, TheFirstKeptCodepointDecidesTheScript) {
  EXPECT_EQ(Script::Unspaced, scriptOf("大学"));
  EXPECT_EQ(Script::Unspaced, scriptOf("「大"));      // the opening bracket is skipped
  EXPECT_EQ(Script::Unspaced, scriptOf("コーヒー"));
  EXPECT_EQ(Script::Hangul, scriptOf("학교에서,"));
  EXPECT_EQ(Script::Other, scriptOf("house"));
  EXPECT_EQ(Script::Other, scriptOf("A大"));
  EXPECT_EQ(Script::Other, scriptOf("。"));
  EXPECT_EQ(Script::Other, scriptOf(""));
}

TEST(CjkLookupRun, RunsFollowTheSpecRules) {
  const struct { std::vector<std::string> words; const char* run; } cases[] = {
      {{"大", "学", "生", "。", "他"}, "大学生"},   // stops at 。
      {{"大学。", "他"}, "大学"},                   // punctuation glued inside a word stops it too
      {{"「大", "学"}, "大学"},                     // only the first word's leading bracket is skipped
      {{"大", "「学"}, "大"},
      {{"時", "々"}, "時々"}, {{"〆", "切"}, "〆切"}, {{"コ", "ー", "ヒ", "ー"}, "コーヒー"},
      {{"大", "A"}, "大"}, {{"大", "학"}, "大"}, {{"大", "\u3000", "学"}, "大"},
      {{"１", "９", "世", "紀"}, "１９世紀"},        // fullwidth digits are not punctuation
      {{"학교에서,"}, "학교에서"}, {{"학교", "에서"}, "학교"},  // Hangul: the token itself
  };
  for (const auto& c : cases) EXPECT_STREQ(runOf(c.words).text(), c.run) << c.run;
}

TEST(CjkLookupRun, TheRunStopsAtTwelveCodepoints) {
  const CjkLookupRun run = runOf(std::vector<std::string>(14, "中"));
  EXPECT_EQ(run.codepoints(), 12);
  EXPECT_EQ(run.bytes(), 36);
}

TEST(CjkLookupRun, AppendSaysWhenTheRunCloses) {
  CjkLookupRun run;
  EXPECT_TRUE(run.append("大", 3));
  EXPECT_FALSE(run.append("。", 3));
  CjkLookupRun hangul;
  EXPECT_FALSE(hangul.append("학교", 6));   // a Hangul run never takes a second word
}

TEST(CjkLookupRun, MatchLengthsMapBackToWords) {
  const CjkLookupRun run = runOf({"「大", "学", "生活"});
  EXPECT_STREQ(run.text(), "大学生活");
  EXPECT_EQ(run.wordsCovering(3), 1);
  EXPECT_EQ(run.wordsCovering(6), 2);
  EXPECT_EQ(run.wordsCovering(9), 3);
  EXPECT_EQ(run.wordsCovering(12), 3);
  EXPECT_EQ(run.wordsCovering(4), 0);   // not the end of a codepoint
  EXPECT_EQ(run.wordsCovering(0), 0);
}

TEST(CjkLookupRun, SupplementaryIdeographsCountAsOneCodepoint) {
  const CjkLookupRun run = runOf({"\U00020000", "\U00020001"});
  EXPECT_EQ(run.codepoints(), 2);
  EXPECT_EQ(run.bytes(), 8);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target CjkLookupRunTest`
Expected: a build error, `CjkLookupRun.h: No such file or directory`.

- [ ] **Step 3: Implement `DictionaryPunctuation.h`, then `CjkLookupRun`**

- `isDictionaryEdgePunctuation` is the body of today's range loop plus the `utf8IsDefaultIgnorable` check, made `inline`.
- `CjkLookupRun` keeps the run in `char text_[MAX_CODEPOINTS * 4 + 1]`, with two arrays: `uint8_t end_[MAX_CODEPOINTS]` (the byte offset just past each codepoint) and `uint8_t word_[MAX_CODEPOINTS]` (the index of the word each one came from). It decodes with `utf8NextCodepoint`.
- `scriptOf` and the first `append` skip what `cleanWord`'s leading loop skips (`Dictionary.cpp:763-771`): ASCII bytes that are not alphanumeric, and edge punctuation.

- [ ] **Step 4: Run the new tests, and the dictionary tests the move must not break**

Run: `cmake --build build/test --target CjkLookupRunTest DictionaryTest && ./build/test/dictionary/CjkLookupRunTest.exe && ./build/test/dictionary/DictionaryTest.exe --gtest_filter='DictionaryTest.CleanWord*:DictionaryTest.Guillemets*'`
Expected: PASS for both.

- [ ] **Step 5: Commit**

```bash
git add src/util/DictionaryPunctuation.h src/util/CjkLookupRun.h src/util/CjkLookupRun.cpp src/util/Dictionary.cpp test/dictionary/CjkLookupRunTest.cpp test/dictionary/CMakeLists.txt
git commit -m "feat(dictionary): the run of CJK text a lookup starts on"
```

### Task 4: The longest-prefix search

**Files:**
- Modify: `src/util/Dictionary.h`. Add the two structs next to `DictLocation` (`:17-25`), the public API after `readResolved` (`:117`), and private helpers and counters near `locate` (`:249`).
- Modify: `src/util/Dictionary.cpp`. The search goes after `resolve()` (`:907-1008`). `bisectSamples` (`.h:246-247`, `.cpp:493-544`) gains a trailing `bool strictlyBelow = false`: when true, it finds the last sample whose headword is strictly below the target (`asciiCaseCmp < 0`), and it neither reads nor records the window cache.
- Test: `test/dictionary/DictionaryTest.cpp`, two changes to its helpers:
  - In `writeDictionary` (`:114-145`), `std::sort` becomes `std::stable_sort`, so that duplicate headwords keep the order a test gives them.
  - `SynonymSpec` (`:95-98`) gains `int occurrence = 0`, which says which of several equal headwords the synonym points at. The ordinal lookup at `:130-134` counts the matches.

**Interfaces:**
- Produces:

```cpp
// One headword found at the start of a run of CJK text, with every .idx entry it has. StarDict's
// order puts entries with byte-equal headwords next to each other, so they are a run in the .idx:
// where it starts and how long it is are all a reader needs to read them all.
struct DictPrefixHit {
  DictLocation location;   // the run's FIRST entry, in .idx order
  uint8_t runBytes = 0;    // bytes of the run of text it covers, a codepoint boundary
  std::string headword;    // the .idx headword, also for a .syn hit; SSO up to 15 bytes (5 CJK chars)
  uint32_t idxStart = 0;   // byte offset in .idx of that first entry
  uint8_t entryCount = 0;  // entries in the run, at most DictPrefixMatch::MAX_ENTRIES_PER_HIT
};

// What Dictionary::resolvePrefixes() found. ~140 bytes; a member or stack local, never on the heap.
struct DictPrefixMatch {
  static constexpr uint8_t MAX_HITS = 3;
  static constexpr uint8_t MAX_ENTRIES_PER_HIT = 8;  // more same-headword entries are not listed
  DictPrefixHit hits[MAX_HITS];  // longest first
  uint8_t count = 0;
  bool aborted = false;   // gave up between probes because the AbortCheck said so: no verdict
  uint8_t probes = 0;     // window scans made, .idx and .syn together
  uint16_t reads = 0;     // file reads made: scan-buffer refills plus sidecar sample reads
};

class Dictionary {
 public:
  using AbortCheck = bool (*)();
  // The search of spec 5.2 for a lookup that starts on CJK text. `run` is searched as given: the
  // caller has cleaned it (CjkLookupRun). Up to MAX_HITS headwords that are prefixes of `run`,
  // longest first, at most one per length, each with its whole run of same-headword entries, each
  // run listed once. None of resolve()'s fallbacks.
  // abortCheck (optional) is asked before every probe after the first; when it says yes the
  // search returns false with matchOut.aborted set, count 0 and *outResult NotFound. outResult as
  // for resolve(): Found, NotFound, LowMemory (no scan buffers), ReadError (no verdict reached).
  bool resolvePrefixes(const char* run, DictPrefixMatch& matchOut, LookupResult* outResult = nullptr,
                       AbortCheck abortCheck = nullptr);
};
```

- [ ] **Step 1: Write the failing tests** in `DictionaryTest.cpp`

Add the helpers:
- `using HitList = std::vector<std::pair<std::string, int>>;`
- `HitList hitsOf(const DictPrefixMatch&)`;
- `std::string cp(uint32_t)`, through `utf8AppendCodepoint`;
- `std::string definitionAt(Dictionary&, const DictLocation&)`, through `readResolved`;
- `bool abortAlways() { return true; }`;
- the shared headword list `kBasic`: `{{"大","big"},{"大学","university"},{"大学生","student"},{"学","study"},{"学生","pupil"},{"生活","life"}}`.

Each test writes its own folder and opens it with `openIndexed` unless it says otherwise. The expected values:

| Test | Dictionary | Call | Expect |
|---|---|---|---|
| `PrefixSearchFindsTheLongestHeadwordAndTwoShorter` | `kBasic` | `resolvePrefixes("大学生活", m, &r)` | true; `r == Found`; `hitsOf(m) == HitList{{"大学生",9},{"大学",6},{"大",3}}`; `definitionAt(hits[0]) == "student"`; `m.probes == 1`; `m.reads > 0`; `!m.aborted` |
| `PrefixSearchKeepsTheThreeLongest` | `大`, `大学`, `大学生`, `大学生活` | `"大学生活动"` | `{{"大学生活",12},{"大学生",9},{"大学",6}}` |
| `PrefixSearchMissesWhenNoHeadwordStartsTheRun` | `kBasic` | `"龘龘"`, and `""` | false; `r == NotFound`; `m.count == 0`; `!m.aborted` |
| `PrefixSearchFindsMatchesBeforeTheWindow` | for each `leading` in {254, 255, 256} and `trailing` in {0, 300}: `leading` fillers `a000`…, then `大`, `大学`, then `trailing` fillers `"大学" + cp(0x4E00 + i)` | `"大学生活"` | `{{"大学",6},{"大",3}}`; `definitionAt(hits[0]) == "university"` (each folder in the `<<` message) |
| `PrefixSearchKeepsDescendingUntilItFindsTheLongestMatch` | `大`, then 300 entries each of `stem + cp(0x4E00 + i)` for the stems `大`, `大学`, `大学生`, `大学生活` | `"大学生活动"` | `{{"大",3}}`; `m.probes == 5`. Each probe lands one layer lower; a cap before the first hit would call this run absent |
| `PrefixSearchRespectsCodepointBoundaries` | `大`, and `std::string("大") + '\xE5'` (a truncated 学) | `"大学"` | `{{"大",3}}` |
| `PrefixSearchFindsTheOtherScriptThroughTheSynonyms` | idx `大`, `大学`; syn `大學`→`大学` | `"大學生"` | `{{"大学",6},{"大",3}}`; `definitionAt(hits[0]) == "university"` |
| `PrefixSearchPrefersTheIndexOnATie` | idx `大学` "university", `学校` "school"; syn `大学`→`学校` | `"大学生"` | `{{"大学",6}}`; `definitionAt(hits[0]) == "university"` |
| `PrefixSearchListsEachDefinitionOnce` | idx `大学`; syn `大学生`→`大学` | `"大学生活"` | `{{"大学",9}}`: the longer hit keeps the shared entry |
| `PrefixSearchCountsEveryEntryOfADuplicateHeadword` | `行` "xing: to walk", `行` "hang: a row", `行人` "pedestrian" | `"行人们"`, then `"行了"` | `{{"行人",6},{"行",3}}` with `hits[0].entryCount == 1`, `hits[1].entryCount == 2`, `definitionAt(hits[1]) == "xing: to walk"` (the first in `.idx` order); then `{{"行",3}}` with `entryCount == 2` |
| `PrefixSearchFindsADuplicateRunAcrossASample` | for each `leading` in {254, 255, 256}: `leading` fillers `a000`…, then `行` "xing: to walk", `行` "hang: a row". At 255 the sample at entry 256 lands on the second 行, so the run starts in the window before | `"行了"` | `{{"行",3}}`, `entryCount == 2`, `definitionAt(hits[0]) == "xing: to walk"` (each `leading` in the `<<` message) |
| `PrefixSearchListsTheWholeRunBehindASynonym` | idx `银` "silver", `银行` "bank", `银行` "row of silver shops"; syn `{"銀行", "银行", /*occurrence=*/1}`, pointing at the SECOND 银行 | `"銀行家"` | `{{"银行",6}}`, `entryCount == 2`, `definitionAt(hits[0]) == "bank"`: the run is listed from its first entry |
| `PrefixSearchOnHangulFindsTheNounBeforeItsParticle` | `학` "learning", `학교` "school" | `"학교에서"` | `{{"학교",6},{"학",3}}` |
| `PrefixSearchWorksBeforeTheIndexIsBuilt` | `kBasic`, `open()` only, with `needsIndex()` true | `"大学生活"` | the three hits of the first test |
| `PrefixSearchGivesUpBetweenProbesWhenAsked` | (a) the `leading=256, trailing=300` layout; (b) `kBasic` | `resolvePrefixes("大学生活", m, &r, &abortAlways)` | (a) false, `m.aborted`, `m.count == 0`, `m.probes == 1`; (b) true, `!m.aborted`, `m.count == 3`: one probe answers, so there is no "between" |
| `LatinDictionaryAnswersACjkRunInOneProbePerFile` | `provision("zipped", …)` (no `.syn`) and `provision("plain", …)` (`.syn`) | `"大学"` | false, `r == NotFound`; `m.probes == 1` for zipped and `2` for plain |

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build/test --target DictionaryTest`
Expected: a build error, `'class Dictionary' has no member named 'resolvePrefixes'` (and `DictPrefixMatch` not declared).

- [ ] **Step 3: Implement `resolvePrefixes`** in `Dictionary.cpp`

- Open a `LookupSession` as `resolve()` does, with the same LowMemory and ReadError mapping (`:922-929`).
- Count `reads` through the session's two `ScanReader`s (a counter bumped in `refill()`), and the sidecar sample reads in `bisectSamples` and `locateByOrdinal`.
- Give the `.syn` probes a `SampleWindow` of their own, as `.idx` has `session.idxWindow`.
- `PREFIX_PROBES_PER_FILE = 3` is a private `static constexpr`.
- The probe loop is the one decision the tests do not dictate:

```
collect(file, run):                      // file = .idx (offset+size suffix) or .syn (ordinal suffix)
  target = run; hits = {}; probesInFile = 0
  loop:
    if (probes of the whole search > 0 and abortCheck and abortCheck()) -> aborted
    start = bisectSamples(target, window)   // w0 = window.low, the headword at `start`
    scan from start while headword <= target (asciiCaseCmp):
      if headword is a prefix of run (asciiCaseCmp-folded bytes) ending on a codepoint boundary
         of run, and no hit of that length yet: record (length, suffix, headword, entry offset)
         -- a later prefix in a scan is longer; keep the MAX_HITS longest
      else if headword is byte-equal (strcmp) to the hit just recorded: entryCount++ (cap 8)
         -- same-headword entries are next to each other in StarDict's order
    probes++, probesInFile++
    if a hit's first entry was the window's first (headword == w0) and start != 0:
      its run may begin in the window before: locateRun(hit)       // one more probe
    stop when: hits.size() == MAX_HITS
               or (hits non-empty and probesInFile >= PREFIX_PROBES_PER_FILE)
               or start == 0                       // nothing sorts before the window
    limit = longest common prefix of run and w0, cut back to a codepoint boundary
    if hits non-empty: limit = min(limit, shortest hit length - 1), cut back to a boundary
    if limit == 0: stop
    target = run[0, limit)                // strictly shorter every time: <= 12 probes in all

locateRun(hit):                          // the .idx run of hit.headword, from its first entry
  start = bisectSamples(.qidx, hit.headword, strictlyBelow = true)
  scan from start to the first entry byte-equal to hit.headword: idxStart, location
  count the byte-equal entries that follow it (cap 8): entryCount

resolvePrefixes:
  idx = collect(.idx, run)
  if hasSyn and (idx is empty or its longest < strlen(run)):   // only a longer match could change the answer
     syn = collect(.syn, run)
     for each kept syn hit (after the scans): locateByOrdinal gives its .idx headword, then locateRun
        -- the ordinal may point into the middle of that headword's run
  merge longest first: one hit per length, the .idx hit on a tie; then drop a hit whose
  first entry (location) equals a longer kept hit's; keep MAX_HITS
```

The abort check runs before every probe after the first, `locateRun`'s included.

Why it is correct:
- Spec §5.2: every prefix of the run that sorts before `w0` is a prefix of `w0`, and is shorter than every hit already seen. The scan stops at `target` rather than at the window's end, so no window is scanned twice.
- A run of same-headword entries can begin before `start` only when its headword is `w0`. Every entry before `start` sorts below `w0`, so it is not byte-equal to any later headword.

- [ ] **Step 4: Run the whole binary** (the Latin tests must not move)

Run: `cmake --build build/test --target DictionaryTest && ./build/test/dictionary/DictionaryTest.exe`
Expected: PASS, every test.

- [ ] **Step 5: Commit**

```bash
git add src/util/Dictionary.h src/util/Dictionary.cpp test/dictionary/DictionaryTest.cpp
git commit -m "feat(dictionary): find the longest headwords that start a run of CJK text"
```

### Task 5: The definitions of a match, in one buffer

**Files:**
- Modify: `src/util/Dictionary.h`, `src/util/Dictionary.cpp:672-753`. Factor the file and dictzip read out of `readDefinition` into a private `bool readEntryInto(const DictLocation& location, uint32_t size, char* dst, LookupResult* outResult)`, which `readDefinition` and the new reader share.
- Test: `test/dictionary/DictionaryTest.cpp`. `writeDictionary` takes a fourth parameter, `const std::string& sametypesequence = "m"`, written into the `.ifo`.

**Interfaces:**
- Consumes: `DictPrefixMatch` (Task 4).
- Produces:

```cpp
// The definitions of match.hits[0..count), in one string sized once from the index sizes (one
// allocation, as readDefinition's). Every entry of every hit, in this order: hit by hit, longest
// first; within a hit, its entryCount entries read from .idx at idxStart, in .idx order. So the
// longest match's entries all come before the shorter matches. Plain: "headword\n" + definition
// per entry (a duplicate headword repeats its line), entries joined by "\n\n". HTML
// (definitionsAreHtml()): "<p><b>" + headword with & < > escaped + "</b></p>" + definition per
// entry, no separator. At most MAX_DEFINITION_BYTES in all: the very first entry's definition is
// cut to fit; every later entry is taken only whole, and at the first one that does not fit, it
// and everything after it are left out. Refused with LowMemory, like readDefinition, when
// ESP.getMaxAllocHeap() < total + 8 KB; a .dict.dz takes its inflate window first.
bool readPrefixDefinitions(const DictPrefixMatch& match, std::string& definitionOut,
                           LookupResult* outResult = nullptr);
```

The entries' locations are read from `.idx` twice, through the session's scan buffer: once to size the buffer, once to fill it. That is a few buffered reads per hit. Holding up to 24 locations instead would put 192 bytes on the stack.

- [ ] **Step 1: Write the failing tests**

| Test | Setup | Expect |
|---|---|---|
| `PrefixDefinitionsListEachEntryUnderItsHeadword` | `kBasic`, then `resolvePrefixes("大学生活")` | `"大学生\nstudent\n\n大学\nuniversity\n\n大\nbig"`, `r == Found` |
| `PrefixDefinitionsListEveryEntryOfTheLongestMatchFirst` | `银` "silver", `银行` "bank", `银行` "row of silver shops" (in that order); `resolvePrefixes("银行家")` | `"银行\nbank\n\n银行\nrow of silver shops\n\n银\nsilver"`: both entries in `.idx` order, then the shorter match |
| `PrefixDefinitionsFillTheBufferBeforeTheShorterMatches` | `银` "silver", `银行` `std::string(32768, 'a')`, `银行` `std::string(32748, 'b')`; `resolvePrefixes("银行家")` | `out.size() == 65532` (7 + 32768 + 2 + 7 + 32748); both 银行 entries are in it; `out.find("silver") == npos`, because the 12 bytes of 银's entry do not fit the 4 that are left |
| `HtmlPrefixDefinitionsBoldTheirEscapedHeadword` | `writeDictionary("DefsHtml", {{"大","<i>big</i>"},{"大学","<i>university</i>"},{"a<b&c","odd"}}, {}, "h")`; runs `"大学"` and `"a<b&c"` (`resolvePrefixes` searches any bytes it is given) | `"<p><b>大学</b></p><i>university</i><p><b>大</b></p><i>big</i>"`; then `"<p><b>a&lt;b&amp;c</b></p>odd"` |
| `PrefixDefinitionsStayWithinTheCap` | `大` "big", `大学` with `std::string(Dictionary::MAX_DEFINITION_BYTES, 'y')`; run `"大学"` | `out.size() == MAX_DEFINITION_BYTES`; `out.rfind("大学\n", 0) == 0`; `out.find("big") == npos` |
| `PrefixDefinitionsFromADictZipMatchThePlainOnes` | `provision("plain")` and `provision("zipped")`; in each, `resolvePrefixes` then `readPrefixDefinitions` for the runs `"quixotic"` and `"longword"` (headwords of the fixture; `longword` spans several dictzip chunks) | for each run, the plain and zipped outputs are equal and start with `"<run>\n" + expectedDefinition("<run>")` |
| `PrefixDefinitionsReportLowMemory` | `kBasic`, run `"大学生活"`; `ESP.setMaxAllocHeap(1024)` around the read, the old value restored | false, `r == LowMemory`, `out.empty()` |
| `PrefixDefinitionsOfNoHitsReadNothing` | an empty `DictPrefixMatch` | false, `r == NotFound` |

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build/test --target DictionaryTest`
Expected: a build error, `'class Dictionary' has no member named 'readPrefixDefinitions'`.

- [ ] **Step 3: Implement** `readEntryInto`, then `readPrefixDefinitions`, as the Interfaces block says. Do it in this order:
  1. open `.idx` (`openSession`); walk each hit's `entryCount` entries from `idxStart`; decide by the fill rule which entries are taken; compute the total from their sizes and the escaped headword lengths;
  2. call `dzScratch.ensure()` (for `.dict.dz` only);
  3. check the heap;
  4. `definitionOut.assign(total, '\0')`;
  5. walk the same entries again, and fill the buffer in place.

  On any read failure, clear `definitionOut` and return that read's result.

- [ ] **Step 4: Run the whole binary**

Run: `cmake --build build/test --target DictionaryTest && ./build/test/dictionary/DictionaryTest.exe`
Expected: PASS, every test.

- [ ] **Step 5: Commit**

```bash
git add src/util/Dictionary.h src/util/Dictionary.cpp test/dictionary/DictionaryTest.cpp
git commit -m "feat(dictionary): read a match's definitions into one buffer, each under its headword"
```

### Task 6: `CMD:ACT:DICT` reports the prefix search

**Files:**
- Modify: `src/SerialTestApi.cpp:346-387` (`lookUpWord`)
- Modify: `docs/serial-test-api.md:407-428` (Dictionary: `ACT:DICT`)

**Interfaces:**
- Consumes:
  - `CjkLookupRun::scriptOf`, `append` and `text` (Task 3);
  - `Dictionary::resolvePrefixes`, `readPrefixDefinitions`, `DictPrefixMatch` (Tasks 4 and 5).
- Produces the reply for a word whose script is not `Other`. The Latin reply is unchanged:
  ```
  DICT:result=<outcome> dict=<d> word=<w>[ headword=<hits[0]> bytes=<n>] run=<run> matched=<codepoints of hits[0]> hits=<h1,h2,h3> entries=<n1,n2,n3> probes=<n> reads=<n> ms=<t>[ exact_ms=<t>]
  ```
  - `matched=0` and empty `hits` and `entries` on a miss.
  - `entries` is each hit's `entryCount`.
  - `ms` times `resolvePrefixes`.
  - `exact_ms` times `resolve(hits[0].headword)` and is sent only on a hit. It needs the prefix search's headword, so it runs second: any cache warmth then favours the old path, and the ratio errs against the new one.
  - `bytes` is the length of `readPrefixDefinitions`'s output.
  - The search runs without an AbortCheck.
  - `logLookup` is given the run as its word.

- [ ] **Step 1: Write the expected replies into the doc first**, as the check Step 4 runs against: the reply grammar above, with the example `DICT:result=found dict=cedict word=大学生活 headword=大学生 bytes=<n> run=大学生活 matched=3 hits=大学生,大学,大 entries=1,1,1 probes=1 reads=<n> ms=<t> exact_ms=<t>`. Also say that a word starting with Han, kana or Hangul takes the prefix search, as the word screen does.

- [ ] **Step 2: Implement** the branch in `lookUpWord`:
  1. `scriptOf(text)`.
  2. For a CJK script, build a `CjkLookupRun` from `text` as one word.
  3. Run the timed `resolvePrefixes`, then (on a hit) the timed exact baseline, then `readPrefixDefinitions`.
  4. Count `matched` with `utf8NextCodepoint` over `hits[0].runBytes`.
  5. Join the headwords, and the entry counts, each with `,` (for `appendField`).

- [ ] **Step 3: Build and flash**

Run: `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8; "$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6`
Expected: `[SUCCESS]`.

- [ ] **Step 4: Check on the X4**, with `cedict` chosen (Task 2 Step 2)

Run: `MSYS_NO_PATHCONV=1 python tools/devctl.py --port COM6 --log C:/tmp/cjkdict/t6.log "cmd:ACT:DICT:大学生活" "cmd:ACT:DICT:龘龘" "cmd:ACT:DICT:學校" "cmd:ACT:DICT:行了"`

Expected:
- `大学生活`: `result=found`, `headword=大学生`, `matched=3`, `hits=大学生,大学,大`. If CC-CEDICT also carries `大学生活`, expect `matched=4` and `hits=大学生活,大学生,大学`.
- `龘龘`: `result=not_found matched=0`.
- `學校`: the traditional form. It is `found headword=学校` when PyGlossary put the traditional forms in `.syn`. Record what it shows: Task 9 documents it.
- `行了`: `hits` starts with `行`, and its `entries` value counts CC-CEDICT's 行 entries (xíng, háng, …). If it is 1, PyGlossary merged them into one entry, which then holds every reading. Record which, for Task 9.

Then switch to `en-de` (Task 2 Step 2's sequence, `"select:en-de"`) and run `"cmd:ACT:DICT:house"`. Expected: today's reply exactly, with no `run=`, `ms=` or other new field. Switch back to `cedict`.

- [ ] **Step 5: Commit**

```bash
git add src/SerialTestApi.cpp docs/serial-test-api.md
git commit -m "feat(serial): ACT:DICT reports the CJK prefix search: hits, probes, reads and time"
```

### Task 7: The word screen looks CJK text up by its longest match

**Files:**
- Modify: `src/activities/reader/DictionaryWordSelectActivity.h`: members after `:203` (`speculationHeadword`) and private helpers after `:128` (`speculateForSelection`)
- Modify: `src/activities/reader/DictionaryWordSelectActivity.cpp:451-486` (`speculateForSelection`), `:529-649` (`performLookup`), and `appendDebugState` (from Task 2)
- Modify: `src/activities/reader/DictionaryDefinitionActivity.h:17-28` (the comment on the pinned font) and `:39-46` (the constructor); `DictionaryDefinitionActivity.cpp:87, 140, 354, 388`
- Modify: `src/util/DictHtmlPages.h:25-26` (the comment "the dictionary pins its own font")
- Modify: `docs/serial-test-api.md` (STATE)

**Interfaces:**
- Consumes:
  - `CjkLookupRun` (Task 3);
  - `Dictionary::resolvePrefixes`, `readPrefixDefinitions`, `DictPrefixMatch`, `Dictionary::AbortCheck` (Tasks 4 and 5);
  - `releaseWordArrays()` and the rebuild (Task 2);
  - `CooperativeAbort::shouldAbortLongTask` (`lib/CooperativeAbort/CooperativeAbort.h`).
- Produces:

```cpp
// DictionaryWordSelectActivity, private
CjkLookupRun::Script scriptAt(int index) const;         // scriptOf over buildWordText(index)
void buildRun(int index, CjkLookupRun& run) const;      // words index.. fed until append() closes
// Noto Sans for a Latin lookup, the reader's own font (`fontId`) for one that started on CJK text
// (spec 5.3). The one place that chooses; roadmap step 6 replaces it.
int definitionFontId(CjkLookupRun::Script script) const;
DictPrefixMatch speculationMatch;   // the CJK speculation's hits for speculationIdx
uint8_t speculationSpan = 1;        // words the longest hit covers from speculationIdx; 1 otherwise

// DictionaryDefinitionActivity: new trailing parameter, stored as `const int bodyFontId`
explicit DictionaryDefinitionActivity(GfxRenderer&, MappedInputManager&, std::string headword,
    std::string definition, bool htmlDefinition = false, std::string dictionaryName = {},
    int fontId = DICTIONARY_FONT_ID);
// An empty headword draws the header with no title (GUI.drawHeader(..., nullptr)); the
// dictionary name and page counter stay where they are.
```

- STATE, inside `#if SERIAL_TEST_API`: Task 2's fields, plus `spec=unknown|found|missing`, `span=<speculationSpan, or 1>` and `match=<headword>`. The headword is `speculationMatch.hits[0]` for CJK and `speculationHeadword` otherwise, and is empty while unknown.

- [ ] **Step 1: Merge Part B** (see the PR table). Rerun `./build/test/dictionary/DictionaryTest.exe` after the merge. Expected: PASS.

- [ ] **Step 2: Give the definition screen a font**

1. Add the constructor parameter and `bodyFontId`.
2. Replace `DICTIONARY_FONT_ID` with `bodyFontId` at `.cpp:87, 140, 388`.
3. At `:354`, pass `headword.empty() ? nullptr : headword.c_str()`.
4. Update the two comments: Latin definitions are pinned to Noto Sans for its IPA, and a lookup that started on CJK text uses the reader's font.
5. Lay the definition out under `RenderLock`. `onEnter()` runs without it (`ActivityManager.cpp:408-409`), and `layoutHtmlPages()` and `wrapText()` measure with `bodyFontId`. A TrueType or CFF reader font may be measured only under the lock (TTF spec 3.4; `checkLock`, `TtfFont.cpp:69-75`). Take `RenderLock lock(*this);` in `onEnter()` around the two, as `FileBrowserActivity::onEnter` does (`FileBrowserActivity.cpp:143`).

- [ ] **Step 3: Send CJK text through the prefix search**

1. **`speculateForSelection`.** When `scriptAt(selected) != Other`:
   - `buildRun`, then `dict.resolvePrefixes(run.text(), speculationMatch, &result, &CooperativeAbort::shouldAbortLongTask)`;
   - on `aborted`, return without touching `speculationIdx`, so the next idle tick searches again;
   - otherwise set `speculationIdx`, set `speculation` as the Latin path does, and set `speculationSpan = run.wordsCovering(hits[0].runBytes)` on a hit (1 otherwise).
   - The Latin branch stays as it is.
2. **`performLookup`.** Decide `scriptAt(selected)` after the busy repaint and Task 2's empty-words return, because the rebuild happens there. For a CJK script:
   - take `speculationMatch` when the speculation is current, otherwise run `resolvePrefixes` with no AbortCheck and store the result as the speculation for `selected`, so that the span shows on return;
   - read with `readPrefixDefinitions`;
   - log with `logLookup(name, run.text(), outcome, hits[0].headword or null)`;
   - start the definition with an empty headword and `definitionFontId(script)`.

   The failure popups map as they do today.
3. **Speculation after an index build.** Where `performLookup` builds the index (`:553-556`), set `speculationDisabled = false` once the build succeeds. Otherwise a dictionary indexed in this session never speculates, and a CJK span could not grow until the screen is opened again.

- [ ] **Step 4: Add the STATE fields and their doc rows**

Add a STATE table row to `docs/serial-test-api.md`, directly after the `sel`, `label` row (the UI text fallback plan adds its rows after `heap`, `maxblock`): "`spec`, `span`, `match` | the dictionary's word screen | what the search found for the word under the cursor: `unknown`, `found` or `missing`; how many words the found headword covers (more than 1 only on CJK text); the headword (the longest, on CJK text)".

- [ ] **Step 5: Make the CJK test book**

Write a minimal EPUB 3 to `C:/tmp/cjkdict/dict_cjk.epub`, with a scratch script in `C:/tmp/cjkdict/` (not committed):
- `mimetype` stored first;
- `META-INF/container.xml`;
- `content.opf` with `dc:language` `zh` and one spine item;
- `nav.xhtml`;
- one chapter. Its `<p>`s, in order:
  1. `大学生活很有意思。我们在图书馆学习。`
  2. `吾輩は猫である。名前はまだ無い。コーヒーを飲む。`
  3. `학교에서 공부했다.`
  4. `Reading is a house of words.`
  5. `中华人民共和国国务院总理昨天会见了代表团` (a long run with no punctuation)

Upload it to `/test/dict_cjk.epub` as Task 2 Step 2 uploads files. Open it once with `cmd:ACT:READER:/test/dict_cjk.epub`.
- If STATE's `font=` is not a CJK family, run `cmd:GET:fontFamily` and write down the value.
- Then `cmd:SET:fontFamily=<n>`, stepping `n` up until `SET_OK`'s label names the X4's CJK family.
- Restore the noted value at the end of Task 9.

Also write `C:/tmp/cjkdict/first_word.txt`, a devctl script: 8 × (`cmd:BTN:UP`, `idle:3000:SCREEN`), then 30 × (`cmd:BTN:LEFT`, `idle:3000:SCREEN`). It puts the cursor on the page's first word.

- [ ] **Step 6: Build, flash, and check the lookup on the X4**

```bash
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6
MSYS_NO_PATHCONV=1 python tools/devctl.py --port COM6 --log C:/tmp/cjkdict/t7.log \
  "cmd:ACT:READER:/test/dict_cjk.epub@1:1" idle:60000:SCREEN cmd:BTN:POWER idle:10000:SCREEN
MSYS_NO_PATHCONV=1 python tools/devctl.py --port COM6 --script C:/tmp/cjkdict/first_word.txt
MSYS_NO_PATHCONV=1 python tools/devctl.py --port COM6 --log C:/tmp/cjkdict/t7.log wait:1 state \
  check:label=大 check:spec=found "check:match=大学生(活)?" "check:span=[34]" \
  cmd:BTN:CONFIRM idle:15000:SCREEN state check:activity=DictionaryDefinition shot:C:/tmp/cjkdict/t7-def.png \
  cmd:BTN:BACK idle:10000:SCREEN state check:activity=DictionaryWordSelect check:label=大 "check:span=[34]"
```

Expected:
- the checks pass, and `match` equals Task 6's `ACT:DICT:大学生活` headword;
- `t7-def.png` shows no title in the header, and a body that starts with the headword line, in readable Han with pinyin vowels and no boxes;
- `grep "Lookup dict=cedict" t7.log` shows `word="大学生活很有意思"` with `found headword=大学生`;
- `grep -c "without RenderLock" C:/tmp/cjkdict/t7.log` is 0 (Step 2 item 5).

- [ ] **Step 7: Check a dictionary switch on CJK text** (Review Focus 1)

Run the Step 6 sequence, but press `cmd:BTN:CONFIRM` again in the definition.

Expected:
- `en-de` answers the run with `popup=` (no entry), and nothing reboots;
- the log line reads `Lookup dict=en-de word="大学生活很有意思": not_found`.

Choose `cedict` again.

- [ ] **Step 8: Check responsiveness** (Review Focus 3, spec row 3)

From `label=大`, send 20 × `cmd:BTN:RIGHT` back to back, with no idle between them, then `idle:20000:SCREEN state`. Twenty presses cross paragraph 1's 17 words into paragraph 2, so at least one line end. Then do the same with `en-de` chosen: that is the baseline, where each word costs only the exact search.

Expected:
- `sel` is 20 more than before, with no press lost;
- the `IDLE:` time is within 1 s of the baseline's;
- after `wait:1`, `state` shows `spec=` for the new word, and its `match` starts with that word's character.

- [ ] **Step 9: Commit**

```bash
git add src/activities/reader/DictionaryWordSelectActivity.h src/activities/reader/DictionaryWordSelectActivity.cpp \
  src/activities/reader/DictionaryDefinitionActivity.h src/activities/reader/DictionaryDefinitionActivity.cpp \
  src/util/DictHtmlPages.h docs/serial-test-api.md
git commit -m "feat(dictionary): CJK text is looked up by its longest dictionary word, in the reader's font"
```

### Task 8: The highlight covers the match; a tap inside it looks it up

**Files:**
- Modify: `src/activities/reader/DictionaryWordSelectActivity.h:161`, plus `spanWordCount`.
- Modify: `src/activities/reader/DictionaryWordSelectActivity.cpp`:
  - `:392-416` (`consumeWordTouch`);
  - `:481-485` (the speculation's repaint);
  - `:730-818` (`drawHighlightWithSnapshot`);
  - `:876-884` (the fast path's prewarm).

**Interfaces:**
- Consumes: `speculationSpan` and `speculationIdx` (Task 7).
- Produces:
  - `int spanWordCount() const;` is `speculationSpan` when `speculationIdx == selected && speculation == Speculation::Found`, and 1 otherwise.
  - `drawHighlightWithSnapshot()` highlights words `[selected, selected + spanWordCount())`.
  - DBG `DICT` `Highlight over %u words takes the full repaint (%u rows)` whenever it returns false for want of runs or capacity.

- [ ] **Step 1: Repaint when the span grows**

In `speculateForSelection`, set `speculationRepaintPending = true` and call `requestUpdate()` for a Found speculation with `speculationSpan > 1`, as well as for Missing. This is the second refresh per cursor stop (§4), not a new one.

- [ ] **Step 2: Draw the span**

- The span's fragments are one contiguous range: `words[selected].firstFragment` up to the end of the last spanned word.
- A fragment's row is its word's `row`, plus 1 for a hyphen-joined word's tail (`hyphenFragment`).
- Each row gets one box, computed as today (`:749-779`).
- The first `MAX_SNAPSHOT_RUNS` boxes are saved while `SNAPSHOT_CAPACITY` holds them. A further row, or a box that does not fit, is still drawn, but the function returns false, so the next move takes the full repaint (§4).
- In the fast path, prewarm every fragment of the span rather than only `words[selected]`'s.

- [ ] **Step 3: Let a tap inside the span look it up**

In `consumeWordTouch`, an index in `(selected, selected + spanWordCount())` counts as `selected` before `ListRowTap::apply`. A tap elsewhere moves the cursor, as today (§5.1).

- [ ] **Step 4: Build and flash**

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload --upload-port COM6`
Expected: `[SUCCESS]`.

- [ ] **Step 5: Check the span at the default size**

Every devctl run in this task passes `--log C:/tmp/cjkdict/t8.log`.

Open the book, POWER, then `first_word.txt`, as in Task 7 Step 6. Then:
- `wait:1 state shot:C:/tmp/cjkdict/t8-span.png`;
- `cmd:BTN:RIGHT idle:5000:SCREEN shot:C:/tmp/cjkdict/t8-moved.png`;
- `wait:1 state shot:C:/tmp/cjkdict/t8-moved2.png`.

Expected (read the PNGs):
- `t8-span.png`: one inverted box over the `match` that STATE shows, 大学生 or 大学生活;
- `t8-moved.png`: 大 drawn plain again, and the box starting at 学;
- `t8-moved2.png`: the box over exactly the characters of the second STATE's `match` (学生, or 学 alone).
- `grep -c "takes the full repaint" C:/tmp/cjkdict/t8.log` is 0 at the default size (spec row 8: "the default size stays differential").

- [ ] **Step 6: Check the largest size and the long run** (Review Focus 2, spec row 8)

1. Run `cmd:GET:fontSize` and write down the value. Then `cmd:SET:fontSize=<n>`, stepping `n` up until `SET_ERROR:out of range`, and keep the last `SET_OK`.
2. Reopen the book and turn to the page that shows paragraph 5 (a screenshot tells). Open the word screen and bring the cursor to that paragraph's first character with `cmd:BTN:DOWN` and `cmd:BTN:LEFT`, an `idle:3000:SCREEN` after each, checking `state` until `label=中`. No other paragraph holds 中.
3. `wait:1 state shot:C:/tmp/cjkdict/t8-large.png`.
4. Press `cmd:BTN:RIGHT idle:5000:SCREEN shot:C:/tmp/cjkdict/t8-large-moved.png` three times.

Expected:
- `t8-large.png` shows every character of STATE's `match` (likely 中华人民共和国) highlighted, across the line end if the line breaks inside it;
- the `-moved` shots leave no inverted pixels where the earlier boxes were;
- `grep -c "takes the full repaint" C:/tmp/cjkdict/t8.log` now counts this step's fallbacks. Record the count for spec row 8.

Restore `fontSize`.

- [ ] **Step 7: Commit**

```bash
git add src/activities/reader/DictionaryWordSelectActivity.h src/activities/reader/DictionaryWordSelectActivity.cpp
git commit -m "feat(dictionary): the highlight covers the whole CJK match, and a tap inside it looks it up"
```

### Task 9: Measure on the X4, document, PR 3

**Files:**
- Modify: `docs/dictionary.md`. Add a section "Chinese, Japanese and Korean" after "How Lookup Works" (`:65-70`), and update the font paragraph `:83` and the "Where to find dictionaries" list `:87-99`.
- Modify: `docs/future_work/cjk.md`:
  - roadmap row 9 (`:32`) becomes `shipped (#<PR 1>, #<PR 2>, #<PR 3>)`;
  - a new "Already shipped" entry after the #554 (B1) entry (`:83-93`), before "Tests and books";
  - the section "Dictionary on CJK pages" (`:287-299`) keeps only what remains open.
  - The UI text fallback plan edits roadmap row 6 and "UI text in CJK" (`:239-286`) of the same file. Edit nothing else here, and find the rows and sections by their names once either plan has merged.
- Delete: `docs/superpowers/specs/2026-10-21-cjk-dictionary-design.md`. Fix every link to it that `git grep -n cjk-dictionary-design` finds, except this plan's own lines.

**Interfaces:** consumes everything above. Produces documentation and the PR.

- [ ] **Step 1: Japanese dictionary (spec rows 1 and 4).** Best effort; stop and record if a step fails.
  1. Download `http://ftp.edrdg.org/pub/Nihongo/JMdict_e.gz` into `C:/tmp/cjkdict/src/` and `gzip -dk` it.
  2. Run `C:/tmp/cjkdict/venv/Scripts/python -m pip install lxml fugashi unidic-lite`.
  3. Convert with `--read-format=JMDict --write-format=Stardict --write-options="dictzip=False"` into `C:/tmp/cjkdict/jmdict/`.
  4. Upload to `/dictionaries/jmdict/` (Task 2 Step 2). If the upload runs past 30 minutes, stop, record the sizes, and ask the maintainer to copy it by card reader.
- [ ] **Step 2: Sizes and first-open indexing (row 1).**
  1. `ls -l` each dictionary's files.
  2. On the X4: `cmd:FS:RM:/dictionaries/cedict/cedict.qidx`, and the `.sidx` if one exists.
  3. Then `"cmd:ACT:DICT:大学"` with `--log`.
  4. Record `Indexed <N> entries (<S> samples) from … in <M> ms` for `.idx` and for `.syn`. Do the same for JMdict.
- [ ] **Step 3: Accuracy (row 4).** This is dev tooling, not committed: `C:/tmp/cjkdict/fmm_compare.py`.
  1. Get the Hong Lou Meng book used in Task 2 with `serial_transfer.py get`, and the Soseki book (`/test/test_cjk_ja_soseki.epub`) the same way.
  2. Load the `.idx` and `.syn` headwords of a dictionary into a set.
  3. Segment each paragraph of one chapter by forward maximum matching, with the same run rules as `CjkLookupRun` (12 code points; stop at punctuation and non-CJK).
  4. Compare the word boundaries with `jieba.lcut` (Chinese) or `fugashi` (Japanese) inside the same runs.
  5. Print boundary precision, recall and F1, and write the segmented words to a file for Step 4.

  Expected: the three numbers, for Hong Lou Meng with CC-CEDICT, and for Soseki with JMdict if Step 1 worked. No target (§7).
- [ ] **Step 4: Search cost (row 2).**
  1. From Step 3's words of two or more characters, pick 50 per book. Each one becomes the run that starts at that word, up to 12 characters.
  2. Write them as `cmd:ACT:DICT:<run>` lines into a devctl script, and run it with `--log`.
  3. Compute the median of `ms / exact_ms` over the hits.

  Expected: at most 1.5, per book. This is a firm gate (the maintainer, 2026-10-24).
  - If a book's median is above it, PR 3 does not open. Find the cost in `probes` and `reads` (extra `locateRun` probes, the `.syn` pass), fix it in Task 4's code with a test, measure again, and tell the coordinator.
  - Record the median `ms`, `probes` and `reads`. The serial search is the Confirm search, and the speculative one is the same search, so one set of numbers covers both.
  - Do the same for JMdict over the Soseki chapter.
- [ ] **Step 5: Definitions in the reader's font (row 7) and Japanese on the X4.**
  1. On `dict_cjk.epub` with `cedict`, time each definition's opening: Confirm to the `IDLE` reply, from `devctl --log`'s timestamps. Record `cmd:TTF` before and after, for a TrueType family.
  2. If a `.cpfont` CJK family exists (Task 1 Step 7), repeat with it.
  3. Choose `jmdict` (Task 2 Step 2's settings sequence, with `"select:jmdict"`). Put the cursor on 吾 (paragraph 2), then `wait:1 state shot:C:/tmp/cjkdict/t9-ja.png`, then Confirm and `shot:C:/tmp/cjkdict/t9-ja-def.png`. Then Back, move the cursor to コ, and `wait:1 state`.
  4. Open `/test/test_cjk_ja_soseki.epub`, a book with ruby, and put the cursor on a base that has a reading (吾輩 if the page shows it). Then `wait:1 state shot:C:/tmp/cjkdict/t9-ruby.png`.

  Expected:
  - readable Han, kana and pinyin in the screenshots;
  - `コーヒー`'s run keeps its ー: STATE `match=コーヒー` with the cursor on コ;
  - on the ruby book (B1 is on master; see "Ruby B1 (merged in #554)"): STATE `label=` is the whole base group (吾輩), the run continues past it (`match` starts with the group, or `spec=missing` if the dictionary lacks it), and `t9-ruby.png` shows the box on the base characters, below the ruby, with the ruby drawn plain.
- [ ] **Step 6: Korean (optional).** If PyGlossary reads CC-KEDICT, convert it, put the cursor on 학 (paragraph 3), and `wait:1 state`. Expected: `match=학교`, `span=1`. Otherwise record "not checked".
- [ ] **Step 7: Span fallbacks (row 8).** Take the counts from Task 8 Steps 5 and 6. Expected: none at the default size.
- [ ] **Step 8: Size (row 10).**

  Run (background; it checks out commits in this worktree, so commit everything first):
  `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8 PATH="$HOME/.platformio/penv/Scripts:$PATH"; python scripts/firmware_size_history.py --commits origin/master HEAD --env gh_release`

  Expected: a flash delta of a few KB, and a static RAM delta of 0 to a few bytes. Record both.
- [ ] **Step 9: Latin regression (row 11, Review Focus 4).**
  1. With `en-de`, run `"cmd:ACT:DICT:house"`. Expected: today's reply.
  2. In the reader, open `/TEST/ePubTest.epub` (or any Latin book), POWER, `wait:1`, Confirm, `shot:C:/tmp/cjkdict/t9-latin.png`. Expected: the headword in the header, the body in Noto Sans as before.
  3. With `cedict` on paragraph 4 of `dict_cjk.epub`: STATE `spec=missing` after `wait:1`.
- [ ] **Step 10: Write the user docs.** In `docs/dictionary.md`:
  - **How it works:** put the cursor on a word's first character; after a moment the highlight grows over the longest word; Confirm shows every entry the dictionary has for that word (all of CC-CEDICT's readings of 行), then up to two shorter words, as far as the 64 KB definition limit allows; in your reading font, each entry under its word; a tap inside the highlight looks it up. Record whether the PyGlossary conversion keeps same-word entries apart or merges them (Task 6 Step 4).
  - **What it does not do:** no deinflection (食べた finds 食); simplified and traditional only as the dictionary carries them (`.syn`).
  - **The sources table** from spec §5.4, with the verified PyGlossary commands from Task 2 Step 1 and this task's Step 1. Recommend the plain `.dict`.
  - **The measured sizes and first-open times** from Step 2.
  - **The font paragraph:** Latin definitions keep Noto Sans; definitions of CJK text use the reading font.
- [ ] **Step 11: Update the roadmap.** In `docs/future_work/cjk.md`:
  - row 9 becomes shipped;
  - a new "Already shipped" entry, in the format of `:56-59`;
  - the "Dictionary on CJK pages" section keeps what remains open, with the measurements behind it:
    - deinflection, and which JMdict `.syn` builds carry inflected forms;
    - moving the cursor word by word, and a manual range;
    - greedy over-long matches, with Step 3's F1;
    - the X3 dense-page measurement and the touch check on the X4 Pro and T5S3 (spec row 12): not run, by the maintainer's decision; the X4 with ballast stood in for the X3;
    - if Task 2 dropped H1: H4, with the numbers.
- [ ] **Step 12: Delete the spec and fix its links.**

  Run: `git rm docs/superpowers/specs/2026-10-21-cjk-dictionary-design.md && git grep -n "cjk-dictionary-design"`
  Expected: only this plan's own lines remain (its Spec line, and Task 9's Files list and this step). The UI text fallback plan cites this plan, not the spec.
- [ ] **Step 13: Commit.** Restore every setting this plan changed: `fontFamily`, `fontSize`, `sleepTimeoutMinutes`, and the chosen dictionary.

```bash
git add docs/dictionary.md docs/future_work/cjk.md
git commit -m "docs: CJK dictionary lookups: how they work, where to get dictionaries, what remains"
```

- [ ] **Step 14: Run the PR gate, then open PR 3.** The body carries:
  - the measurements of spec rows 1-4, 7, 8, 10 and 11, with row 2 against the firm 1.5× gate;
  - the stop-rule outcome from Task 2;
  - "row 12 (touch) not run, and no X3 run: the maintainer's decision; the X4 with heap ballast stood in for the X3".

---

## Spec coverage

| Spec | Task |
|---|---|
| §5.7 L1 (decision 6, PR 1) | 1 |
| §5.6 H1 and the stop rule, rows 5 and 6 | 2 |
| §5.1 run rules, script choice, cap, kept letters | 3 |
| §5.2 C2 search, step 3, `.syn` pass, codepoint boundaries, no fallbacks, abort between probes, Latin dictionary cost; every same-headword entry (the maintainer's answer) | 4 |
| §5.3 shorter matches, entry formats, one buffer within `MAX_DEFINITION_BYTES`; the longest match's entries before the shorter matches | 5 |
| §5.2 `ACT:DICT` reports length, hits, reads and time | 6 |
| §5.1 speculation and Confirm; §5.3 the reader's font and the headword in the body | 7 |
| §5.1 span highlight and tap; §4 snapshot limits | 8 |
| §5.4 sources and PyGlossary; §7 rows 1-4, 7, 8, 10, 11; §6 docs; roadmap | 9 |
| §5.5 no deinflection | nothing to build; documented in 9 |
| §7 row 12 (touch), X3 rows | not run, by the maintainer's decision; recorded in Task 9 Steps 11 and 14 |
