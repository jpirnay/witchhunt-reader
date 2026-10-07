# List input harmonization

Status: open items as of 2026-10-07. Collected from the list-input harmonization design (now `docs/design/list-input-harmonization.md`).

The scheme (design section 1) is live on the `MenuListActivity` / `UiListActivity` / `TabbedUiListActivity` families, the OPDS catalog and format picker and `OpdsSettingsActivity`. The items below are what is left. Each must end flash-negative (the C3 partition is about 95 % full) or name what it buys instead.

## Screens with declared pairs still on their own input

Starred pages, Global bookmarks and the Wi-Fi network list still use `ButtonNavigator::onNextList` and `GUI.drawList`. Their Left/Right actions (Rename / Delete, Forget / Rescan) work, but they page by no button, do not draw the `«` / `»` hint glyphs and do not follow the long-press rules. Their `pageList` override only swallows a swipe so it fires no action.

- Where: `src/activities/reader/StarredPagesActivity.cpp`, `src/activities/home/GlobalBookmarksActivity.cpp`, `src/activities/network/WifiSelectionActivity.cpp`.
- Target mapping: Bookmarks and Starred pages Rename / Delete; Wi-Fi Options (saved network) / Rescan. Rows move onto `fui::list`.
- The file browser list views and folder picker (`FileBrowserActivity`, derived from `UiListActivity`) run their own event loop and call `ButtonNavigator::onNextList`. Target: list views none / Options with long Confirm for the KOReader pull; folder picker New folder / Move here, through a declaration.
- Home's list layout (`HomeActivity`) uses `ButtonNavigator::nextIndex` directly. Target: buttons through `ListController`; rows stay theme-drawn.
- Next step: one PR for the three declared-pair screens, one for the file browser, one for Home. Each updates its USER_GUIDE section (3.1, 3.3, 3.4).

## The custom painters

The EPUB and XTC chapter lists and the Markdown TOC (`EpubReaderChapterSelectionActivity`, `XtcReaderChapterSelectionActivity`, `MdReaderTocSelectionActivity`) derive from `Activity` and call `ButtonNavigator::onNextList`. Footnotes (`EpubReaderFootnotesActivity`) and the KOReader sync result (`KOReaderSyncActivity`) read button events and draw their rows by hand. The button-remap wizard (`ButtonRemapActivity`) draws with `GUI.drawList`.

- Target: all but the remap wizard take input through `ListController` (Footnotes keeps Power for select through `onOtherEvent`) and draw with `fui::list`. Chapter levels are indented with leading spaces in the label, as upstream does. The remap wizard keeps its own input on purpose (it captures whichever button is pressed next) and only moves its rows onto FUI so `drawList` can go.
- Ruled out: SDK changes. FreeInkUI in our pin matches upstream's, which is what lets upstream screens port without a bump.
- Next step: port upstream's FUI chapter selectors, then footnotes and the sync result. USER_GUIDE section 6 (chapter selection) changes with it.

## Cleanup once the last list has moved

- Delete `BaseTheme::drawList` and `LyraTheme::drawList` (and the `std::function` row thunks) after the last caller above moves. `ListTouchBand` stays while the Home carousel records a band.
- Delete `ButtonNavigator`'s list functions (`onNextList` / `onPreviousList`, `onListNav`, `onListPageNav`, the double-tap log). `onPressAndContinuous` and friends stay for the slider, keyboard and frontlight panel.
- Delete the injected-button fallback in `ActivityManager::dispatchListSwipe` once every list screen overrides `Activity::pageList` through its controller.
- Docs: add a developer page for the scheme and how to declare a list (none exists yet), remove stale rows in `docs/touch-gestures.md`, and do a final pass on USER_GUIDE 5.2 and 5.7. USER_GUIDE already has "Moving through lists".

## Open questions

- Delete confirmation on Bookmarks and Starred pages. The accidental swipe trigger is gone, but a short Right still deletes the selected entry without asking. Decide whether to add a confirm (file a separate issue).
- Label width. `« ` plus a long translated label may still truncate in a hint box. Check the longest translations of Search, Rename, Delete, Options, Forget and Rescan and shorten them in the YAML if needed.
