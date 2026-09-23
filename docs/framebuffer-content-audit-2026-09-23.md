# Which buffer holds what, and when — framebuffer content audit (2026-09-23)

> **Status (later the same day).** The defects below are real and stand as code findings.
> The causal claim this document originally made — that D1 was the mechanism behind the X3
> sleep-cover ghosting — was **wrong**: the fixes were built, flashed and changed nothing, and
> test T0 plus a code deduction then located the cause in the UC8253 driver's failure to
> track greys on the glass (`x3-sleep-ghosting-hypotheses.md` §6,
> `display-baseline-audit-2026-09-23.md` §5). Every fix listed here was reverted with the
> branch and survives only in the reflog (`c17d939ab` firmware, `08222e1` SDK). None is in
> the tree. The document is restored for its state tables (§1–§3) and its open findings.

Trigger: reader → sleep shows reader content ghosting through the sleep screen, most
visible on X3. Same class as the T5S3 regression fixed in 817be7759 ("stop seeding the
grayscale baseline from a lent framebuffer"), so this audit asks the general question
rather than patching the symptom: **at any given moment, which buffer holds what?**

A full refresh is not a fix here. A full refresh is what you reach for when nobody can
say what is in the buffer; the point of this document is to make that answerable.

Nothing in this document is board-specific unless it says so. The defects below are in
`FreeInkDisplay` / `GfxRenderer` / the activity layer and reach X3, X4 and T5S3 alike;
only how *visible* they are differs by controller.

---

## 1. The model

Dual-buffer mode (this fork never defines `EINK_DISPLAY_SINGLE_BUFFER_MODE`):

| Pointer | Intended meaning |
|---|---|
| `frameBuffer` | write target — `clearScreen()`, glyphs, `drawBitmap` land here |
| `frameBufferActive` (secondary) | the frame **on the panel** |
| `_secondaryLent` | non-null while the secondary block is out on loan as scratch |

`displayBuffer()` / `triggerDisplay()` / `displayGrayscaleFrame()` end in `swapBuffers()`,
so immediately after a refresh:

```
frameBufferActive = the frame just displayed          (panel content)
frameBuffer       = the frame displayed BEFORE it     (one generation stale)
```

`displayGrayBuffer()` deliberately does **not** swap — the two-push AA path relies on
`frameBufferActive` still holding the B/W base it pushed.

The invariant every compositing consumer depends on is:

> **I1 — after a refresh, `frameBufferActive` is the panel content, and `frameBuffer`
> can be restored from it.**

I1 is true only while the secondary is resident. Every other state has to be handled
explicitly, and that is where the bugs are.

---

## 2. Producers — everything that makes `frameBuffer` ≠ panel content

| # | Producer | `frameBuffer` afterwards | Recoverable from `frameBufferActive`? |
|---|---|---|---|
| P1 | any `displayBuffer()` (swap) | previous frame | yes |
| P2 | Background-A pre-render (`renderPageContentOnly`, `EpubReaderActivity.cpp:5005`) | the **next** page | yes |
| P3 | grayscale plane pass (`GfxRenderer.h:769` / `:825`) — two `clearScreen(0x00)` + plane renders | **LSB or MSB plane** | yes *if resident* |
| P4 | `borrowSecondaryBuffer()` (Background-B arena, image warm scratch) | untouched, but `frameBufferActive == nullptr` | **no** — there is no copy of the panel content anywhere |
| P5 | `releaseSecondaryBuffer()` | seeded from the displayed frame *at release time*, then freely overwritten | **no** |
| P6 | image warm pass decode side effects | decoder pixels (cleared right after) | yes |
| P7 | `returnSecondaryBuffer()` / `reallocSecondaryBuffer()` / `reallocBuffers()` | untouched — but the **secondary** is now a copy of the write buffer (or white), while still reporting resident | **no**, and it lies |

P7 is the one that makes `hasSecondaryBuffer()` the wrong question. Its own comment in
`reallocSecondaryBuffer()` already says the seed is "UNPROVEN", and `returnSecondaryBuffer()`
memcpy's the write buffer in wholesale. So a secondary can be resident and hold a frame that
was never on the panel — which is what the 2026-09-17 T5S3 drawer trace was actually showing
when it logged `hasSecondary=1` next to a wrong frame. That reading was filed at the time as
"the reader parks the pre-rendered next page in the secondary"; it does not, and P7 explains
the log without that.

P2 is worth calling out because the tree says the opposite in two places:
`EpubReaderActivity.cpp:1211` ("it lives in the secondary framebuffer") and the step-2
rationale in `ActivityManager.cpp:697`. The pre-render runs `clearScreen()` +
`page.render()` — i.e. into the **write** buffer. `renderBufferDisplayPass` says so
correctly ("frame buffer holds pre-rendered content", `:3253`). The two-step preparation
in `dispatchLightPanelGesture()` still produces the right result, but not for the reason
its comment gives; step 2 earns its place because step 1 silently no-ops under P4/P5,
not because the secondary holds the next page.

---

## 3. Consumers — everything that assumes I1

| Consumer | Where | Assumes |
|---|---|---|
| `syncWriteBufferFromDisplayed()` | `BaseTheme::drawPopup`, `BookInfoActivity:259`, `RecentBooksActivity:721`, `DictionaryWordSelectActivity:815`, `FrontlightPanelActivity:295`, `SleepActivity:971` | secondary is resident and is the panel content |
| `prepareFramebufferForCapture()` | `ActivityManager:644`, screenshot `main.cpp:1687` | activity can re-render the visible page |
| `cleanupGrayscale*Buffers()` | tail of every plane pass | the buffer it is handed is the displayed B/W frame |
| SleepActivity OVERLAY | `SleepActivity.cpp:1067` | "the frame buffer already holds the page" |
| SleepActivity QUICK_RESUME | `SleepActivity.cpp:1051` | same |
| `saveSleepFrameBuffer()` | `main.cpp:387` | `renderer.getFrameBuffer()` is the frame just displayed |

---

## 4. Defects

### D1 — `cleanupGrayscaleWithPreviousBuffer()`'s fallback is never correct *(all controllers)*

`freeink-sdk/.../FreeInkDisplay.cpp:1059`

```cpp
const uint8_t* baseline = frameBufferActive ? frameBufferActive
                                            : (_secondaryLent ? nullptr : frameBuffer);
if (!baseline) return;
if (!_inverted) _driver->cleanupGrayscaleBuffers(_bus, baseline);
if (frameBuffer && frameBuffer != baseline) memcpy(frameBuffer, baseline, bufferSize);
```

Every caller of this function is the tail — or the abort point — of a grayscale plane
pass (`GfxRenderer.h:734`, `:806`, `:867`). By construction those passes have just done
`clearScreen(0x00)` twice and rendered planes into `frameBuffer`. So:

* **released** (`frameBufferActive == nullptr`, not lent): `baseline == frameBuffer ==
  the MSB plane`. The driver is handed a grayscale plane as "the displayed B/W frame" —
  on X3 `Uc8253X3Driver::cleanupGrayscaleBuffers` writes it into **both** DTM1 and DTM2
  and then asserts `_redRamSynced = true`, `lsbValid = false`. The `memcpy` is a
  self-copy and does nothing, so the write buffer keeps the plane as well.
* **lent**: early return. Correct for the driver (817be7759), but the write buffer is
  left holding the plane and **nothing ever restores it**.

817be7759's premise — *"that fallback is correct after a RELEASE, which seeds
`frameBuffer` from the displayed frame first"* — is true at the moment of the release
and false by the time this function runs. The release is upstream of two `clearScreen`s.

~~Consequence on X3: … That is the reported symptom, mechanism and all.~~ **Struck.** The
precondition (secondary absent *during* a plane pass) never held in the captured run
(`borrow=0`, `hasSecondary=1`), and the reader gates AA on `hasSecondaryBuffer()`. This is a
latent defect, not the observed one.

Consequence everywhere: the write framebuffer is left holding a grayscale plane, which
then feeds every consumer in §3.

**Written and host-tested, then reverted** (reflog `96f031d`): the baseline is `frameBufferActive` and nothing else,
and with the secondary away the driver is told there is none — a contract every driver
already implements as "take a clean/full sync on the next push". Covered by
`test_pro.cpp::testGrayCleanupBaseline`, which fails on the pre-fix code at the released
case. Follow-up `08222e1` tightens it further to `hasDisplayedFrame()` (see D5).

### D5 — "secondary resident" was being used to mean "secondary holds the panel" *(all controllers)*

The consequence of P7, and the reason D1's first fix was not enough on its own.
`syncWriteBufferFromActive()` copied `frameBufferActive` whenever it was non-null and
reported nothing, so every consumer in §3 could be handed a frame that was never
displayed — silently.

**Written and host-tested, then reverted** (reflog `08222e1`): `_displayedFrameValid` is set only by `swapBuffers()`
(the one place a frame that reached the panel lands in the secondary) and cleared by every
path that hands the block out or reseeds it. `hasDisplayedFrame()` exposes it,
`syncWriteBufferFromActive()` is gated on it and returns whether it copied anything, and
the grayscale cleanup asks it too. Threaded through `HalDisplay` and `GfxRenderer`.

### D2 — the reader's exit hook covers one of three cases

`restoreCurrentPageToBufferIfPreRendered()` (`EpubReaderActivity.cpp:5151`) returns
immediately unless `preRenderedPage.ready`. It therefore repairs P2 only. It does not
repair P1 (the ordinary swap staleness — the write buffer holds the *previous* page) and
cannot repair P3's residue. `LineReaderActivity` / `XtcReaderActivity` have no equivalent
hook at all, and `LineReaderActivity::onExit()` does not even call
`enforceExitFullRefresh` (only its explicit Back/Home path does, `:87`).

### D3 — `goToSleep()` skips the preparation every other transition performs

`ActivityManager::goToSleep()` (`:585`) replaces the activity and pumps `loop()`. Unlike
`dispatchLightPanelGesture()` (`:706`) and the screenshot path (`main.cpp:1685`), it does
not run the `syncWriteBufferFromDisplayed()` + `prepareFramebufferForCapture()` pair. The
reader's `onExit()` happens to cover the pre-render case for EPUB only (D2), so the two
sleep modes that composite onto the write buffer —

* OVERLAY (`SleepActivity.cpp:1067`: *"When coming from a reader activity the frame
  buffer already holds the page"*), and
* QUICK_RESUME (`:1051`: *"Keep whatever is currently in the framebuffer (the reader
  page)"*)

— composite onto the **previous** page in the ordinary case, and onto a grayscale plane
in the D1 case.

### D4 — Quick Resume persists the wrong frame

`renderLastScreenSleepScreen()` draws the moon into `frameBuffer` and calls
`displayBuffer(HALF_REFRESH)`, which swaps; `GfxRenderer::displayBuffer` then re-reads
the pointer (`GfxRenderer.cpp:2978`). `saveSleepFrameBuffer()` (`main.cpp:387`) runs
afterwards and writes `renderer.getFrameBuffer()` — which is now the *other* slot. The
persisted Quick Resume frame is therefore the frame displayed before the sleep screen,
not the moon frame that is on the panel. Combined with D3 the two errors partly cancel
(the file ends up holding the real current page, without the moon) while the panel shows
the previous page — but neither is what either side intends, and nothing documents the
cancellation.

---

## 5. Why X3 shows it worst — corrected

The original text here attributed the X3 visibility to D1 poisoning DTM1. That was the wrong
mechanism for the reported symptom. The correct per-controller account of where the baseline
lives and why UC8253 is exposed is `display-baseline-audit-2026-09-23.md` §2 and §5: the
driver declares a clean B/W sync after an AA cleanup while the glass holds grey at every
glyph edge, and its differential gray base then gives those pixels the gentle WW cell. Fixed
on UC8253 with `_grayOnGlass`; the same omission remains on UC8279 and SSD1677.
---

## 6. What was done — and then reverted

All five rows below were reverted with the branch on 2026-09-23. **D1 was re-landed later that
day** (SDK `4a320d6`, host-tested, upstream PR #117). D5, D2, D3 and D4 remain open pending a
re-audit — D3/D4's device checks (T5/T6) were skipped by decision. The written fixes are listed
so they need not be re-derived. See `x3-sleep-ghosting-hypotheses.md` §9.

| Defect | Fix (reverted; in reflog) |
|---|---|
| D1 | freeink-sdk `96f031d` — the grayscale cleanup never accepts the write buffer as a baseline; with none available the driver is told so and takes a clean sync. Host test included. |
| D5 | freeink-sdk `08222e1` — `hasDisplayedFrame()`; `syncWriteBufferFromActive()` gated on it and returning whether it copied. Host test included. |
| D2 | `restoreCurrentPageToBufferIfPreRendered()` → `restoreCurrentPageToBufferIfStale()`, which also fires when no displayed frame is available. The P2 comment corrected. |
| D3 | `goToSleep()` does the sync + `prepareFramebufferForCapture()` pair, under a `RenderLock`, before the pending-action pump destroys the outgoing activity. The busy-indicator gate now asks `hasDisplayedFrame()` instead of `hasSecondaryBuffer()`. |
| D4 | `saveSleepFrameBuffer()` syncs from the displayed frame first — correct whether or not the sleep screen's push swapped. |

Txt / Md / Line readers need no hook of their own: they are B/W-only (no plane pass) and
have no pre-render, so the ordinary swap staleness is all that affects them and D3's sync
covers it. Xtc runs a plane pass but re-renders B/W into the framebuffer before its
cleanup, so it leaves the write buffer correct.

**Not device-verified.** The SDK host suite (`run_pro.py`, `run_uc8279.py`,
`run_uc8253_power.py`) passes, and D1's test fails on the pre-fix code. What still needs an
X3 in hand: sleep from the reader with a grayscale cover, on a book that is running
background section builds, which is the state that produces the poisoned baseline.

Still open, deliberately: §1–§3 belong in `docs/secondary-buffer-management.md`, which
documents the *pointers* but not their *contents over time*.
