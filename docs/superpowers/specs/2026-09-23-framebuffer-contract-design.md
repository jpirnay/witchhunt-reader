# Making the write-framebuffer contract hold — design

> **Status.** Written before the per-controller baseline audit and before the cause of the X3
> sleep ghosting was found. The SDK change it leans on in *Non-goals* ("the caller-side mistake
> is already structurally impossible: `cleanupGrayscaleWithPreviousBuffer()` takes no pointer")
> was reverted with the branch, so that sentence is currently **false** — the fallback is back.
> The four mechanisms are unchanged and still cover findings A/B (mechanisms 1–3), D and J
> (mechanism 4) of `x3-sleep-ghosting-hypotheses.md` §3. Not scheduled; kept as the agreed
> lighter design.

Companion to `docs/framebuffer-content-audit-2026-09-23.md`, which is the *why*. This is
the *what*: four small mechanisms that keep the audit's conclusions true without anyone
having to have read it.

## Problem

The rule is one sentence — **if you draw on top of what the user is looking at, the write
framebuffer has to hold what the user is looking at first** — and it was enforced nowhere.
Each call site performed its own two-step dance (`syncWriteBufferFromDisplayed()` then
`prepareFramebufferForCapture()`) and explained it in a paragraph of comment. The audit
found five defects from that arrangement, and one of the explanatory comments had been
confidently wrong long enough to propagate into a second file.

Documentation alone has already been tried here and did not hold, because the knowledge
lived at call sites: every new call site had to find it and re-derive it. The fix is to
move the knowledge into one routine and one hook, so the default path is the correct one.

## Non-goals

Recorded so they are not re-proposed:

- **No RAII guard.** Considered and rejected. A guard earns its place when something must
  be undone at scope exit; establishing a precondition has nothing to release. It would be
  a function wearing a costume.
- **No change to `drawPopup` / `drawBusyIndicator` signatures.** Replacing
  `overlayDisplayedFrame` with typed overloads would touch both themes and every popup
  call site, and most of those sites pass `false` and were never part of the problem.
- **No new type in the SDK for "the displayed frame".** The caller-side mistake is already
  structurally impossible: `cleanupGrayscaleWithPreviousBuffer()` takes no pointer. What
  remains is a driver-side risk, and a test covers that more cheaply than a type.

## Mechanism 1 — one routine replaces the two-step dance

```cpp
// Activity
// Make the write framebuffer hold the frame that is on the panel, so this activity can draw
// ON TOP of it instead of repainting. Returns false when that could not be established --
// then repaint fully, do not composite.
bool ensureWriteBufferShowsPanel();

// ActivityManager, for transitions and for callers outside any activity
bool ensureWriteBufferShowsPanel();   // delegates to currentActivity
```

Implementation is the existing two steps, in the existing order, in one place:
`renderer.syncWriteBufferFromDisplayed()`; if that returns false, `redrawVisibleFrame()`;
the result is the return value.

Every current site becomes one checked line. Sites that composite unconditionally today
gain a fallback branch they did not have.

## Mechanism 2 — a hook the base class asks for

`Activity::prepareFramebufferForCapture()` → `virtual bool redrawVisibleFrame()`, default
`return false` ("I cannot"). The rename is not the point; being *asked* is. A new activity
or reader type sees an override its siblings answer, rather than needing to find a comment.

- `EpubReaderActivity` — drops the `IfStale` predicate entirely. Mechanism 1 decides *when*
  to redraw; the activity only answers *how*. `restoreCurrentPageToBufferIfStale()` becomes
  `redrawVisibleFrame()` and is unconditional.
- `TxtReaderActivity`, `MdReaderActivity` — via the existing
  `TxtReaderActivity::drawCurrentPageToBuffer(path, renderer)` static.
- `XtcReaderActivity` — via its own `drawCurrentPageToBuffer` static.

All three statics already exist and are already used for exactly this purpose by
`SleepActivity::renderOverlaySleepScreen()`.

## Mechanism 3 — a driver conformance test

The remaining new-device hazard is the other end of the contract: a driver that is told
"there is no baseline" and keeps its synced claim anyway. Every driver already implements
`cleanupGrayscaleBuffers(bus, nullptr)` correctly — UC8253 drops `_redRamSynced`,
UC8179/UC8279X4 set `_needFullClear`, UC8279 sets `_forceFullSyncNext`, the rest reload
wholesale — but nothing holds them to it.

Add to the SDK host suite: for each linked driver, run a grayscale pass, call
`cleanupGrayscaleBuffers(bus, nullptr)`, and assert the next refresh is a clean/full sync
rather than a differential. Nine drivers, one test. A new controller fails it on day one
instead of shipping ghosting.

## Mechanism 4 — the reader exit refresh, which three readers never had

`ReaderUtils::enforceExitFullRefresh()` schedules the next screen's refresh as HALF, so the
screen after a reader does not diff against a panel the controller's RAM no longer
describes. Only `EpubReaderActivity::onExit()` calls it. `TxtReaderActivity`,
`MdReaderActivity` and `XtcReaderActivity` do not — every `enforceExitFullRefresh` hit in
those three files is inside a submenu-launch path, not an exit path.

Xtc is the one that matters: it runs a full grayscale plane pass
(`displayGrayBuffer()` + `cleanupGrayscaleWithFrameBuffer()`), so it leaves the panel
showing greys while the controller RAM holds the B/W frame — the exact condition the EPUB
reader guards against. Txt and Md are 1-bit only, so their exposure is smaller but real.

Fix: call it from `LineReaderActivity::onExit()` (covering Txt and Md in their shared base)
and from `XtcReaderActivity::onExit()`. Guarded the same way the EPUB reader guards it —
`getEffectiveTextAntiAliasing() || renderer.supportsGrayFrame()` is EPUB-specific, so for
these the condition is "this reader can produce grey", i.e. unconditional for Xtc and
`supportsGrayFrame()` for the line readers.

## Files touched

| File | Change |
|---|---|
| `src/activities/Activity.h` | `redrawVisibleFrame()`; `ensureWriteBufferShowsPanel()` |
| `src/activities/Activity.cpp` | the routine |
| `src/activities/ActivityManager.{h,cpp}` | delegating routine; `goToSleep` + drawer + `prepareFramebufferForCapture` use it |
| `src/activities/reader/EpubReaderActivity.{h,cpp}` | hook rename, predicate dropped |
| `src/activities/reader/{Txt,Md,Xtc}ReaderActivity.{h,cpp}` | hook overrides |
| `src/activities/reader/LineReaderActivity.cpp` | exit refresh |
| `src/activities/reader/XtcReaderActivity.cpp` | exit refresh |
| `src/activities/home/BookInfoActivity.cpp`, `home/RecentBooksActivity.cpp`, `reader/DictionaryWordSelectActivity.cpp`, `util/FrontlightPanelActivity.cpp`, `boot_sleep/SleepActivity.cpp` | one checked line each |
| `src/main.cpp` | screenshot + `saveSleepFrameBuffer()` |
| `freeink-sdk .../test/host/test_pro.cpp` | conformance test |

Roughly 8 files of substance plus mechanical call-site updates.

## Testing

- **Mechanism 3** gets real coverage in the SDK host suite, which builds both dual- and
  single-buffer and already runs in CI.
- **Mechanisms 1, 2, 4** get none. There is no host harness that compiles `GfxRenderer` or
  `HalDisplay`, so they are enforced by the compiler (the hook must be answered) and
  reviewed by eye. This is a known weakness, stated rather than papered over. Building one
  is a larger piece of work and is not in scope here.
- Device check unchanged from the audit: sleep from the reader onto a grayscale cover on a
  book that is running background section builds.

## Order of work

1. Mechanism 2 (the hook) — mechanism 1 depends on it.
2. Mechanism 1 (the routine) + migrate every call site.
3. Mechanism 4 (reader exit refresh) — independent, smallest.
4. Mechanism 3 (conformance test) — independent, in the submodule.

One commit each, in that order.
