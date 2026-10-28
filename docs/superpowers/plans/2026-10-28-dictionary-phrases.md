# Dictionary Phrases Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Confirm on a word of Latin-script text that stands in a phrase shows the phrase first: the longest phrase headword around the word (A), otherwise the word's own article opened at the bold idiom that matches the text and marked there (B), after following a cross-reference stub once (C).

**Architecture:**
- Pure pieces, host-tested with no storage: `PhraseContext` (the words around the tapped word, spec 5.1); `ArticlePhrase` (B's reading and matching of bold runs, C's stub rule and their one notation table, and the definition screen's `PhraseWordFinder`); `cutDictionaryHtml` (cutting an HTML entry so that it still lays out).
- `Dictionary` gains a phrase search over one lookup session (`resolvePhrase`), an exact lookup, a read with a lead, and a fill rule for `readPrefixDefinitions` that cuts the word's entry for room. `lookUpInContext` (`PhraseLookup`) runs A, then C and B. The word screen and `CMD:ACT:DICT` both call it.
- The word screen builds the context at Confirm, before Part B frees its arrays. The definition screen opens on the page of the matched idiom and draws it white on black.

**Tech Stack:**
- C++20 firmware (`-std=gnu++2a`, ESP32-C3, PlatformIO, `-fno-exceptions`).
- GoogleTest host tests under `test/` (CMake, Ninja, MSYS2 UCRT64).
- `tools/devctl.py` over the serial test API, on the X4.

**Spec:** `docs/superpowers/specs/2026-10-28-dictionary-phrases-design.md`. Every "(my call)" in it is decided. Task 11 deletes the spec and this plan once their lasting parts are in `docs/dictionary.md`.

**Builds on** (merged into master before Task 1; this plan cites them as they are on their branches):
- CJK dictionary step 9, `feat/cjk-dictionary` at `1fccfc02f` (Tasks 1-5 of `docs/superpowers/plans/2026-10-23-cjk-dictionary.md` done, Tasks 6-9 to come): `Dictionary::resolvePrefixes`, `DictPrefixMatch`, `readPrefixDefinitions`, `MAX_PREFIX_RUN_BYTES`, `CjkLookupRun`, `isDictionaryEdgePunctuation`; after its Task 7, the word screen's `scriptAt()`, `definitionFontId()` and the definition screen's `fontId` parameter; after its Task 6, the CJK branch of `lookUpWord` in `SerialTestApi.cpp`.
- Part B, `feat/dict-release-word-arrays` at `9d2906103`: `DictionaryWordSelectActivity::releaseWordArrays()`, the rebuild in `extractWords()`, `appendDebugState()`.

## Global Constraints

**From the spec, verbatim:**
- §3 anti-goals: "Code that branches on the book's or the dictionary's language." "A Latin lookup that becomes noticeably slower, or a cursor move that does more card reads than today." "Steady-state RAM, or transient RAM beyond the existing lookup session and the definition buffer." "Losing the word's own definition: the phrase comes first, the word stays reachable."
- §3 non-goals: "Japanese, Chinese and Korean: step 9 covers them; this design does not change a CJK lookup." "A new dictionary format, a converter, shipped dictionaries, a new setting."
- §4: "No steady-state RAM; transient RAM within the lookup session plus a few bytes; stack locals under 256 bytes per function; no allocation in a scan loop. The definition buffer (`MAX_DEFINITION_BYTES`, 64 KB cap) is the only large buffer, and B and C work on it in place."
- §4, speed gate: "the time from Confirm to the definition's first page on screen. Its median with phrases on stays within 1.5x master's on the X4, over about 50 words from two English books (about half inside phrases the dictionaries hold, misses included), with Oxford and with PONS, timed from the log." The books: "a generated, phrase-heavy test book, and `/test/Alice.epub` from the X4's card".
- §4, flash: "at most 4.5 KB in `gh_release` for the whole feature (A, B, C and the two screens), measured from the map file … more needs the maintainer's yes."
- §4: "The first-hit behaviour of every existing lookup path stays as it is when nothing phrase-like is found: today's host tests stay green."
- §5.1: "It is built only when the lookup runs (Confirm), not while the cursor moves." §5.5: "The word screen's highlight and speculation do not change for Latin text".
- §5.3: "The placeholder and optional-word lists are one small constant table: dictionary notation, not per-language code. New notations are added there." §5.4: "The cues live in the same constant table as B's placeholders".
- §7: test dictionaries are "written by the tests (as `DictionaryTest`'s `writeDictionary` does), using **made-up** entries that copy only the two markup styles of section 2, never real dictionary text".

**Project rules:**
- **Host tests** from Git Bash only, never PowerShell. Configure each worktree once, from its root:
  `cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1 -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=C:/_development/witchhunt-reader/build/test/_deps/googletest-src -DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=C:/tmp/wt-serial-api/build/test/_deps/arduinojson-src`
  Build with `cmake --build build/test -j 8 -- -k 0` (`epub_build_inventory` does not build on Windows).
- **Firmware:** `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8`, then `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`, in the background (a cold build exceeds 600 s), one `pio run` at a time. Build `default` and `gh_release` before the PR; leave the S3 environments to CI. Format with clang-format 21 (`/c/tmp/cf21/clang_format/data/bin/clang-format.exe`; reinstall with `python -m pip install --target C:/tmp/cf21 "clang-format>=21,<22"`).
- **Device:** the X4 on **COM5**, the only device. Every `devctl.py` / `serial_transfer.py` call is prefixed `MSYS_NO_PATHCONV=1` and passes `--port COM5`. The X4's UI is German: a session starts with `cmd:GET:language` and `cmd:SET:language=EN`, `cmd:GET:sleepTimeoutMinutes` and `cmd:SET:sleepTimeoutMinutes=30`, and restores both at its end. Never send `SLEEP` or `BTN:POWER:LONG`. Keep a `CMD:` line under 64 bytes. Logs, scripts and screenshots go to `C:/tmp/dictphr/`. The maintainer's dictionaries are on the card as `/dictionaries/oxford` (Oxford, English-English) and `/dictionaries/en-de` (PONS, English-German).
- **The maintainer's real dictionaries** (`C:/tmp/dictrecon`) may be read by scratch scripts to check a rule's effect. Nothing from them is committed or quoted beyond a few words.
- **Heap:** no bare `new`; `makeUniqueNoThrow` for fallible allocations; `heapHasBlockFor` or `ESP.getMaxAllocHeap()` before a large allocation; a size note on every new one.
- **Layering:** `Dictionary` knows nothing of `MappedInputManager` or of phrase notation (that is `ArticlePhrase`'s). No `std::function`. No new setting, no new user-facing string, no section or cache version change.

## Review Focus

1. **Text as books set it around the tapped word:** curly quotes, a comma or a closing bracket against it, an apostrophe inside it, an all-caps heading, a sentence-initial capital outside ASCII. Expected: the marks go, the apostrophe stays, and case never stops a match. Pinned by Task 3 `ContextStripsMarksAndKeepsApostrophes`, Task 4 `PhraseSearchFindsACapitalisedPhrase` and Task 6 `ArticlePhraseIgnoresCase`.
2. **A tapped word at the edge of the page** (the first or last word, the phrase running on to the other page). Expected: the lookup uses the words the page has; with no neighbour it is today's lookup, and the phrase search is not run at all. Pinned by Task 3 `ContextWithoutNeighboursIsTheWordAlone`, Task 4 `PhraseIsMadeOfWholeWordsAndAtLeastTwo` (`probes == 0`) and Task 11 Step 5.
3. **Switching dictionaries from the definition screen**, after Part B freed the word arrays. Expected: the other dictionary's result (phrase, marked idiom, word, or "Not found") from a context rebuilt out of the rebuilt arrays; never the previous dictionary's mark. Pinned by Task 11 Step 5.
4. **Low heap when a combined definition is read** (phrase plus word, or stub plus followed entry). Expected: the word's own entry (or the stub) as today, otherwise the low-memory popup; never an abort. Pinned by Task 8 `LowHeapForThePhraseShowsTheWord` and `LowHeapForTheFollowedEntryShowsTheStub`.
5. **The idiom near a page break of the definition, or not findable on the laid-out pages** (bionic reading on, a hyphenated line break inside it). Expected: the screen opens on the page where the idiom starts; when its words cannot be found, on page 1 with nothing marked, never on a wrong page. Pinned by Task 9 `FinderSpansPages`, `FinderJoinsBionicPieces` and `FinderReportsNothingRatherThanAGuess`.

## Rulings on the spec

Decided while planning; a reviewer holds each task to them. Each names what it costs if it is wrong.

1. **Paragraph ends** (5.1 against 7). The context crosses them, as 5.1's "(my call)" says. Section 7's "paragraph ends not" is read as "a paragraph end stops the context only through its punctuation". If wrong: none beyond the heading case the spec accepts.
2. **Sentence punctuation** is judged on the side of the word facing the tapped word. There, it leaves the word out; on the far side, the word is taken without the mark and nothing beyond it. The tapped word's own trailing mark closes the side after it, its leading mark the side before. If wrong: one word more or less in a context.
3. **A hit of A must end at a word end of the context.** "look up" inside "look upwards" does not count. If wrong: none; 5.2's coverage rule is about whole words.
4. **Case for A.** The index is ordered by `asciiCaseCmp`, and the prefix search folds ASCII case, so one search covers ASCII capitals. A second search is made only when the run's first codepoint is a non-ASCII capital (`utf8LowerCodepoint` changes it) whose lowercase has the same byte length. That covers Latin-1, Greek and Cyrillic; `ẞ` is not retried. If wrong: an extra descent on such capitals.
5. **Pruning.** No probe targets, and no hit records, a run shorter than the shortest hit that could count (`minBytes`: from the start to the tapped word's end, plus two bytes when the start is the tapped word). The results are the same; only short-prefix descents go. This is not the spec's speed fallback, which stays available (Task 11 Step 7). If wrong: none.
6. **Ties of A.** "Longest" is in bytes; a tie goes to the earlier start, then to the context as written over the base-form one.
7. **The base form** (spec 5.2 as amended):
   - `resolve()`'s headword, when it differs from the tapped word under ASCII case folding and has no space;
   - otherwise, when the word's own entry is at most 2 KB and is a stub (5.4), the headword its target resolves to.
   
   That holds for any cue, not only an inflection's (*favor* → *favour* as *looked* → *look*), so the table needs no kind column. A runs on a miss only on a NotFound verdict; a read failure keeps today's error popup. If wrong: a few more descents on spelling-variant stubs.
8. **The word's entry in A's definition** is the one entry today's lookup finds. `DictLocation` gains the `.idx` offset of its entry (`idxEntry`), so that entry reads like a prefix hit. Cost: 4 bytes in each `DictLocation`.
9. **The cut for room.** An HTML entry is cut where no tag, entity or UTF-8 sequence is split, and the elements still open are closed. Both dictionaries wrap every article in one element, so a byte cut would fail to parse and drop to plain text. The 16 KB styled cap applies to `sametypesequence=h` only, the one format the styled view lays out; everything else is cut to 64 KB. A cut entry that would keep fewer than 64 bytes of its definition is left out. If wrong: about 350 bytes of flash.
10. **B's bold runs are read nesting-aware.** Oxford nests the alternative's `<b>` inside the phrase's ("by (or from) all accounts"); the run ends at the `</b>` that closes the outer one. 5.3's unclosed-bracket case stays handled.
11. **Brackets without "or"** ("[with sb]", "(good)", which PONS and Oxford put inside phrases) mark optional words. A matched optional word counts as literal; a placeholder never does.
12. **The headword in a phrase.** `∼` and `~` are replaced by the headword inside a token ("∼s" reads "accounts"). The tapped word also matches any token equal to the headword, so *looked* matches Oxford's "look" and *favor* matches PONS's "∼" after C sends it to *favour*.
13. **Bounds on B.** A bold run is read up to 128 bytes, a reading has at most 12 tokens and 4 placeholders, and at most two "or" groups give readings. A longer run is read as far as it goes, its last partial token dropped. If wrong: an over-long run is matched on its first part only.
14. **Markup.** B and C run when `sametypesequence` is a single `h`, `x` or `g` (`Dictionary::definitionsHaveMarkup()`, new), as 5.3 says. The styled view stays `h` only.
15. **C, as measured with the amended rule** (spec 5.4's table: 2,488 Oxford and 38,687 PONS stubs, 37,566 of those the italic "↑" kind):
    - The 256-byte text limit stays. The Oxford entries between 256 and 384 bytes are full articles.
    - Oxford writes its targets in capitals ("another term for FORESTS"). The exact lookup is `locate()` over `.idx`, which folds ASCII case as the index does (no `.syn`, no stems).
    - The `<sup>` homograph number is dropped from the target. Cues are compared ASCII case-insensitively, and a word cue must start a word ("foresee" is no "see").
    - A target that names the stub's own entry is not followed. The header names the followed headword.
    - The 336 short Oxford articles whose etymology ends in a cue (spec §9) are followed as the spec's rule says (question 1 below).
16. **The mark** is the phrase's words drawn white on a black box, as the word screen highlights a word. PONS has 6,387 underlined runs of its own, so an underline would not single the phrase out.
    - Styled view: the page where the run's words first appear in order, every lettered piece bold.
    - Plain view: the line where the converter's tracked offset of the run lands.
    - When the run is not found, page 1 with nothing marked.
17. **`CMD:ACT:DICT` gains a context form**, `ACT:DICT:<words with the tapped one in [brackets]>`, in dev builds only (`SERIAL_TEST_API`), so it is outside the flash budget. It lets the device run check A, B and C on the real dictionaries without moving the cursor. The bare-word reply does not change.
18. **Timing lines** are DBG and compiled out of release builds. The speed baseline is this branch at Task 2: master's behaviour plus those lines.
19. **Flash** is measured on `gh_release` against Task 1's map. Past 4,500 bytes, the plan stops for the maintainer's yes.
20. **The test book** is committed in `test/epubs` with its golden, because the pipeline corpus scans that directory.
21. **Where the context lives** (spec §8). `PhraseContext` is a class of its own beside `CjkLookupRun`: it builds by words and in both directions, the run forward and by codepoints. The two share the edge rule (`isDictionaryEdgePunctuation`, through `dictionaryWordBounds`) and the 48-byte limit. If wrong: a merge later, at no cost now.

## The maintainer's answers (2026-10-28), now in the spec

- PONS's inflected forms: an italic target after "↑" is a stub target, and a stub's target is A's base form (spec 5.2, 5.4).
- Oxford's inflection cues are in the table (5.4).
- Flash: up to 4.5 KB; nothing is trimmed (4).
- The gate's books: the test book and `/test/Alice.epub` (4).

**Open question 1.** 336 of Oxford's stub matches are short full articles whose etymology ends in a cue, "(see X)" or "short for X". In 167 of them the cue sits inside an open bracket. Should a cue inside an open bracket not count? That is one generic check, and 167 fewer false stubs. Until answered, the spec's rule stands.

## Branch, PR and gates

- Branch `feat/dictionary-phrases` from `origin/master`, worktree `C:/tmp/wt-dictphr` (Task 1). One PR: `feat(dictionary): look a word up in its phrase`.
- **The PR gate** (Task 11), in Git Bash from the worktree:
  1. `git fetch origin && git merge origin/master`, then the whole suite: `cmake --build build/test -j 8 -- -k 0` and `ctest --test-dir build/test -j 8 2>&1 | tail -25`. Expected: only the failures Task 1 recorded as known.
  2. `default` and `gh_release` build, each log ending in `[SUCCESS]`.
  3. `git diff --name-only --diff-filter=d origin/master... -- '*.cpp' '*.h' | xargs -r /c/tmp/cf21/clang_format/data/bin/clang-format.exe -i`, then `git status --short`. Expected: no change, or one `style: clang-format 21` commit.
  4. `gh pr list --head feat/dictionary-phrases --state all` shows no merged PR for the branch. Push, then `gh pr create --base master --title "feat(dictionary): look a word up in its phrase" --body-file C:/tmp/dictphr/pr.md`.

## File map

Line numbers are those of the two branches named above. Task 1 maps them to master, and once a task has changed a file, functions are found by name.

| File | Task | What |
|---|---|---|
| `src/activities/reader/DictionaryWordSelectActivity.{h,cpp}` | 2, 10 | timing lines; the context at Confirm; `lookUpInContext` in the Latin branch |
| `src/activities/reader/DictionaryDefinitionActivity.{h,cpp}` | 2, 9 | timing line; start page, the mark, STATE |
| `test/epubs/make_test_dict_phrases.py`, `test_dict_phrases.epub`, `test/epub_pipeline/goldens/test_dict_phrases.golden.txt` (new) | 2 | the test book |
| `src/util/DictionaryPunctuation.h` | 3 | `dictionaryWordBounds`, shared by `cleanWord`, the context and B |
| `src/util/PhraseContext.{h,cpp}`, `test/dictionary/PhraseContextTest.cpp` (new) | 3 | spec 5.1 |
| `src/util/Dictionary.{h,cpp}`, `test/dictionary/DictionaryTest.cpp` | 3-5, 7, 8 | `resolvePhrase`, `idxEntry`, styled-cap fill, `resolveExact`, the lead, `definitionsHaveMarkup` |
| `src/util/DictHtmlNormalize.{h,cpp}`, `test/dictionary/DictHtmlNormalizeTest.cpp`, `lib/SaxParser/SaxParser{.h,Yxml.cpp}` | 5 | `cutDictionaryHtml`; `SaxParser::isHtmlVoidElement` made public |
| `src/util/HtmlToPlainText.{h,cpp}`, `test/dictionary/HtmlToPlainTextTest.cpp` | 6 | a char-buffer core with a tracked offset |
| `src/util/ArticlePhrase.{h,cpp}`, `test/dictionary/ArticlePhraseTest.cpp` (new) | 6, 7, 9 | B, C, the notation table, `PhraseWordFinder` |
| `src/util/PhraseLookup.{h,cpp}` (new) | 8 | A then C then B |
| `test/dictionary/CMakeLists.txt` | 3-9 | two new executables; `DictionaryTest`'s new sources |
| `src/SerialTestApi.cpp`, `docs/serial-test-api.md` | 9, 10 | `ACT:DICT` context form; definition-screen STATE |
| `docs/dictionary.md`; the spec and this plan deleted | 11 | user docs |

---

### Task 1: Pre-flight against master

**Files:** none in the repo. Findings go to `C:/tmp/dictphr/ledger.md`, which is not committed.

**Interfaces:**
- Produces: the worktree; a ledger that maps every symbol this plan cites to its master location (or its new name); the known host-test failures; the `gh_release` base size and map.

- [ ] **Step 1: Check that the work this plan builds on has merged.**
  Run: `git -C C:/_development/witchhunt-reader fetch origin && git -C C:/_development/witchhunt-reader log origin/master --oneline | grep -E "from jpirnay/feat/(cjk-dictionary|dict-release-word-arrays)$"`
  Expected: both merges are listed. If step 9's is missing, stop: this plan is not to be executed yet. If only Part B's is missing, check the CJK plan's ledger: when its stop rule dropped Part B, there is no `releaseWordArrays()`, so the ordering this plan keeps before it is moot. Record that and go on.

- [ ] **Step 2: Create the worktree.**
  ```bash
  git -C C:/_development/witchhunt-reader worktree add C:/tmp/wt-dictphr -b feat/dictionary-phrases origin/master
  git -C C:/tmp/wt-dictphr submodule update --init freeink-sdk
  mkdir -p /c/tmp/dictphr
  ```
  If master lacks this plan or its spec, check both out from `docs/dictionary-phrases` and commit them: `docs: the dictionary phrases spec and plan`.

- [ ] **Step 3: Map the cited symbols.** `git grep -n` every existing symbol a task's **Files** block names, and write its file and line into the ledger. Note in particular step 9's `scriptAt` split in `performLookup`, the definition screen's `fontId` parameter, `lookUpWord`'s CJK branch and the STATE rows `sel, label` and `spec, span, match`. A renamed symbol is recorded with its new name, and every later task uses that name. If one is gone with no successor, stop and report.

- [ ] **Step 4: Configure and run the whole host suite** (Global Constraints). Record each failing test as known. Expected: only Windows-only failures (precedent: two `FirstChapter`, two `LongChapterPageTables`, `KeptReadingState`).

- [ ] **Step 5: Record the flash base.** Build `gh_release` in the background, then `cp .pio/build/gh_release/firmware.map C:/tmp/dictphr/base-gh_release.map` and copy the `Flash:` line into the ledger.

- [ ] **Step 6: Check the device.** Run `python tools/devctl.py --port COM5 cmd:FS:LS:/dictionaries cmd:FS:LS:/test cmd:GET:language cmd:GET:btnShortPower`. Expected: `oxford`, `en-de` and `/test/Alice.epub` are listed. Record the language, and whether Power opens the word screen (otherwise open it from the reader menu, `cmd:BTN:CONFIRM "select:Dictionary" cmd:BTN:CONFIRM`).

### Task 2: Timing lines, the test book, and the baseline on the X4

**Files:**
- Modify: `src/activities/reader/DictionaryWordSelectActivity.cpp`: the top of `performLookup` (h1 `:558`), and `render`'s popup branch (`:955-963`)
- Modify: `src/activities/reader/DictionaryDefinitionActivity.{h,cpp}`: `render` (`:347-403`), plus a `bool announced = false;` member
- Create: `test/epubs/make_test_dict_phrases.py`, `test/epubs/test_dict_phrases.epub`, `test/epub_pipeline/goldens/test_dict_phrases.golden.txt`

**Interfaces:**
- Produces three DBG `DICT` lines, with this exact text (Task 11's gate script parses them):
  - `Confirm: lookup starts`: first statement of `performLookup()`. That function is also the dictionary-switch path, which arrives with the arrays released, so the line names no word.
  - `Lookup popup on screen`: after `GUI.drawPopup` in `render()`, for the NotFound and Error popups only.
  - `Definition on screen (page %d/%d)`: after `displayBuffer()` in the definition screen's first `render()`.
- Produces `/test/test_dict_phrases.epub`, one spine item. An `<h2>IN FAVOUR OF THE MOTION</h2>` comes first, then one `<p>` each:
  1. `The vote was close.`
  2. `The council voted in favor of the motion.`
  3. `They voted in favour of the plan.`
  4. `In favour of the plan, he spoke at length.`
  5. `She took it into account.`
  6. `She looked up the word in the dictionary.`
  7. `She looked it up.`
  8. `They give up easily.`
  9. `As a matter of fact, it rained.`
  10. `It was a de facto ban.`
  11. `They formed an ad hoc committee.`
  12. `Her bank balance was low.`
  13. `The motion failed. In the end nobody cared.`
  
  The script prints the 0-based page word index of each **target**: FAVOUR (heading), favor, favour (p3), favour (p4), account, looked (p6), looked (p7), give, matter, facto, hoc, balance, end. It also prints the index of each **other gate word**: vote, council, motion (p2), plan (p3), length, dictionary, easily, rained, ban, committee, low, nobody.

- [ ] **Step 1: Write the book script** after `make_test_lists.py`'s layout (`CONTAINER`, `OPF` with `dc:language` `en`, `NCX`, one `chapter1.xhtml`). The docstring says what each sentence checks. The indices count whitespace-separated tokens across the heading and the paragraphs, in order. Run `python test/epubs/make_test_dict_phrases.py`.

- [ ] **Step 2: Create its golden.** The corpus is listed when the test binary runs, so run the binary rather than `ctest`, whose list predates the book:
  Run: `cmake --build build/test --target EpubPipelineTest && UPDATE_GOLDENS=1 ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='*test_dict_phrases*' && ./build/test/epub_pipeline/EpubPipelineTest.exe --gtest_filter='*test_dict_phrases*' && git status --short test/epub_pipeline/goldens`
  Expected: the second run passes; exactly one new golden, no other golden changed. Read it: the heading and the 13 paragraphs on one page, in order.

- [ ] **Step 3: Add the three log lines.**

- [ ] **Step 4: Build `default`, flash it** (`pio run -e default -t upload --upload-port COM5`), and upload the book: `cmd:ACT:USB_TRANSFER`, then `python tools/serial_transfer.py --port COM5 put test/epubs/test_dict_phrases.epub /test/test_dict_phrases.epub`.

- [ ] **Step 5: Write the gate scripts** in `C:/tmp/dictphr/` (scratch, not committed).
  - **Book 1** is the test book's 25 words: the 13 targets and the 12 others.
  - **Book 2** is `/test/Alice.epub` (spec §4): 25 words on one page of prose, about half inside phrases ("at any rate", "of course", "in a hurry", compounds). To list the page's words, step `cmd:BTN:RIGHT` + `state` across it once. The ledger records the page, the words and their `sel`.
  - `gate-<book>.txt` (a devctl script) opens the book's page with `cmd:ACT:READER:<book>@<spine>:<page>`, then the word screen. It brings the cursor to the first word with a fixed run of presses (15 × `cmd:BTN:UP`, then 20 × `cmd:BTN:LEFT`, `idle:3000:SCREEN` after each), then `state check:sel=0`. Then, for each word in increasing `sel`: `cmd:BTN:RIGHT idle:3000:SCREEN` up to its `sel`, `wait:1 state check:label=<word>`, `cmd:BTN:CONFIRM idle:20000:SCREEN state`, `cmd:BTN:BACK idle:10000:SCREEN`. Part B keeps `selected` across the definition, so the next word is reached from there.
  - `gate.py <log>` pairs each `Confirm: lookup starts` with the next `Definition on screen` or `Lookup popup on screen` line by their `[ms]` stamps. It prints each word's milliseconds, the median per dictionary, and the `heap=` / `maxblock=` of the STATE that follows each Confirm.

- [ ] **Step 6: Measure the baseline.** Choose `oxford`: `cmd:ACT:HOME idle:30000 cmd:ACT:SETTINGS:READER idle:20000 "select:Dictionary" cmd:BTN:CONFIRM idle:20000:SCREEN "select:oxford" cmd:BTN:CONFIRM idle:20000`. Then run both gate scripts with `--log C:/tmp/dictphr/base-oxford.log`. Do the same with `en-de` (`base-pons.log`).
  Expected: 50 timings per dictionary. Record both medians and the heap figures in the ledger.

- [ ] **Step 7: Commit.**
  ```bash
  git add test/epubs/make_test_dict_phrases.py test/epubs/test_dict_phrases.epub test/epub_pipeline/goldens/test_dict_phrases.golden.txt \
    src/activities/reader/DictionaryWordSelectActivity.cpp src/activities/reader/DictionaryDefinitionActivity.h src/activities/reader/DictionaryDefinitionActivity.cpp
  git commit -m "test(dictionary): timing lines and a phrase test book for the speed gate"
  ```

### Task 3: The words around the tapped word

**Files:**
- Modify: `src/util/DictionaryPunctuation.h`: add `dictionaryWordBounds`
- Modify: `src/util/Dictionary.cpp`: `cleanWord` (`:741-776`) calls it. `isWordByte` and `edgePunctuationLen` (`:63-76`) move into it.
- Create: `src/util/PhraseContext.h`, `src/util/PhraseContext.cpp`
- Create: `test/dictionary/PhraseContextTest.cpp`
- Modify: `test/dictionary/CMakeLists.txt`: a `PhraseContextTest` executable built like `CjkLookupRunTest`, from `PhraseContextTest.cpp`, `src/util/PhraseContext.cpp` and `lib/Utf8/Utf8.cpp`

**Interfaces:**
- Produces:

```cpp
// DictionaryPunctuation.h -- inline
// The bytes of text[0, length) that Dictionary::cleanWord() keeps, as [start, end), before it folds
// case: leading and trailing non-alphanumeric ASCII and isDictionaryEdgePunctuation() codepoints
// stripped. start == end when nothing is kept.
inline void dictionaryWordBounds(const char* text, size_t length, size_t& start, size_t& end);

// PhraseContext.h -- ~80 bytes, a stack local of its callers; no heap
// The words around a tapped word that a lookup searches for phrases (spec 5.1): the tapped word, up
// to MAX_BEFORE words before it and as many after it as fit MAX_BYTES from the first, joined with
// single spaces, each cut to its dictionaryWordBounds() with soft hyphens (U+00AD) removed; case
// is kept. A word nothing is left of is skipped.
class PhraseContext {
 public:
  static constexpr size_t MAX_BYTES = 48;  // == Dictionary::MAX_PREFIX_RUN_BYTES (asserted in the .cpp)
  static constexpr uint8_t MAX_BEFORE = 3;
  static constexpr uint8_t MAX_WORDS = 24;
  // Starts over on the tapped word. False, the context left empty, when nothing is left of it or
  // it is longer than MAX_BYTES.
  bool setTapped(const char* text, size_t length);
  // The page's next word before the earliest taken (nearest first), or after the last taken. True
  // while that side takes more words. Sentence punctuation (. ! ? ; : and U+3002 U+FF0E U+FF01
  // U+FF1F U+FF1B U+FF1A) in the stripped edge facing the tapped word leaves the word out and
  // closes the side; in the far edge, the word is taken and the side closes. MAX_BEFORE words
  // before, or a word that does not fit MAX_BYTES, close a side too.
  bool addBefore(const char* text, size_t length);
  bool addAfter(const char* text, size_t length);
  // This context with the tapped word replaced by `form`, words after it dropped from the end
  // until it fits. False when `form` is empty, holds a space, equals the tapped word under ASCII
  // case folding, or does not fit with the words before it.
  bool withTappedWord(const char* form, PhraseContext& out) const;
  // Words split on ASCII spaces; the word holding a [ ] pair is tapped, the two brackets removed
  // ("[favour]," taps "favour,"); fed through the calls above (CMD:ACT:DICT and the tests). False
  // when no word holds a pair.
  static bool fromBracketedText(const char* text, size_t length, PhraseContext& out);
  const char* text() const;  // NUL-terminated
  uint8_t bytes() const;
  uint8_t words() const;  // 0 when empty
  uint8_t tapped() const;
  uint8_t wordStart(uint8_t i) const;
  uint8_t wordEnd(uint8_t i) const;  // one past its last byte
};
```

- [ ] **Step 1: Write the failing tests** in `PhraseContextTest.cpp`, with `PhraseContext ctx(const char* bracketed)` built through `fromBracketedText` (ASSERTing that it succeeds):

| Test | Input | Expect |
|---|---|---|
| `JoinsThreeWordsBeforeAndWhatFitsAfter` | `"The council voted in [favor] of the motion."` | `text()` `"council voted in favor of the motion"`, `words()` 7, `tapped()` 3, `wordStart(3)` 17, `wordEnd(3)` 22 |
| `SentencePunctuationClosesASide` | `"the motion. In [favour] of it; then"`; `"voted! in [favour]"`; `"in [favour] ?of x"`; `"!in [favour] of"`; `"in [favour] of it。 x"`; `"x： in [favour]"`; `"x [favour]. of"` | `"In favour of it"`; `"in favour"`; `"in favour"`; `"in favour of"`; `"in favour of it"`; `"in favour"`; `"x favour"` |
| `ContextStripsMarksAndKeepsApostrophes` (RF 1) | `"“In [favour],” she said"`; `"(voted) in [favour]"`; `"on [someone’s] account"`; `"[don't] go"` | `"In favour she said"`; `"voted in favour"`; `"on someone’s account"`; `"don't go"` |
| `PunctuationIsTheOnlyBoundary` | `"Chapter One In [favour] of"` (a heading ran into the paragraph) | `"Chapter One In favour of"` |
| `RemovesSoftHyphens` | `"in [fa\u00ADvour] of"` | `"in favour of"`, `wordEnd(1)` 9 |
| `StaysWithinFortyEightBytes` | `"aaaaaaaaaa bbbbbbbbbb cccccccccc [x] yyyyyyyyyy zz w"` | `bytes()` 48, `"aaaaaaaaaa bbbbbbbbbb cccccccccc x yyyyyyyyyy zz"` |
| `LeavesOutBeforeWordsTheTappedWordNeedsRoomFor` | `"bbbbbbbbbbbbbbbbbbbb aaaaaaaaaaaaaaaaaaaa [cccccccccc]"` | `"aaaaaaaaaaaaaaaaaaaa cccccccccc"` |
| `RefusesAnEmptyOrOverlongTappedWord` | `setTapped("«»", 4)`; `setTapped` of 49 × `a`; `fromBracketedText("no bracket")` | each false; `words()` 0 |
| `ContextWithoutNeighboursIsTheWordAlone` (RF 2) | `"[favour]"` | `words()` 1, `tapped()` 0 |
| `TheBaseFormTakesTheTappedWordsPlace` | `ctx("she [looked] up the word")`: `withTappedWord("look")`, `("LOOKED")`, `("look up")`; `ctx("[go] aaaaaaaaaa bbbbbbbbbb cccccccccc dddddddd").withTappedWord("gooooooo")` | `"she look up the word"`, `tapped()` 1, `wordEnd(1)` 8; false; false; `"gooooooo aaaaaaaaaa bbbbbbbbbb cccccccccc"` |

- [ ] **Step 2: Run them to see them fail.**
  Run: `cmake -S test -B build/test && cmake --build build/test --target PhraseContextTest`
  Expected: a build error, `PhraseContext.h: No such file or directory`.

- [ ] **Step 3: Implement `dictionaryWordBounds`, move `cleanWord` onto it, then implement `PhraseContext`.**

- [ ] **Step 4: Run the new tests, and `cleanWord`'s.**
  Run: `cmake --build build/test --target PhraseContextTest DictionaryTest && ./build/test/dictionary/PhraseContextTest.exe && ./build/test/dictionary/DictionaryTest.exe --gtest_filter='*CleanWord*:*Guillemets*'`
  Expected: PASS.

- [ ] **Step 5: Commit.**
  ```bash
  git add src/util/DictionaryPunctuation.h src/util/Dictionary.cpp src/util/PhraseContext.h src/util/PhraseContext.cpp test/dictionary/PhraseContextTest.cpp test/dictionary/CMakeLists.txt
  git commit -m "feat(dictionary): the words around a tapped word, for phrase lookups"
  ```

### Task 4: A, the phrase headword

**Files:**
- Modify: `src/util/Dictionary.h`. Add `class PhraseContext;` before `Dictionary`, the public `resolvePhrase` after `resolvePrefixes` (`:151-152`), and the private `prefixSearch` near `collectPrefixes` (`:325-326`).
- Modify: `src/util/Dictionary.cpp`. `PrefixSearch` (`:998-1016`) gains `uint8_t minBytes = 0`. `collectPrefixes` (`:1139-1222`) honours it. `resolvePrefixes`' body after `openSession` (`:1285-1338`) moves into `prefixSearch`.
- Test: `test/dictionary/DictionaryTest.cpp`
- Modify: `test/dictionary/CMakeLists.txt`: add `${REPO_ROOT}/src/util/PhraseContext.cpp` to `DictionaryTest`

**Interfaces:**
- Consumes: `PhraseContext` (Task 3).
- Produces:

```cpp
// Spec 5.2 (A): the longest headword of two or more whole words of `context` that covers its
// tapped word, from every start up to the tapped word, in ONE lookup session. `baseForm`
// (nullable) is resolve()'s headword for the tapped word: the starts are searched again with it in
// the tapped word's place (PhraseContext::withTappedWord). On a hit, matchOut.hits[0] is the phrase
// (count 1, with its whole same-headword run); probes and reads count every search. No search at
// all when context.words() < 2. outResult as for resolvePrefixes().
bool resolvePhrase(const PhraseContext& context, const char* baseForm, DictPrefixMatch& matchOut,
                   LookupResult* outResult = nullptr);

// private: resolvePrefixes() inside an open session. Hits shorter than minBytes are not recorded,
// and no probe targets a prefix shorter than minBytes (Ruling 5); resolvePrefixes() passes 0.
bool prefixSearch(LookupSession& session, const char* run, uint8_t minBytes, DictPrefixMatch& matchOut,
                  LookupResult* outResult, AbortCheck abortCheck);
```

- [ ] **Step 1: Write the failing tests.**
  - The shared dictionary `kPhrases` (plain, `m`): `in` "inside", `of` "from", `favour` "goodwill", `in favour of` "backing", `look` "see", `look up` "search", `de facto` "really", `balance` "even", `bank balance` "money held", `balance sheet` "statement", `ab cd` "one", `cd ef` "two", `über alles` "above all".
  - The helper `DictPrefixMatch phraseOf(Dictionary&, const char* bracketed, const char* baseForm = nullptr, Dictionary::LookupResult* r = nullptr)`.

| Test | Call | Expect |
|---|---|---|
| `PhraseHeadwordIsFoundFromEveryStart` | `"voted [in] favour of the motion"`, `"voted in [favour] of the motion"`, `"voted in favour [of] the motion"` | each: `count` 1, `hits[0].headword` `"in favour of"`, `runBytes` 12, `r == Found` |
| `PhraseMustCoverTheTappedWord` | `"voted in favour of [the] motion"`, `"[voted] in favour of the motion"` | `count` 0, `r == NotFound` |
| `PhraseIsMadeOfWholeWordsAndAtLeastTwo` (RF 2) | `"[look] upwards"`; `"[favour]"` | `count` 0 for both; the second has `probes == 0` |
| `TheLongestPhraseWinsAndATieGoesToTheEarlierStart` | `"bank [balance] sheet"`; `"ab [cd] ef"` | `"balance sheet"`; `"ab cd"` |
| `PhraseSearchFindsACapitalisedPhrase` (RF 1) | `"In [favour] of it"`; `"IN FAVOUR [OF] IT"`; `"Über [alles] hinaus"` | `"in favour of"`; `"in favour of"`; `"über alles"` |
| `TheBaseFormFindsAPhraseTheWrittenFormMisses` | `"she [looked] up the word"` with `baseForm` `"look"`, then without | `"look up"`; `count` 0 |
| `PhraseIsFoundAroundAWordWithNoEntry` | `"a de [facto] ban"` | `"de facto"` |
| `PhraseSearchNeverProbesForRunsTooShortToCount` | its own dictionary: `in`, then 300 fillers `"in a000"` … `"in a299"`; `"[in] favour"` | `count` 0, `probes == 1` (the descent for `"in "` would find only `in`, which cannot count) |

- [ ] **Step 2: Run them to see them fail.**
  Run: `cmake --build build/test --target DictionaryTest`
  Expected: a build error, `'class Dictionary' has no member named 'resolvePhrase'`.

- [ ] **Step 3: Implement.** `resolvePrefixes` opens the session and calls `prefixSearch(..., 0, ...)`. `resolvePhrase` searches the context as written, then its `withTappedWord(baseForm)` variant, each from start 0 to the tapped word, with `minBytes` per Ruling 5 and the case retry per Ruling 4. Rulings 3 and 6 decide what counts and what wins.
  - Each frame stays under 256 bytes: `matchOut` serves as the scratch of each search, the best hit is a local `DictPrefixHit`, and the lowercased run goes in a helper.
  - A recorded hit's headword over 15 bytes allocates once, where step 9 already assigns it. That is per hit (at most three per probe), not per scanned entry, and it stays.

- [ ] **Step 4: Run the whole binary** (the CJK prefix tests must not move).
  Run: `cmake --build build/test --target DictionaryTest && ./build/test/dictionary/DictionaryTest.exe`
  Expected: PASS, every test.

- [ ] **Step 5: Commit.**
  ```bash
  git add src/util/Dictionary.h src/util/Dictionary.cpp test/dictionary/DictionaryTest.cpp test/dictionary/CMakeLists.txt
  git commit -m "feat(dictionary): find the longest phrase headword around a word, in one lookup session"
  ```

### Task 5: A, the phrase's definition

**Files:**
- Modify: `lib/SaxParser/SaxParser/SaxParser.h`, `lib/SaxParser/SaxParserYxml.cpp:163`: `isHtmlVoidElement` becomes `public: static bool SaxParser::isHtmlVoidElement(const char* name)`, unchanged otherwise.
- Modify: `src/util/DictHtmlNormalize.{h,cpp}`: add `cutDictionaryHtml`.
- Modify: `src/util/Dictionary.{h,cpp}`:
  - `DictLocation` (`.h:18-25`) gains `uint32_t idxEntry = 0;`, set by `locate`, `locateByOrdinal`, `collectPrefixes` and `locateRun`;
  - `readPrefixDefinitions` (`.cpp:1417-1496`) gains `styledCap`.
- Test: `test/dictionary/DictHtmlNormalizeTest.cpp`, `test/dictionary/DictionaryTest.cpp`
- Modify: `test/dictionary/CMakeLists.txt`: `DictionaryTest` adds `${REPO_ROOT}/src/util/DictHtmlNormalize.cpp`, links `SaxParser`, and gets `lib/SaxParser` on its include path.

**Interfaces:**
- Consumes: `resolvePhrase` (Task 4).
- Produces:

```cpp
// DictHtmlNormalize.h
// Cuts the HTML in html[0, length) to at most `limit` bytes, in place, so that it still lays out:
// at the last point where no tag, entity or UTF-8 sequence is split and the end tags of the
// elements still open there (void elements and <x/> excepted) fit before `limit`; those end tags
// are written after it. An end tag pops the innermost open element of its name; at most 16 open
// elements are tracked, names to 12 bytes. Returns the new length; `length` itself when it fits.
size_t cutDictionaryHtml(char* html, size_t length, size_t limit);

// Dictionary.h
struct DictLocation { /* ... */ uint32_t idxEntry = 0; };  // byte offset in .idx of the entry it names

// styledCap == 0: today's rule. styledCap > 0 (a phrase, spec 5.2): hits[0]'s entries by today's
// rule; then cap = styledCap when hits[0]'s entries took at most styledCap bytes, else
// MAX_DEFINITION_BYTES; each entry of the later hits is cut to what is left under cap
// (cutDictionaryHtml for HTML, a codepoint boundary otherwise); an entry left fewer than 64 bytes of
// its definition is left out, with every entry after it.
bool readPrefixDefinitions(const DictPrefixMatch& match, std::string& definitionOut,
                           LookupResult* outResult = nullptr, uint32_t styledCap = 0);
```

- [ ] **Step 1: Write the failing cut tests** in `DictHtmlNormalizeTest.cpp`. A helper `std::string cut(std::string html, size_t limit)` applies `cutDictionaryHtml` to a copy and resizes it.

| Test | Input | Expect |
|---|---|---|
| `CutHtmlClosesTheElementsStillOpen` | `"<div><span>abcdefghij</span><b>klm</b></div>"`, limit 25 | `"<div><span>a</span></div>"` |
| `CutHtmlAlwaysLeavesSomethingThatParses` | `"<blockquote><span><b>in ∼ of sth</b> für etw &amp; <i>akk</i></span><br>x</blockquote>"`, every limit from 0 to its size | `size() <= limit`; `parseNormalized(out).ok`; the bytes before the appended end tags are a prefix of the input that ends outside a tag, the `&amp;` and a UTF-8 sequence |
| `CutHtmlLeavesAFittingFragmentAlone` | the same, limit = its size | unchanged |
| `CutHtmlNeverClosesAVoidElement` | `"<div>a<br>b<img src=x>c</div>"`, limit 20 | no `</br>`, no `</img>`; parses |

- [ ] **Step 2: Write the failing fill tests** in `DictionaryTest.cpp`.
  - The dictionary `PhraseDefs` (`h`): `give up` `"<i>to stop</i>"`; `give` `"<div><span>"` + 17,000 × `g` + `"</span></div>"`; `in favour of` `"<i>backing</i>"`; `favour` `"<div>liking</div>"`; `big phrase` `"<div>"` + 20,000 × `p` + `"</div>"`; `huge phrase` `"<div>"` + 60,000 × `q` + `"</div>"`.
  - The helper `appendWordHit(Dictionary&, DictPrefixMatch&, const char* word)` runs `resolve(word)` and appends `{location, 0, 1, headword, location.idxEntry}`.

| Test | Match | Expect |
|---|---|---|
| `PhraseDefinitionsListThePhraseThenTheWord` | `phraseOf("voted in [favour] of it")` + word `favour`, cap 16384 | `"<p><b>in favour of</b></p><i>backing</i><p><b>favour</b></p><div>liking</div>"` |
| `TheWordIsCutToKeepTheWholeUnderTheStyledCap` | `"they [give] up"` + `give`, cap 16384 | `size()` 16384; starts `"<p><b>give up</b></p><i>to stop</i><p><b>give</b></p><div><span>g"`; ends `"g</span></div>"` |
| `ThePhraseIsNeverCutForTheWord` | `"a [big] phrase"` + `give`, cap 16384 | both entries whole: size = the two headings + 20,011 + 17,024 |
| `PastTheDefinitionCapTheWordIsCutToo` | `"a [huge] phrase"` + `give`, cap 16384 | `size()` == `MAX_DEFINITION_BYTES`; the phrase entry whole; ends `"</span></div>"` |
| `PlainDictionariesCutAtACodepointBoundary` | plain (`m`): `x y` with 40,000 × `a`, `x` with 20,000 × `ü`; `"[x] y"` + `x`, cap `MAX_DEFINITION_BYTES` | `size()` 65535 or 65536; ends on a whole `ü` |
| `ResolvedLocationsKnowTheirIndexEntry` | `resolve("favour")`, `resolve("favours")` (stem), and, in a dictionary of its own with syn `favor` → `favour`, `resolve("favor")` (a `.syn` hit) and a `resolvePrefixes("favour")` hit | each `idxEntry` equals that entry's byte offset in `words.idx`, which the test computes from the sorted headwords (`word.size() + 9` each) |

- [ ] **Step 3: Run them to see them fail.**
  Run: `cmake --build build/test --target DictHtmlNormalizeTest DictionaryTest`
  Expected: build errors naming `cutDictionaryHtml`, `idxEntry` and `readPrefixDefinitions`' fourth argument.

- [ ] **Step 4: Implement.** The buffer is still sized once, from pass 1's upper bound. A cut entry is read at its full room, then cut, and the string is `resize`d down at the end, which does not reallocate.

- [ ] **Step 5: Run the three binaries.**
  Run: `cmake --build build/test --target DictHtmlNormalizeTest DictionaryTest && ./build/test/dictionary/DictHtmlNormalizeTest.exe && ./build/test/dictionary/DictionaryTest.exe && ctest --test-dir build/test -R SaxParser`
  Expected: PASS.

- [ ] **Step 6: Commit.**
  ```bash
  git add lib/SaxParser src/util/DictHtmlNormalize.h src/util/DictHtmlNormalize.cpp src/util/Dictionary.h src/util/Dictionary.cpp \
    test/dictionary/DictHtmlNormalizeTest.cpp test/dictionary/DictionaryTest.cpp test/dictionary/CMakeLists.txt
  git commit -m "feat(dictionary): a phrase's entries come first; the word's are cut so the whole stays styled"
  ```

### Task 6: B, the idiom inside the article

**Files:**
- Modify: `src/util/HtmlToPlainText.{h,cpp}`. The body of `htmlToPlainTextInPlace` (`:126-183`) becomes a char-buffer core with an optional tracked offset; the `std::string` function wraps it.
- Create: `src/util/ArticlePhrase.h`, `src/util/ArticlePhrase.cpp`
- Test: `test/dictionary/HtmlToPlainTextTest.cpp`; create `test/dictionary/ArticlePhraseTest.cpp`
- Modify: `test/dictionary/CMakeLists.txt`: an `ArticlePhraseTest` executable from `ArticlePhraseTest.cpp`, `src/util/ArticlePhrase.cpp`, `src/util/PhraseContext.cpp`, `src/util/HtmlToPlainText.cpp`, `lib/Epub/Epub/htmlEntities.cpp` and `lib/Utf8/Utf8.cpp`, with `HtmlToPlainTextTest`'s include dirs plus `lib/Utf8` and `test/zip_entry_reader`

**Interfaces:**
- Consumes: `PhraseContext`, `dictionaryWordBounds` (Task 3).
- Produces:

```cpp
// HtmlToPlainText.h
// The conversion below over text[0, length), in place; returns the new length. When trackOffset
// is given, *trackOffset (a byte offset of the input) becomes the output offset at which the input
// from there on lands.
size_t htmlToPlainText(char* text, size_t length, size_t* trackOffset = nullptr);
void htmlToPlainTextInPlace(std::string& text, size_t* trackOffset = nullptr);

// ArticlePhrase.h
// B's match in a definition (spec 5.3). ~72 bytes; copied into the definition screen.
struct ArticlePhraseMatch {
  static constexpr size_t MAX_TEXT_BYTES = 63;
  uint32_t offset = 0;   // byte offset in the definition of the matched run's outer "<b"
  uint8_t literals = 0;  // literal words matched; 0 = no match
  char text[MAX_TEXT_BYTES + 1] = {};  // the run as shown: tags removed, entities decoded, cut at a word end
  bool found() const { return literals > 0; }
};
// The bold run of `definition` that matches `context` best (spec 5.3), `headword` being the
// article's (what resolve() or C resolved to). One pass, no allocation, no card read.
bool findArticlePhrase(const char* definition, size_t length, const PhraseContext& context, const char* headword,
                       ArticlePhraseMatch& out);
```

**Rules** (spec 5.3 and Rulings 10-13):
- **The notation table.** It lives in `ArticlePhrase.cpp`, one `constexpr` array of `{text, kind}`:
  - Placeholder: `someone, somebody, something, someone's, somebody's, one, one's, oneself, sb, sth, sb's, sth's, sb/sth, jdn, jdm, jds, etw, ——, ...`;
  - OptionalLeading: `to`;
  - Task 7 adds the cues to the same table.
- **A run** starts at `<b` followed by `>` or whitespace (ASCII case folded). Nested `<b>` raise the depth, and the run ends at the `</b>` that closes the outer one. Up to 128 raw bytes are copied to a stack buffer and decoded with `htmlToPlainText` (newlines read as spaces).
- **Tokens:**
  - split on ASCII whitespace, with `( ) [ ]` as tokens of their own;
  - a token that is in the table as Placeholder, whole, cut to its `dictionaryWordBounds`, or in one of its `/` alternatives, is a placeholder. It is checked whole first, because `——` and `...` are nothing but edge punctuation;
  - every other token is cut to its `dictionaryWordBounds`, and dropped when nothing is left;
  - the first token, when it is in the table as OptionalLeading, is optional;
  - tokens inside a bracket group not opening with `or` are optional.
- **Readings.** An `(or …)` / `[or …]` group (to the end of the run when unclosed) gives a reading in which its words replace the token before the group. The base reading drops every such group.
- **Comparison.** Codepoint by codepoint under `utf8LowerCodepoint`, with `∼` / `~` in a token replaced by the headword. A token's `/` alternatives are tried in turn.
- **Score.** Per reading, take a contiguous part of its tokens aligned with contiguous context words, with the tapped word aligned to a literal or optional token. That token may equal the tapped word or the headword (Ruling 12).
  - A placeholder absorbs one to three words and scores nothing.
  - An optional token may be absent, and scores when matched.
  - Every other matched token scores one.
  
  A run matches at a score of 2 or more. The best is the highest; ties go to the earliest run, then the earliest reading.

- [ ] **Step 1: Write the failing converter tests** in `HtmlToPlainTextTest.cpp`:
  - `ConvertsACharBufferInPlace`: `char buf[] = "<b>a&amp;b</b> c"`; `htmlToPlainText(buf, strlen(buf))` returns 5, and `buf` starts with `"a&b c"`.
  - `TracksWhereAnOffsetLands`: `std::string s = "<p>x</p><div><b>in ∼ of</b></div>"`; `size_t at = s.find("<b>")`; after `htmlToPlainTextInPlace(s, &at)`, `s.compare(at, 9, "in ∼ of") == 0`.

- [ ] **Step 2: Write the failing matcher tests** in `ArticlePhraseTest.cpp`, with `ArticlePhraseMatch match(const std::string& article, const char* bracketed, const char* headword)`. The two made-up articles in the spec's markup styles:

```cpp
// Oxford's style: the headword spelled out, alternatives nested in bold.
const std::string kOxford =
    "<div><b>favour</b> n. liking</div><div>⧼PHRASES⧽</div>"
    "<div><span>▫ </span><b>in (or <b>out of</b>) favour</b> liked or not</div>"
    "<div><span>▫ </span><b>in favour of</b> backing</div>";
// PONS's style: the headword as ∼, placeholders sb / sth.
const std::string kPons =
    "<blockquote><span><b>fa·vour</b></span> <i>n</i> ▪ <span><b>to be in ∼ with sb</b></span> x; "
    "<span><b>in ∼ of sth</b></span> y; <span><b>to vote in ∼ of sth</b></span> z</blockquote>";
```

The headword is `favour` where a row names none; "same" repeats the row's context.

| Test | Article, context, headword | Expect |
|---|---|---|
| `FindsTheSpelledOutPhrase` | `kOxford`, `"voted in [favour] of the motion"`, `favour` | `text` `"in favour of"`, `literals` 3, `offset == kOxford.find("<b>in favour of")` |
| `TheTildeStandsForTheHeadword` | `kPons`, same | `"in ∼ of sth"`, 3, `offset == kPons.find("<b>in ∼ of sth")` (ties with "to vote …": the first wins) |
| `TheTappedWordMatchesTheHeadwordInAnyForm` | `kPons`, `"voted in [favor] of it"`, `favour`; `"<b>look something up</b> find"`, `"she [looked] it up"`, `look`; `"<span><b>by </b></span>[<i>or </i><b>from</b>] <span><b>all ∼s</b></span>"`, `"by all [accounts]"`, `account` | `"in ∼ of sth"` 3; `"look something up"` 2; `"all ∼s"` 2 |
| `EachPlaceholderAbsorbsOneToThreeWords` | for each placeholder `p`: `"<b>keep " + p + " in ∼</b>"`, headword `mind`, with `"keep it in [mind]"`, `"keep the old rule in [mind]"`, `"keep the very old rule in [mind]"` | `literals` 3, 3, 2 (four words leave only "in ∼") |
| `ALeadingToIsOptionalAndCountsWhenThere` | `kPons`, `"to vote in [favour] of it"`; `"they vote in [favour] of it"` | `"to vote in ∼ of sth"` 5; the same, 4 |
| `BothAlternativeFormsAreRead` | `"<b>call (or <b>bring</b>) someone to account</b>"`, `"we bring him to [account]"`, `account`; `"<b>in [or out of] ∼</b>"`, `"out of [favour] now"`, `favour`; `"<b>on my/her/his ∼</b>"`, `"on her [account]"`, `account` | 3; 3; 3 |
| `BracketsWithoutOrAreOptional` | `"<b>to be in ∼ [with sb]</b>"`, `"to be in [favour] with them"`; `"to be in [favour] now"` | 5; 4 |
| `AnUnclosedBracketIsReadAsFarAsItGoes` | `"<b>in (or out of</b> the"`, `"came out [of] the woods"`, `woods` | `text` `"in (or out of"`, 2 |
| `APartialMatchCounts` | `"<b>take something into account</b>"`, `"she took it into [account]"`, `account` | 2 |
| `MostLiteralWordsWinAndATieGoesToTheFirst` | `"<b>in ∼ now</b> <b>in ∼ then</b>"`, `"in [favour] of it"`; `"<b>in ∼</b> <b>in ∼ of sth</b>"`, same | `offset` 0; `offset` of the second run |
| `ALoneWordIsNoPhrase` | `"<b>out of print</b> <b>of</b>"`, `"voted in favour [of] the motion"`, `favour`; `"<b>1</b> <b>II</b> <b>favour<sup>2</sup> I</b>"`, `"in [favour] of"`, `favour` | `found()` false; false |
| `EntitiesAndNestedTagsAreDecoded` | `"<b>do <u>me</u> a ∼!</b>"`, `"do me a [favour]!"`, `favour`; `"<b>rock &amp; roll</b>"`, `"rock & [roll] music"`, `roll` | `"do me a ∼!"` 4; `"rock & roll"` 2 (the bare `&` is dropped on both sides) |
| `ArticlePhraseIgnoresCase` (RF 1) | `kOxford`, `"IN [FAVOUR] OF THE MOTION"`; `"<b>über alles</b>"`, `"Über [alles]"`, `alles` | 3; 2 |
| `NoMatchLeavesNothingMarked` | `kPons`, `"voted for the [motion]"`, `favour` | `found()` false |

- [ ] **Step 3: Run them to see them fail.**
  Run: `cmake -S test -B build/test && cmake --build build/test --target HtmlToPlainTextTest ArticlePhraseTest`
  Expected: build errors naming `htmlToPlainText` and `ArticlePhrase.h`.

- [ ] **Step 4: Implement** the converter core, then `findArticlePhrase` as the rules say, every frame under 256 bytes.

- [ ] **Step 5: Run both binaries.**
  Run: `cmake --build build/test --target HtmlToPlainTextTest ArticlePhraseTest && ./build/test/dictionary/HtmlToPlainTextTest.exe && ./build/test/dictionary/ArticlePhraseTest.exe`
  Expected: PASS, the old converter tests included.

- [ ] **Step 6: Commit.**
  ```bash
  git add src/util/HtmlToPlainText.h src/util/HtmlToPlainText.cpp src/util/ArticlePhrase.h src/util/ArticlePhrase.cpp \
    test/dictionary/HtmlToPlainTextTest.cpp test/dictionary/ArticlePhraseTest.cpp test/dictionary/CMakeLists.txt
  git commit -m "feat(dictionary): find the idiom of an article that matches the text around the word"
  ```

### Task 7: C, cross-reference stubs

**Files:**
- Modify: `src/util/ArticlePhrase.{h,cpp}`: the cues, and `crossReferenceTarget`
- Modify: `src/util/Dictionary.{h,cpp}`:
  - `IfoFacts` (`.cpp:97-121`) gains `markupDefinitions`;
  - add `definitionsHaveMarkup()`, `resolveExact`, and `readResolved`'s `lead`;
  - `readDefinition` (`.cpp:652-687`) takes the lead.
- Test: `test/dictionary/ArticlePhraseTest.cpp`, `test/dictionary/DictionaryTest.cpp`

**Interfaces:**
- Produces:

```cpp
// ArticlePhrase.h
// Spec 5.4: when `definition` is a cross-reference stub, its target. A stub has at most 256 bytes
// of text outside tags and ends with its target run (only punctuation, whitespace and end tags
// after it): a bold run not inside <i>, after any cue of the table; or an italic run after the
// "↑" cue. "After" means the text before the run (tags removed, trailing whitespace trimmed) ends
// with the cue, compared ASCII case-insensitively, a word cue starting a word. The target is the
// run's text without its <sup> content, cut to its dictionaryWordBounds().
bool crossReferenceTarget(const char* definition, size_t length, char* target, size_t capacity);

// Dictionary.h
bool definitionsHaveMarkup() const;  // sametypesequence is a single h, x or g
// The .idx entry of `headword` as locate() finds it (ASCII case folded, exact case preferred):
// none of resolve()'s fallbacks, no .syn. outResult as for resolve().
bool resolveExact(const char* headword, DictLocation& locationOut, std::string& headwordOut,
                  LookupResult* outResult = nullptr);
// `lead` (nullable, not aliasing definitionOut) is copied in front of the definition, in the
// same one allocation; the heap check counts it, and the definition is cut to MAX_DEFINITION_BYTES
// minus it.
bool readResolved(const DictLocation& location, std::string& definitionOut, LookupResult* outResult = nullptr,
                  const std::string* lead = nullptr);
```

- The cues, appended to Task 6's table as kind `CrossReferenceCue`, longest first where one is the end of another:
  - the spec's first list: `see also, see, ↑, →, another term for, variant spelling of, variant of, pl of, pt of, pp of, abbreviation of, abbrev of, siehe, s.`;
  - Oxford's inflection cues: `past and past participle of, past participle of, past tense of, past of, plural of, short for, archaic spelling of, dialect form of, comparative of, superlative of`.

- [ ] **Step 1: Write the failing tests.**

| Test (file) | Input | Expect |
|---|---|---|
| `AStubNamesItsTarget` (ArticlePhrase) | `"<blockquote><span><b>fa·vor</b></span> <i>n</i> <small>AM</small> <i>see </i><span><b>favour</b></span></blockquote>"` | true, `"favour"` |
| `EveryCueInTheTableWorks` (ArticlePhrase) | for each cue `c`: `"<i>" + c + "</i> <b>target</b>."`; `"ANOTHER TERM FOR <b>FORESTS</b>"` | `"target"` each; `"FORESTS"` |
| `AHomographNumberIsNotPartOfTheTarget` (ArticlePhrase) | `"see <b>favour<sup>1</sup></b>"` | `"favour"` |
| `TrailingMarksAndEndTagsAreAllowed` (ArticlePhrase) | `"<div>see <b>favour</b>.</div>"` | `"favour"` |
| `AnItalicTargetAfterTheArrowCounts` (ArticlePhrase) | `"<blockquote>↑ <i>look</i></blockquote>"`; PONS's *gave* shape, `"<blockquote><span><b>gave</b></span> <i>verb</i> <span><i>pt of </i></span><span><b>give</b></span></blockquote><blockquote>↑ <i>give</i></blockquote>"` | `"look"`; `"give"` |
| `AnItalicTargetElsewhereDoesNot` (ArticlePhrase) | `"<i>see</i> <i>look</i>"`; `"↑ <b>x</b> <i>y</i>"` | false; false |
| `NotAStub` (ArticlePhrase) | 300 bytes of text then `"see <b>x</b>"`; `"<i>adj</i> <b>-ious</b>"`; `"<i>see <b>favour</b></i>"`; `"see <b>favour</b> for more"`; `"foresee <b>x</b>"` | false for each |
| `MarkupFollowsTheSametypesequence` (Dictionary) | `.ifo` with `h`, `x`, `g`; `m`, `hm`, `xh` | `definitionsHaveMarkup()` true, true, true; false, false, false; `definitionsAreHtml()` only for `h` |
| `ExactResolveTakesNoFallbacks` (Dictionary) | `dog`, `favour`; syn `colour` → `favour`: `resolveExact("dogs")`, `("FAVOUR")`, `("colour")` | false `NotFound`; true, headword `"favour"`; false |
| `ReadResolvedPutsTheLeadFirst` (Dictionary) | `lead = "<i>see</i> <b>favour</b>"`, `favour`'s location | `out == lead + "<definition>"`; `ESP.setMaxAllocHeap(lead.size() + 8192)` around it: false, `LowMemory`, `out` empty |

- [ ] **Step 2: Run them to see them fail.**
  Run: `cmake --build build/test --target ArticlePhraseTest DictionaryTest`
  Expected: build errors naming `crossReferenceTarget`, `definitionsHaveMarkup`, `resolveExact` and `readResolved`'s fourth argument.

- [ ] **Step 3: Implement.** The stub check reads its run with Task 6's run reader. `resolveExact` opens a session and calls `locate()` once.

- [ ] **Step 4: Run both binaries.**
  Run: `cmake --build build/test --target ArticlePhraseTest DictionaryTest && ./build/test/dictionary/ArticlePhraseTest.exe && ./build/test/dictionary/DictionaryTest.exe`
  Expected: PASS.

- [ ] **Step 5: Commit.**
  ```bash
  git add src/util/ArticlePhrase.h src/util/ArticlePhrase.cpp src/util/Dictionary.h src/util/Dictionary.cpp \
    test/dictionary/ArticlePhraseTest.cpp test/dictionary/DictionaryTest.cpp
  git commit -m "feat(dictionary): recognise cross-reference and inflection stubs, and read an entry after a lead"
  ```

### Task 8: The lookup at Confirm

**Files:**
- Create: `src/util/PhraseLookup.h`, `src/util/PhraseLookup.cpp`
- Create: `test/dictionary/PhraseArticles.h`, holding Task 6's `kOxford` and `kPons`, which `ArticlePhraseTest.cpp` then includes
- Test: `test/dictionary/DictionaryTest.cpp`
- Modify: `test/dictionary/CMakeLists.txt`: `DictionaryTest` adds `PhraseLookup.cpp`, `ArticlePhrase.cpp`, `HtmlToPlainText.cpp`, `lib/Epub/Epub/htmlEntities.cpp`, and `lib/Epub` on its include path

**Interfaces:**
- Consumes: `resolvePhrase` (Task 4); `readPrefixDefinitions` with `styledCap` and `idxEntry` (Task 5); `findArticlePhrase` (Task 6); `crossReferenceTarget`, `resolveExact`, `readResolved` with a lead, `definitionsHaveMarkup` (Task 7).
- Produces:

```cpp
// What a lookup of a Latin word at Confirm shows (spec 5.2-5.4).
struct PhraseLookupResult {
  std::string headword;       // the header's: the phrase (A), the followed headword (C), else the word's
  std::string definition;
  ArticlePhraseMatch mark;    // B's match in `definition`; mark.found() false when none
  bool phraseHeadword = false;  // A
  bool followed = false;        // C
  uint8_t probes = 0;           // A's window scans and file reads, for the log line
  uint16_t reads = 0;
};
// `word` / `wordHeadword`: what the word's own search found (word.found false on a genuine miss).
// 0. On a markup dictionary, a found word whose entry is at most 2 KB is read first (the spec's
//    one extra read) and checked for a stub whose target resolveExact() finds at another location.
// 1. A: dict.resolvePhrase(context, base form per Ruling 7, the stub's target included). On a hit:
//    the read of step 0 is released; the phrase's entries, then the word's one entry (from
//    word.idxEntry), through readPrefixDefinitions(..., styledCap). Failing that with LowMemory,
//    go on as if A had found nothing (Review Focus 4).
// 2. Else, with the word found: its entry (step 0's, or readResolved now). A stub's target is read
//    with the stub as its lead, once (on failure the stub stays); then B over the definition with
//    the headword.
// False with *outResult the reason when there is nothing to show.
bool lookUpInContext(Dictionary& dict, const PhraseContext& context, const DictLocation& word,
                     const std::string& wordHeadword, uint32_t styledCap, PhraseLookupResult& out,
                     Dictionary::LookupResult* outResult);
```

- [ ] **Step 1: Write the failing tests.**
  - The made-up dictionary `PonsLike` (`h`):
    - `favor`: `"<span><b>fa·vor</b></span> <i>n</i> <small>AM</small> <i>see </i><span><b>favour</b></span>"`;
    - `favour`: `kPons`'s text, made a `const char*` shared with `ArticlePhraseTest` through a small `test/dictionary/PhraseArticles.h`;
    - `give`: `"<div><span>"` + 15,000 × `g` + `"</span></div>"`; `give up`: `"<i>to stop</i>"`;
    - `loop`: `"<i>see</i> <b>nowhere</b>"`; `one`: `"<i>see</i> <b>two</b>"`; `two`: `"<i>see</i> <b>three</b>"`; `three`: `"<i>3</i>"`;
    - `de facto`: `"<i>really</i>"`; `motion`: `"<i>a proposal</i>"`;
    - `looked`: `"<blockquote>↑ <i>look</i></blockquote>"`; `look`: `"<i>to see</i>"`; `look up`: `"<i>to search</i>"`.
  - `OxfordLike` (`h`): `favour` as `kOxford`; `account`: `"<div><b>account</b> n.</div><div>▫ <b>take something into account</b> weigh</div>"`.
  - `PlainLike` (`m`): `favour` `"in favour of: backing"`; `favor` `"see favour"`.
  - The helper `bool lookUp(Dictionary&, const char* bracketed, PhraseLookupResult&, Dictionary::LookupResult*)` runs `resolve()` on the tapped word, then `lookUpInContext` with `styledCap` 16384.
  - The dictionary is `PonsLike` where a row names none.

| Test | Dictionary, context | Expect |
|---|---|---|
| `ACrossReferenceIsFollowedOnceAndTheIdiomFoundInIt` | `PonsLike`, `"voted in [favor] of the motion"` | true; `headword` `"favour"`; `followed`; `definition` == the `favor` stub + the `favour` entry; `mark.text` `"in ∼ of sth"`, `mark.offset == definition.find("<b>in ∼ of sth")` |
| `AStubToAMissingHeadwordIsShownAsItIs` | `"the [loop] here"` | `definition` == `loop`'s entry; not `followed` |
| `AChainIsFollowedOneHop` | `"[one] two"` | `headword` `"two"`; `definition` == `one`'s + `two`'s entries |
| `APhraseHeadwordComesFirst` | `"they [give] up easily"` | `headword` `"give up"`; `phraseHeadword`; `definition` starts `"<p><b>give up</b></p><i>to stop</i><p><b>give</b></p>"`; no `mark` |
| `TheStemmedFormFindsThePhrase` | `"she [gives] up easily"` | `headword` `"give up"` |
| `AnInflectionStubGivesTheBaseForm` | `"she [looked] up the word"` | `headword` `"look up"`; `phraseHeadword`; `definition` `"<p><b>look up</b></p><i>to search</i><p><b>looked</b></p><blockquote>↑ <i>look</i></blockquote>"` |
| `AnInflectionStubIsFollowedWhenNoPhraseFits` | `"she [looked] it up"` | `headword` `"look"`; `followed`; `definition` == the `looked` stub + `"<i>to see</i>"` |
| `APhraseIsFoundWhenTheWordHasNoEntry` | `"a de [facto] ban"` | `headword` `"de facto"`; `definition` `"<p><b>de facto</b></p><i>really</i>"` |
| `TheIdiomInsideTheArticleIsMarked` | `OxfordLike`, `"voted in [favour] of the motion"` | `headword` `"favour"`; `definition` == the entry byte for byte; `mark.text` `"in favour of"` |
| `PlainDictionariesShowTheWordAsToday` | `PlainLike`, `"voted in [favor] of it"` | `definition` `"see favour"`; not `followed`; no `mark` |
| `NothingPhraseLikeLeavesTheLookupAsToday` | `PonsLike`, `"voted for the [motion]"` | `headword` `"motion"`; `definition` == `readResolved(motion)`; nothing set |
| `AMissStaysAMiss` | `PonsLike`, `"the [zebra] crossing"` | false, `NotFound` |
| `LowHeapForThePhraseShowsTheWord` (RF 4) | `PonsLike`, `"they [give] up easily"`, `ESP.setMaxAllocHeap(23240)` around it: give alone needs 15,024 + 8,192, the phrase 15,077 + 8,192 | `headword` `"give"`; `definition` == `give`'s entry; not `phraseHeadword` |
| `LowHeapForTheFollowedEntryShowsTheStub` (RF 4) | `PonsLike`, `"voted in [favor] of it"`, max alloc = the stub's size + 8192 + 10 | true; `definition` == the stub; not `followed` |

- [ ] **Step 2: Run them to see them fail.**
  Run: `cmake --build build/test --target DictionaryTest`
  Expected: a build error, `PhraseLookup.h: No such file or directory`.

- [ ] **Step 3: Implement `lookUpInContext`** as its comment says.
  - A's buffer and the word's are never held at once: the A attempt reads into `out.definition` only, and the word path starts from a cleared one.
  - Keep each frame under 256 bytes: the `DictPrefixMatch` for A (~150 bytes) and C's target buffer go in separate helper functions.

- [ ] **Step 4: Run every dictionary binary.**
  Run: `cmake --build build/test --target DictionaryTest ArticlePhraseTest PhraseContextTest && for t in DictionaryTest ArticlePhraseTest PhraseContextTest; do ./build/test/dictionary/$t.exe || break; done`
  Expected: PASS.

- [ ] **Step 5: Commit.**
  ```bash
  git add src/util/PhraseLookup.h src/util/PhraseLookup.cpp test/dictionary/DictionaryTest.cpp test/dictionary/PhraseArticles.h \
    test/dictionary/ArticlePhraseTest.cpp test/dictionary/CMakeLists.txt
  git commit -m "feat(dictionary): a lookup at Confirm tries the phrase, then the stub's target, then the idiom inside"
  ```

### Task 9: The definition screen opens on the idiom and marks it

**Files:**
- Modify: `src/util/ArticlePhrase.{h,cpp}`: `PhraseWordFinder`
- Modify: `src/activities/reader/DictionaryDefinitionActivity.h`:
  - `MAX_STYLED_HTML_BYTES` moves here from the `.cpp`'s anonymous namespace (`:36`), as `public: static constexpr size_t`;
  - the constructor gains `ArticlePhraseMatch mark = {}` after step 9's `fontId`;
  - new members; `appendDebugState` inside `#if SERIAL_TEST_API`.
- Modify: `src/activities/reader/DictionaryDefinitionActivity.cpp`: `onEnter`, `drawBody`, `render`
- Modify: `docs/serial-test-api.md`: the STATE table, a row after `spec, span, match`
- Test: `test/dictionary/ArticlePhraseTest.cpp`

**Interfaces:**
- Consumes: `ArticlePhraseMatch` (Task 6); `htmlToPlainTextInPlace`'s tracked offset (Task 6).
- Produces:

```cpp
// ArticlePhrase.h -- ~180 bytes, a stack local of its caller
// Finds a matched run's words among laid-out pieces, fed in reading order across every page: the
// phrase's words (ArticlePhraseMatch::text split on spaces, cut to dictionaryWordBounds) must
// appear as consecutive words, each made of the pieces glued to it (`joined`), compared as B
// compares, and every piece of them holding a letter or digit bold.
class PhraseWordFinder {
 public:
  explicit PhraseWordFinder(const char* phrase);
  void feed(const char* text, size_t length, bool joined, bool bold);
  bool finish();           // after the last piece; true once the phrase was found
  bool found() const;
  uint16_t first() const;  // the first and last piece of the phrase, counted from 0 in feed order
  uint16_t last() const;
};
```

- **The definition screen:**
  - **Styled path:** after `layoutHtmlPages()`, every `PageLine` word of every page is fed (`joined = i > 0 && wordContinues(i)`, `bold = wordStyle(i) & BOLD`). The screen then opens on the page that holds `first()`.
  - **Plain path:** `htmlToPlainTextInPlace(definition, &offset)` with `offset = mark.offset`. After `wrapText()`, the screen opens on the page of the line that holds `offset`, and the span `[offset, offset + strlen(mark.text))` is the mark.
  - **The draw:** after the second `drawBody` pass in `render()`, each marked piece (styled) or span part (plain) on the current page is filled black (`fillRect` over its box) and drawn white (`drawText(..., false, style)`), as `drawHighlightWithSnapshot` draws a word.
  - Paging back from there works as it does today.
- STATE, inside `#if SERIAL_TEST_API`: `page=<n>/<total>`, `headword=<header headword>`, `mark=<mark.text when a page was marked, else empty>`. The doc row reads: "`page`, `headword`, `mark` | the dictionary's definition screen | the page shown `n/count`; the headword in the header; the idiom marked on the page it opened on (empty when none)".
- Task 2's DBG line `Definition on screen (page %d/%d)` now reports the start page.

- [ ] **Step 1: Write the failing finder tests.** `struct Piece { const char* text; bool joined; bool bold; };`, plus a helper that feeds a list of pieces and calls `finish()`.

| Test | Phrase, pieces (`text`/`joined`/`bold`) | Expect |
|---|---|---|
| `FinderFindsTheBoldRunInOrder` | `"in ∼ of sth"`: x/0/0, in/0/1, ∼/0/1, of/0/1, sth/0/1, y/0/0 | found, `first` 1, `last` 4 |
| `FinderJoinsBionicPieces` (RF 5) | `"in favour of"`: in/0/1, fav/0/1, our/1/1, of/0/1 | `first` 0, `last` 3 |
| `FinderSkipsAPlainOccurrence` | `"in favour of"`: in/0/0, favour/0/0, of/0/0, then in/0/1, fav/0/1, our/1/0 (bionic tail), of/0/1, then in/0/1, favour/0/1, of/0/1 | `first` 7 |
| `FinderRestartsInsideAPartialMatch` | `"a a b"`: a, a, a, b, all bold | `first` 1, `last` 3 |
| `FinderIgnoresAPunctuationPiece` | `"in ∼ of sth"`: in/0/1, ∼/0/1, of/0/1, sth/0/1, ;/1/0 | found, `last` 4 |
| `FinderSpansPages` (RF 5) | `"in ∼ of"` after 300 bold filler pieces `w` | `first` 300 |
| `FinderReportsNothingRatherThanAGuess` (RF 5) | `"in favour of"`: in/0/1, favour/0/1, then `finish()` | `finish()` false, `found()` false |

- [ ] **Step 2: Run them to see them fail.**
  Run: `cmake --build build/test --target ArticlePhraseTest`
  Expected: a build error naming `PhraseWordFinder`.

- [ ] **Step 3: Implement the finder.** No allocation: it is fed every word of a definition.

- [ ] **Step 4: Run the binary.**
  Run: `./build/test/dictionary/ArticlePhraseTest.exe` (after building it)
  Expected: PASS.

- [ ] **Step 5: Wire the definition screen** as described, and update the doc row. Members:
  - `ArticlePhraseMatch mark`;
  - `int markPage = -1`;
  - `uint16_t markFirst, markLast` (styled; global piece indices), `uint16_t markPageFirstPiece`;
  - `uint32_t markStart, markEnd` (plain).
  
  When nothing is found, `markPage` stays -1 and the screen opens on page 1.

- [ ] **Step 6: Build `default`.** Expected: `[SUCCESS]`. The screen is checked on the device in Task 11.

- [ ] **Step 7: Commit.**
  ```bash
  git add src/util/ArticlePhrase.h src/util/ArticlePhrase.cpp test/dictionary/ArticlePhraseTest.cpp \
    src/activities/reader/DictionaryDefinitionActivity.h src/activities/reader/DictionaryDefinitionActivity.cpp docs/serial-test-api.md
  git commit -m "feat(dictionary): the definition opens on the matched idiom, drawn white on black"
  ```

### Task 10: The word screen and `ACT:DICT` look phrases up

**Files:**
- Modify: `src/activities/reader/DictionaryWordSelectActivity.h`: private `buildPhraseContext` and `performLatinLookup` next to `buildWordText` (`:116`)
- Modify: `src/activities/reader/DictionaryWordSelectActivity.cpp`: `performLookup`'s Latin branch (h1 `:595-649`, after step 9's `scriptAt(selected)` decision)
- Modify: `src/SerialTestApi.cpp`: `lookUpWord` (`:350-387` on `feat/cjk-dictionary`)
- Modify: `docs/serial-test-api.md`: the `ACT:DICT` section (`:407-428`)

**Interfaces:**
- Consumes: `PhraseContext` (Task 3), `lookUpInContext` (Task 8), `DictionaryDefinitionActivity::MAX_STYLED_HTML_BYTES` and the `mark` parameter (Task 9).
- Produces:
  - `void buildPhraseContext(int index, PhraseContext& out) const;`. It feeds `buildWordText(i, …, /*dropJoinHyphen=*/true)` of `index`, then of `index-1, index-2, …` to `addBefore` and of `index+1, …` to `addAfter`, until each returns false or the page ends.
  - In the Latin branch, in this order:
    1. the context, built after the busy repaint and the empty-words return, and so before `releaseWordArrays()`;
    2. the word's index search as today: the speculation when it is current, else `resolve()` with the hyphen-joined retry;
    3. on a hit, or a NotFound verdict, `lookUpInContext(dict, context, location, headword, dict.definitionsAreHtml() ? DictionaryDefinitionActivity::MAX_STYLED_HTML_BYTES : Dictionary::MAX_DEFINITION_BYTES, …)`;
    4. a read failure of the word's search maps to today's popup without running it;
    5. the definition is started with `result.headword`, `result.definition` and `result.mark`; a false return maps to today's popups by its `LookupResult`.
    
    `logLookup` keeps its line, with the header headword. The CJK branch does not change.
  - Frames (spec §4: under 256 bytes each). The context (~80 bytes) and the `PhraseLookupResult` (~130) do not fit beside `performLookup`'s 128-byte word buffer. The Latin branch therefore moves into a private `Dictionary::LookupResult performLatinLookup(const char* text)`, which builds the context, runs the search and `lookUpInContext`, logs, and pushes the definition. `performLookup` keeps the busy repaint, the index build, the script split and the popups for the result it returns.
  - DBG `DICT` `Phrase context="%s" phrase="%s" xref="%s" mark="%s" probes=%u reads=%u ms=%lu`: the A headword when `phraseHeadword`, the followed headword when `followed`, otherwise empty.
  - `CMD:ACT:DICT:<text>` where `text` holds a `[bracketed]` word: `PhraseContext::fromBracketedText`; `resolve()` on the tapped word; `lookUpInContext` as above. Reply:
    `DICT:result=<outcome> dict=<d> word=<tapped>[ headword=<header headword> bytes=<n>] context=<context> phrase=<A> xref=<C> mark=<B> probes=<n> ms=<t>`
    Without brackets, the reply is today's (and step 9's for CJK).

- [ ] **Step 1: Write the doc first.** In `docs/serial-test-api.md`: the context form, its reply, and the example `CMD:ACT:DICT:voted in [favor] of the motion` → `DICT:result=found dict=en-de word=favor headword=favour bytes=<n> context="voted in favor of the motion" phrase="" xref=favour mark="in ∼ of sth" probes=<n> ms=<t>`. Say that a command line stays under 64 bytes.

- [ ] **Step 2: Implement** the word screen and `lookUpWord` changes.

- [ ] **Step 3: Run the host suite's dictionary binaries** (no host test covers the screens). Expected: PASS. Then build `default` and `gh_release`. Expected: both `[SUCCESS]`.

- [ ] **Step 4: Measure flash** (spec §4, Ruling 19). Copy `.pio/build/gh_release/firmware.map`, and compare it with `C:/tmp/dictphr/base-gh_release.map`: the `Flash:` delta, plus a per-object `.text` + `.rodata` breakdown from a scratch script over the two maps (the objects of `PhraseContext`, `ArticlePhrase`, `PhraseLookup`, `Dictionary`, `DictHtmlNormalize`, `HtmlToPlainText` and the two activities).
  Expected: at most 4,500 bytes (spec §4). Otherwise stop: send the breakdown to the coordinator for the maintainer's decision, and continue only on a yes.

- [ ] **Step 5: Smoke-test on the X4.** Flash `default`, then run `python tools/devctl.py --port COM5 --log C:/tmp/dictphr/t10.log` with `"cmd:ACT:DICT:voted in [favor] of the motion"`, `"cmd:ACT:DICT:a de [facto] ban"` and `"cmd:ACT:DICT:house"`, once with `en-de` chosen and once with `oxford`.
  Expected:
  - `en-de`: `xref=favour mark="in ∼ of sth"`; `phrase="de facto"`; `house` in exactly today's reply (no new field).
  - `oxford`: `result=not_found` for the first (no `favor`, no stubs); `phrase="de facto"`.

- [ ] **Step 6: Commit.**
  ```bash
  git add src/activities/reader/DictionaryWordSelectActivity.h src/activities/reader/DictionaryWordSelectActivity.cpp src/SerialTestApi.cpp docs/serial-test-api.md
  git commit -m "feat(dictionary): the word screen and ACT:DICT look a word up in its phrase"
  ```

### Task 11: Validate on the X4, document, open the PR

**Files:**
- Modify: `docs/dictionary.md`: a subsection "Phrases" at the end of "How Lookup Works"; the definition-screen paragraph
- Delete: `docs/superpowers/specs/2026-10-28-dictionary-phrases-design.md` and this plan. Fix every link to them that `git grep -n dictionary-phrases` finds.

**Interfaces:** consumes everything above. Produces the measurements, the docs and the PR.

- [ ] **Step 1: Run the book, both dictionaries.** Flash the branch, start the session (Global Constraints), and open `/test/test_dict_phrases.epub@1:1`. For each of the 13 targets (Task 2's indices):
  - bring the cursor to the word, `wait:1 state check:label=<word>`, `cmd:BTN:CONFIRM idle:20000:SCREEN state shot:C:/tmp/dictphr/<n>-<dict>.png`, `cmd:BTN:BACK idle:10000:SCREEN`;
  - grep its `Phrase context=` line.
  
  Do this with `oxford` and with `en-de`. Expected, where the spec or the measured data promise it (the rest is recorded as found):

| Target | Oxford | PONS (`en-de`) |
|---|---|---|
| FAVOUR (heading) | B: `mark=in favour of` | B: `mark=in ∼ of sth` |
| favor (p2) | "Not found" popup | C then B: `headword=favour`, `xref=favour`, `mark=in ∼ of sth`, the stub's line above the entry |
| favour (p3, p4) | B: `mark=in favour of` | B: `mark=in ∼ of sth` |
| account | B: `mark=take something into account` (partial) | B: the `into ∼` phrase |
| looked (p6) | B in `look`: the "look up" phrase | A through the "↑ look" stub: `headword=look up` |
| looked (p7) | B in `look`: the "look something up" phrase | C: `xref=look`, the "↑ look" line above `look`'s entry |
| give | B in `give`, on the plain view (the entry is over 16 KB): the "give up" phrase marked on its line | A: `headword=give up`, then `give` cut, styled (`Styled layout starting` in the log) |
| matter | recorded | B: `mark=as a ∼ of fact` |
| facto, hoc, balance | A: `de facto`, `ad hoc`, `bank balance` | the same |
| end | the context omits "failed" (`context="In the end nobody cared"`) | the same |

  In every screenshot: the header headword is right, the marked words are white on black on the page STATE names, and paging back reaches page 1.

- [ ] **Step 2: Speed gate** (spec §4). Run Task 2's gate scripts on this build, with `oxford` (`gate-oxford.log`) and with `en-de` (`gate-pons.log`).
  Expected: for each dictionary, the median `Confirm` → `on screen` time is at most 1.5 × Task 2's. This is a gate: a miss blocks the PR.

- [ ] **Step 3: Heap.** From the same runs, compare `heap` / `maxblock` on the definition screen with Task 2's.
  Expected: within 256 bytes for words with no phrase. Phrase words are recorded; their definitions are larger by design.

- [ ] **Step 4: Flash** (Task 10 Step 4's numbers, on the final tree). Expected: unchanged, or still under the agreed limit.

- [ ] **Step 5: Review Focus checks.**
  - (RF 2) the page's first word (`sel=0`, "IN") and last word ("cared"): Confirm opens their lookups, `Phrase context=` shows only the words of this page, and nothing reboots.
  - (RF 3) on `favor`, Confirm in the definition: the screen switches to the other dictionary's result (with `oxford`: the "Not found" popup); a second Confirm comes back. STATE `mark=` belongs to the dictionary shown, and the word screen's `sel=` / `label=` are unchanged afterwards.
  - (RF 5) with `cmd:SET:bionicReading=1`, `favour` (p3) on `en-de`: the mark still lands on `in ∼ of sth`. Restore `bionicReading`.
  - (Spec §7, line ends crossed) a target whose `Phrase context=` runs across a line break in its screenshot. If none does at the default size, step `fontSize` up until p2 wraps inside "in favor of", check `favor` there, and restore `fontSize`.

- [ ] **Step 6: The run on the second book.** The 25 Book-2 words of the gate log give their `Phrase` lines. Record how many found a phrase, of which kind, and any wrong jump (spec §9: "If device screenshots still show wrong jumps, raise the minimum to three literal words for phrases of four or more"). A wrong jump is a finding for the coordinator, not something to fix silently.

- [ ] **Step 7: If Step 2 failed,** add the spec's fallback (§5.2 "Cost") as a separate commit with a test: "drop starts whose first probe shows no headword with that prefix". `resolvePhrase` skips a start whose first probe saw no headword that begins with that start's first word followed by a space. The test is `PhraseSearchSkipsAStartWithNoPhraseOfItsFirstWord`, which asserts `probes` on a filler layout. Measure again. Still over: stop and report.

- [ ] **Step 8: Write the user docs.** The "Phrases" subsection of `docs/dictionary.md`:
  - what Confirm does on a word in a phrase (A, B, C, with the spec's examples);
  - that the word's own entry stays: after the phrase, below the stub's line, or before the marked page;
  - inflected forms: found through the stemmer, or through the dictionary's own stub ("↑ look"), which also gives the phrase search its base form;
  - what it does not do (separable verbs, spelling tables);
  - where notations are added (the table in `ArticlePhrase.cpp`).
  
  In the definition-screen paragraph: the screen can open on a later page, with the idiom marked.

- [ ] **Step 9: Delete the spec and this plan; fix their links.**
  Run: `git rm docs/superpowers/specs/2026-10-28-dictionary-phrases-design.md docs/superpowers/plans/2026-10-28-dictionary-phrases.md && git grep -n "dictionary-phrases"`
  Expected: no hits.

- [ ] **Step 10: Commit.** Restore `language`, `sleepTimeoutMinutes` and the chosen dictionary first.
  ```bash
  git add docs/dictionary.md
  git commit -m "docs: dictionary phrases: how a word is looked up in its phrase"
  ```

- [ ] **Step 11: Run the PR gate and open the PR.** The body carries:
  - a summary;
  - the host-test result;
  - the flash delta with its breakdown;
  - the speed medians against the baseline, per dictionary;
  - the heap comparison;
  - Step 1's table as found, with the screenshots on an orphan assets branch (`raw.githubusercontent.com` links, as the repo's earlier PRs do).

---

## Spec coverage

| Spec | Task |
|---|---|
| §5.1 the context: three before, 48 bytes, punctuation stops, edges, soft hyphens, built at Confirm | 3, 10 |
| §5.2 A: every start, two words covering the tapped word, longest then earliest, case, base form (also a stub's), A on a miss, one session, the extra read | 4, 8 |
| §5.2 what A shows: the phrase's entries, then the word's; the 16 KB cut; never the phrase's | 5, 8 |
| §5.3 B: reading bold runs, `∼`, placeholders, optional `to`, alternatives, unclosed bracket, matching rules, the table | 6 |
| §5.3 what B shows: the page and the mark, styled and plain | 9 |
| §5.4 C: the stub rule, the italic target after "↑", both cue lists in the table, one hop, the stub's line kept | 7, 8 |
| §5.5 header headword for A; the word screen unchanged before Confirm | 8, 10 |
| §4 speed gate, flash, RAM, first-hit behaviour | 2, 10, 11; every host task's full binary run |
| §7 host tests (A, B, C, the context) and the X4 run | 3-9, 11 |
| §8 left to the builder: mark, one case search, the context's home, the stub threshold, `ACT:DICT` | Rulings 4, 15-17, 21 |
| §9 risks: false matches, speed, other bold | 6 (`ALoneWordIsNoPhrase`), 11 Steps 2, 6, 7 |
