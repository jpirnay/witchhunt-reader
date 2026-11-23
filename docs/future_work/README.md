# Future work

Open work that is known, analysed, and not done yet. Each file collects the open items for one
topic: what is open, why it matters, where in the code, what was already tried, and the next step.
When an item is done, delete it from its file; when a file has no items left, delete the file.

A topic that several PRs work on at the same time keeps its items as one file each, in a folder named after the
topic ([memory-and-parsing/](memory-and-parsing/)); the topic's own file stays as its page and lists no items. A PR
that closes such an item deletes its file, and a follow-up it finds goes into a new file. A row in this table describes
its topic, not each item, so closing or adding an item does not touch the table. Items that share one file or one list
make parallel PRs conflict: two PRs that each delete a neighbouring bullet edit adjacent lines.

| File | Topic |
|---|---|
| [alternative-sync-providers.md](alternative-sync-providers.md) | OPDS progression sync (Kavita, Audiobookshelf) and BookOrbit: Plan A in the firmware or push through plugin device events (the owner's decision), BookOrbit's KOSync test |
| [boards-and-touch.md](boards-and-touch.md) | Light sleep with the frontlight (X4 Pro, T5S3), the T5S3 SD clock, trace scaffolding, the X4 Pro fast-DU shortcut, device validation (touch sessions, the T5S3's cold opens, the X4's clock drift, the Picco's touch in light sleep), touch follow-ups, the BOOX Picco after its first build, the motion sensor, USB attach in light sleep on the X4, crash traces, the button cursor on touch boards |
| [book-cache.md](book-cache.md) | Book replacement that still wipes reading state, TXT/MD caches, the cache a USB Transfer `rm` leaves, cache-clear inconsistencies |
| [cjk-ui-faces-on-card.md](cjk-ui-faces-on-card.md) | The CJK UI faces into the CJK languages' card packs: why they stayed in the image, what moving them takes, card mode |
| [cjk.md](cjk.md) | CJK follow-ups: ruby, emphasis and push-in edge cases, hand-converted `.cpfont` bugs, 龘, the Font Manager and font-selector previews, the glyph store's index rebuild, UI text, the dictionary on CJK pages, line spacing, the page glyph arena, review edge cases, the matcha-reader decisions (furigana toggle, per-script settings, manga) |
| [complex-scripts.md](complex-scripts.md) | Right-to-left text and the shaped scripts: the shaping engine's speed after #683/#872/#874, the right-to-left line pass, the Arabic gaps, Markdown bidi across spans, Myanmar's follow-ups, flash, what is not planned |
| [dictionary.md](dictionary.md) | The dictionary beyond CJK: phrase lookups (planned, not started), definitions that fall back to plain text, swipes in a definition, a word across a page break, `.dict.dz` without a resident chunk table, vocabulary history (decide) |
| [display-and-refresh.md](display-and-refresh.md) | Framebuffer baselines and the PanelSel split, windowed refresh, `drawImage()` rotation, pre-render coverage, the heap-restart sleep race, one-pass text AA (long reads, the X4's hold row, the UC8279 X3 tail fix, other controllers), Murphy M3 gray, sleep images scaled up, sleep pictures above four levels (X3 and X4 plans, temperature, DC balance), landscape screenshots, hazards for the next SDK resync |
| [file-transfer.md](file-transfer.md) | USB Transfer without a protected-path rule; deleting full folders and dot entries from the web and WebDAV (decide; porting upstream #4031 is one option) |
| [font-flash.md](font-flash.md) | The font picker's card read, remaining flash and speed levers on the font path, glyph advances and scaled kerns rounded inconsistently, the C3 sdkconfig trim (decide) |
| [library.md](library.md) | Library: measuring a large card, "Library too large", card-reader changes, silent build failures, interrupted builds, New's covers, author lists, sidecar case, cover thumbnails' midtones (decide), tests |
| [list-input-harmonization.md](list-input-harmonization.md) | Screens not yet on ListController and `fui::list` (which also brings them the tap flash), and the cleanup after them |
| [memory-and-parsing.md](memory-and-parsing.md) | The section build's memory and parsing: tables, pictures, page limits, the font prewarm, image extraction and decoding, CSS scoping, anchors, allocations on the build path. One file per item in [memory-and-parsing/](memory-and-parsing/) |
| [murphy-small-screen.md](murphy-small-screen.md) | Murphy M3: fixed sizes and layouts that assume a panel larger than 240x416 (System Information, XTC pages, Home's narrow card, landscape overlays), the root cause left first |
| [opds-and-kosync.md](opds-and-kosync.md) | Gutenberg's repeated OpenSearch fetch and its over-long attribute warning, KOSync redirect gate and log labels, the auto-push's XPath mapping on the heap |
| [plugins.md](plugins.md) | Plugins after #797-#861: `protected-content` with a real account, flows not tried live, events step 2 parked (`book.downloaded`, `require_meta`: Readeck, ybx), the survey's leftovers (`/api/settings?keys=`, the job queue: loan renewal) |
| [protected-content.md](protected-content.md) | Protected content (DRM) after D1 (#821), D3 (#857) and D2 (#880): a real Adobe account end to end, the messages' translations, LCP through Free-Ink (asked), D2's device leads; decide: books from Adobe Digital Editions, the licence key over `/download`, a protected book's covers on another reader |
| [reader-priority-scheme.md](reader-priority-scheme.md) | What is left of the reader's priority scheme (the record is [../design/reader-priority-scheme.md](../design/reader-priority-scheme.md)): pages without their gray during a build, look-ahead that lends the frame on screen, an S3 page turn discarding Background-B's build, the build through its page's refresh on the X3 and T5S3, the page count, order slips on the loop task, the second core; open decisions |
| [reader-state.md](reader-state.md) | Mark as read vs. a pending page save, menu actions after a layout change, sync mid-build, front matter skipped after a first-open pull, `CMD:REBOOT` dropping a page save, a deleted font family left as the TXT/Markdown font, bookmarks after a relayout (Global Bookmarks labels, same-layout rebuilds, the X3 run), Back after a jump (decide) |
| [screen-flow.md](screen-flow.md) | Screens that end on a message and wait for Back: one timed return for all of them; the reader bars' layout change that still waits |
| [screen-layout.md](screen-layout.md) | Overlaps and layout arithmetic left from the header/body audit, cleanup |
| [test-suite.md](test-suite.md) | Host test suite: firmware paths with no host test, tests blind to corruption, goldens to extend, renames and moves, small cleanups from the second audit, the epub_optimizer sweep |
| [txt-pipeline.md](txt-pipeline.md) | TXT on the EPUB pipeline and the browser converter to UTF-8: decided, not started, parked; design, traps, order of work |
| [ui-languages-and-keyboards.md](ui-languages-and-keyboards.md) | Estonian as an interface language and a Vietnamese keyboard (both decide), a CJK keyboard |

Design records for shipped features, cited from code, are in [../design/](../design/); reference
documentation is in [../contributing/](../contributing/) and the other files under `docs/`.
