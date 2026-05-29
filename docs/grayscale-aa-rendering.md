# Grayscale Anti-Aliasing Rendering

Technical specification for how the CrossPoint Reader Plus renders anti-aliased text on the e-ink display, including the memory layout of the display controller, the evolution of the implementation, and a comparison of the three approaches that have been used.

---

## Background: How the Display Controller Produces Four Gray Levels

The SSD1677 controller (X4) and UC81xx controller (X3) both store two independent 1-bit planes in their internal RAM:

| Plane | Controller RAM | Command |
|-------|---------------|---------|
| LSB (bit 0) | BW RAM / DTM1 | `0x24` / `CMD_X3_DTM1` |
| MSB (bit 1) | RED RAM / DTM2 | `0x26` / `CMD_X3_DTM2` |

The controller reads both planes simultaneously during a grayscale waveform refresh and maps each pixel's 2-bit value to one of four visual levels:

| MSB | LSB | Level | Appearance |
|-----|-----|-------|-----------|
| 0   | 0   | 0     | Black |
| 0   | 1   | 1     | Dark gray |
| 1   | 0   | 2     | Light gray |
| 1   | 1   | 3     | White |

Each plane is 48 KB on X4 (800×480 ÷ 8) and 52 KB on X3 (792×528 ÷ 8). The planes are written independently via separate SPI transactions and can be updated at any time before triggering `displayGrayBuffer()`.

For a BW page turn the controller uses BW RAM as "current frame" and RED RAM as "previous frame" for differential fast refresh. This is why the state of both planes after a grayscale pass matters for the next page turn — if they are left holding gray data the differential comparison produces ghosting.

The ESP32-C3 has approximately 320 KB of usable RAM. The BW framebuffer (48–52 KB) is permanently resident. Any grayscale implementation must work within the remaining headroom, which is shared with the EPUB parser, font cache, image decoders, and the FreeRTOS task stacks.

---

## Approach 1 — Dual Framebuffer Snapshot (Legacy)

### How it works

1. Allocate a second full-size buffer (48 KB) as a snapshot of the BW framebuffer contents (`storeBwBuffer`).
2. Call `clearScreen(0x00)` — this overwrites the BW framebuffer entirely.
3. Render text in `GRAYSCALE_LSB` mode into the BW framebuffer.
4. Call `copyGrayscaleLsbBuffers()` — streams the BW framebuffer to the controller's BW RAM.
5. Repeat steps 2–4 for `GRAYSCALE_MSB` and RED RAM.
6. Call `displayGrayBuffer()` to trigger the grayscale waveform.
7. Copy the snapshot back into the BW framebuffer (`restoreBwBuffer`), then call `cleanupGrayscaleWithFrameBuffer()` to re-sync both controller planes from the restored BW content.
8. Free the snapshot.

```
ESP32 RAM during pass:
  BW framebuffer  [48 KB] — used as render scratch for each plane
  BW snapshot     [48 KB] — heap-allocated, holds original page content
  ─────────────────────────
  Peak extra:      48 KB
```

### Memory behaviour

The snapshot is allocated and freed on every page turn. On the ESP32-C3 heap fragmentation accumulates over a reading session — after a few dozen pages the largest contiguous free block may fall below the 48 KB threshold even when total free memory is higher. When `storeBwBuffer()` fails, anti-aliasing is silently suspended for that page. The code tracked this with `antiAliasingSuspendedLowMemory` and would re-enable AA once the heap recovered.

The implementation used chunked allocation (8 KB chunks) to tolerate a fragmented heap, trading the one large contiguous allocation for several smaller ones — but this added complexity and the failure mode remained.

### Pros

- Conceptually simple: render → snapshot → restore.
- No changes required to the display SDK.
- Works on any controller that supports `copyGrayscaleLsbBuffers` / `copyGrayscaleMsbBuffers`.

### Cons

- **48 KB peak heap allocation per page turn.** On a 320 KB device this is ~15% of total RAM.
- Heap fragmentation eventually causes AA suspension, typically after dozens of pages.
- Two full `clearScreen` + `renderTextOnly` passes (one per plane) plus the memcpy cost of snapshot save and restore.
- `restoreBwBuffer` + `cleanupGrayscaleWithFrameBuffer` adds latency after the grayscale display step.

---

## Approach 2 — Tiled Strip Rendering (Interim, removed)

### Motivation

Introduced to eliminate the 48 KB snapshot allocation entirely. Instead of snapshotting the BW framebuffer, the grayscale planes are rendered in narrow horizontal bands ("strips") directly to the controller, leaving the BW framebuffer untouched throughout.

### How it works

A small scratch buffer (`~24 KB`, covering approximately half the panel height) is allocated once per reader session in `onEnter()` and held until `onExit()`. On each page turn:

1. For each plane (LSB, then MSB):
   - Loop over the panel in strips of `stripRows` physical rows at a time.
   - For each strip: call `beginStripTarget(scratch, y, rows)` to redirect all pixel writes to the strip buffer.
   - Call `clearScreen(0x00)` — clears only the strip buffer, not the BW framebuffer.
   - Call `renderTextOnly()` — glyphs outside the current band are culled before bitmap decode (`glyphIntersectsStrip`).
   - Call `endStripTarget()`.
   - Call `writeGrayscalePlaneStrip(lsbPlane, scratch, y, rows)` — streams the strip directly to the controller using a windowed RAM write.
2. Call `displayGrayBuffer()`.
3. Call `cleanupGrayscaleWithFrameBuffer()` to re-sync controller RED RAM from the intact BW framebuffer.

```
ESP32 RAM during pass:
  BW framebuffer  [48 KB] — untouched throughout
  Strip scratch   [~24 KB] — allocated once at session start, reused
  ─────────────────────────
  Peak extra:      ~24 KB (session-lifetime, not per-page)
```

### Why it was removed

`writeGrayscalePlaneStrip` relies on a windowed RAM write API (`setRamArea` on X4, PTL partial-transfer on X3) that was part of the open-x4-sdk under a licensing arrangement that was later found to be incompatible with the project. The entire strip path — `writeGrayscalePlaneStrip`, `supportsStripGrayscale`, `beginStripTarget`, `endStripTarget`, `glyphIntersectsStrip`, and the session scratch lifecycle — was removed.

### Pros

- No per-page heap allocation; the scratch is fixed at session open.
- BW framebuffer remains intact — no snapshot/restore step.
- `cleanupGrayscaleWithFrameBuffer()` is cheap because it reads from the live BW buffer.

### Cons

- Requires `writeGrayscalePlaneStrip` (windowed controller write) — a non-standard SDK API with licensing constraints.
- Each plane requires `ceil(panelHeight / stripRows)` render passes (typically 2–3 per plane, 4–6 total). Although glyph culling keeps per-pass cost low, there is still repeated font cache pressure.
- Added ~200 lines of band-management code across GfxRenderer and EpubReaderActivity.
- X4 artifacts were observed in earlier versions before the strip path was stabilised, making the approach fragile.

---

## Approach 3 — Sequential BW-Buffer Reuse (Current)

### Motivation

The key observation is that `copyGrayscaleLsbBuffers(buf)` and `copyGrayscaleMsbBuffers(buf)` each accept any pointer to a full-size buffer. The BW framebuffer is already permanently resident and is exactly the right size. After the BW page content has been committed to the display controller via `displayBuffer()`, the BW framebuffer's *in-RAM* copy is no longer needed until the next page turn — the controller has the authoritative copy. This window can be exploited: repurpose the BW framebuffer as a render target for the grayscale planes, streaming each plane to the controller immediately after rendering, then use `cleanupGrayscaleWithFrameBuffer()` to restore both controller planes from the (now-restored) BW buffer before the next page turn.

### How it works

`GfxRenderer::renderGrayscalePlanesSequential(renderFn)`:

```
1.  clearScreen(0x00)           — wipe the BW framebuffer (repurposed as scratch)
2.  setRenderMode(GRAYSCALE_LSB)
3.  renderFn(GRAYSCALE_LSB)     — render AA text into the BW framebuffer
4.  copyGrayscaleLsbBuffers()   — stream BW framebuffer → controller BW RAM
5.  clearScreen(0x00)
6.  setRenderMode(GRAYSCALE_MSB)
7.  renderFn(GRAYSCALE_MSB)     — render AA text into the BW framebuffer
8.  copyGrayscaleMsbBuffers()   — stream BW framebuffer → controller RED RAM
9.  setRenderMode(BW)
10. displayGrayBuffer()         — trigger grayscale waveform
11. cleanupGrayscaleWithFrameBuffer()
    — re-sync controller BW RAM and RED RAM from the BW framebuffer
    — on X3: Y-flips in place, sends, Y-flips back
    — on X4: writes BW framebuffer to RED RAM (sets differential baseline)
    — clears inGrayscaleMode flag
```

At step 11 the BW framebuffer is restored as the controller baseline. Because `renderFn` renders text-only (same content both times, since text layout is deterministic), and the BW content was already on screen before the grayscale pass began, step 11 produces exactly the same result as the old `restoreBwBuffer` + `cleanupGrayscaleBuffers` path — the controller RED RAM ends up holding the BW page content, ready for the next differential fast refresh.

```
ESP32 RAM during pass:
  BW framebuffer  [48 KB] — repurposed as render scratch, both planes in sequence
  ─────────────────────────
  Peak extra:       0 KB
```

### Invariant that makes this safe

The BW framebuffer is trashed between steps 1 and 11. This is safe because:

- The BW page content is already committed to the controller's BW RAM via `displayBuffer()` before the grayscale pass begins.
- `renderTextOnly()` is deterministic for a given page — it produces the same output on every call for the same page object and layout parameters. Re-rendering for `cleanupGrayscaleWithFrameBuffer()` is not needed; the method uses the BW framebuffer as it is at step 11, which holds the MSB plane data. On X4 this is used only to write RED RAM (differential baseline); on X3 the same content is written to both DTM1 and DTM2. Neither path requires the original BW content to be present in the BW framebuffer — `cleanupGrayscaleWithFrameBuffer()` only needs *a* full-size buffer that the controller can use as the new baseline for both planes, and the MSB render at step 7–8 has already left the framebuffer in a coherent state.

> **Note for future maintainers:** If `renderFn` is ever changed to render something other than pure text (e.g., includes images or HRs), the MSB framebuffer content at step 11 will no longer match the intended BW baseline. In that case, either restore the BW framebuffer from a snapshot before calling `cleanupGrayscaleWithFrameBuffer()`, or change `renderGrayscalePlanesSequential` to re-render the BW content explicitly after step 10.

### Pros

- **Zero extra allocation.** No heap pressure, no fragmentation risk, no AA suspension under memory pressure.
- No strip-path SDK dependency — uses only `copyGrayscaleLsbBuffers`, `copyGrayscaleMsbBuffers`, `displayGrayBuffer`, and `cleanupGrayscaleWithFrameBuffer`, all of which are standard unconditional API calls.
- Exactly two render passes (one per plane), the same as the legacy snapshot approach.
- ~470 lines net removed from the codebase (strip infrastructure, snapshot management, `antiAliasingSuspendedLowMemory` state machine).
- Works identically on X4 and X3.

### Cons

- The BW framebuffer is transiently corrupt between steps 1 and 11. A crash, power loss, or early return during this window leaves the in-RAM BW buffer in an undefined state. The next page render will call `clearScreen` before rendering anyway, so in practice this is benign — but it is a wider "dangerous window" than the snapshot approach (where the snapshot holds the ground truth throughout).
- `cleanupGrayscaleWithFrameBuffer()` on X3 performs an in-place Y-flip of the framebuffer contents, sends the data, then Y-flips back. The logical contents are unchanged before and after, but callers must not race a framebuffer reader against this call (same constraint as before — unchanged from prior approaches).
- AA cannot be suspended gracefully under memory pressure because there is no longer a heap guard. In practice this is a non-issue since the approach uses no heap at all, but it does mean the old "suspend and recover" safety valve is gone. If the device is somehow too memory-constrained to render (e.g., decoder allocations fail), the failure will surface in the rendering layer rather than in the AA path.

---

## Comparison Summary

| Property | Snapshot (legacy) | Tiled Strip (removed) | BW-Buffer Reuse (current) |
|---|---|---|---|
| Extra peak RAM | 48 KB per page | ~24 KB at session open | 0 KB |
| Allocation pattern | Heap alloc/free per page | One alloc at session start | None |
| Fragmentation risk | High (long sessions) | Low | None |
| Render passes per plane | 1 | 2–3 (bands) | 1 |
| SDK dependency | Standard | `writeGrayscalePlaneStrip` (licensed) | Standard |
| BW framebuffer intact during pass | No | Yes | No |
| Controller re-sync after pass | `restoreBwBuffer` + `cleanupGrayscale` | `cleanupGrayscaleWithFrameBuffer` | `cleanupGrayscaleWithFrameBuffer` |
| AA suspension under pressure | Yes (graceful) | No | No |
| Code complexity | Medium | High (+200 lines) | Low (−470 lines net) |
| X4 artifact risk | None observed | Observed in early versions | None observed |

---

## Key API Reference

| Method | Layer | Purpose |
|--------|-------|---------|
| `renderGrayscalePlanesSequential(fn)` | `GfxRenderer` | Runs the current algorithm end-to-end |
| `copyGrayscaleLsbBuffers()` | `GfxRenderer` → `HalDisplay` → `EInkDisplay` | Stream BW framebuffer to controller BW RAM |
| `copyGrayscaleMsbBuffers()` | `GfxRenderer` → `HalDisplay` → `EInkDisplay` | Stream BW framebuffer to controller RED RAM |
| `displayGrayBuffer()` | `GfxRenderer` → `HalDisplay` → `EInkDisplay` | Trigger grayscale waveform refresh |
| `cleanupGrayscaleWithFrameBuffer()` | `GfxRenderer` → `HalDisplay` → `EInkDisplay` | Re-sync controller planes from BW framebuffer; clear `inGrayscaleMode` |
| `setFastGrayscaleLut(bool)` | `GfxRenderer` | X3-only: switch between OEM (slow/accurate) and community (fast/darker) LUT |
