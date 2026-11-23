# List input harmonization

Status: open items from the list-input harmonization design (`docs/design/list-input-harmonization.md`).

The scheme (design section 1) is live on the `MenuListActivity` / `UiListActivity` / `TabbedUiListActivity` families (except the file browser, below) and the OPDS catalog and format picker. The items below are what is left. Each must end flash-negative or name what it buys instead. Re-measure the `default` build (X3, X4) before relying on a budget: #868's body gives 6,178,901 B of the 6,553,600 B app slot on 2026-11-21.

**Tap flash.** Rows on FreeInkUI (`UiListActivity`, `TabbedUiListActivity`, `UiAppHost`) get FreeInkUI's tap flash, which paints the tapped row gray in the refresh that shows the result (`FreeInkApp::route()`). A screen that draws its own rows and records a `ListTouchBand` gets only the selection repaint (`ActivityManager::dispatchListTap()`, `Selected` branch). Those are the screens below that move their rows onto `fui::list`, plus the OPDS catalog, which takes its input through `ListController` but draws its rows by hand (`OpdsBookBrowserActivity`). Moving the rows brings the flash with them; a second flash for the band path is ruled out (design, section 2b).

## Screens with declared pairs still on their own input

Starred pages, Global bookmarks and the Wi-Fi network list still use `ButtonNavigator::onNextList` and `GUI.drawList`. Their Left/Right actions (Rename / Delete, Forget / Rescan) work, but they page by no button, do not draw the `«` / `»` hint glyphs and do not follow the long-press rules. Their `pageList` override only swallows a swipe so it fires no action. A tap on a row gets no tap flash.

- Where: `src/activities/reader/StarredPagesActivity.cpp`, `src/activities/home/GlobalBookmarksActivity.cpp`, `src/activities/network/WifiSelectionActivity.cpp`.
- Target mapping: Bookmarks and Starred pages Rename / Delete; Wi-Fi Options (saved network) / Rescan. Rows move onto `fui::list`, which brings the tap flash.
- The file browser list views and folder picker (`FileBrowserActivity`, derived from `UiListActivity`) override `navigateButtons()` and `handleCustomInput()` with their own event loop and call `ButtonNavigator::onNextList`. Target: list views none / Options with long Confirm for the KOReader pull; folder picker New folder / Move here, through a declaration. In the Library the declaration is `tabbed`: long Up/Down already switch tabs there, and the tab bar has no focus position until the browser is on the controller (`docs/design/library-index.md`, "The screen"). Settings and the reader menu offer Confirm on a focused bar; once the browser is on the controller, the Library can too, for readers on button-only boards who miss the tabs.
- Home's list layout (`HomeActivity`) uses `ButtonNavigator::nextIndex` directly. Target: buttons through `ListController`; rows stay theme-drawn.
- Next step: one PR for the three declared-pair screens, one for the file browser, one for Home. Each updates the USER_GUIDE sections of the screens it changes: Home Screen; Library Screen and Recent, New and Authors; and the swipe row in 5.2, which still says a swipe does nothing on Bookmarks, Starred pages and Wi-Fi networks.

## The custom painters

The EPUB chapter list is on `UiListActivity` (#593, for issue #582): rows from a provider, titles wrapped to two lines, levels as `fui::ListItem::indent`. The XTC chapter list and the Markdown TOC (`XtcReaderChapterSelectionActivity`, `MdReaderTocSelectionActivity`) still derive from `Activity`, call `ButtonNavigator::onNextList` and truncate their titles. Footnotes (`EpubReaderFootnotesActivity`) and the KOReader sync result (`KOReaderSyncActivity`) read button events and draw their rows by hand. All four record a `ListTouchBand`, so a tap gets no tap flash. The button-remap wizard (`ButtonRemapActivity`) draws with `GUI.drawList`; it is left out of the tap flash on purpose.

- Target: all but the remap wizard take input through `ListController` (Footnotes keeps Power for select through `onOtherEvent`) and draw with `fui::list`. Chapter levels use `ListItem::indent`, as the EPUB list does: leading spaces, upstream's way, are dropped on a wrapped line. The remap wizard keeps its own input on purpose (it captures whichever button is pressed next) and only moves its rows onto FUI so `drawList` can go.
- SDK changes stay the exception: `ListItem::indent` (SDK `5bd4a0a`) is the one so far, because no app-side workaround survives wrapping.
- Next step: the XTC chapter list and the Markdown TOC, as the EPUB list did (USER_GUIDE section 6 then drops its XTC/Markdown sentence), then footnotes and the sync result. The XTC and Markdown move also settles their rows-per-page arithmetic (`screen-layout.md`, "Wrong arithmetic").

## Cleanup once the last list has moved

- Delete `BaseTheme::drawList` and `LyraTheme::drawList` (and the `std::function` row thunks) after the last caller above moves. `ListTouchBand` stays while the Home carousel records a band.
- Delete `ButtonNavigator`'s list functions (`onNextList` / `onPreviousList`, `onListNav`, `onListPageNav`, the double-tap log). `onPressAndContinuous` and friends stay for the slider, keyboard and frontlight panel.
- Delete the injected-button fallback in `ActivityManager::dispatchListSwipe` once every list screen overrides `Activity::pageList` through its controller.
- Docs: add a developer page for the scheme and how to declare a list (`docs/contributing/architecture.md` only names the bases), check the "Swipe up / down over a list: Page the list" row in `docs/touch-gestures.md` (true only once every list pages on a swipe), and do a final pass on USER_GUIDE 5.2 and 5.7. USER_GUIDE already has "Moving through lists".

## Open questions

- Delete confirmation on Bookmarks and Starred pages. The accidental swipe trigger is gone, but a short Right still deletes the selected entry without asking. Decide whether to add a confirm (file a separate issue).
- Label width. `« ` plus a long translated label may still truncate in a hint box. Check the longest translations of Search, Rename, Delete, Options, Forget and Rescan and shorten them in the YAML if needed.
