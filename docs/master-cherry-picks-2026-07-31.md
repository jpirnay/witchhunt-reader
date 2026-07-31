# Master cherry-picks — standalone-merit fixes from the Stage-1 / fresh-reader effort (2026-07-31)

> ## ⚠️ SUPERSEDED — do not follow Group A as written
>
> A later salvage pass (same day) verified this list against master's actual **source** and found that
> **all of Group A (A1 tjpgd, A2 ZipFile spine-stat cache, A3 img/svg dims) plus `BufferedFileIO.h` were
> ALREADY on master.** This document was written from the branch's commit list and could not see what
> master had gained independently.
>
> It also **missed** the picks that mattered — the OPF manifest-index abort fix (`6e899b4e` + `14ce6dee`)
> and `Epub::loadForCover()` (`5c45bc52`), which fixed a crash class still live on master. Those shipped
> as **PR #99**.
>
> **Read `docs/fresh-reader-handover-2026-07-31.md` §9 instead.** Group B below is still accurate
> (inert on master, correctly left on this branch) — but see §9.4 on B2, which sits on the code path
> implicated in the unresolved Small Gods truncation.

The fresh-reader (content.bin single-source) work is **PAUSED** (see
`docs/fresh-reader-handover-2026-07-31.md`). This document collects the fixes made during that effort
that have **standalone value on `master`**, independent of whether the fresh reader ever ships. Each is
listed with its source commit on `feat-stage1-extraction`, what it fixes, the risk, and how to apply it.

Branch off `master` (e.g. `fix/master-standalone-picks`) and apply in the order below. They are grouped
by coupling to `EPUB_STAGE1`: the first group is pure master-side value; the second only matters when the
Stage-1 pipeline is compiled but is harmless (inert) otherwise.

---

## Group A — pure master value (apply unconditionally)

### A1. tjpgd Windows host-build fix — `9f4ca076`
- **What**: `lib/TJpgDec/src/tjpgd.h` gated its manual `int`/`uint` typedefs on `_WIN32`, which collided
  with MinGW's real `<stdint.h>` and **broke every Windows host-pipeline build**. Narrowed the guard to
  `_MSC_VER < 1600` (old MSVC only), so MinGW uses the real stdint.
- **Why master**: unblocks host builds/tests on Windows/MinGW. Zero device impact.
- **Risk**: none. Pure `#if` narrowing.
- **Apply**: `git cherry-pick 9f4ca076` (drops the unrelated `docs/parser-stage1-step4-handover.md` hunk —
  keep only the `tjpgd.h` change, or `git cherry-pick -n` then `git restore --staged docs/...`).

### A2. ZipFile per-book spine-stat cache — `e44655f4`
- **What**: every spine open re-scanned the ZIP central directory (`O(entries)` per spine). Added a
  per-book spine-stat cache so the central-dir scan happens once, not per-spine. Touches `ZipFile.{cpp,h}`,
  `Epub.{cpp,h}`, `Section.cpp`, `bench_pagelayout/main.cpp`, `platformio.ini`.
- **Why master**: real perf win on multi-spine books (King's Avatar: 1732 spines → 1732 central-dir scans
  eliminated). Independent of Stage-1.
- **Risk**: low, but it's the largest of the picks (touches ZipFile + Epub). Has **one** `STAGE1` reference
  — check it cherry-picks clean onto master; if the guard doesn't apply, the cache logic itself is
  flag-independent. Verify with the existing `zip_entry_reader` host test + a device open of a big book.
- **Apply**: `git cherry-pick e44655f4`. Resolve any `platformio.ini` conflict by keeping master's env
  layout and only taking the cache-related bench define if present.

### A3. img/SVG explicit width+height for cover dims (ring-free) — part of `ab641e50`
- **What**: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` now captures `width`/`height` attributes on
  `<img>`/`<svg>` (via `atoi` in the IMAGE_TAGS attribute loop) and uses them FIRST — before the manifest,
  before reading the ZIP local-file header for intrinsic dims. This fixed a **blank titlepage** where the
  cover's dimensions were only discoverable by a ZIP-header read that OOM'd under the inflate ring.
- **Why master**: any book whose titlepage/cover declares explicit `width`/`height` (common: Calibre
  `<svg viewBox>` wrappers, e.g. Small Gods `titlepage.xhtml` = 455×751) renders without needing a
  ring-costly header read. Pure correctness/robustness; no Stage-1 dependency.
- **Risk**: low. It only *adds* a dims source that takes precedence when present; the manifest/header paths
  remain as fallback.
- **Apply**: this commit **bundles** the dims fix with fresh-reader-only borrow logic in
  `EpubReaderActivity.cpp`. Cherry-pick ONLY the `ChapterHtmlSlimParser.cpp` hunk:
  `git cherry-pick -n ab641e50 && git restore --staged --worktree src/activities/reader/EpubReaderActivity.cpp`
  then commit just the parser change.

---

## Group B — Stage-1 pipeline fixes (only bite when EPUB_STAGE1 is compiled; inert on plain master)

Master today does not compile the content.bin pipeline, so these are **latent** — apply them if/when the
Stage-1 work resumes or if master ever enables any part of it. They're documented here so they aren't lost.

### B1. FootnoteEntry zero-inits its full fixed arrays — `f1bb1db7`
- **What**: `writeBlock` serializes the full `FOOTNOTE_NUMBER_LEN`/`FOOTNOTE_HREF_LEN` bytes, but the
  ctor only set the leading NUL — tail bytes after the terminator were uninitialized garbage, so the same
  book compiled twice produced content.bin files differing in that padding (broke byte-identity / cache
  fingerprinting). Fix: `memset` the whole arrays.
- **Risk**: none. One-file, ctor-only.
- **Apply**: `git cherry-pick f1bb1db7`. Harmless on master (FootnoteEntry is a tiny struct; the memset is
  correct regardless of Stage-1).

### B2. Buffered temp-file reads during parse — `8c4bf1bf`
- **What**: the sliced parse fed the visitor 1 KB at a time straight off the temp XHTML — one physical SD
  read per chunk (~570 reads / ~1.1 s on Small Gods' single 570 KB spine). Added a read buffer so physical
  reads are batched (8 KB) while still feeding the parser in `PARSE_CHUNK_BYTES` units.
- **Risk**: low; localized to the tempExtract path.
- **Note**: only relevant with the Stage-1 sliced build. **Caution**: this touches the exact code path
  implicated in the unresolved "Small Gods commits only 8 blocks on device" bug (see fresh-reader handover
  §8.2) — if resuming Stage-1, fix that truncation FIRST, then re-validate this buffering.

### B3. Buffered content.bin writes — `d40962f0`
- **What**: content.bin was written with many tiny SD writes (the compile's dominant cost). Buffered them.
  Depends on `lib/Serialization/BufferedFileIO.h` (present on the branch).
- **Risk**: low; Stage-1-only.
- **Standalone spin-off for master**: `BufferedFileIO.h` is a **general-purpose buffered file wrapper**.
  Master's own section-file serialize does the same tiny-write pattern — applying `BufferedFileIO` there is
  an independent perf win. That's a small, self-contained follow-up (not a cherry-pick — a new small change
  on master using the header).

### B4. Giant-spine block-offset OOM — chunked write, lazy-seek read — `a04f9e6f` (+ `522aca51` streaming)
- **What**: the per-block offset table grew in RAM and OOM'd on giant single-spine books. Fixed with a
  chunked on-SD sidecar + lazy-seek reads (`a04f9e6f`), later refined to stream the table to a sidecar for
  O(1) resident memory (`522aca51`).
- **Risk**: Stage-1-only; do not port to master unless the content.bin path is compiled.

---

## Non-code lessons worth keeping (already in memory, restated for master work)

- **HWCDC busy-wait RX starvation**: a tight `while (!logSerial.available())` busy-wait starves the C3's
  USB-CDC RX task — always `delay()`/`yield()` in serial read-wait loops. (memory: `hwcdc-busywait-rx-starvation`.)
- **preventAutoSleep gates CPU clock**: when a long build runs off the loop, `preventAutoSleep()` must
  return true for its whole duration, or the CPU drops to 10 MHz and a 10 s parse becomes 124 s. (memory /
  fresh-reader handover §.) Master's section build already handles this via `hasActiveBuild()`.
- **SdFat = one handle per file**; **C3 real port is COM6 (COM3 = fake AMT SOL)**; **native USB
  re-enumerates on reset**. (fresh-reader handover §7.)

---

## Suggested apply order (Group A only, for an immediate master PR)

```
git checkout master && git pull
git checkout -b fix/master-standalone-picks
git cherry-pick 9f4ca076          # A1 tjpgd (drop the docs hunk)
git cherry-pick e44655f4          # A2 zip spine-stat cache (resolve platformio.ini)
git cherry-pick -n ab641e50       # A3 img/svg dims — then unstage the reader file:
git restore --staged --worktree src/activities/reader/EpubReaderActivity.cpp
git commit -m "fix: use explicit img/svg width+height for image dims (ring-free cover render)"
```

Then build `[env:default]` (master reader) and smoke-test: open a multi-spine book (perf), open Small Gods
(cover renders from declared SVG dims), run the Windows host test suite (tjpgd unblocked).

Group B stays on `feat-stage1-extraction` until the Stage-1 pipeline resumes.
