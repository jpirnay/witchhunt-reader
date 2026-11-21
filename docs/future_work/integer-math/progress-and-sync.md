# Progress and sync in fixed point

Part of [Integer maths](../integer-math.md). Area 4. Counts from `scripts/float_inventory.py` on 2026-11-21, after area 1.

**Open.** Book progress is a float percentage or fraction from `Epub::calculateProgress` through the reader's percent
jumps, status bars, the sleep screen's overlay, reading statistics and KOReader sync: about 250 call sites in 41
functions. None of it runs per pixel; it is here so that whole files can join the check.

**Where.** `ProgressMapper.cpp` (57), `SleepActivity.cpp` (42: `getBookOverlayInfo`, `renderOverlaySleepScreen`),
`EpubReaderActivity.cpp` (42: `saveProgress`, `resolvePercent`, `statusBarBookPercent`, the reader menu),
`ReadingStats.cpp` (15: paces and ETA), `EpubNavigationTarget.cpp` (9), `KOReaderSyncActivity.cpp` (8),
`ProgressComparison.cpp` (7), `BaseTheme::drawStatusBar` and `statusBarProgressPercent` (7), `Epub.cpp` (6:
`calculateProgress`), `ChapterXPathForwardMapper.cpp`/`ReverseMapper.cpp` (12), `LibraryOrder.cpp` (5: series
index), `BookSearchActivity.cpp` (5), `ReadingStatsActivity.cpp` (5), `KOReaderSyncClient.cpp` (4),
`Txt`/`MdReaderActivity::renderStatusBar` (8), `FinishedBookActivity.cpp` (3), `BookProgressPresentation.cpp` (3),
`KOReaderProgressReply.h` (2), the reading-stats detail card (an inlined lambda).

**Boundary.** KOReader's protocol sends `percentage` as a JSON number: convert it to the fixed-point type where
`parseProgressReply` reads it, and format it back with integer digits. Check the stored formats (reading stats JSON,
progress files) before changing a field's type.

**Next step.** One progress type (for example parts per 10,000 or per million in a `uint32_t`) from
`calculateProgress` outwards; percent shown to the user rounds from it. The sync and progress suites
(`kosync_progress_reply`, `epub_progress_record`, the mapper tests) pin the behaviour.
