# Future work

Open work that is known, analysed, and not done yet. Each file collects the open items for one
topic: what is open, why it matters, where in the code, what was already tried, and the next step.
When an item is done, delete it from its file; when a file has no items left, delete the file.

| File | Topic |
|---|---|
| [alternative-sync-providers.md](alternative-sync-providers.md) | OPDS progression sync (Kavita, Audiobookshelf) and BookOrbit, offered in the reader itself |
| [cjk.md](cjk.md) | CJK: justification, squeeze and grid (A3), ruby (B), CJK `.cpfont` bugs, 龘, the Font Manager follow-ups, TTF SD mode, UI text, the dictionary, arena follow-ups, edge cases from the A2 reviews |
| [book-cache.md](book-cache.md) | Book replacement and deletion that still wipe reading state, TXT/MD caches, cache-clear inconsistencies |
| [boards-and-touch.md](boards-and-touch.md) | Light sleep with the frontlight, board-name checks still to convert, the T5S3 SD clock, trace scaffolding, the X4 Pro fast-DU shortcut, device validation, touch follow-ups |
| [display-and-refresh.md](display-and-refresh.md) | Framebuffer baselines, sleep ghosting follow-ups, windowed refresh, busy indicator, pre-render coverage |
| [dictionary.md](dictionary.md) | The dictionary beyond CJK: phrase lookups (planned, parked), what was considered and not taken |
| [font-flash.md](font-flash.md) | Remaining flash and speed levers on the font path |
| [library.md](library.md) | Library: measuring a large card, "Library too large", card-reader changes, silent build failures, interrupted builds, New's covers, author lists, sidecar case, tabs on button-only boards, tests |
| [list-input-harmonization.md](list-input-harmonization.md) | Screens not yet on ListController, and the cleanup after them |
| [memory-and-parsing.md](memory-and-parsing.md) | Nested-table text, case-blind class and id selectors, a block-ending `<br>`'s blank line, the simplified-chapter hint, the image lane and Background-B, heap transients, preempted decodes, progressive JPEGs at 1/8 (covers that restart, the I/O of libjpeg's script), damaged JPEGs (parked) |
| [murphy-gray.md](murphy-gray.md) | Murphy M3: what is still open about gray, UI ghosting, why HALF and image midtones stay as they are, what is offered upstream |
| [murphy-small-screen.md](murphy-small-screen.md) | Murphy M3: fixed sizes and layouts that assume a panel larger than 240x416, with the central root causes first |
| [one-pass-text-aa.md](one-pass-text-aa.md) | One-pass text AA: shipped on the X4 Pro with UC8179, the Murphy M3, the X3 and the X4; measurements, the UC8279 gray swap behind upstream #4344, what stays open, the Murphy waveform and its measurements, what carries over to other drivers |
| [opds-and-kosync.md](opds-and-kosync.md) | Gutenberg's repeated OpenSearch fetch and truncated-field warning, KOSync redirect gate and log labels |
| [reader-state.md](reader-state.md) | Mark as read vs. a pending page save, menu actions after a layout change, sync mid-build, dead override overloads |
| [screen-flow.md](screen-flow.md) | Screens that end on a message and wait for Back: one timed return for all of them |
| [screen-layout.md](screen-layout.md) | Overlaps and layout arithmetic left from the header/body audit, cleanup |
| [test-suite.md](test-suite.md) | Host test suite: firmware paths with no host test, tests blind to corruption, goldens to extend, renames and moves, small cleanups from the second audit |
| [txt-pipeline.md](txt-pipeline.md) | TXT on the EPUB pipeline and the browser converter to UTF-8: decided, not started; design, traps, order of work |
| [writing-modes.md](writing-modes.md) | Right-to-left (Hebrew, Arabic) and vertical (Chinese, Japanese, Korean) text: the roadmap, findings from real books, what step 4 inherits from vertical text; complex shaping for Arabic and the Indic scripts (Hindi), planned after CJK |
| [writing-modes-handover.md](writing-modes-handover.md) | The writing-modes and scripts work as of 2026-11-08: what is merged, what is left in a working order (the shaped-text heap follow-up, the right-to-left baseline profile, the engine speed, Hebrew reading, smaller Arabic and Indic gaps, CJK leftovers), the measurements behind it, how device runs are done, and the traps |

Design records for shipped features, cited from code, are in [../design/](../design/); reference
documentation is in [../contributing/](../contributing/) and the other files under `docs/`.
