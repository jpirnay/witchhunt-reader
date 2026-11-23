# Host test suite

Status: open items from the second test-suite audit (#547). The audit removed about 270 redundant test
cases; the items below are coverage it found missing, tests to strengthen, rename or move, and small code
cleanups it noticed. None of the suites added since closes one.

## Firmware paths with no host test

- `ListController::tapVertical` (`src/activities/ListController.cpp`): single Up/Down step,
  double-tap paging, the `completesDoubleTap` hand-off and the fitsOnePage double step. It is the
  only path for non-long Up/Down and has had no host test since 2e395e8e7. Next step: a
  ListController test that drives it with synthetic taps.
- `recordCarouselCoverTargets()` (`LyraCarouselTheme.cpp`): centre-first record order, clamp, and
  skipping absent slots, via `TapTargets::homeCovers()`. The layout tests only cover `compute()` and
  `clampToScreen()`. Next step: make it callable without the theme or renderer, then test it.
- `GifToFramebufferConverter::decodeToFramebuffer`: row mapping, scaling and the cache (f9885e4a3
  fixed a real bug here). The GIF decode tests (`GifDecode`, `GifDecodeStress`) use the test-only
  `decodeFirstFrameToGrayscale`.
  Next step: drive the real entry point through the renderer stub.
- `ListRowTap::apply(..., std::atomic<int>&)`: the overload is untested, though `UiListActivity`,
  `TabbedUiListActivity` and `FileBrowserActivity` call it. Next step: a test beside the existing
  `ListRowTap` cases.
- `ReaderUtils::isTouchMenuTap` / `detectTouchPageTurn`: they call `TapZones::zoneFor`, but the
  header needs `GfxRenderer` and `MappedInputManager` and has no host test. Next step: a fake-input
  test that pins the page-turn mapping (prev/next, inverted tap, y bounds).
- `ReleaseJsonParser::setFirmwareAssetName` (tests/OTA): no test calls it. Every board but the X3/X4
  updates from `firmware-<board>.bin`, the name taken from the build's board tag (`OtaUpdater`); only
  the default `firmware.bin` is exercised. `ReleaseJsonParserTest` already has a `firmware-x4pro.bin`
  asset among its fixtures but never selects it. Next step: one case per board asset name.
- `HttpRange`: no test states why the budget is record block plus Wi-Fi margin, except through the
  ladder values. If the margin is retuned, add a measured-value table test.

## Tests that cannot see a corruption

- `zip_entry_reader` and `CssParserTest` compare inflated bytes only against the other inflate path,
  or not at all (`zip_entry_reader`'s `LoggingStub.cpp` stubs `uzlib_crc32` to return its input). With
  the memcpy LZ copy's source offset corrupted they still pass. Next step: a known-content oracle
  (CRC-32 or a hash of a deflated fixture entry).
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
  accessor-based unsupported-selector checks (`CssCascade.SiblingAndOverlongChainsStayUnsupported`
  counts `unsupportedSelectors()`).

## Renames, moves and folds

- `test/epub_pipeline/SidecarFilesTest.cpp` (14 cases) compiles no EPUB code, only
  `lib/FsHelpers/SidecarFiles`. Move it to `test/sidecar_files`, beside that directory's 7 cases.
- `BookMetadataCacheTest.AnInOrderTocReadsTheSpineAboutOnceWithoutTheIndex` is named after a
  per-spine index that no longer exists. Rename it, e.g. `AnInOrderTocReadsTheSpineAboutOnce`.
- `test/epub_benchmark`'s suite name `ExpatBaseline` (its four cases in `EpubParserBenchmark.cpp`) and
  its comments name expat, which `SaxParser` replaced. Rename. (It is a hand-run tool since #643.)
- Optional: fold the 36 `css_gaps` tests into `epub_css/CssParserTest.cpp` and drop the binary and its
  `LoggingStub`. The case count does not change.

## Small code cleanups

- `naturalLess` in `FinishedBookActivity.cpp` (a file-local function) is an untested copy of
  `FsHelpers::naturalCompare`. Use the tested one and delete the copy.
- CRC-32 is written twice, the same bit-by-bit loop: `FsHelpers::crc32` (`lib/FsHelpers/Crc32.h`, which
  the slot files `ReadingStatsSlotFile` and `RecentBooksFile` share) and `serialtransfer::crc32Update`
  (`SerialTransferProtocol.cpp`, which `CjkUiFonts.cpp` and `SerialTestApi.cpp` borrow). Each has its
  own check-value test (`SlotFileCrc`, `SerialTransferCrc`). Next step: keep `FsHelpers::crc32`, point
  the serial callers at it, and keep one check-value test. `FlashLangPartition.cpp`, `LangPack.cpp` and
  `HyphenationStore.cpp` wrap the ROM's `esp_rom_crc32_le`, which costs no flash; they stay.
- `FirmwareBoardTag.MagicFirstCharIsUnique` checks the test's own copy of the magic. Make it a
  `static_assert` next to `MAGIC_LEN` in `FirmwareBoardTag.cpp` and drop the test.
- Four tests replace the global `operator new`/`operator delete` with `malloc`/`free` to count allocations, and
  GCC on CI's Linux warns `-Wmismatched-new-delete` at their sized `operator delete`: `BookmarksTest.cpp:32`,
  `CjkLineBreakTest.cpp:164`, `HtmlToPlainTextTest.cpp:25`, `InlineListFunctionRefTest.cpp:30`. Harmless, but
  it buries new warnings in the unit-test log. Next step: try `noinline` on the replacements, else turn the
  warning off for those four targets, and check the CI log is clean. Seven more suites replace them through
  helpers of their own (`vertical_text`, `ttf_font`, `cjk_layout`, `book_metadata_cache`,
  `file_browser_model`, `hal_storage`, `ot_shaper_port`); whether they warn too is not checked.

## epub_optimizer sweep

The real-book sweep (`test/web/epub_optimizer_test.mjs`) runs every EPUB in `test/epubs` and
`test/fixtures`, 70 books now. When it ran 46, 44 of them printed "nothing split". Trim it to the
structurally distinct books: anchors, TOC edges, footnotes, images, CJK and moby-dick.
