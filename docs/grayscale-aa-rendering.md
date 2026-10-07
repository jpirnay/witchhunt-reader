# Grayscale Anti-Aliasing Rendering

How the reader renders anti-aliased text on the e-ink panels: the two-plane model, the current
algorithm and the paths the reader chooses between, and the two earlier approaches it replaced.
Where each controller keeps its previous frame, and what a grayscale pass leaves behind on each, is in
[secondary-buffer-management.md](secondary-buffer-management.md).

---

## Background: two planes, four levels

The X3, X4 and X4 Pro controllers (UC8253 or UC8279d on the X3; SSD1677, UC8179 or UC8279 on the X4
and X4 Pro) hold two independent 1-bit planes:

| Plane | SSD1677 | UltraChip (UC8253, UC8279, UC8179) |
|-------|---------|-------------------------------------|
| LSB | BW RAM (`0x24`) | DTM1 (`0x10`) |
| MSB | RED RAM (`0x26`) | DTM2 (`0x13`) |

A grayscale waveform reads both planes at once and maps each pixel's two bits to a level. The
firmware writes two encodings (see `GfxRenderer::setAbsoluteGrayPlanes()` for the bit patterns):

- **Overlay** (reader text AA): a plane bit is set only for a grey pixel, and the B/W base supplies
  black and white. (LSB, MSB) = 11 dark grey, 01 light grey, 00 "leave the pixel as the base drew it".
- **Absolute** (images and the sleep cover, where `supportsAbsoluteGrayPlanes()`): the planes carry
  every pixel. (LSB, MSB) = 00 black, 10 dark grey, 01 light grey, 11 white.

Each driver translates these into its controller's own selector pattern (the SSD1677 complements
absolute planes for its factory LUT; UC8179 combines them with its snapshot of the base).

The T5S3 is different. Its controller is LovyanGFX's `Panel_EPD`, which keeps a 4-bpp buffer of its
own. `LgfxEpdDriver` copies the planes into host-side buffers and darkens the matching pixels of its
8-bit canvas, and in normal reading the planes are captured during the B/W render and go out with the
page as one waveform (see "Which path the reader takes").

Each plane is one framebuffer: 48,000 B on the X4 and X4 Pro (800×480 ÷ 8), 52,272 B on the X3
(792×528 ÷ 8). For a B/W page turn the same two planes hold "new frame" and "previous frame", so after
a grayscale pass they have to be put back to the B/W page before the next differential refresh, or the
diff runs against plane data and ghosts. That is the cleanup step below.

The X3 and X4 run on an ESP32-C3 with no PSRAM. Both framebuffers are resident, and any grayscale
scheme has to fit in the heap that is left, shared with the EPUB parser, the font cache, the image
decoders and the task stacks.

---

## Current approach: reuse the B/W framebuffer as plane scratch

### Why it works

`copyGrayscaleLsbBuffers()` and `copyGrayscaleMsbBuffers()` stream whatever full-size buffer they are
given. Once the B/W page has been pushed with `displayBuffer()`, the swap has left that page in
`frameBufferActive`, and the write buffer is not needed until the next render. So the write buffer can
serve as the render target for each plane in turn, with each plane streamed to the controller as soon
as it is rendered. No extra allocation.

### How it works

`GfxRenderer::renderGrayscalePlanesSequential(renderFn, shouldAbort)`:

```
1. clearScreen(0x00)                  the write buffer becomes plane scratch
2. setRenderMode(GRAYSCALE_LSB); renderFn(GRAYSCALE_LSB)
3. shouldAbort()? -> abandonGrayscalePass()
4. copyGrayscaleLsbBuffers()          stream the plane to the controller
5. clearScreen(0x00); setRenderMode(GRAYSCALE_MSB); renderFn(GRAYSCALE_MSB)
6. shouldAbort()? -> abandonGrayscalePass()
7. copyGrayscaleMsbBuffers()
8. setRenderMode(BW); displayGrayBuffer()        the gray waveform
9. cleanupGrayscaleWithPreviousBuffer()
```

Step 9 hands the driver `frameBufferActive`, the full B/W page (images included) that the last
`displayBuffer()` swapped there, as its baseline for the next fast refresh, and copies it back into the
write buffer. Each driver restores its own state from it (the per-controller table is in
[secondary-buffer-management.md](secondary-buffer-management.md)). With the secondary away (lent or
released) it hands the driver `nullptr` instead: the driver drops its synced claim and takes a clean
sync on its next push, and the write buffer keeps the last plane. The write buffer is never accepted
as a baseline, because by step 9 it holds a plane (SDK `4a320d6`). The reader only runs AA with the
secondary resident, so that branch is a safety net.

`abandonGrayscalePass()` runs the same cleanup without the gray flush, so an aborted pass leaves the
B/W page on screen and the baseline intact.

`renderGrayscalePlanesInterleaved()` is the same sequence entered while an async B/W refresh is still
running (`triggerDisplayAsync()`): the LSB plane renders during the waveform, and
`finishDisplayAsync()` waits out the rest before the first plane write.

```
ESP32 RAM during the pass:
  B/W framebuffer  [48-52 KB]  render scratch for both planes
  Extra:            0 KB
```

### Which path the reader takes

All three need text anti-aliasing on and a resident, non-degraded secondary
(`EpubReaderActivity::renderContents()` checks `hasSecondaryBuffer()`).

| Path | Where | What happens |
|---|---|---|
| Single push | `supportsGrayFrame()`: the T5S3 | `beginGrayCapture()` makes the B/W render write both planes as it goes; page and greys go out as one waveform (`triggerGrayscaleFrame()`). |
| Inline | `!usesDeferredAa()`: an async-capable panel that is not an X3 (X4, X4 Pro) | B/W page via `triggerDisplayAsync()`, then `renderGrayscalePlanesInterleaved()`. Skipped when a page turn is already on its way. |
| Deferred | the X3, and any panel that cannot overlap | The B/W page is shown; `runDeferredGrayscalePass()` runs `renderGrayscalePlanesSequential()` from idle time, and a page turn aborts it. |

### Absolute planes: images and the sleep cover

`GfxRenderer::beginAbsoluteGrayPass()` opens an absolute pass on panels that support one (the sleep
cover in `SleepActivity`, and `BmpViewerActivity`). It consumes a pending refresh override as the base
mode, the SDK pushes the B/W base, and the planes then go out in the absolute encoding. On the UC8279d
this selects the long XTH4 waveform (the vertical-banding fix ported from crosspoint-reader PR #3469);
on the UC8253 the planes go out under the `gc` bank with both endpoints idle, after a real base push;
on the SSD1677 the factory waveform draws base and greys in one pass (`GrayscaleBase::Combined`). The
T5S3 has no absolute mode: its sleep cover paints 8-bit grey into the borrowed panel canvas
(`borrowGray8Canvas()`, `displayGray8Canvas()`).

### Greys left on the glass

The cleanup puts controller RAM back to the B/W page, but the glass still holds grey at every AA edge.
A later differential push that trusts the RAM gives those pixels only a gentle white-to-white drive.
`Uc8253X3Driver` therefore tracks `_grayOnGlass` (set by every gray waveform, cleared only by a
waveform that drives every pixel) and takes a clean base for the next grayscale pass while it is set;
UC8179 and UC8279 X4 use `_redriveAfterGray`. UC8279d and SSD1677 do not track it (open, see
[future_work/display-and-refresh.md](future_work/display-and-refresh.md)).

### Timing (X3 UC8253, device log 2026-09-23)

| Refresh | Duration |
|---|---|
| Page turn (`X3_DRF_fast`) | ~382 ms |
| AA gray waveform (`X3_DRF`) | ~228 ms |
| Periodic scrub (`X3_DRF_half`) | ~460 ms |

`setFastGrayscaleLut()` stores the reader's Fast anti-aliasing preference, but no current driver reads
it: the X3 driver carries a single `gc` nudge bank (open, see future_work).

### Invariant that makes this safe

The write buffer holds plane data from step 1 to step 9. That is safe because:

- the B/W page is already on the panel and in `frameBufferActive` before the pass begins, and
- step 9 restores the driver's baseline and the write buffer from `frameBufferActive`, not from the
  write buffer, so images and all other B/W content come back too.

> **Note for maintainers:** this depends on `frameBufferActive` holding the B/W page that was pushed
> just before the pass. If another swapping refresh happens between the B/W push and the pass,
> `frameBufferActive` no longer holds the right frame and the next FAST differential ghosts.

Planes are sent row-reversed by `EpdBus::sendPlaneFlipped()` while streaming; the framebuffer itself
is never flipped in place.

### Trade-offs

- Zero extra allocation, so no fragmentation and no AA suspension under memory pressure; and also no
  "suspend and recover" safety valve.
- Two render passes (one per plane) on the staged paths; the single-push path walks the page once.
- The write buffer is not valid B/W content during the pass. Anything that reads it in that window
  (a screenshot, an overlay) sees a plane.

---

## Earlier approaches (removed)

### Snapshot (legacy)

Allocate a second 48 KB buffer, copy the B/W page into it (`storeBwBuffer()`), render each plane into
the framebuffer, stream it, then copy the snapshot back (`restoreBwBuffer()`) and resync the
controller from it. Simple, but it allocated 48 KB on every page turn. On the C3 the heap fragmented
over a session until the allocation failed and AA was silently suspended
(`antiAliasingSuspendedLowMemory`). `storeBwBuffer()` and `restoreBwBuffer()` remain in `GfxRenderer`
for the screenshot path.

### Tiled strips (interim)

Render each plane in horizontal strips into a ~24 KB scratch held for the reader session, culling
glyphs outside the strip, and stream each strip with a windowed RAM write, leaving the B/W framebuffer
untouched. It saved the per-page allocation but needed 2-3 render passes per plane and a windowed
write API from the open-x4-sdk whose licence turned out to be incompatible with the project. The
firmware's strip path was removed; `GfxRenderer::isStripActive()` and `glyphIntersectsStrip()` remain
as no-op stubs. The SDK has its own `writeGrayscalePlaneStrip()` again, for absolute-plane uploads,
but the reader does not use it.

### Comparison

| Property | Snapshot | Tiled strips | B/W-buffer reuse (current) |
|---|---|---|---|
| Extra peak RAM | 48 KB per page | ~24 KB for the session | 0 KB |
| Allocation pattern | alloc/free every page | one alloc at session start | none |
| Fragmentation risk | high over long sessions | low | none |
| Render passes per plane | 1 | 2-3 (strips) | 1 |
| B/W framebuffer intact during the pass | no | yes | no |
| Controller resync source | the snapshot | the live framebuffer | `frameBufferActive` |
| AA suspension under pressure | yes | no | no |

---

## Key API reference

| Method | Layer | Purpose |
|--------|-------|---------|
| `renderGrayscalePlanesSequential(fn, abort)` | `GfxRenderer` | The staged pass, end to end |
| `renderGrayscalePlanesInterleaved(fn, abort)` | `GfxRenderer` | The same, overlapping an async B/W refresh |
| `beginGrayCapture()` / `displayGrayscaleFrame()` / `triggerGrayscaleFrame()` | `GfxRenderer` | Single-push AA where `supportsGrayFrame()` |
| `beginAbsoluteGrayPass()` | `GfxRenderer` → `HalDisplay` | Open an absolute pass and push its B/W base |
| `copyGrayscaleLsbBuffers()` / `copyGrayscaleMsbBuffers()` | `GfxRenderer` → `HalDisplay` → `FreeInkDisplay` | Stream a plane to the controller |
| `displayGrayBuffer()` | `GfxRenderer` → `HalDisplay` → `FreeInkDisplay` | Run the grayscale waveform (no swap) |
| `cleanupGrayscaleWithPreviousBuffer()` | `GfxRenderer` → `HalDisplay` → `FreeInkDisplay` | Restore the driver's baseline and the write buffer from `frameBufferActive`; with the secondary away, tell the driver there is no baseline |
| `setFastGrayscaleLut(bool)` | `GfxRenderer` | Stores the Fast AA preference; currently read by no driver |

## Scheduling: the deferred pass runs before the next-page pre-render

On the deferred path the AA pass runs before Background A re-arms, because the planes and a
pre-rendered page compete for the same heap. The cost (a quick turn inside the AA window pays a full
render) and the candidate reordering are in
[future_work/display-and-refresh.md](future_work/display-and-refresh.md) under "Pre-render before the
deferred AA pass".
