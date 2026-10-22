# Host test suite

Status: open items as of 2026-10-22, from the second test-suite audit (PR #547). The audit removed
about 270 redundant test cases; the items below are coverage it found missing, tests to strengthen,
rename or move, and small code cleanups it noticed.

## Firmware paths with no host test

- `ListController::tapVertical` (`src/activities/ListController.cpp`): single Up/Down step,
  double-tap paging, the `completesDoubleTap` hand-off and the fitsOnePage double step. It is the
  only path for non-long Up/Down and has had no host test since 2e395e8e7. Next step: a
  ListController test that drives it with synthetic taps.
- `recordCarouselCoverTargets()` (`LyraCarouselTheme.cpp`): centre-first record order, clamp, and
  skipping absent slots, via `TapTargets::homeCovers()`. The layout tests only cover `compute()` and
  `clampToScreen()`. Next step: make it callable without the theme or renderer, then test it.
- `GifToFramebufferConverter::decodeToFramebuffer`: row mapping, scaling and the cache (f9885e4a3
  fixed a real bug here). The 10 GIF decode tests use the test-only `decodeFirstFrameToGrayscale`.
  Next step: drive the real entry point through the renderer stub.
- `ListRowTap::apply(..., std::atomic<int>&)`: the overload is untested, though `UiListActivity`,
  `TabbedUiListActivity` and `FileBrowserActivity` call it. Next step: a test beside the existing
  `ListRowTap` cases.
- `ReaderUtils::isTouchMenuTap` / `detectTouchPageTurn`: they call `TapZones::zoneFor`, but the
  header needs `GfxRenderer` and `MappedInputManager` and has no host test. Next step: a fake-input
  test that pins the page-turn mapping (prev/next, inverted tap, y bounds).
- `ReleaseJsonParser::setFirmwareAssetName` (tests/OTA): no test calls it. x4pro and lilygo pick
  `firmware-<board>.bin` in `OtaUpdater`; only the default `firmware.bin` is exercised. Next step:
  one case per board asset name.
- `HttpRange`: no test states why the budget is record block plus Wi-Fi margin, except through the
  ladder values. If the margin is retuned, add a measured-value table test.

## Tests that cannot see a corruption

- `zip_entry_reader` and `CssParserTest` compare inflated bytes only against the other inflate path,
  or not at all. With the memcpy LZ copy's source offset corrupted they still pass. Next step: a
  known-content oracle (CRC-32 or a hash of a deflated fixture entry).
- `tinflate.c` is still built with `UZLIB_CONF_USE_MEMCPY=0` by `epub_pipeline` (EpubPipelineTest,
  the dump tools) and by `font_group_stream`'s `uzlib_font_ring` and its dependents
  (`font_cache_manager`, `fallback_face_provider`, `flash_lang_partition`, `ui_font_compression`).
  Next step: `target_compile_definitions(... UZLIB_CONF_USE_MEMCPY=1)` on them. The ring path does
  not use the memcpy copy, so the font targets gain nothing; the pipeline targets do.
- `test/ui_font_compression`: the bitmap is checked against the header's own CRC and size, not
  against independent bytes. If an independent ground truth is wanted, `compress_ui_face.py` should
  verify the round trip at generation time.

## Goldens and fixtures to extend so that unit tests can go

- A `<pre>` styled by its own rule with an inline `<span>` inside is not in
  `test_block_emphasis.epub`; only `BlockEmphasisFixture.BlockquoteAndPreReachTheirChildren` covers
  it. Next step: add it as a chapter, regenerate the golden, delete that test.
- No golden reaches the widow on the last page of a paragraph longer than a page
  (`ChapterHtmlSlimParser::keepTogetherBreak`'s "further on" branch on a continuation page, whole or
  laid out in parts). `test_keep_together.epub`'s long chapter ends with 26 lines on its last page.
  Next step: size it so the last page would take one line, then delete the Heap cases of
  `KeepTogetherFixture.ParagraphLongerThanAPageStillSplits` and
  `ParagraphLaidOutInPartsKeepsItsLastLines`.
- `CssCascade`: a resolve-level test that `h1 + p` does not style `p` would replace the
  accessor-based unsupported-selector checks.

## Renames, moves and folds

- `test/epub_pipeline/SidecarFilesTest.cpp` (14 cases) compiles no EPUB code, only
  `lib/FsHelpers/SidecarFiles`. Move it to `test/sidecar_files`, beside that directory's 6 cases.
- `BookMetadataCacheTest.AnInOrderTocReadsTheSpineAboutOnceWithoutTheIndex` is named after a
  per-spine index that no longer exists. Rename it, e.g. `AnInOrderTocReadsTheSpineAboutOnce`.
- `test/epub_benchmark` is still built in ALL (no `EXCLUDE_FROM_ALL`), and its suite name
  `ExpatBaseline` and its comments name expat, which `SaxParser` replaced. Exclude it from ALL and
  rename.
- Optional: fold the 20 remaining `css_gaps` tests into `epub_css/CssParserTest.cpp` and drop the
  binary and its `LoggingStub`. The case count does not change.

## Small code cleanups

- `FinishedBookActivity::naturalLess` (`FinishedBookActivity.cpp:298`) is an untested copy of
  `FsHelpers::naturalCompare`. Use the tested one and delete the copy.
- Three CRC-32 implementations exist: `serialtransfer::crc32Update`, `ReadingStatsSlotFile::crc32`
  and the static `crc32` in `FlashLangPartition.cpp`, each with its own check-value test.
  `CjkUiFonts.cpp` already borrows the serial one. Next step: one shared implementation and one
  check-value test.
- `TypographyProfile::direction` is never assigned or read. Remove it, or keep it for RTL (spec
  section 3).
- `FirmwareBoardTag.MagicFirstCharIsUnique` checks the test's own copy of the magic. Make it a
  `static_assert` next to `MAGIC_LEN` in `FirmwareBoardTag.cpp` and drop the test.

## epub_optimizer sweep

The real-book sweep runs 46 books and 44 of them print "nothing split". Trim it to the structurally
distinct books: anchors, TOC edges, footnotes, images, CJK and moby-dick.
