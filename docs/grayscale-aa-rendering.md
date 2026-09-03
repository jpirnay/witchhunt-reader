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

### Timing (approximate, X3 OEM LUT)

| Phase | Duration |
|-------|----------|
| BW render + display | ~560 ms |
| LSB + MSB render + copy | ~180 ms |
| Grayscale waveform (OEM GC) | ~2400 ms |
| Snapshot restore + re-sync | ~120 ms |
| **Total** | **~3260 ms** |

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

### Timing (approximate, X3 OEM LUT)

| Phase | Duration |
|-------|----------|
| BW render + display | ~560 ms |
| LSB + MSB render + copy (2–3 bands each) | ~250 ms |
| Grayscale waveform (OEM GC) | ~2400 ms |
| Re-sync (`cleanupGrayscaleWithFrameBuffer`) | ~20 ms |
| **Total** | **~3230 ms** |

The extra render passes per band add ~70 ms vs the snapshot path, offset slightly by skipping snapshot save/restore.

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

The key observation is that `copyGrayscaleLsbBuffers(buf)` and `copyGrayscaleMsbBuffers(buf)` each accept any pointer to a full-size buffer. The BW framebuffer is already permanently resident and is exactly the right size. After the BW page content has been committed to the display controller via `displayBuffer()`, the BW framebuffer's *in-RAM* copy is no longer needed until the next page turn — the controller has the authoritative copy. This window can be exploited: repurpose the BW framebuffer as a render target for the grayscale planes, streaming each plane to the controller immediately after rendering.

After the grayscale waveform, `cleanupGrayscaleWithPreviousBuffer()` reseeds both controller planes (DTM1 and DTM2) from `frameBufferActive` — the internal EInkDisplay buffer that holds the full BW page exactly as `displayBuffer()` committed it, including images. This gives a correct differential baseline for the next FAST refresh without re-rendering or snapshotting.

### How it works

`GfxRenderer::renderGrayscalePlanesSequential(renderFn)`:

```
1.  clearScreen(0x00)           — wipe the BW framebuffer (repurposed as scratch)
2.  setRenderMode(GRAYSCALE_LSB)
3.  renderFn(GRAYSCALE_LSB)     — render AA text into the BW framebuffer
4.  copyGrayscaleLsbBuffers()   — stream BW framebuffer → controller DTM1
5.  clearScreen(0x00)
6.  setRenderMode(GRAYSCALE_MSB)
7.  renderFn(GRAYSCALE_MSB)     — render AA text into the BW framebuffer
8.  copyGrayscaleMsbBuffers()   — stream BW framebuffer → controller DTM2
9.  setRenderMode(BW)
10. displayGrayBuffer()         — trigger grayscale waveform
11. cleanupGrayscaleWithPreviousBuffer()
    — reseeds DTM1 and DTM2 from frameBufferActive (the just-displayed BW page)
    — on X3: Y-flips in place, sends to both planes, Y-flips back
    — on X4: writes frameBufferActive to RED RAM (differential baseline)
    — clears inGrayscaleMode flag
```

`frameBufferActive` holds the just-displayed BW frame because `displayBuffer()` calls `swapBuffers()` on both X4 and X3 at the end of every refresh — the buffer that was just sent to the panel becomes `frameBufferActive`, and the other (now empty) slot becomes the active `frameBuffer` for the next render.

```
ESP32 RAM during pass:
  BW framebuffer  [48 KB] — repurposed as render scratch, both planes in sequence
  ─────────────────────────
  Peak extra:       0 KB
```

### Timing (measured, X3 OEM LUT)

| Phase | Duration |
|-------|----------|
| BW render + display | ~560 ms (190 ms render, 127 ms PON, 382 ms DRF, 52 ms POF) |
| LSB + MSB render + copy (`planes`) | ~176 ms |
| Grayscale waveform (OEM GC, `gray`) | ~2368 ms |
| Re-sync (`restore`) | ~66 ms |
| **Total** | **~3170 ms** |

With the community fast LUT (`fastAntiAliasing=true`) the grayscale waveform drops to ~130 ms, giving a total of approximately **930 ms**.

### Invariant that makes this safe

The BW framebuffer is trashed between steps 1 and 11. This is safe because:

- The BW page content is already committed to the controller and preserved in `frameBufferActive` before the grayscale pass begins.
- `cleanupGrayscaleWithPreviousBuffer()` uses `frameBufferActive` — not the current (trashed) framebuffer — to reseed both controller planes. This includes images and all BW content, not just text.

> **Note for future maintainers:** `cleanupGrayscaleWithPreviousBuffer()` depends on `frameBufferActive` holding the full BW page that was displayed immediately before the grayscale pass. This is guaranteed as long as no other `displayBuffer()` call intervenes between the BW refresh and the grayscale pass. If the render flow is restructured such that another display call happens in between, `frameBufferActive` will no longer hold the right content and the next FAST differential will ghost.

### Pros

- **Zero extra allocation.** No heap pressure, no fragmentation risk, no AA suspension under memory pressure.
- No strip-path SDK dependency — uses only `copyGrayscaleLsbBuffers`, `copyGrayscaleMsbBuffers`, `displayGrayBuffer`, and `cleanupGrayscaleWithPreviousBuffer`, all standard unconditional API calls.
- Exactly two render passes (one per plane), the same as the legacy snapshot approach.
- `cleanupGrayscaleWithPreviousBuffer` uses the full BW content including images, giving a correct differential baseline even on image pages.
- ~470 lines net removed from the codebase (strip infrastructure, snapshot management, `antiAliasingSuspendedLowMemory` state machine).
- Works identically on X4 and X3.

### Cons

- The BW framebuffer is transiently corrupt between steps 1 and 11. A crash, power loss, or early return during this window leaves the in-RAM BW buffer in an undefined state. The next page render will call `clearScreen` before rendering anyway, so in practice this is benign — but it is a wider "dangerous window" than the snapshot approach (where the snapshot holds the ground truth throughout).
- `cleanupGrayscaleWithPreviousBuffer()` on X3 performs an in-place Y-flip of the framebuffer contents, sends the data, then Y-flips back. The logical contents are unchanged before and after, but callers must not race a framebuffer reader against this call.
- AA cannot be suspended gracefully under memory pressure because there is no longer a heap guard. In practice this is a non-issue since the approach uses no heap at all, but it does mean the old "suspend and recover" safety valve is gone.

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
| Controller re-sync source | `restoreBwBuffer` snapshot | live BW framebuffer | `frameBufferActive` (full BW incl. images) |
| AA suspension under pressure | Yes (graceful) | No | No |
| Code complexity | Medium | High (+200 lines) | Low (−470 lines net) |
| X4 artifact risk | None observed | Observed in early versions | None observed |
| **Total time (X3, OEM LUT)** | **~3260 ms** | **~3230 ms** | **~3170 ms** |
| **Total time (X3, fast LUT)** | **~1020 ms** | **~990 ms** | **~930 ms** |

---

## Key API Reference

| Method | Layer | Purpose |
|--------|-------|---------|
| `renderGrayscalePlanesSequential(fn)` | `GfxRenderer` | Runs the current algorithm end-to-end |
| `copyGrayscaleLsbBuffers()` | `GfxRenderer` → `HalDisplay` → `EInkDisplay` | Stream BW framebuffer to controller DTM1 |
| `copyGrayscaleMsbBuffers()` | `GfxRenderer` → `HalDisplay` → `EInkDisplay` | Stream BW framebuffer to controller DTM2 |
| `displayGrayBuffer()` | `GfxRenderer` → `HalDisplay` → `EInkDisplay` | Trigger grayscale waveform refresh |
| `cleanupGrayscaleWithPreviousBuffer()` | `GfxRenderer` → `EInkDisplay` | Reseed controller planes from `frameBufferActive` (just-displayed BW page); clear `inGrayscaleMode` |
| `setFastGrayscaleLut(bool)` | `GfxRenderer` | X3-only: switch between OEM (slow/accurate) and community (fast/darker) LUT |

## Scheduling: the AA pass delays the next-page pre-render

The deferred AA runs *before* Background-A re-arms, because the AA planes and a pre-rendered page
compete for the same heap. On a panel where the pass is slow this is felt directly: on the T5S3 the
AA costs ~605 ms (`planes 80 + gray 473 + restore 52`), and for that whole window the reader has no
pre-rendered next page, so a quick turn pays a full render instead of a buffer swap.

Measurements, the candidate reordering (pre-render first), and why it has not been done are in
[background-rendering.md](background-rendering.md) under "A — next-page pre-render".
