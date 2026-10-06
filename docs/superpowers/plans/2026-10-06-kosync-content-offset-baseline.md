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

### Final-review fix round (the branch's final numbers)

Tied page runs resolve to their last page; the mappers parse like the layout parser (void-tag repair, case-insensitive `<body>`); a page starting at the chapter's total names the end of the last text node.

- Host suite: `100% tests passed out of 1670` (8 Skipped by their own conditions, none failed); EpubPipelineTest 598 tests, 596 passed, 2 skipped. Goldens hash unchanged (f0b7d80e...af29f).
- Round trip: all 30 books, every page comes back on itself (zero drift).
- Firmware: `RAM: 16.5% (used 53928 bytes from 327680 bytes)`, `Flash: 94.9% (used 6216943 bytes from 6553600 bytes)`; `firmware.bin` 6,230,464 B, +7,008 B against 6,223,456 B.
