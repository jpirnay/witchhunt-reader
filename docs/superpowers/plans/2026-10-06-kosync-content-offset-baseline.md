# Content-offset baseline (before any change)

- Host suite: 1464/1464.
- Round-trip allowances: test_table_cell_overflow {behind 2, ahead 0}; test_spine_toc_edges {behind 2, ahead 1}; all others {1, 0}.
- Goldens sha256-of-sha256s: f0b7d80e6e6c724558e253c33c7a7f6c257f2001c0ca090e71a4b345286af29f
- X3 (debug build, pre-change firmware; capture 2026-10-06 14:01-14:05, C:/tmp/x3-baseline.log):
  - Book B, *The Anarchy* "9 The Corpse of India" (spine 30, Calibre-wrapped): parse-and-build 7,143 ms; lowest during the build `free=42,176 contig=36,852` (one dip to `contig=30,708`); `Min Free` 35,176 B (set by the book open, not lowered by the build); no watermark drops during the build. Note: no power-cycle before this book.
  - Sync (manual, ended in an upload, fresh TLS handshake): before `free=63,804 contig=57,332`; `TLS handshake heap: preLargest=57,332 troughLargest=40,948 consumedContig=16,384 troughFree=50,412`; after the PUT `free=62,780 contig=49,140`.
  - Book A, *Quang Âm Chi Ngoại* "Chương 2955" (spine 2955, body-child paragraphs, 32 pages): parse-and-build 1,845 ms; lowest during the build `free=44,680 contig=36,852`; `Min Free` 26,604 B (26,608 before the chapter, set by opening the 2,956-spine book; not lowered by the build). Power-cycled before this book.

## After Task 3 (X3, debug build `C:\tmp\firmware-task3-502265c4a.bin`, log `C:\tmp\x3-after-task3.log`, 2026-10-06 15:24)

Same two chapters and one sync, in a different order from the baseline: Book A first (fresh boot after flashing, opened from Home with its metadata cache already on the card), then Book B without a power-cycle, then the sync. Numbers are taken from the same log lines in both runs; where the baseline doc above quoted a different source, the `createSectionFile ... done` line is the comparison.

| | Baseline | After Task 3 | Delta |
|---|---|---|---|
| Book A `createSectionFile spine=2955 done: total` | 2,019 ms (parse 1,046) | 1,988 ms (parse 1,063) | parse +17 ms |
| Book A arena `highWater` / parse lane | 33,404 / 14,912 | 33,404 / 12,673 | 0 / −2,239 |
| Book B `createSectionFile spine=29 done: total` | 7,678 ms (parse 6,649, finalize 53) | 10,012 ms (parse 6,641, finalize 49) | parse −8 ms; total +2,334 ms outside parse (see note) |
| Book B arena `highWater` / parse lane | 46,048 / 9,736 | 46,056 / 9,664 | +8 / −72 |
| Watermark drop attributable to a build | none | −4 B (Book A), −28 B (Book B) at `render_start` | noise |
| Sync TLS `troughFree` / `consumedContig` | 50,412 / 16,384 | 50,288 / 16,384 | −124 B / 0 |
| Sync `after_updateProgress` free / contig | 62,780 / 49,140 | 62,692 / 55,284 | −88 B / +6,144 B |

- **Parse cost of the per-page offset counting: nil.** Both chapters parse within ±20 ms of the baseline and the arena high-water marks are identical (Book A) or +8 B (Book B). The parse lanes are smaller, not larger.
- **Book B total +2.3 s is not in the parser.** Setup, parse and finalize are all equal; the extra wall time sits between them (yields to the foreground; the 10 s MEM sample during the build read `CPU: 10 MHz`, i.e. the build was idle-waiting when sampled). The baseline build ran on an idle device at 160 MHz throughout.
- **Min Free is not comparable between the runs, and the difference is not this branch's.** After: Book A's open from Home with the 2,956-entry metadata cache already present dipped the watermark from 43,672 to 18,888 during the 2.7 s `Loading ePub` phase, before the reader's `onEnter` (which then reported free=65,244). Baseline: Book A was indexed from scratch (9.6 s, `No 47296 B block for the spine href index ... linear TOC lookup`) and its subsequent load left the watermark at 26,608. Neither path touches code this branch changes (`Epub`, `BookMetadataCache`); the builds then lowered the watermark by 4 B and 28 B. Book B's 18,856 is inherited from Book A (no power-cycle between them).
- **Observation for a separate issue:** loading an existing 2,956-entry metadata cache from Home leaves only 18.9 KB min free on this build, 7.7 KB less than the index-then-load path of the baseline. Worth a look on its own; it predates this branch.
- **Spec stop rule (>2 KB lower min free from the LUT change):** not triggered. The 16-bit delta encoding stays unneeded.
- The pushed position on this firmware is still the element path (`/body/DocFragment[30]/body/div...`), as expected: the offset push is Task 5.

### Addendum: Book B from a clean boot (Task 3 firmware, pasted monitor log, 2026-10-06 15:33)

The first after-run opened Book B without a power-cycle after Book A, so its Min Free (18,856) was inherited. A clean-boot run of Book B alone, with the reader menu opened mid-build and a sync from there:

| Point | Min Free |
|---|---|
| After boot, on Home | 43,712 |
| Mid-build, page 30 of 229 (10 s MEM sample) | 39,272 |
| Reader exit after menu + handoff (build interrupted at 39 pages) | 30,384 |
| After the sync (inflate to temp + two SAX passes + TLS) | 19,464 |

Baseline, same sync path (log 14:03): Min Free 40,868 before, **19,048** after the PUT. The sync trough is the same on both firmwares (+416 B on Task 3) and is the pre-change sync path: the 174 KB chapter inflated to a temp file and parsed twice (`countTotalTextBytes`, then `findXPathForProgress`), then the TLS handshake (troughFree 50,456 here, 50,412 baseline). Task 4 replaces the temp file with a streamed, early-stopping parse; the "after Task 4" measurement should read this same line.

The build's own minimum cannot be isolated from the Min Free counter in either run (the baseline's was set by the book open at 35,176 and never lowered by the build; this run's 30,384 includes the menu overlay on top of the borrowed arena). The only Task 3 transient on the parse path is 4 B per buffered word (`wordVisibleOffsets`), bounded by the page-sized word buffer; parse time and arena high-water are unchanged. A clean A/B of the build alone (clean boot, open Book B, no interaction until `Background-C ... complete`, read the next Min Free line) on both firmwares would settle it if wanted.

## After Task 8 (host)

- Host suite: `100% tests passed out of 1666` (ctest registers 1666 tests; 8 report Skipped by their own conditions on this platform, none failed).
- Goldens sha256-of-sha256s: f0b7d80e6e6c724558e253c33c7a7f6c257f2001c0ca090e71a4b345286af29f (equals the baseline).
- Round-trip allowances, measured: test_kerning_ligature {behind 1, ahead 0}; test_spine_toc_edges {behind 1, ahead 0}; all others {0, 0} (before: table_cell_overflow {2,0}, spine_toc_edges {2,1}, others {1,0}). Pages that push a text point come back exactly.
- Firmware `pio run -e default` (release): `RAM: 16.5% (used 53928 bytes from 327680 bytes)`, `Flash: 94.9% (used 6216755 bytes from 6553600 bytes)`; `firmware.bin` 6,230,272 B, +6,816 B against 6,223,456 B.

### Task 8b (deep text points on)

- Host suite: `100% tests passed out of 1666`; EpubPipelineTest 592 passed. Goldens hash unchanged (f0b7d80e...af29f).
- Round trip: every allowance {0, 0}, then the table removed: all 30 books, every page comes back on itself.
- Firmware: `RAM: 16.5% (used 53928 bytes from 327680 bytes)`, `Flash: 94.9% (used 6216789 bytes from 6553600 bytes)`; `firmware.bin` 6,230,304 B, +6,848 B against 6,223,456 B.

### Final-review fix round (numbers at b4b9c1f1d)

Tied page runs resolve to their last page; the mappers parse like the layout parser (void-tag repair, case-insensitive `<body>`); a page starting at the chapter's total names the end of the last text node.

- Host suite: `100% tests passed out of 1670` (8 Skipped by their own conditions, none failed); EpubPipelineTest 598 tests, 596 passed, 2 skipped. Goldens hash unchanged (f0b7d80e...af29f).
- Round trip: all 30 books, every page comes back on itself (zero drift).
- Firmware: `RAM: 16.5% (used 53928 bytes from 327680 bytes)`, `Flash: 94.9% (used 6216943 bytes from 6553600 bytes)`; `firmware.bin` 6,230,464 B, +7,008 B against 6,223,456 B.

## After Task 8 (X3), run 1 (firmware-final-b4b9c1f1d)

X3 debug build of b4b9c1f1d, log `C:/tmp/x3-after-task8.log`.

- **Book A build** (*Quang Âm Chi Ngoại*, spine 2955): `createSectionFile` total 2,118 ms (parse 1,044; baseline 1,046); arena `highWater` 33,404 (identical to the baseline). The push named `/body/DocFragment[2956]/body/div[1]/h1[1]/p[1]/text()[1].0`.
- **Book B sync** (*The Anarchy*, 174 KB chapter), taken mid-build (190 of 229 pages laid out, so the push fell back to the fraction): **Min Free 10,880 B against the baseline's 19,048 B**, a regression of ~8 KB.
- **Book A sync** (22 KB chapter): Min Free 23,440 B.
- **TLS `troughFree`**: 42,992 and 37,752 against the baseline's 50,412.

**Cause.** `Sync mem[before_performSync]: free=63712` is read after WiFi is up, and the local mapping ran there. It streams the chapter with the 32 KB inflate ring and the ~9.7 KB SaxParser state (plus the SaxFeedSink's buffers) alive at once. The old temp-file path had the ring alone, then the parser alone. Book A's chapter is small, so its ring is too and its trough stayed higher. TLS `troughFree` also fell (42,992 and 37,752 against 50,412), in both syncs, from the same pre-TLS heap (61.6 KB free, 57,332 contiguous): the cause is not established by this log, and run 2 reads it again.

**Run 2 follows with two fixes:**
1. The push position is mapped in `KOReaderSyncActivity::onEnter`, after the secondary framebuffer is released (~120 KB free) and before WiFi is brought up. The result is cached for the session (`Sync mem[after_local_mapping]`).
2. The mappers parse with `SaxParser::Profile::Lean`, without the unread attribute table. On the C3 `stateBytes` is 4,672 B instead of 9,712 B.

Build for run 2 (host and firmware):
- Host suite: `100% tests passed out of 1673`. Goldens hash unchanged (f0b7d80e...af29f).
- Firmware: `RAM: 16.5% (used 53928 bytes from 327680 bytes)`, `Flash: 94.9% (used 6217163 bytes from 6553600 bytes)`; `firmware.bin` 6,230,688 B, +7,232 B against 6,223,456 B.

## After Task 8 (X3), run 2 (firmware-run2-75ecffa2f: push mapped before WiFi, lean SAX profile; log `C:\tmp\x3-after-task8-run2.log`, 2026-10-06 22:07)

Sequence: Quang Âm "Chương 2955" build + push from page 3; power-cycle; The Anarchy "9 The Corpse of India" build to completion (229 pages) + push from page 3; then from page 5: a pull (intent 1), a compare from the landed page (cancelled), and a second pull from page 5.

| | Baseline | Run 2 | Delta |
|---|---|---|---|
| Quang Âm `createSectionFile` total / parse | 2,019 / 1,046 ms | 2,099 / 1,036 ms | parse −10 ms |
| Quang Âm arena `highWater` | 33,404 | 33,416 | +12 B (the Full SAX state grew by a pointer and a capacity) |
| The Anarchy total / parse (229 pages, build completed) | 7,678 / 6,649 ms | 7,784 / 6,792 ms | parse +143 ms (+2.2 %, one sample) |
| The Anarchy arena `highWater` | 46,048 | 46,056 | +8 B |
| Push, Quang Âm: Min Free before / after the sync | n/a | 26,784 / 26,784 | untouched |
| Push, The Anarchy (174 KB chapter): Min Free before / after | 19,048 after (baseline sync) | 32,800 / 32,800 | untouched (run 1: 10,880) |
| `Sync mem[after_local_mapping]` (before WiFi) | n/a | free=120,544 / 120,132 | the push maps with the radio down |
| Largest block entering TLS after an early mapping | 57,332 | 47,092 (Quang Âm), 42,996 (The Anarchy) | −10 to −14 KB; TLS `troughLargest` 38,900 / 34,804 against the 26,624 gate |
| Pull (intent 1) from page 5, remote mapping after the GET: Min Free after | 19,048 (baseline compare + upload) | 12,588, then 10,816 on the repeat | −6.5 to −8.2 KB |
| Compare (intent 0) from the pushed page: Min Free after | | 12,992 | same remote mapping under WiFi |

- **Pushed records (checklist item 2):** Quang Âm page 3 → `/body/DocFragment[2956]/body/p[16]/text()[1].0` (offset 1,310); The Anarchy page 3 → `/body/DocFragment[30]/body/div[1]/p[5]/text()[1].467` (offset 848). Both text points; the device's own reverse pass resolved the second back to offset 848 exactly (`text-node-exact match textNode=1 char=467`). A hand count against the XHTML was not possible (the book is only on the card).
- **Pull landing (checklist item 3):** from page 5 the pull applied the record and the reader resolved `navTarget.kind=7` (VisibleOffset) to `currentPage=3`, the pushed page. The "ahead" verdict was not exercised: both syncs from page 5 were pulls (intent 1), which apply the remote by design; only Compare (intent 0) shows the verdict, and the one compare was run from the pushed page itself.
- **Push trough: fixed.** Mapping before WiFi leaves Min Free untouched on both chapters (run 1: 10,880 on The Anarchy). The cost is a smaller largest block when TLS starts (the mapping's blocks come and go inside the region WiFi then carves up); the handshake kept 8 to 12 KB above its gate.
- **Pull and compare trough: still below the baseline.** The remote mapping has to follow the GET, so it runs with WiFi up: ~64 KB free minus the inflate window and state (~43 KB), the lean SAX state (4.7 KB), the sink and the reloaded Epub leaves 10.8 to 13.0 KB, against ~19 KB on the temp-file path, which never held the inflate and the parser at once. Options recorded in the ledger; decision pending.

## After Task 8 (X3), run 3 (firmware-run3-59f9161af: pulls and foreign compares map with the radio down, own-push cache; log `C:\tmp\x3-after-task8-run3.log`, 2026-10-07 08:48)

The device had been rotated to portrait since run 2 (screen 528x792 instead of 792x528), so the chapter was laid out again: 215 pages instead of 229. The server still held run 2's record (offset 848, page 3 of the 229-page layout). The offset-based sync is layout-independent, so the run proceeds without any special handling; it is also the first time the branch was exercised across a re-layout.

| Sync (The Anarchy, spine 29) | Path taken | Min Free after | Notes |
|---|---|---|---|
| Compare from page 4/215, foreign record (run 2's push, no cache yet) | local mapping before WiFi (free 120,440); GET; WiFi down; remote mapping at free 100,020 → 99,860; verdict local ahead; reconnect for the upload (free 63,160); PUT | 28,424 | the low point is the WiFi re-init for the upload, not a mapping; TLS troughFree 52,004 (GET) and 41,748 (PUT) |
| Compare from page 6/215, own record | `Remote record is our last push (spine=29 page=4 off=1669); no mapping needed`; verdict local ahead; PUT on the reused session in 89 ms | 32,400 | the Epub stays loaded through this TLS session (reviewer note), ~10 KB less headroom there |
| Pull from page 8/215 | GET; WiFi down; remote mapping at free 100,496 → 100,292; apply; reboot; `resolveInto result: currentPage=6` | 34,936 | landed on the page pushed in the row above |
| Auto-push on close | skipped by the reader: `0 pages this session, threshold is 9` | | the own-record path is the same code as row 2 minus the prompt |

Against the pre-change temp-file path (~19,000 B at the same point) and run 2 (10,816 to 12,992 B): every remote mapping now runs with the radio down and about 100 KB free, and the session's low point moved to WiFi bring-up, where it was before this branch.

Pushed records: page 4 → `/body/DocFragment[30]/body/div[1]/p[6]/text()[1].690` (offset 1,669); page 6 → `/body/DocFragment[30]/body/div[1]/p[9]/text()[2].31` (offset 2,721). The device's own reverse pass resolved both back to their exact offsets (`text-node-exact match`), and the pull landed on the pushed page. Checklist items 1 to 3 are done with these runs; the build numbers for this run (parse 7,359 ms, 215 pages) are for the portrait layout and not comparable to the landscape baseline.
