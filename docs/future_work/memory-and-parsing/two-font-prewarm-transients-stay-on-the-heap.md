# Two font prewarm transients stay on the heap

Part of [Memory and parsing](../memory-and-parsing.md).

**Open, parked until a device log shows it.** A page render lends the secondary buffer to its font prewarm, and on
the X3 the prewarm no longer sets the session's heap minimum (#515, #519; [Memory Allocation
Strategy §9.4](../../memory-allocation-strategy.md#94-font-prewarm-transients)). Two prewarms cannot lend, because the
buffer holds a frame the display needs, and take their memory from the heap:

- **A pre-render with TrueType misses** (15-26 KB on the X3, `PreRenderGate.h`): the buffer holds the page on screen
  (`EpubReaderActivity::renderPreRenderPass`). Its heap floor already asks for that cost
  (`EpubReaderActivity::preRenderMinFreeHeap`). Most turns show a pre-rendered page.
- **A C3 page that goes out composed one-pass** (X3, X4, One-Pass Anti-Aliasing on, FAST, text only; since 2.50,
  029416411): the buffer holds the frame on screen that the one-pass needs (`prewarmKeepsFrameOnScreen` in
  `EpubReaderActivity::renderContents`). That page's prewarm is back on the heap as before #519: the built-in
  fonts' group ring (the prewarm's free low median was 43,434 B on the X3 then), and a TrueType miss's 10.5-14.5 KB
  until `releaseTtfMissMemory()` frees it before the black-and-white render. A page whose glyphs starved before
  (`glyphStarvedPage`) still lends, and the S3 boards lend a PSRAM arena and keep the frame either way.

**What reopens it.** A device log in which a session's lowest `[MEM]` free or largest-block reading falls inside
`renderContents` or `renderPreRenderPass`, on the X3, in a book set in a TrueType face (triage spec §4). The per-phase
lows that closed it came from a measurement-only patch (`heap_caps_monitor_local_minimum_free_size_start` restarted at
each phase boundary of `renderContents`); a permanent version would cost the same monitor Background-B's borrow uses.
