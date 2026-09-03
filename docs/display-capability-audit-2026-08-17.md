# Display decisions: ask the driver, not the board — audit, 2026-08-17

Written after the T5S3's AA pass inverted the whole page. That bug was one
instance of a pattern, and the pattern is worth fixing deliberately rather than
one symptom at a time.

**The rule this document argues for:** every display decision — refresh mode,
grayscale strategy, buffer lifecycle, cancellation — should be derived from an
SDK capability primitive describing the *panel*, never from a board name, a
controller name, or `isX3()`.

## The root cause of the class

`FreeInkDisplay::PanelSel` has three values — `X4`, `X3`, `M5` — and **defaults
to `X4`**. Driver *selection* handles this correctly: `selectDriver()`'s comment
says single-driver builds (M5 / Murphy / de-link / **LilyGo**) fall through to
the one linked driver, so the T5S3 does get `LgfxEpdDriver`.

But `_panelSel` is *also* used as a stand-in for the host-side **buffer and
baseline model**, and there it is never corrected. The T5S3 reports `X4`:

| Site | Keys on | Consequence on the T5S3 |
|---|---|---|
| `resolveReleasedMode()` | `_panelSel == X4` | FAST→HALF downgrade whose whole justification is SSD1677's host-managed RED RAM, which this panel does not have |
| `syncRedRamFromFrameBuffer()` | early-returns only for `X3` | runs; harmless only because `seedPreviousFrame()` is a base-class no-op here |
| `isRedRamSynced()` | `_panelSel != X3` | reports `redSynced=1` for a plane that does not exist |
| our `GfxRenderer::isX3()` | `_panelSel == X3` | every `!isX3()` branch in the reader takes the **X4** path |

So "is this an X4?" is answering four unrelated questions, on a board that is
none of them. This is the same defect B0 removed from `deviceIsX3()`, one layer
down — and it is why the reader ran inline AA on a panel that cannot overlap.

## What the SDK already offers that we do not use

`PanelDriver` exposes a rich capability surface. **Our firmware calls none of
it.** Of 71 public methods on the facade, `HalDisplay` exposes 29.

Grep for these in `src/` and `lib/` and you get zero hits:

| Primitive | The question it answers |
|---|---|
| `supportsAsyncDisplay()` / `supportsAsyncRefresh()` | will `triggerDisplayAsync()` actually overlap? (**now used** — the AA fix) |
| `supportsStripGrayscale()` | can planes be streamed in strips? |
| `supportsFactoryGrayscale()` | does it accept SSD1677 absolute selector planes? |
| `supportsBusyGrayscaleStaging()` | can planes be encoded while the previous waveform is BUSY? |
| `combinesGrayscaleBase()` | must the BW base go through `displayGrayscaleBase()` so base+planes share one waveform? |
| `displayGrayscaleBase()` | the base-frame path for a grayscale overlay |
| `preconditionGrayscale()` | windowed settle pass before planes |
| `prepareGrayscaleTarget(bw)` | hand the driver the BW target |
| `beginDisplayWork()` / `abortPostRefresh()` / `postRefreshAborted()` | cancellation of optional post-refresh work |
| `displayCommitted()` | did a frame actually reach the panel? |
| `runMaintenance()` / `hasPendingMaintenance()` / `controllerIdle()` | deferred panel maintenance |
| `setBackgroundHint(dark)` | inverted-content residue handling |

Two of these are worth calling out because we **reimplement** them:

- `abortPostRefresh()` / `postRefreshAborted()` is the SDK's cancellation model
  for optional post-refresh work. Our `aaPreemptedByNavigation()` is a
  hand-rolled equivalent that the driver knows nothing about.
- `displayCommitted()` exists precisely so a caller's refresh cadence is consumed
  on *commit* rather than on *submit*. Our `pagesUntilFullRefresh` cadence
  assumes submission always commits.

## The bug that prompted this

`displayGray(bus, fb, turnOff, lut, factoryMode)` has a `fb` parameter that
**one driver reads and another ignores**, with no stated contract:

- `Ssd1677Driver::displayGray()` opens with `(void)fb;` — planes are already in
  controller RAM and the BW page is retained by the panel.
- `LgfxEpdDriver::displayGray()` calls `fillCanvasGray(fb)`, rebuilding every
  pixel from that base, mapping a clear base bit to `kGrayBlack`.

The caller (`renderGrayscalePlanesSequential`) leaves the **last plane** in the
framebuffer — text only, background `0x00` — because for the driver it was
written against, the argument was dead. On LGFX that paints the background black
and leaves only the AA marks: the reported inversion.

Worked around in `HalDisplay::displayGrayBuffer()` by reseeding the write buffer
from the on-screen frame, gated on the controller. **DEVICE-VALIDATED
2026-08-17: the inversion is gone.** That confirms the reading of the contract —
LGFX genuinely composes from `fb`, and it was being handed a plane.

**That gate is itself a name-check** (`displayController == LgfxEpd`) and should
become a capability once the SDK has one. It is listed here as a known debt, not
as the finished shape.

## Predicted: the AA push mode does not follow the BW base mode

Found by asking whether the two-phase model is even right for this controller.
It is: `display()` fills the canvas B/W and pushes it, `displayGray()` re-composes
the *same* canvas with the LSB/MSB planes and pushes again, and `Panel_EPD`'s
per-pixel diff means only the changed (anti-aliased edge) pixels are driven. That
is a sound superimpose model and matches how the reader drives it.

The flaw is the mode pairing. `displayGray()` hardcodes `epd_fast`, while
`display()` uses `epdModeFor(mode)`:

| Refresh | phase 1 (BW) | phase 2 (AA) | |
|---|---|---|---|
| FAST | `epd_fast` | `epd_fast` | match — only AA edges driven |
| HALF | `epd_text` | `epd_fast` | **mismatch** |
| FULL | `epd_text` | `epd_fast` | **mismatch** |

`displayGray()`'s own comment states the consequence: *"Panel_EPD's per-pixel diff
keys on the epd_mode LUT offset, so switching modes here would re-drive every
pixel (full-screen inversion flash)."* So on every HALF or FULL page — one in
`refreshFrequency`, default 15 — the AA pass should flash the whole screen.

**Predicted, not yet observed**: it has been invisible because the AA pass never
reached the panel until 230e3f50. Expect it on roughly every fifteenth page.

Two ways to fix, in preference order:

1. **SDK (correct):** have the driver remember the epd_mode of its last base push
   and reuse it in `displayGray()`, so the diff baseline always matches. Small and
   local to `LgfxEpdDriver`.
2. **Firmware (workaround):** skip the grayscale overlay when the page's base was
   displayed with HALF/FULL. Costs AA on those pages, which is a visible
   inconsistency, so it is second choice.

## Direction

1. **SDK — split `PanelSel` from the buffer/baseline model.** The questions
   `resolveReleasedMode()` and `syncRedRamFromFrameBuffer()` ask are "does this
   controller keep a host-managed previous-frame plane?" — a driver capability
   (`seedPreviousFrame()` already exists and is a no-op where the answer is no).
   Deriving them from a capability instead of `_panelSel == X4` fixes every row
   of the first table at once.
2. **SDK — state `displayGray()`'s `fb` contract**, and honour it: either pass
   `frameBufferActive`, or have `LgfxEpdDriver` overlay onto its existing canvas.
3. **HAL — expose the primitives we need as we need them.** Thin passthroughs,
   no policy. Policy stays in the activity.
4. **Firmware — convert decision sites as each is exercised on hardware.** Not a
   big-bang refactor: each conversion changes behaviour on some board and needs
   device validation. `usesDeferredAa()` is the model — one predicate, used by
   the strategy *and* its scheduling, so the two cannot drift apart.

## Framebuffer lifecycle — still to map

The borrowed/released framebuffer states interact with all of the above and are
not yet audited:

- `lendBuildStorage()` / `returnBuildStorage()` — borrow the framebuffer's bytes
  without freeing (the C3 anti-fragmentation path)
- `releaseSecondaryBuffer()` / `reallocSecondaryBuffer()` — the reader's
  build-time release, which opts into `setSingleBufferFastDiff(true)` after
  seeding RED
- `resolveReleasedMode()` — the downgrade above

On the T5S3 these run with PSRAM and no RED plane, i.e. with neither of the two
pressures they were designed around. Whether the release/realloc dance should
happen there **at all** is an open question, not just a question of which mode it
picks. Worth answering before adding more per-state special cases.

---

## Status update, 2026-08-17 (end of day)

**The SDK-side half of this audit landed upstream.** `Free-Ink/freeink-sdk#47`
merged as `28c72f4`; this repo's submodule tracks upstream `main` again and the
temporary fork pin is gone. What that brought:

- `LgfxEpdDriver::displayGray()` no longer treats its `fb` argument as a base to
  rebuild from — it is now `(void)fb; overlayCanvasGray();`, composing onto the
  live canvas and darkening only plane-selected pixels. This is the fix for "the
  bug that prompted this", and it is device-validated.
- The merged `LILYGO_T5_PRO_GT911` profile carries the home key, PCF8563 RTC and
  bezel insets, so the three runtime overrides that shadowed it in `main.cpp` are
  deleted.

**Still open from this document, unchanged:**

- **`PanelSel` still conflates driver selection with the buffer/baseline model**
  (§"The root cause of the class"). #47 fixed one symptom; the split proposed
  under "Direction" was not attempted.
- **The predicted AA-push/BW-base mode mismatch** (§"Predicted") remains
  unobserved and unfixed — it needs a HALF or FULL page on an LGFX panel to
  surface.
- **The framebuffer lifecycle map** (§ above) is still not done.

**One firmware-side workaround is deliberately retained.** `HalDisplay::displayGrayBuffer()`
still reseeds the write buffer from the on-screen frame on `LgfxEpd`. After #47 the
driver never reads that buffer, so the reseed no longer serves its stated purpose —
but it touches the *host* framebuffer, a different object from the LGFX canvas the
overlay composes onto, and whether the plane-restore step depends on it cannot be
settled off the panel. It costs one buffer copy per AA pass on one board; being
wrong costs the page-inversion bug again. Remove it behind a device test, not an
argument. The call-site comment says the same.
