# Font selector previews in a lent-framebuffer arena — design

Working spec (docs layout convention: removed or folded into `docs/memory-allocation-strategy.md`
before the work merges). Agreed with the user on 2026-10-07.

## Problem

The font selector (`FontSelectionActivity`) builds each SD font's preview by loading the font,
drawing a sample sentence and storing the strip (`FontPreviewCache`). On an X3 the heap is lowest at
exactly that moment. Measured 2026-10-07 with the #395 build:

| State | Free heap |
|---|---|
| Home (steady) | 51.5 KB |
| Settings | 42.4 KB |
| + Settings submenu + font selector (both stacked on Settings) | 31.2 KB |
| + one SD font loaded for a preview (Bitter 16 pt) | 19.1 KB, low point **10.1–10.6 KB**, biggest block **10.7 KB** |
| previews cached, font dropped | 31.1 KB |

A preview load costs ~12 KB, plus transients such as the 4 KB kern chunk buffer. That leaves the
selector one awkward allocation away from failing. The warm-up already gives up for lack of a large
block (`[FPRV] Stopping warm-up … below floor`). After a reading session it was worse (4,664 B);
PR #404 fixes that part.

## Goal and success criteria

The font selector is safe while it builds previews, from **both entry points**: Settings, and a
book's reader menu (per-book font family).

- During preview generation, free heap stays **≥ 20 KB** and the biggest free block **≥ 16 KB**
  (Settings path, X3, Bitter 16 pt with previews not yet cached). The `[FPRV]` before/after heap lines
  move by at most 3 KB per load (the `SdCardFont` object and map entries stay on the heap).
- Previews look the same: a stored `.fpv` strip is byte-identical to one built on the heap.
- X4 keeps FAST refreshes in the selector.
- The reader's behaviour and memory are unchanged.

Non-goals: other Settings sub-screens; the ~20 KB the Settings screens hold while covered; moving
preview generation elsewhere (Home idle).

## Design

### 1. Allocator seam in `SdCardFont`

- Every array `SdCardFont` allocates goes through two private helpers instead of
  `new (std::nothrow) T[n]` / `delete[]`:
  - `allocArray<T>(n)`: from the font's `BuildArena` when one is set, otherwise the heap, exactly as
    today;
  - `freeArray(p)`: a no-op in arena mode, `delete[]` otherwise.

  Covered: full intervals, kern class tables, ligature pairs, the mini interval/glyph/bitmap/kern
  tables, the per-prewarm mapping and `needsRead` arrays, and the glyph-miss overflow bitmaps.
- `SdCardFont::useArena(BuildArena*)` may be called only before `load`/`loadFromMmap`. The arena is
  fixed for the font's lifetime, so an array is never freed through the wrong path.
- Short-lived buffers (kern chunk buffer, row buffer, codepoint list, kern scratch) use a reserved
  arena block that is rewound before the function returns. That takes the transient spike off the
  heap too. In heap mode they stay `unique_ptr`s, as today.
- Stays on the heap: the `SdCardFont` object itself and the renderer's map entries (both small).
- Rule: a font must be unloaded before its arena block is rewound (same rule as
  `FontCacheManager::ScopedSlotArena`). Section 2 enforces it.
- Heap mode is unchanged byte for byte. The reader never sets an arena.

### 2. Plumbing and the selector's preview scope

- One optional `BuildArena*` parameter (default `nullptr`) threads through
  `SdCardFontSystem::ensureLoadedForPreview` → `ensureLoaded` → `SdCardFontManager::loadFamily`, which
  calls `font->useArena(arena)` before loading. Existing callers are untouched.
- The selector gets two helpers, the only way it loads or drops a preview font:
  - `loadPreviewFont(family)`: calls `dropPreviewFont()` first, then reserves a block on the
    selector's arena and calls `ensureLoadedForPreview(…, arena)`;
  - `dropPreviewFont()`: `sdFontSystem.unload(renderer)`, then releases that block.

  All existing `sdFontSystem.unload(renderer)` calls in the selector become `dropPreviewFont()`. These
  are in `onExit`, `prepareSdPreview` (cached-strip hit), `updatePreviewFontLocked` (built-in row)
  and `advanceWarmup`. The arena then never holds more than one family, and unload always precedes
  rewind.
- Cached strip: no font is loaded and no block is reserved (as today).
- From a book's menu, previewing the family the reader already has loaded at that size:
  `ensureLoaded` returns early with the reader's heap copy. The block stays empty, and
  `dropPreviewFont()` unloads it, as `onExit` does today.

### 3. Lending the secondary framebuffer

- **At `onEnter`, before the first preview load:** first finish any pending async refresh. Then, as
  `HomeActivity` does: `syncWriteBufferFromDisplayed()`, `syncRedRamFromFrameBuffer()` (not on X3),
  `borrowSecondaryBuffer()`, wrap the region in a `BuildArena`, and `setSingleBufferFastDiff(true)`.
  The secondary still holds the screen being left, so it is the right FAST baseline.
- **At `onExit`:** `dropPreviewFont()`, then destroy the arena, `returnSecondaryBuffer()` and
  `setSingleBufferFastDiff(false)` (Home's order, `HomeActivity.cpp:461-473`). It cannot fail: the
  region never entered the heap.
- **Display:** the selector draws no greyscale. The X4 keeps FAST via the RED seed, and the X3 keeps
  its baseline in the controller.
- **Async hazard (X3, X4 Pro):** while the buffer is lent, a finishing async refresh re-reads the
  write buffer. So nothing may draw into it while a refresh is pending. The selector never draws
  deliberately during a refresh (no grey planes; warm-up previews are ordinary frames), so it relies
  on the framework finishing each refresh before the next frame, as Home's carousel does while
  lending. The plan verifies this in the render path. If it does not hold, use blocking refreshes
  while lent (`shipSleepGrayBase` rule).
- **Docs:** add the font selector to "who borrows" in `docs/memory-allocation-strategy.md` §9.1.

### 4. Fallback and failures

- **No buffer to borrow** (a reader's background build holds it; a board has none): the arena
  pointer is null and every preview loads on the heap exactly as today. The existing warm-up floor
  check stays.
- **Arena too small for a load** (not expected; ~15–20 KB needed of ~52 KB): drop the font, rewind,
  retry that one font on the heap, log it.

## Testing

**Host**, a new suite beside `test/sd_font_intervals`, reusing its synthetic `.cpfont` writer:
1. Arena-mode load and prewarm give identical glyph metadata, bitmaps and kerning to heap mode.
2. After a load the arena is in use; after unload and block release it is back to its start with no
   crash, so no arena pointer reaches `delete[]`.
3. Two consecutive loads in one scope use the same amount, so transient blocks are rewound.
4. An arena too small for the load makes it fail cleanly with nothing leaked.
5. The existing suites (heap mode) still pass.

**Device, X3:**
- Settings › Font Family at 16 pt with uncached previews: each `[FPRV]` line moves by at most 3 KB; low point
  ≥ 20 KB with a biggest block ≥ 16 KB.
- The same from a book's reader menu.
- `.fpv` byte comparison between a heap-built and an arena-built strip.
- X4: FAST refreshes kept in the selector, if an X4 is available.

## Risks and checks for the plan

- `SdCardFont` frees arrays in several places, including re-prewarm replacing mini tables
  (allocate new, then free old) and `unloadMetadata`/`reloadMetadata`. In arena mode re-prewarm just
  grows the arena until the font is dropped; one preview prewarms once, so this stays small.
  `unloadMetadata` is reader-only.
- An `SdCardFont` loaded from flash (`loadFromMmap`) aliases its metadata to flash. Only the
  prewarm arrays go to the arena.
- Confirm in the plan that the activity render path finishes a pending async refresh before drawing
  the next frame.
