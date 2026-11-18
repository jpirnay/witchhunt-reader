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
