# Memory and parsing: implementation plan

Spec: [2026-11-18-memory-and-parsing-triage.md](../specs/2026-11-18-memory-and-parsing-triage.md) (the authority; each
task names its section). Every task is one pull request on its own branch, in its own worktree, from `origin/master`.
Tasks run in the spec's tracks ("Order of work"): serial inside a track, parallel across tracks.

## Global Constraints

- **The spec section a task names is its requirements.** Read the spec's "The rules the designs follow" and
  "Codecs" sections and the task's section in full. Where the code at master disagrees with the spec's line numbers,
  find the symbol; where it disagrees with the spec's premise, stop and report NEEDS_CONTEXT with the evidence.
- **Memory, in order:** the build's arena or the lent region when one is present and has room (`BuildArena`); else the
  heap, asked first (`heapHasBlockFor`, `heapReserveFor`, `makeUniqueNoThrow`, `ChunkedVector`), with a defined
  refusal (`ChapterHtmlSlimParser::stopForHeap` keeps the pages, or a softer exit that keeps the content); the S3 boards
  take the same path with a larger region (`HalCapabilities::buildsInPsram()`, `keepsSecondaryBuffer()`), never a
  board name. No content is dropped silently. `-fno-exceptions`: no throwing allocation whose size comes from data.
- **Read first:** `.skills/SKILL.md` (the agent guide) and `.claude/skills/heap-discipline/SKILL.md`; follow their
  self-review checklists.
- **Worktree:** from `C:/_development/witchhunt-reader`, `git fetch origin`, then
  `git worktree add -b <branch> <worktree> origin/master`, then in the worktree
  `git submodule update --init --reference C:/_development/witchhunt-reader/freeink-sdk freeink-sdk`. Work and commit
  only there. Never touch the main checkout or another task's worktree.
- **Host tests only, from the Bash tool (never PowerShell):** configure with
  `cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_SDFAT=/c/tmp/SdFat-2.3.1 -DGIT_EXECUTABLE="C:/Program Files/Git/cmd/git.exe"`,
  build single targets while iterating (`cmake --build build/test --target <T> -j 6`), and before the final commit
  build every target (`cmake --build build/test -j 6 -- -k 0`) and run `ctest --test-dir build/test -j 6`. Goldens:
  regenerate only intended changes with `UPDATE_GOLDENS=1 ctest --test-dir build/test -R EpubPipeline`, and explain
  every changed golden line in the report. Known Windows-only failures, not yours: tests that throw
  "filesystem error: cannot remove all ... book.bin" (a temp dir removed while a file is open), the heap figures in
  `OwnedBuildRegionTest.cpp:91` and `HeadingCarryTest.cpp:287`, `HeapPeakRegression` under load,
  `KOReaderProgressReplyTest` not built. Compare against `git stash`-free evidence: name the failures you saw.
- **No firmware builds, no devices, no COM ports, no push, no PR.** The controller builds the firmware, runs the
  device checks and opens the PR.
- **Formatting:** clang-format 21.1.8 at
  `C:/Users/jens/AppData/Local/Temp/claude/b6fb8d1046822d55/2abfc625-d9e9-4a99-a9a2-9470b7c5f110/scratchpad/cf21/clang_format/data/bin/clang-format.exe`
  (`-style=file -i` on each changed C++ file). The pre-commit hook formats with the `clang-format` on `PATH`; put that
  folder first on `PATH` (POSIX form, `/c/Users/...`) for `git commit`.
- **Commits:** `type(scope): summary`, lowercase, imperative; one concern per commit; no co-author trailer. A ported
  idea credits its source in the code comment, as the agent guide says.
- **Cache versions:** a task whose spec section says "bump `SECTION_FILE_VERSION`" bumps it to master's value plus
  one (`lib/Epub/Epub/Section.cpp`); the controller renumbers at merge when another PR took that number.
- **The open-work doc:** in the same PR, delete the item's section (or the part done) from
  `docs/future_work/memory-and-parsing.md` and keep its row in `docs/future_work/README.md` true.
- **Report:** write the full report to the report file the dispatch names; reply with the short contract only.

## Task 1: §10a, `Page::elements` grows through one helper

Branch `fix/parser-page-element-growth`, worktree `C:/tmp/wt-mp-10a`. Track L, first. Spec §10 "10a" and §10's
fault-injection paragraph. No section version bump.

## Task 2: §1, text inside a nested table

Branch `fix/nested-table-text`, worktree `C:/tmp/wt-mp-01`. Track T, first. Spec §1 in full, including its tests
(the `make_test_nested_tables.py` generator and the `AnchorMapTest` case). Bumps `SECTION_FILE_VERSION`.

## Task 3: §2, a `<br>` decides its gap where it stands

Branch `fix/br-ends-line`, worktree `C:/tmp/wt-mp-02`. Track B, first. Spec §2 in full, including the table of cases
(add the missing rows to `make_test_br_in_blocks.py`) and the risk paragraph's fallback for the last row. Bumps
`SECTION_FILE_VERSION`. The Calibre page count before and after is the controller's device step; give the controller
the host page counts of the fixtures that change.

## Task 4: §11, the `shrink_to_fit()` sweep

Branch `fix/shrink-to-fit-swap`, worktree `C:/tmp/wt-mp-11`. Track M. Spec §11 in full. No section version bump.

## Task 5: §7, a 1/8 decode reads the first DC scan only

Branch `perf/progressive-first-dc-scan`, worktree `C:/tmp/wt-mp-07`. Track J. Spec §7 in full.

## Task 6: §10b, the parser object and the TOC anchors

Branch `fix/section-parser-nothrow`, worktree `C:/tmp/wt-mp-10b`. Track M. Spec §10 "10b" and §10's fault-injection
paragraph. No section version bump.

## Task 7: §13a, each stylesheet's rule range and its imports

Branch `feat/css-sheet-ranges`, worktree `C:/tmp/wt-mp-13a`. Track C, first. Spec §13 "13a" (and read §13 whole for
what 13b will need). First settle the spec's open point: whether the tokenizer follows `@import` anywhere; if it does,
say so and leave the edges out. Bumps `CSS_CACHE_VERSION`, not `SECTION_FILE_VERSION`.

## Task 8: §3a, the truncated-chapter hint in every language

Branch `fix/truncated-hint-tr`, worktree `C:/tmp/wt-mp-03a`. Track M. Spec §3 "3a"; the owner decided §3 on
2026-11-18: **no hint for a simplified chapter** (option A).

- `TRUNCATED_SECTION_HINT_LINE_1/2` (`src/activities/reader/EpubReaderActivity.cpp`, near `:355`) become two `STR_*`
  keys read through `tr()`. Add them to `lib/I18n/translations/english.yaml` and translate them into every other
  language file. Line 2 names three settings ("No embedded style | No images | AA Off"): in each language use that
  file's own labels for those settings (find the keys the settings rows use in `src/SettingsList.h`), so the hint
  names what the reader will see in the menu. Reuse crosspoint-reader's wording where `upstream/develop`'s
  translation files carry the same English; machine-translate the rest and say so in the report. Use only characters
  the file already uses (the build fails on a glyph a UI face lacks). Keep each file's line endings.
- Never run `gen_i18n.py` by hand to regenerate the code tables (a manual run bloats the build by ~31 KB); the
  language packs are written only by `python scripts/gen_i18n.py --packs assets/sd-fonts` (`docs/i18n.md`): run that
  and commit the packs it changes.
- `docs/future_work/memory-and-parsing.md`: delete the "No hint when a chapter was simplified" section (record the
  owner's decision in the commit message) and add in its place an open item from spec §3 "3b": the page-elements
  limit (`Page::MAX_ELEMENTS`) drops the tail of a page; fix only with a fixture that reaches it, by ending the page
  at the limit, not with a hint. Keep its README row true.
- Test: the host i18n tests that exist (find them under `test/`), and the full suite once before the final commit.

## Task 9: §5a and §5b, a resumed decode makes progress before it parks again

Branch `perf/image-resume-progress`, worktree `C:/tmp/wt-mp-05a`. Track J. Spec §5 "5a" and "5b": after a resume the
decode ignores the abort poll until it has finished one band (progressive) or `kResumeMinRows` (4) MCU rows
(baseline); the `ImageParkTest` case raises input before the resume's first poll inside a `host_clock::Ticking` scope
and asserts the parked row advanced; the PR states the worst band time from the host clock. In
`docs/future_work/memory-and-parsing.md` rewrite the preempted-decodes item: the extraction is not lost work (it runs
to its end and renames its `.part`), its cost is page-turn latency behind the `RenderLock` (that is 5c, a device
measurement, stays open); remove the "resume parks at once" case once fixed; PNG and the resized 1/8 decode are not
worth it (5d). No cache change.

## Task 10: §8a, a failed baseline decode draws the placeholder once per session

Branch `fix/jpeg-failed-placeholder`, worktree `C:/tmp/wt-mp-08a`. Track J, after Task 9 (both touch
`JpegToFramebufferConverter.cpp`). Spec §8 "8a". The damaged-JPEG item in `docs/future_work/memory-and-parsing.md`
stays parked; note what 8a changed in it.

## Wave 2 (after wave 1 merged, 2026-11-19)

## Task 11: §9a, a table cell's line list is allocated once

Branch `fix/table-cell-lines-once`, worktree `C:/tmp/wt-mp-09a`. Track T (after §1, #832). Spec §9 "9a". Gate: every
golden byte-identical. No section version bump. This also settles §10's cell-lines site (`:6866` at the spec's base).

## Task 12: §10c, the per-element stacks grow through one helper

Branch `fix/parser-stack-growth`, worktree `C:/tmp/wt-mp-10c`. Track B (after §2, #834). Spec §10 "10c" and §10's
fault-injection paragraph (`STACK_FAULT_INJECTION`). No section version bump.

## Task 13: §14, an inline anchor is recorded at its own line

Branch `fix/inline-anchor-page`, worktree `C:/tmp/wt-mp-14`. Track L (after §10b, #836; §9b/§9c wait for a Linux
bench). Spec §14. Bumps `SECTION_FILE_VERSION` to master's value plus one.

## Task 14: §13b, a chapter's lookups use only the sheets it links

Branch `feat/css-scope-per-chapter`, worktree `C:/tmp/wt-mp-13b`. Track C (after §13a, #835). Spec §13 "13b", and
the 13a review's notes for this step: write the sheet table after the index patch so a valid table marks a complete
file, and treat a v36 cache without a valid table as damaged (delete and rebuild, not fail open); load the table with
the index (in the arena when resident); the 64-import cap drops the whole table (consider 128); `importsOpen` stays
true after `@namespace`/`@layer`. #835 cost +2,722 B of flash, more than recording a table suggests: find what it
added (is `loadSheetTable` linked though unused?) and keep 13b's flash small. Bumps `SECTION_FILE_VERSION` to
master's value plus one (it may collide with Task 13; the controller renumbers at merge).
