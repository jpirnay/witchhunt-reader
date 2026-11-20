# The font prewarm sets the session's heap minimum

Part of [Memory and parsing](../memory-and-parsing.md).

**Partly done (#515, 2026-10-19).** Re-measured on the X3 (Build Through test book, `[MEM]` and per-phase lows of `renderContents`): the prewarm no longer sets the session's minimum. With the image decodes out of Background-B's way (#515, [the image lane](../../background-rendering.md#image-lane--pixel-caches-ahead-of-the-reader)) the low point of a session sat at ~33 KB, inside B's build, and a normal render's phases stayed within 1-2 KB of their start:

| Page render, X3 | start | prewarm low | BW render low |
|---|---|---|---|
| TrueType body (BookerlyTTF), glyphs in the store | 50.5 KB | 48.6 KB | 50.3 KB |
| the same, first page after the glyph store was installed again, before | 50.3 KB | 38.7 KB | 39.9 KB |
| the same, after | 50.5 KB | 48.7 KB | 50.3 KB |
| built-in Bookerly | 53.0 KB | 45.0 KB | 49.4 KB |

What changed: a render lends the secondary buffer to the prewarm's TrueType misses as it lends it to an image decode (the glyph store has no glyph yet: a 10.5-14.5 KB scratch block, in SD mode a 4.2 KB page cache, each taken from the lent region and given back per miss), and whatever a miss still made on the heap is freed before the BW render instead of when RenderLock goes back (`renderContents`, `EpubReaderActivity.cpp:6652`; the lend at `:6934`, the release at `:6940`).

**Still open.** Two transients remain, neither the session's minimum on these measurements:
- The built-in fonts' prewarm: 8 KB, of which the page slots (3.5 KB) stay for the BW render and the AA pass, and the
  rest is the group stream ring, already freed per group. **Done since (#519, 2026-10-19):** `renderContents` lends the
  buffer to every prewarm, not only with a TrueType set, and `FontDecompressor` takes a ring from its slot arena, else
  from the region lent to the glyph misses (`glyph_miss::Scope`; `ringArena`, `FontDecompressor.cpp:69`), giving it back
  before the next group; the slots stay on the heap. On the host the prewarm's heap peak loses its largest ring (3,036 B
  for a Noto Sans 14 sample; up to 4,096 B, `GROUP_RING_MAX_BYTES` in `lib/EpdFont/scripts/fontconvert.py:937`). On the
  X3 (#519's test) the prewarm's free low median went from 43,434 to 45,832 B. The pre-render still cannot lend the
  buffer (it holds the page on screen), so most turns, which show a pre-rendered page, are unchanged. Since 2.50
  (029416411) a C3 page that goes out composed one-pass (X3, X4, One-Pass Anti-Aliasing on, FAST, text only) does not
  lend either (`prewarmKeepsFrameOnScreen`, `EpubReaderActivity.cpp:6913-6914`): the buffer holds the frame on screen
  the one-pass needs, so that page's prewarm is back on the heap (the 43,434 B median for the ring; a TrueType miss's
  10.5-14.5 KB until `releaseTtfMissMemory()` frees it before the BW render). A page whose glyphs starved before
  (`glyphStarvedPage`) still lends, and the S3 boards lend a PSRAM arena and keep the frame either way.
- A pre-render with TrueType misses (15-26 KB on the X3, `PreRenderGate.h`) cannot lend for the same reason
  (`renderPreRenderPass`, `EpubReaderActivity.cpp:5021`). Its heap floor already asks for that cost (`:1623`).

**Next step.** None until a session's minimum is found in a render again. The per-phase lows came from a measurement-only patch (`heap_caps_monitor_local_minimum_free_size_start` restarted at each phase boundary of `renderContents`); a permanent version would cost the same monitor B's borrow uses.
