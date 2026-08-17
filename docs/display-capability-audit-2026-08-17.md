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
from the on-screen frame, gated on the controller. **That gate is itself a
name-check** and should become a capability once the SDK has one.

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
