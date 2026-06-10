# Background B (section pre-analysis) — handoff for another PC

Self-contained pickup note. Pairs with the full design in
[epubreader-control-flow-refactor.md](epubreader-control-flow-refactor.md) §2.6–2.7.
This file is the "where we are, what's verified, exactly what to do next."

## Goal

Build the *next consecutive* section's cache during reader idle time, in small
loop-sized slices that resume from their last point — so crossing into that section
is instant instead of triggering a blocking "Indexing…" parse. Mirrors how Background A
(next-page pre-render) already works.

## Branch & history

Branch: **`feat-section-background-build`**.

The Background-B groundwork is committed and is an **ancestor of the current branch tip**
(other unrelated work — half-refresh/font fixes — sits on top; that's fine, it doesn't
touch the B code). Relevant commits, oldest→newest:

- `1e82830a` refactor(epubreader): RenderPass dispatch (render() carved into pass helpers,
  incl. `buildSection()` — the foreground build seam).
- `c60346d3` refactor(epubreader): `serviceBackgroundWork()` idle dispatcher (deferred-AA
  folded in; the slot where B will be called).
- `025c9314` feat(section): `stepSectionBuild()` skeleton + `BuildParams`/`BuildStep`.
- `473f0622` refactor(section): **carve** `createSectionFile` into
  `runBuildSetup`/`runBuildParse`/`runBuildFinalize` on a shared `Section::BuildState`.
- `61baac50` feat(epubreader): `DEBUG_BACKGROUND_WORK` overlay + serial counters.
- `71633966` docs: progress/resume point.

## Verified current state (checked against code, not assumed)

- `Section::stepSectionBuild()` exists but is **called from nowhere** — B is not wired in.
- `Section::runBuildParse(st, budgetMs)` **ignores `budgetMs`** and consumes the whole
  stream in one `epub->readItemContentsToStream(...)` call. No slicing yet.
- `BuildState` (defined in `Section.cpp`) holds: `BuildParams params`, `progressFn`,
  `propertyHash`, `localPath`, `contentBase`, `imageBasePath`, `inflatedSize`, `cssParser`,
  `lut`, `std::unique_ptr<ChapterHtmlSlimParser> visitor`, parse flags, timing. The visitor
  is heap-owned so its address (and its `&st.lut` capture) is stable across calls.
- `EpubReaderActivity::serviceBackgroundWork()` currently only calls
  `runDeferredGrayscalePass()`. Its comment already names B as the next slot, after A.
- `ZipFile` public API is still **one-shot**: only `readFileToStream(filename, Print&, chunkSize)`.
  No steppable inflate reader yet — that's sub-commit 3.
- `ChapterHtmlSlimParser` feed surface: `setup(totalInflatedSize)`, `write(buf,len)`
  (the `Print` sink that drives one yxml `feed`), `finalize()`. Already incremental per chunk.
- `DEBUG_BACKGROUND_WORK` defaults to **0**. Background A counters (`aRuns`/`aCompletes`)
  are wired in `renderPreRenderPass`; B counters/percent are inert until B runs.

## The one hard constraint (don't relitigate)

The parse is a synchronous push pipeline: `ZipFile` inflate loop → `out.write()` →
`ChapterHtmlSlimParser` (yxml `SaxParser.feed()`) → `onPageComplete()` serialises each
page to SD as it completes. **inflate has no seek and yxml's state IS its parse position**,
so you cannot tear down/rebuild mid-stream — you keep the live `ZipFile` reader + visitor
alive in `BuildState` and just *stop feeding* them for a tick. Consequence: a half-built
section can be **paused within a session only**, not across reboot/book-close. The blocking
foreground path stays as the fallback. See [[section-build-parse-pipeline-yield-seam]].

## Next steps (each a separate, buildable commit)

### Sub-commit 3 — resumable ZipFile reader (no Section change)
Add a steppable inflate reader to `ZipFile` so `Section` can own the pull side of the loop.
- New type, e.g. `ZipFile::EntryReader`, with: `open(filename)`, `step(uint8_t* out,
  size_t cap, size_t* produced, bool* done)` (inflate up to `cap` bytes), `close()`. This is
  essentially the body of the `while(true)` loop in `readFileToStream`
  (`lib/ZipFile/ZipFile.cpp` ~485–519) lifted into an object that holds `ZipInflateCtx` +
  the file handle live across calls.
- Keep `readFileToStream` working (reimplement it on top of `EntryReader`, or leave as-is).
- **Unit test** (host, `test/`): feed a fixture entry through `EntryReader::step` in small
  chunks and assert the concatenated bytes equal a one-shot `readFileToStream` of the same
  entry. Fixtures already exist under `test/fixtures/moby-dick/`.
- Heap note: `EntryReader` holds the inflate ring buffer + 2× `chunkSize` scratch for the
  reader's whole lifetime — justify it (it replaces the same buffers `readFileToStream` held
  transiently; net-neutral while a build is active).

### Sub-commit 4 — slice PARSE under budgetMs (the commit that actually yields)
In `Section`: make `runBuildParse` drive the `EntryReader` → `visitor->write()` in a loop,
checking `millis()` against `budgetMs`; return a new partial result (`BuildPhaseResult` gains
a "more work" value, or `stepSectionBuild` returns `BuildStep::More`) when over budget, with
`BuildState` retaining the live reader+visitor.
- Granularity: yield at the **page boundary** — a single `onPageComplete` SD write is
  un-yieldable. So "finish the current page, then yield if over budget," not hard preemption.
- `stepSectionBuild` becomes truly re-entrant: first call runs Setup + starts Parse; later
  calls resume Parse; the final call runs Finalize. `BuildState` moves from a stack local in
  `createSectionFile`/`stepSectionBuild` to a **`std::unique_ptr<BuildState>` Section member**
  so it survives across `stepSectionBuild` calls. `createSectionFile` (blocking) calls
  `stepSectionBuild(params, 0)` in a loop until terminal — keep it as the run-to-completion
  path so the foreground behaviour is unchanged.
- Wire B debug counters here: `bgCounters_.bRuns` per slice, `bgCounters_.bCompletes` on Done,
  `backgroundBuildPercent_` from bytes-inflated / `inflatedSize` (the parser already tracks
  this for `progressFn`).
- Set `SECTION_FILE_VERSION` is **not** needed (output format unchanged).

### Resume-on-consecutive-boundary-cross (mirror prerender invalidation)
- Tag the in-flight `BuildState`/its Section with the spine index it is building.
- When the user crosses into that **exact consecutive** section, the foreground
  `buildSection()` (render.cpp BuildSection pass) must **continue** the existing partial build
  to completion instead of starting fresh — i.e. detect "a live B BuildState for this spine
  exists" and pump `stepSectionBuild(params, 0)` to Done.
- On a **non-consecutive** jump (chapter select, percent, anchor, starred, footnote): discard
  the partial `BuildState` and build the target from scratch (blocking). Same lifecycle as
  `preRenderedPage.ready = false` invalidation on non-forward navigation.
- Param mismatch (font/margins changed) also discards — the property hash differs, so the
  partial cache is for the wrong variant.

### Sub-commit 5 — wire B into serviceBackgroundWork()
- In `serviceBackgroundWork()`, after the deferred-AA and **after Background A has finished**
  (`preRenderedPage.ready || no next page`), call `stepSectionBuild(nextSectionParams,
  budgetMs)` for `currentSpineIndex + 1` (only if that section's cache doesn't already exist).
- **Heap gates (refuse, don't OOM):** B runs with the secondary framebuffer **live** (the
  foreground reader needs it), so it has ~52 KB less headroom than the blocking path. Refuse
  to start B when the chapter needs embedded CSS and free/contig heap is below
  `EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES`/`..CONTIG..` (Section.cpp), and refuse when an image
  decode would need the 52 KB block. On any refusal, leave the section for the foreground
  blocking path.
- `budgetMs` sizing: same idle window Background A uses during the display waveform; start
  conservative (e.g. 30–50 ms) and tune via the serial counters.

## Watch-outs

- **CRLF churn (bit us before):** this machine's editor intermittently rewrites files to
  CRLF; the repo is LF with no `.gitattributes`. Before committing, `git status` may show huge
  EOL-only diffs in files you didn't touch (`yxml.c`, `bench/*`, …). Restore those to HEAD
  (`git checkout HEAD -- <file>`) and commit only files you actually edited. Consider adding
  `.gitattributes` with `* text=auto eol=lf` to end this permanently.
- **`DEBUG_BACKGROUND_WORK` stays committed as 0.** Flip to 1 locally for on-device testing
  (top of `EpubReaderActivity.cpp`); never commit it at 1.
- **Build to confirm before trusting a commit.** Use `pio run`. The last debug-instrumentation
  commit was re-applied from clean edits but its committed form wasn't re-compiled in-session —
  do a confirming `pio run` first thing.
- Host tests for the ZipFile reader: configure `test/` with CMake, build, run the new target.
  The yxml/host build uses static C++ runtime linking (already on the branch) to avoid a
  UCRT64/MINGW64 libstdc++ mismatch — see [[host-test-libstdcxx-dll-mismatch]].

## Quick orientation commands (run first on the new PC)

```
git log --oneline 1e82830a~1..HEAD        # see the B groundwork + what's on top
grep -rn stepSectionBuild lib/ src/       # confirm still unwired (only Section.{h,cpp})
grep -nA3 "runBuildParse(BuildState" lib/Epub/Epub/Section.cpp   # confirm budget still ignored
pio run                                    # confirm green baseline
```
