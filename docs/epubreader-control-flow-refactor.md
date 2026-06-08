# EpubReader control-flow refactor & Background-B design

This document is a concrete, executable plan. Part 1 is a pure-restructuring
cleanup that makes the reader's control flow read as the state machine it
already is. Part 2 is a *design* for the new "Background B" capability
(idle-time section pre-analysis) — it is not yet implemented and carries real
ESP32-C3 heap risk, so it is specified separately and gated behind Part 1.

## Progress / resume point (last updated mid-effort)

Work branch: **`feat-section-background-build`** (off `redesign-epubreader`, which holds
the Part 1 cleanup + the yxml merge + hardening). Firmware build is green at each commit
below.

Committed so far (newest first):
- `feat(epubreader)`: DEBUG_BACKGROUND_WORK overlay + serial counters (flag defaults 0).
  Background A counters are wired; B counters/percent stay idle until B is scheduled.
- `refactor(section)`: **sub-commit 2** — createSectionFile carved into
  runBuildSetup/runBuildParse/runBuildFinalize on a shared `Section::BuildState` (visitor
  now heap-owned). Behavior-preserving; parse still runs whole (budgetMs ignored).
- `feat(section)`: **sub-commit 1** — `stepSectionBuild()` skeleton + BuildParams/BuildStep.
- `refactor(epubreader)`: `serviceBackgroundWork()` idle dispatcher (deferred-AA folded in).
- Part 1 (RenderPass dispatch) + yxml merge/hardening landed on `redesign-epubreader`.

**Next up — sub-commit 3** (see §2.7): add a resumable/steppable reader to `ZipFile`
(open an entry, "inflate up to N bytes" steppable object) and unit-test it against the
one-shot `readFileToStream` (same bytes out). No `Section` change yet. Then sub-commit 4
slices PARSE under `budgetMs` (wire B counters there), then resume-on-boundary-cross, then
sub-commit 5 wires B into `serviceBackgroundWork()` behind the A-before-B + heap gates.

Watch-outs when resuming:
- The editor on this machine intermittently converts files to **CRLF**; the repo is LF and
  has no `.gitattributes`. Before committing, `git status` may show large EOL-only diffs in
  files you didn't touch (yxml.c, bench/*, etc.) — restore those to HEAD, don't commit the
  churn. Only commit files you actually edited.
- `DEBUG_BACKGROUND_WORK` must be committed as `0`; enable locally for testing only.

## Target logical flow (from the request)

1. Consume a page from the section cache, render it, display on e-ink.
2. If there is no section cache or the section changed, establish a section
   read + parse, creating that cache.
3. Wait for a button press; decide the action (page nav, menu, render-param
   change + rerender).
4. **Background A** — during idle, render the next logical page.
5. **Background B** — during idle, analyse the next section, *only if Background
   A is finished*.

None of these is intrinsically coupled; the reader is a dispatcher that hands
small idle time-slices to cooperative background routines that can do partial
work and resume from their last cursor.

## Where the code stands today

| Logical step | Today | State |
|---|---|---|
| 1 consume+render+display | `render()` → `loadPageFromSectionFile()` → `renderContents()` | present |
| 2 build section on miss/change | `render()` lazy-builds when `!section` via `Section::createSectionFile()` | present, **blocking** |
| 3 button dispatch | `loop()` consumes `buttonEvents`, dispatches | present |
| 4 Background A | `pendingPreRender` → `renderPageContentOnly()` into framebuffer | present, clean |
| 5 Background B | — | **absent; Section API can't support it yet** |

The architecture you describe is *mostly already here* — but steps 1, 2, 4 and
the fast page-turn display path are all multiplexed inside one ~430-line
`render()` method, selected by booleans captured at the top
(`isPreRenderPass`, `isBufferDisplayPass`, `currentSpineIndex == spineCount`,
`!section`, else). That is the "encode intent in ad-hoc bools + nesting" shape
the control-flow-clarity guidance calls out. Background A is already a clean
cooperative-scheduling model (scheduled by a flag, run opportunistically during
the ~2–4 s display waveform, heap-gated, discarded on page turn) — it is the
proven pattern the rest should follow.

---

## Part 1 — render() dispatcher cleanup (safe, no behavior change)

### 1.1 Introduce an explicit pass enum

`render()` already selects exactly one of a **closed set** of modes. Make it a
type. In `EpubReaderActivity.h`:

```cpp
// One render() invocation services exactly one pass. Selected from the pending
// flags at entry; see classifyRenderPass(). Exhaustive switch, no default — a
// new pass must be handled explicitly.
enum class RenderPass : uint8_t {
  FinishedBook,   // currentSpineIndex == spineCount: hand off to FinishedBookActivity
  PreRender,      // Background A: render next page content into the framebuffer only
  BufferDisplay,  // fast page-turn: framebuffer already holds content; add status bar + flush
  BuildSection,   // step 2: no section loaded → build/load the cache, then render
  Normal,         // step 1: section present → load page, render, display
};
```

### 1.2 Extract a classifier

A small pure function turns the current top-of-`render()` flag juggling into one
readable decision. It must preserve today's precedence and the
`isRefreshPending()` fall-through:

```cpp
RenderPass EpubReaderActivity::classifyRenderPass() const {
  if (currentSpineIndex == epub->getSpineItemsCount()) return RenderPass::FinishedBook;
  if (pendingPreRender)                                 return RenderPass::PreRender;
  // BufferDisplay only when the prior waveform has settled; otherwise fall back
  // to a full Normal render (current behavior at line ~1888).
  if (usePreRenderedBuffer && !renderer.isRefreshPending()) return RenderPass::BufferDisplay;
  if (!section)                                          return RenderPass::BuildSection;
  return RenderPass::Normal;
}
```

Note: the existing code *captures and clears* `pendingPreRender` /
`usePreRenderedBuffer` early (lines 1875–1881) and clears
`preRenderedPage.ready` when neither pre-render flag is set. Preserve that:
clear the consumed flags inside `render()` right after classification, before
dispatch, so re-entrancy semantics are unchanged.

### 1.3 Extract one helper per pass

Lift the existing blocks verbatim (no logic change) into named methods. Line
ranges are from the current `EpubReaderActivity.cpp`:

| New method | Body lifted from | Notes |
|---|---|---|
| `renderFinishedBookPass(RenderLock& lock)` | 1793–1838 | the `currentSpineIndex == spineCount` block |
| `renderPreRenderPass(...)` | 1913–1935 | Background A; takes the oriented margins |
| `renderBufferDisplayPass(...)` | 1888–1910 | fast page-turn display |
| `buildSection(...)` | 1937–2092 | **the Background-B seam** — see Part 2 |
| `renderNormalPass(RenderLock& lock, ...)` | 2094–2186 | clearScreen → load page → renderContents → progress save |

The shared margin/viewport setup (1840–1869) stays at the top of `render()`
before dispatch, since four of five passes need it. `render()` then becomes:

```cpp
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) return;
  recoverSecondaryBufferIfNeeded();          // 1766–1774, extracted for clarity
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) { /* 1777–1781 */ return; }
  clampSpineIndex(spineCount);               // 1784–1790

  // … margin/viewport/lastRenderStats setup (1840–1869) …

  const RenderPass pass = classifyRenderPass();
  consumeRenderPassFlags(pass);              // clear pendingPreRender/usePreRenderedBuffer + stale ready

  switch (pass) {
    case RenderPass::FinishedBook:   renderFinishedBookPass(lock); return;
    case RenderPass::PreRender:      renderPreRenderPass(/*margins*/); return;
    case RenderPass::BufferDisplay:  renderBufferDisplayPass(/*margins*/); return;
    case RenderPass::BuildSection:   if (!buildSection(/*…*/)) return; [[fallthrough]];
    case RenderPass::Normal:         renderNormalPass(lock, /*margins*/); return;
  }
}
```

The `BuildSection → Normal` fallthrough mirrors today's behavior exactly: after
a successful build the same method continues into the normal load+render path
(today this is just straight-line code after the `if (!section)` block). Keep it
explicit with `[[fallthrough]]` so the intent is visible. No `default:` — a new
`RenderPass` value must force a compile error per the codebase's switch
discipline.

### 1.4 Verification for Part 1

- Pure extraction: each helper's body is the current code moved, not rewritten.
- Build host tests + a device smoke test: open a book, page forward/back across
  a chapter boundary (exercises BuildSection), rapid page turns (exercises
  PreRender + BufferDisplay), open the finished-book flow at end of book.
- `git diff` should show movement, not new conditionals. Run `/code-review` to
  confirm no behavioral drift.

This part is independently shippable and should land as its own commit(s),
one concern each, per refactor-for-review.

---

## Part 2 — Background B design (idle-time section pre-analysis)

**Status: design only. Do not implement before Part 1 lands and the heap risk
below is accepted.**

### 2.1 Why it can't be bolted on today

`Section::createSectionFile()` ([Section.h:70](../lib/Epub/Epub/Section.h#L70))
is a single blocking call. It:

- streams the whole spine item through a SAX push-parser in one shot —
  `epub->readItemContentsToStream(localPath, visitor, 1024)`
  ([Section.cpp:513](../lib/Epub/Epub/Section.cpp#L513)) — with no way to stop
  after N chunks and resume;
- has a heavy one-shot memory choreography in the caller (`render()` ~1976–2017):
  drop SD font metadata → release the ~52 KB secondary framebuffer → build →
  warm image caches → realloc framebuffer → restore font metadata, all behind a
  blocking "Indexing…" popup.

Neither is idle-time-slice friendly. Background B is therefore a **new
capability**, not a refactor.

### 2.2 Two implementation options

**Option A — chunked resumable parser (true partial work).**
Add a resumable entry point to `Section` that owns the parser across calls:

```cpp
// Returns Done when the section is fully parsed, More when it yielded mid-way
// after roughly budgetMs of work, Failed on I/O/parse error.
enum class BuildStep { More, Done, Failed };
BuildStep stepSectionBuild(/* same render params */, uint32_t budgetMs);
```

Internally this requires hoisting the locals currently scoped inside
`createSectionFile()` (the `ChapterHtmlSlimParser visitor`, the stream cursor,
`lut`, `cssParser`) into a heap-allocated `BuildState` member kept alive between
calls, and replacing the one-shot `readItemContentsToStream(...)` with a
pull-N-bytes-then-yield loop that checks an elapsed-time budget. The SAX visitor
is already push-based and stateful, so feeding it incrementally is feasible — the
work is making the *driver* re-entrant and persisting the stream offset.

- Pros: matches the request precisely (small slices, resume from cursor).
- Cons: the memory choreography (framebuffer release/realloc, font-metadata
  drop/restore) cannot straddle yields — the framebuffer must stay live so the
  foreground reader can still display the *current* page between B's slices. So
  B's parse must run **without** releasing the secondary buffer, i.e. under a
  tighter heap budget than the blocking path enjoys. On embedded-CSS chapters
  that already gate on `EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES`
  ([Section.cpp:387](../lib/Epub/Epub/Section.cpp#L387)), B must refuse to start
  and leave the chapter for the blocking foreground path. This is the core risk.

**Option B — opportunistic full prebuild on a generous idle window (simpler).**
Don't make the parser resumable. Instead, when the reader is deeply idle (no
input for a threshold, Background A already complete, heap healthy), build the
*entire* next section in one shot on the loop task, reusing the existing
`createSectionFile()` path, then keep that `Section` cached. A page turn arriving
mid-build is impossible to interrupt cleanly, so B only starts when idle is
confirmed and accepts that the build runs to completion once begun.

- Pros: reuses the proven blocking path; far less code; no re-entrant parser.
- Cons: not "small time slices" — it's one long slice. A button press during the
  build is delayed until it finishes (hundreds of ms to seconds). Mitigable by
  only triggering after a long idle threshold, but it is a weaker fit for the
  stated model.

### 2.3 Recommended shape

Implement the **idle scheduler** generically (it's useful regardless of A/B),
then start with **Option B** behind it, and only escalate to Option A if the
button-latency-during-prebuild proves unacceptable in practice:

```cpp
// Called from loop() when no input is pending and no waveform is in flight.
void EpubReaderActivity::serviceBackgroundWork() {
  if (pendingGrayscale_.active) return;          // deferred AA has priority (visible quality)
  if (scheduleBackgroundARenderIfNeeded()) return;   // A before B (the stated rule)
  if (!backgroundAComplete()) return;            // gate: B only after A finished
  scheduleBackgroundBPrebuildIfIdle();           // B: prebuild next section
}
```

The A-before-B priority maps directly onto the request. `backgroundAComplete()`
is `preRenderedPage.ready || next-page-doesn't-exist`. B's heap gate reuses the
existing `PRE_RENDER_MIN_FREE_HEAP_BYTES` style check, plus the embedded-CSS
refusal above.

### 2.4 Risks & gates before implementing

- **Heap.** B competing with a live framebuffer is the dominant risk on
  ESP32-C3. Gate hard; prefer refusing to start over OOM. Validate with the
  `DEBUG_MEMORY_CONSUMPTION` snapshots already in the reader.
- **Cache variant explosion.** A prebuilt next section uses the *current* render
  params; if the user changes font/margins, the prebuild is wasted (handled by
  the existing property-hash cache naming + `evictOldVariants`, so it's
  correctness-safe, just wasted work). Only prebuild when params are stable.
- **Scope.** This adds firmware surface for a reading-speed benefit. Run it past
  the scope-discipline RAM-cost-vs-reading-benefit gate before building.

### 2.6 Sequentializing createSectionFile into loop-sized slices

> **Update (yxml integrated).** This branch has merged `feat-yxml-saxparser-clean`,
> replacing Expat with **yxml behind a `SaxParser` abstraction**
> ([lib/SaxParser/SaxParser.h](../lib/SaxParser/SaxParser/SaxParser.h)). The analysis
> below was written against Expat; the structural conclusion is unchanged, but the
> seam is now *cleaner*, and two specifics shift:
> - The push API is explicit and slice-friendly: `feed(buf, len)` (incremental),
>   `stop()` (early abort — already wired so a stale half-parse can be torn down on
>   navigation), and `byteOffset()` (first-class cursor, replaces
>   `XML_GetCurrentByteIndex`). "Expat parser + SAX state" in the table below is now
>   "`SaxParser` (yxml) + SAX state" held in `ChapterHtmlSlimParser::saxParser_`.
> - yxml's resident state is **smaller and fixed-capacity** (no per-document heap
>   growth) — measured `SaxParserImpl` ~10 KB. This directly eases the heap-budget
>   risk that §2.4 flagged as dominant. The fixed caps truncate-and-flag rather than
>   overflow; this branch widened the two content-HTML-risky caps (`kMaxAttrs` 8→12,
>   `kAttrValueLen` 256→384) and made the state alloc fallible. See
>   [[section-build-parse-pipeline-yield-seam]].
> - yxml's `byteOffset()` semantics differ from Expat's (past the tag name, not the
>   event-start index); `SECTION_FILE_VERSION` was bumped 44→45 for the persisted LUT
>   seek hints. A resumable build must read offsets in yxml terms.
>
> Net: building Background B on the live yxml `SaxParser.feed()`/`stop()` API is both
> the consistent choice and the lower-risk one. The rest of this section still holds.


This is the heart of Background B: can `Section::createSectionFile()` be broken into
blocks that each fit one epubreader loop tick, run "on top of each other" (i.e. one
after another across successive ticks), and resume from a saved cursor? Below is the
block decomposition grounded in the actual call graph.

#### The execution pipeline (what actually runs)

`createSectionFile()` ([Section.cpp:376](../lib/Epub/Epub/Section.cpp#L376)) is a
straight-line function with three temporal phases:

```
SETUP (cheap, bounded)              Section.cpp:381–509
  evictOldVariants, property hash, open output file, write header,
  load CSS from cache, collect TOC anchors, load pagelist, visitor.setup()

PARSE (expensive, unbounded)        Section.cpp:512–528   ← the whole cost lives here
  readItemContentsToStream(localPath, visitor, 1024)
    └─ ZipFile::readFileToStream  ZipFile.cpp:485  while(true) inflate loop
         └─ out.write(chunk)       → ChapterHtmlSlimParser (Expat push parser)
              └─ XML_Parse(...)    → SAX callbacks build currentPage
                   └─ completePageFn(page) → Section::onPageComplete → serialize page to SD
  visitor.finalize()              flush last partial page

FINALIZE (cheap, bounded by pageCount)  Section.cpp:562–674
  write LUT, anchor map, page-break labels, paragraph LUT, patch header,
  buildTocBoundaries, reopen file for read
```

Key measured fact: the build is logged as `total = stream + setup + parse + finalize`
([Section.cpp:672](../lib/Epub/Epub/Section.cpp#L672)). **PARSE dominates**; SETUP and
FINALIZE are both O(pageCount) bookkeeping. So slicing only has to attack PARSE.

#### The single natural yield seam

There is exactly one loop that runs long enough to slice: the inflate `while (true)`
in `ZipFile::readFileToStream` ([ZipFile.cpp:485–519](../lib/ZipFile/ZipFile.cpp#L485-L519)).
Each iteration inflates up to `chunkSize` (1024) bytes and pushes them to
`out.write()`, which drives one `XML_Parse` step. **One iteration ≈ one slice.** A
budget check (`millis() - sliceStart >= budgetMs`) placed at the top of that loop is
the whole mechanism — break out, return "More", resume next tick.

Everything downstream of `out.write()` is already incremental and stateless *between
chunks* at the call level: Expat keeps its own resumable state, and `onPageComplete`
already serializes each finished page to SD as it goes ([Section.cpp:185](../lib/Epub/Epub/Section.cpp#L185)),
so a page that completed in slice N is durably on disk before slice N+1 runs. We are
not buffering the whole chapter in RAM — that part of the design is already done for us.

#### What state must survive a yield (the hard part)

To pause mid-PARSE and resume, the following must persist across loop ticks instead of
living on the `createSectionFile` stack frame:

| State | Where it is today | Size | Note |
|---|---|---|---|
| `ZipFile` + open SD handle + inflate ring buffer | `ScopedOpenClose` local in readFileToStream | ring buffer + 2×1024 scratch | held open the whole parse; closing/reopening mid-stream means re-seeking the deflate stream — **inflate cannot resume from an arbitrary byte**, so the file + inflate ctx must stay live |
| Expat parser + SAX state | `ChapterHtmlSlimParser visitor` local ([Section.cpp:489](../lib/Epub/Epub/Section.cpp#L489)) | large: depth stack, `currentPage`, `currentTextBlock`, CSS context, float stack | the bulk of the resident state; must be heap-owned by the Section across ticks |
| Output `file` (section .bin being written) | `Section::file` member | handle | already a member — good |
| `lut` vector (page offsets so far) | local in createSectionFile ([Section.cpp:435](../lib/Epub/Epub/Section.cpp#L435)) | 4 B × pages-so-far | must move to a member |
| CSS parser (loaded rules) | `Epub::cssParser` ([Section.cpp:444](../lib/Epub/Epub/Section.cpp#L444)) | 50–150 KB | already a member; stays loaded across the whole build either way |

This is the crux: **the inflate stream and the Expat parser cannot be torn down and
rebuilt between slices.** zlib/inflate has no seek; Expat's state is its parse position.
So "resume from cursor" really means "keep both objects alive in a heap-owned
`BuildState` and just stop feeding them for this tick." The cursor is implicit — it's
the live position of two stateful objects, not a byte offset we can persist to disk and
reload. **Consequence: a half-built section cannot survive a reboot or a book close;
it can only be paused within one reader session.** That is acceptable (the foreground
blocking path remains the fallback), but it must be a conscious design decision.

#### The memory-choreography conflict (the real blocker)

The blocking path frees ~52 KB by releasing the secondary framebuffer for the *entire*
build and reallocating after ([buildSection](../src/activities/reader/EpubReaderActivity.cpp),
the `releaseSecondaryBuffer()` / `reallocSecondaryBuffer()` pair). A sliced background
build **cannot hold the framebuffer released across ticks** — the foreground reader needs
it to display the *current* page between slices. So Background B's parse must run with
the secondary buffer *live*, i.e. under ~52 KB less headroom than the blocking path
assumes. Combined with the large resident Expat + CSS state, this is the dominant risk
and forces two hard gates:

- B refuses to start (defers to the foreground blocking path) when the chapter needs
  embedded CSS and free/contig heap is below the existing
  `EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES` thresholds ([Section.cpp:387](../lib/Epub/Epub/Section.cpp#L387)).
- B refuses to start when an image warm/decode would need the ~52 KB block that the
  live framebuffer is now occupying.

#### Proposed block sequence (the answer to "can these be sequentialized")

Yes — into four ordered, resumable steps, only one of which loops:

```
enum class BuildStep { Setup, Parse, Finalize, Done, Failed };

stepSectionBuild(params, budgetMs):
  switch (state.step):
    Setup:    run Section.cpp:381–509 once (cheap, fits one tick); → Parse
    Parse:    feed inflate→Expat in a loop until budgetMs elapsed or stream Done;
              return More if time ran out (state stays in Parse), else → Finalize
    Finalize: run Section.cpp:562–674 once (cheap, O(pageCount)); → Done
    Done/Failed: terminal
```

- SETUP and FINALIZE each comfortably fit one tick (both are bounded bookkeeping), so
  they don't need internal slicing — only PARSE does.
- PARSE is the one re-entrant loop, sliced at the inflate-iteration boundary.
- The scheduler from §2.3 calls `stepSectionBuild(..., budgetMs)` once per idle tick
  while it returns `More`, with `budgetMs` sized to the loop's idle budget (the same
  window Background A uses during the display waveform). A button press between ticks is
  honoured immediately — the half-built state just sits until the next idle tick, or is
  discarded if the user navigated away.

#### Slice-size sanity check

The budget check is cheap and the slice granularity is one inflate chunk (1024 inflated
bytes → a small number of `XML_Parse` calls). That is far finer than a loop tick needs,
so `budgetMs` can be tuned freely (e.g. feed N chunks, then check the clock) without the
risk of a single un-yieldable unit blowing the budget — *except* one: a single
`onPageComplete` serializes a full page to SD ([Section.cpp:185](../lib/Epub/Epub/Section.cpp#L185)),
and that SD write is itself un-yieldable. So the practical minimum slice is "until the
current page boundary," and `budgetMs` should be read as "finish the page you're on,
then yield if over budget," not a hard preemption. For typical pages this is tens of ms —
acceptable for the reader loop. Pathologically large pages (huge tables) remain a single
un-sliceable unit; the completeness/log rule from the skill applies: `log()` when a page
exceeds the budget so it's visible rather than silent.

### 2.5 Suggested commit sequence

1. Part 1.1–1.3: `RenderPass` enum + `classifyRenderPass` + extract the five
   pass helpers (one commit, pure restructuring). **DONE.**
2. Part 1.3: extract `buildSection()` cleanly as the future B seam (can fold into
   commit 1 or stand alone). **DONE.**
3. Extract `serviceBackgroundWork()` + fold the existing Background A scheduling
   and deferred-AA pass into it (behavior-preserving; sets up the scheduler).
4. *(gated on acceptance — the Option A path, now the recommended one given §2.6)*
   Hoist the PARSE-phase locals (`ZipFile`/inflate ctx, `ChapterHtmlSlimParser`
   visitor, `lut`) into a heap-owned `Section::BuildState`, and split
   `createSectionFile` into `stepSectionBuild(params, budgetMs)` returning
   `BuildStep::{Setup,Parse,Finalize,Done,Failed}`. SETUP/FINALIZE run whole;
   PARSE slices at the inflate-loop boundary (page-boundary granularity).
5. Wire `stepSectionBuild` into `serviceBackgroundWork()` behind the A-before-B
   gate + the two heap refusals (embedded-CSS threshold, framebuffer-live image
   decode). On any refusal or a navigation into the section, fall back to the
   existing blocking `buildSection()` foreground path.

Note: §2.6 supersedes the earlier "start with Option B" suggestion. Because the
parse is already a chunked inflate→Expat push pipeline that serializes each page
to SD as it goes, the resumable Option A is less work than first assumed and is the
better fit — Option B (one long idle slice) buys little once the inflate loop is
already the slice boundary.

### 2.7 Step 4 implementation plan — bisectable sub-commits

Branch: `feat-section-background-build` (on top of the verified groundwork).

**The driver-ownership complication.** The inflate `while(true)` loop lives *inside*
`ZipFile::readFileToStream` ([ZipFile.cpp:485](../lib/ZipFile/ZipFile.cpp#L485)), not in
`Section`. Today `Section` calls `epub->readItemContentsToStream(localPath, visitor, 1024)`
and that call only returns when the whole entry is inflated. To yield mid-parse, `Section`
must own the pull side of the loop instead of handing it to `ZipFile`. Two ways:

- **(a) Inversion via a resumable ZipFile reader.** Add a `ZipFile` API that opens an entry
  and exposes "inflate up to N bytes into this buffer, return produced + done-flag"
  (essentially the body of the `while` loop as a steppable object). `Section::BuildState`
  holds that reader + the visitor and pumps `reader.step()` → `visitor.feed()` until the
  time budget is hit. This is the clean design and keeps inflate state encapsulated in
  `ZipFile` where it belongs.
- **(b) Budgeted callback.** Keep `readItemContentsToStream`, but give the inflate loop a
  per-iteration callback that can signal "pause." Less code, but it leaks the yield concern
  into `ZipFile` and still needs the loop's locals to persist — so it ends up needing (a)'s
  state object anyway. Prefer (a).

**BuildState member set (classified by phase).** Every local in `createSectionFile`
sorted by which phase first needs it and whether it must survive a yield:

| Member | Set in | Used by | Why it must persist |
|---|---|---|---|
| `BuildParams params` | entry | all | re-passed every slice |
| `uint32_t propertyHash` | Setup | Setup, Finalize (image evict on fail) | derives filePath / imageBasePath |
| `std::string localPath` | Setup | Parse (stream source), Finalize (logResolveStats) | stream identity |
| `std::string contentBase, imageBasePath` | Setup | Parse (visitor ctor) | parser config |
| `size_t inflatedSize` | Setup | Parse (visitor.setup, progress), Finalize (fileSize) | total size |
| `CssParser* cssParser` | Setup | Parse, Finalize (clear) | loaded rules |
| `std::vector<uint32_t> lut` | Setup(empty)→Parse(filled) | Finalize (write LUT) | per-page offsets accumulated across slices |
| `std::unique_ptr<ChapterHtmlSlimParser> visitor` | Setup | Parse (fed incrementally) | **the live SAX/yxml state — the whole point** |
| `BuildStep phase` | entry | step dispatch | resume cursor |
| timing stamps (`phase*Start`) | each phase | logging | cosmetic; can be per-call |

Note `visitor` becomes a heap `unique_ptr` (it's currently a stack local) so it outlives
a single `stepSectionBuild` call. `file` and `pageCount` are already `Section` members.

**The two fallback recursions stay in the entry function, not the phase methods:**
- Heap-too-low → no-CSS retry ([Section.cpp:385–398](../lib/Epub/Epub/Section.cpp#L385-L398))
  runs *before* any state exists; it stays at the top of `createSectionFile`/the build
  entry and just re-enters with `embeddedStyle=false`.
- Parse-failed-with-CSS → no-CSS retry ([Section.cpp:539–551](../lib/Epub/Epub/Section.cpp#L539-L551))
  happens after Parse; in the carved form the Finalize method reports "retry no-CSS" as a
  distinct result, and the **entry function** tears down the BuildState and restarts from
  Setup with `embeddedStyle=false`. The sliced path treats this the same way — a restart is
  a fresh BuildState, which is acceptable (rare, and a parse failure means the partial work
  is unusable anyway).

This keeps the recursion/​fallback logic in one place and the three phase methods purely
linear, which is what makes them safe to call either blocking (all three in a row) or
sliced (Parse re-entered across ticks).

**Sub-commit sequence (each builds + is behavior-neutral until the last):**

1. **Skeleton, no yield.** Add `enum class Section::BuildStep { Setup, Parse, Finalize,
   Done, Failed }` and `BuildStep stepSectionBuild(const BuildParams&, uint32_t budgetMs)`.
   `BuildParams` is a struct bundling the 10 render params + the two control flags (replaces
   the long `createSectionFile` argument list at the call site). For this commit,
   `stepSectionBuild` simply calls the existing `createSectionFile(...)` to completion and
   returns `Done`/`Failed` — `budgetMs` ignored. No `BuildState`, no parser hoisting. Pure
   API addition; `createSectionFile` untouched. Lets the call site migrate to the new entry
   point first.
2. **Introduce `BuildState`, run SETUP once.** Move the SETUP-phase locals
   ([Section.cpp:404–509](../lib/Epub/Epub/Section.cpp#L404-L509)) into a heap-owned
   `std::unique_ptr<BuildState>` member; `stepSectionBuild` runs SETUP on first call, stores
   the state, then runs PARSE+FINALIZE in one shot (still no yield). Verifies the state
   container holds the visitor/lut/cssParser correctly across the Setup→Parse boundary.
3. **Resumable ZipFile reader (design (a)).** Add the steppable inflate reader to `ZipFile`,
   unit-test it on a fixture against the one-shot `readFileToStream` (same bytes out). No
   `Section` change yet.
4. **Slice PARSE.** `stepSectionBuild` pumps the resumable reader → visitor under a
   `budgetMs` clock, returning `Parse`/`More` when time runs out, `Finalize` when the stream
   ends. FINALIZE still runs whole. This is the commit that actually yields.
5. **Wire into `serviceBackgroundWork()`** behind the A-before-B gate + heap refusals, with
   the foreground `buildSection()` blocking path as the fallback on refusal or navigation.

The blocking `buildSection()` foreground path keeps calling `createSectionFile` (or the
run-to-completion `stepSectionBuild`) throughout — Background B is purely additive until
sub-commit 5.
