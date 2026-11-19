# Future work

Open work that is known, analysed, and not done yet. Each file collects the open items for one
topic: what is open, why it matters, where in the code, what was already tried, and the next step.
When an item is done, delete it from its file; when a file has no items left, delete the file.

| File | Topic |
|---|---|
| [alternative-sync-providers.md](alternative-sync-providers.md) | OPDS progression sync (Kavita, Audiobookshelf) and BookOrbit, offered in the reader itself |
| [cjk-ui-faces-on-card.md](cjk-ui-faces-on-card.md) | The CJK UI faces into the CJK languages' card packs: why they stayed in the image in 2.41, and what moving them takes |
| [cjk.md](cjk.md) | CJK: the follow-ups of what shipped (justification, squeeze and grid (A3), ruby and emphasis dots (B), TTF SD mode), CJK `.cpfont` bugs, 龘, the Font Manager follow-ups, the glyph store's index rebuild, UI text, the dictionary, line spacing, arena follow-ups, edge cases from the reviews, the matcha-reader decisions (furigana toggle, per-script settings, deinflection table, manga) |
| [book-cache.md](book-cache.md) | Book replacement that still wipes reading state, TXT/MD caches, the cache a USB Transfer `rm` leaves, cache-clear inconsistencies |
| [boards-and-touch.md](boards-and-touch.md) | Light sleep with the frontlight, the T5S3 SD clock, trace scaffolding, the X4 Pro fast-DU shortcut, device validation (also the 2.53 cold opens on the X4 and the other S3 boards), touch follow-ups, the BOOX Picco after its first build, the motion sensor's power and SDK gating, USB attach in light sleep on the X4, symbolized crash traces, the button cursor on touch boards |
| [display-and-refresh.md](display-and-refresh.md) | Framebuffer baselines, sleep ghosting follow-ups, windowed refresh, busy indicator, pre-render coverage, one-pass text AA (long reads, the X4's hold row, the UC8279 X3 tail fix, a Background-B borrow forcing two passes, the kept first page's two passes, other controllers), Murphy M3 gray (DC over weeks, temperature, Free-Ink#175), sleep images scaled up, landscape screenshots, hazards for the next SDK resync |
| [dictionary.md](dictionary.md) | The dictionary beyond CJK: phrase lookups (planned, not started), swipes in a definition, a word across a page break, `.dict.dz` without a resident chunk table, vocabulary history (decide) |
| [file-transfer.md](file-transfer.md) | USB Transfer without a protected-path rule; deleting full folders and hidden files from the web and WebDAV (decide) |
| [font-flash.md](font-flash.md) | The font picker's card read, remaining flash and speed levers on the font path, glyph advances rounded twice, the C3 sdkconfig trim (decide) |
| [library.md](library.md) | Library: measuring a large card, "Library too large", card-reader changes, silent build failures, interrupted builds, New's covers, author lists, sidecar case, tabs on button-only boards, cover thumbnails' midtones (decide), tests |
| [list-input-harmonization.md](list-input-harmonization.md) | Screens not yet on ListController, and the cleanup after them |
| [memory-and-parsing.md](memory-and-parsing.md) | Nested-table text, the simplified-chapter hint, heap transients, preempted decodes, progressive JPEGs at 1/8 (covers that restart, the I/O of libjpeg's script), damaged JPEGs (parked), per-line and per-cell objects on the heap, throwing allocations left in the section build, `<style>` blocks, stylesheets per chapter, inline anchors, JPEG source reads |
| [murphy-small-screen.md](murphy-small-screen.md) | Murphy M3: fixed sizes and layouts that assume a panel larger than 240x416, with the central root causes first |
| [opds-and-kosync.md](opds-and-kosync.md) | Gutenberg's repeated OpenSearch fetch and truncated-field warning, KOSync redirect gate and log labels, the auto-push's XPath mapping on the heap |
| [plugins.md](plugins.md) | Plugins after #797-#824: `api.crypto`, flows not tried live, `/api/fetch` errors (#827), events (later), the survey's leftovers |
| [protected-content.md](protected-content.md) | Protected content (DRM): handover after D1 (merged, run on the X4 Pro and the X3), the listing change, the cache enciphered at rest (D2), the Adobe plugin's `api.crypto` (D3, for Libby and Onleihe), LCP through Free-Ink (asked) |
| [reader-priority-scheme.md](reader-priority-scheme.md) | The reader's render passes and background work against one priority scheme (complete page, then the page's own pictures, then the rest): where the code breaks it, how long each controller needs which framebuffer, the gray during a chapter build on the X3 and X4, look-ahead that costs page turns their one-pass, the second core on the S3 boards; order of work and open decisions |
| [reader-state.md](reader-state.md) | Mark as read vs. a pending page save, menu actions after a layout change, sync mid-build, front matter skipped after a first-open pull, dead override overloads, `CMD:REBOOT` dropping a page save, a deleted font family left as the TXT/Markdown font, bookmarks after a relayout (Global Bookmarks labels, same-layout rebuilds, the X3 run, the accepted slower turn), Back after a jump (decide) |
| [screen-flow.md](screen-flow.md) | Screens that end on a message and wait for Back: one timed return for all of them; a quick font menu in the reader |
| [screen-layout.md](screen-layout.md) | Overlaps and layout arithmetic left from the header/body audit, cleanup |
| [test-suite.md](test-suite.md) | Host test suite: firmware paths with no host test, tests blind to corruption, goldens to extend, renames and moves, small cleanups from the second audit, the epub_optimizer sweep |
| [txt-pipeline.md](txt-pipeline.md) | TXT on the EPUB pipeline and the browser converter to UTF-8: decided, not started; design, traps, order of work |
| [ui-languages-and-keyboards.md](ui-languages-and-keyboards.md) | Estonian as an interface language and a Vietnamese keyboard (both decide) |
| [writing-modes-handover.md](writing-modes-handover.md) | The writing-modes and scripts work as of 2026-11-09: what is merged, what is left in a working order (the shaping engine's speed after the code audit, what the X3 profile found beyond it, the smaller Arabic gaps, UI text and its untranslated settings labels, Myanmar's follow-ups, flash, the limits of vertical text), the measurements behind it, how device runs are done, and the traps |

Design records for shipped features, cited from code, are in [../design/](../design/); reference
documentation is in [../contributing/](../contributing/) and the other files under `docs/`.
