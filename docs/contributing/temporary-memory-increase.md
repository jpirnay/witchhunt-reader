# Temporary Memory Increase Logic

The reader temporarily reclaims large heap blocks during operations that require more contiguous memory than normal rendering uses. This document explains what gets released, when, why, and what happens when releases are not enough.

## The two display buffers

`GfxRenderer` uses two frame buffers managed by the e-ink display driver:

**Primary buffer** (`frameBuffer`, ~48 KB): The active draw target. Every render call writes here. Released only as a last resort before a reboot — any render after release would crash.

**Secondary buffer** (`frameBufferActive` / secondary, ~52 KB): The previous-frame snapshot, used for AA grayscale passes and fast differential refresh. Can be released and reallocated freely without affecting BW rendering. Releasing it degrades AA and fast refresh temporarily but the device stays functional.

## Normal secondary release sequence

Two operations need more contiguous heap than normal rendering allows and use the same release/realloc wrapper:

1. Initial section build (cache miss on chapter open)
2. CSS-fallback rebuild (loaded a no-CSS cache, immediately rebuilds with CSS)

The sequence for each is identical:

```
renderer.dropFontMetadata()       // drop SD font kern/interval tables (~40-50 KB)
renderer.releaseSecondaryBuffer() // free ~52 KB
  → createSectionFile()           // CSS parser + XHTML inflate + layout
  → section->warmAllImageCaches() // PNG/JPEG decode → .pxc files on SD
renderer.clearScreen()
renderer.reallocSecondaryBuffer() // restore ~52 KB
renderer.restoreFontMetadata()    // lazy-reload kern/interval on next prewarm
```

**Why the warm pass must precede realloc**: The PNG decoder needs ~59 KB as a single contiguous block. After `reallocSecondaryBuffer` the largest free block drops by ~52 KB, which is often enough to push the decoder below its requirement. By decoding while the secondary is still released, the heap is maximally contiguous. The decode writes pixels to the framebuffer as a side effect; `clearScreen()` discards them before the real render.

The warm pass only decodes images that both (a) lack a `.pxc` cache file and (b) would not be shown as a placeholder under the current `forceLoad` policy — large images the user hasn't confirmed are skipped, matching render-time behaviour exactly.

## Heap thresholds

All thresholds are compile-time overridable via preprocessor defines.

| Threshold | Default | Purpose |
|---|---|---|
| `EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES` | 96 KB | CSS parser minimum free heap before section build |
| `EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES` | 36 KB | CSS parser minimum contiguous block |
| `SILENT_INDEX_MIN_FREE_HEAP_BYTES` | 64 KB | Silent next-chapter trigger minimum free heap |
| `SILENT_INDEX_MIN_CONTIG_HEAP_BYTES` | 24 KB | Silent next-chapter trigger minimum contiguous block |
| `RESTART_MIN_FREE_HEAP_BYTES` | 96 KB | Reboot defrag minimum free heap (must be plenty of total heap) |
| `SECONDARY_BUFFER_BYTES` | 52 KB | Secondary buffer size, used as realloc threshold |

If free heap is below `EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES` or `EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES` when `embeddedStyle=true`, `createSectionFile` recurses with `embeddedStyle=false` (no CSS, degraded typography) rather than failing entirely.

## Degraded mode (`secondaryBufferDegraded_`)

If `reallocSecondaryBuffer()` fails:

- `secondaryBufferDegraded_ = true`
- Text AA is disabled for all subsequent renders
- Display always uses `FULL_REFRESH` instead of `HALF_REFRESH`

The reader recovers automatically: on each render it checks `renderer.reallocSecondaryBuffer()` and clears `secondaryBufferDegraded_` on success.

## Fragmentation recovery: the pre-reboot warm pass

If `reallocSecondaryBuffer()` fails AND the heap profile matches fragmentation (plenty of total free heap, but no contiguous block large enough) AND one recovery attempt has not already been made this session, `maybeRestartForFragmentedHeap` runs:

**Condition:**
```
freeHeap >= RESTART_MIN_FREE_HEAP_BYTES (96 KB)  // plenty of total RAM
contigHeap < SECONDARY_BUFFER_BYTES (52 KB)       // but no contiguous block
!fragmentationRecoveryRestartAttempted_            // first attempt only
```

**Flow:**

1. Save reading progress to SD.
2. Allocate a scratch buffer: `scratchSize = renderer.getDisplayWidthBytes() * renderer.getDisplayHeight()` (~20 KB on current hardware).
3. Call `renderer.releaseFrameBuffersWithScratch(scratch, scratchSize)` — frees the primary buffer (~48 KB) and installs `scratch` as the temporary framebuffer. Pixel writes from the warm pass land in the scratch and are discarded on reboot.
4. Call `section->warmAllImageCaches(0, 0, forceLoad, true)` — now with both primary (~48 KB) and secondary (~52 KB) freed, ~96 KB of heap is available as a single contiguous block. The PNG decoder fits comfortably.
5. Leak the scratch buffer intentionally (reboot is unconditional after this point).
6. Call `trySilentRestartToReaderForHeapRecovery()` — sets the RTC reboot target, reboots. `heapRecoveryRestartLatch` in RTC memory prevents a second recovery reboot if the first one also fails.

After the reboot the section cache is on SD and the `.pxc` image caches are also on SD. The fresh heap has no fragmentation and the reader renders normally.

## `releaseFrameBuffersWithScratch`

```cpp
bool GfxRenderer::releaseFrameBuffersWithScratch(uint8_t* scratch, size_t scratchSize);
```

Calls `display.releaseBuffers()` (frees both `frameBuffer0` and `frameBuffer1` via `free()`), then sets `frameBuffer = scratch`. After this call rendering "works" in the sense that pixel writes do not crash — they go to the scratch buffer — but nothing meaningful is displayed. The device must reboot before any real display operation.

`scratchSize` must be `>= panelWidthBytes * panelHeight`. The function validates this and returns false if not.

## Primary vs secondary: why the asymmetry

The secondary buffer can be released mid-session because BW rendering only uses `frameBuffer` (primary). The secondary is only needed for the AA pass (read as the previous-frame snapshot) and fast differential refresh. Losing it degrades quality temporarily but never corrupts the display.

The primary buffer cannot be released mid-session — it is the active draw target and `frameBuffer = nullptr` would cause a null-pointer write on the next render call. `releaseFrameBuffersWithScratch` substitutes the scratch to prevent this, but marks the device as pre-reboot only.
