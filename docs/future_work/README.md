# Future work

Open work that is known, analysed, and not done yet. Each file collects the open items for one
topic: what is open, why it matters, where in the code, what was already tried, and the next step.
When an item is done, delete it from its file; when a file has no items left, delete the file.

| File | Topic |
|---|---|
| [alternative-sync-providers.md](alternative-sync-providers.md) | OPDS progression sync (Kavita, Audiobookshelf) and BookOrbit, offered in the reader itself |
| [cjk-ui-faces-on-card.md](cjk-ui-faces-on-card.md) | The CJK UI faces into the CJK languages' card packs: why they stayed in the image in 2.41, and what moving them takes |
| [cjk.md](cjk.md) | CJK: the follow-ups of what shipped (justification, squeeze and grid (A3), ruby and emphasis dots (B), TTF SD mode), CJK `.cpfont` bugs, 龘, the Font Manager follow-ups, the glyph store's index rebuild, UI text, the dictionary, line spacing, arena follow-ups, edge cases from the reviews |
| [book-cache.md](book-cache.md) | Book replacement and deletion that still wipe reading state, TXT/MD caches, cache-clear inconsistencies |
| [boards-and-touch.md](boards-and-touch.md) | Light sleep with the frontlight, the T5S3 SD clock, trace scaffolding, the X4 Pro fast-DU shortcut, device validation, touch follow-ups |
| [display-and-refresh.md](display-and-refresh.md) | Framebuffer baselines, sleep ghosting follow-ups, windowed refresh, busy indicator, pre-render coverage, one-pass text AA (long reads, the X4's hold row, the UC8279 X3 tail fix, a Background-B borrow forcing two passes, other controllers), Murphy M3 gray (DC over weeks, temperature, Free-Ink#175) |
| [dictionary.md](dictionary.md) | The dictionary beyond CJK: phrase lookups (planned, not started) |
| [font-flash.md](font-flash.md) | Remaining flash and speed levers on the font path |
| [library.md](library.md) | Library: measuring a large card, "Library too large", card-reader changes, silent build failures, interrupted builds, New's covers, author lists, sidecar case, tabs on button-only boards, tests |
| [list-input-harmonization.md](list-input-harmonization.md) | Screens not yet on ListController, and the cleanup after them |
| [memory-and-parsing.md](memory-and-parsing.md) | Nested-table text, a block-ending `<br>`'s blank line, the simplified-chapter hint, heap transients, preempted decodes, progressive JPEGs at 1/8 (covers that restart, the I/O of libjpeg's script), damaged JPEGs (parked), per-line and per-cell objects on the heap |
| [murphy-small-screen.md](murphy-small-screen.md) | Murphy M3: fixed sizes and layouts that assume a panel larger than 240x416, with the central root causes first |
| [opds-and-kosync.md](opds-and-kosync.md) | Gutenberg's repeated OpenSearch fetch and truncated-field warning, KOSync redirect gate and log labels |
| [reader-state.md](reader-state.md) | Mark as read vs. a pending page save, menu actions after a layout change, sync mid-build, front matter skipped after a first-open pull, dead override overloads, `CMD:REBOOT` dropping a page save, a deleted font family left as the TXT/Markdown font |
| [screen-flow.md](screen-flow.md) | Screens that end on a message and wait for Back: one timed return for all of them; a quick font menu in the reader |
| [screen-layout.md](screen-layout.md) | Overlaps and layout arithmetic left from the header/body audit, cleanup |
| [test-suite.md](test-suite.md) | Host test suite: firmware paths with no host test, tests blind to corruption, goldens to extend, renames and moves, small cleanups from the second audit, the epub_optimizer sweep |
| [txt-pipeline.md](txt-pipeline.md) | TXT on the EPUB pipeline and the browser converter to UTF-8: decided, not started; design, traps, order of work |
| [writing-modes-handover.md](writing-modes-handover.md) | The writing-modes and scripts work as of 2026-11-09: what is merged, what is left in a working order (the shaping engine's speed after the code audit, what the X3 profile found beyond it, the smaller Arabic gaps, UI text and its untranslated settings labels, Myanmar's follow-ups, flash, the limits of vertical text), the measurements behind it, how device runs are done, and the traps |

Design records for shipped features, cited from code, are in [../design/](../design/); reference
documentation is in [../contributing/](../contributing/) and the other files under `docs/`.
