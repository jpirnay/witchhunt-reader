# List input harmonization

Status: open items as of 2026-10-07. Collected from the list-input harmonization design (now `docs/design/list-input-harmonization.md`).

The scheme (design section 1) is live on the `MenuListActivity` / `UiListActivity` / `TabbedUiListActivity` families (except the file browser, below) and the OPDS catalog and format picker. The items below are what is left. Each must end flash-negative (the C3 partition is about 95 % full) or name what it buys instead.

## Screens with declared pairs still on their own input

Starred pages, Global bookmarks and the Wi-Fi network list still use `ButtonNavigator::onNextList` and `GUI.drawList`. Their Left/Right actions (Rename / Delete, Forget / Rescan) work, but they page by no button, do not draw the `«` / `»` hint glyphs and do not follow the long-press rules. Their `pageList` override only swallows a swipe so it fires no action.

- Where: `src/activities/reader/StarredPagesActivity.cpp`, `src/activities/home/GlobalBookmarksActivity.cpp`, `src/activities/network/WifiSelectionActivity.cpp`.
- Target mapping: Bookmarks and Starred pages Rename / Delete; Wi-Fi Options (saved network) / Rescan. Rows move onto `fui::list`.
- The file browser list views and folder picker (`FileBrowserActivity`, derived from `UiListActivity`) override `navigateButtons()` and `handleCustomInput()` with their own event loop and call `ButtonNavigator::onNextList`. Target: list views none / Options with long Confirm for the KOReader pull; folder picker New folder / Move here, through a declaration. In the Library the declaration is `tabbed`: long Up/Down already switch tabs there, and the tab bar has no focus position until the browser is on the controller (`library.md`, "Tabs on button-only boards").
- Home's list layout (`HomeActivity`) uses `ButtonNavigator::nextIndex` directly. Target: buttons through `ListController`; rows stay theme-drawn.
- Next step: one PR for the three declared-pair screens, one for the file browser, one for Home. Each updates the USER_GUIDE sections of the screens it changes: Home Screen; Library Screen and Recent, New and Authors; and the swipe row in 5.2, which still says a swipe does nothing on Bookmarks, Starred pages and Wi-Fi networks.

## The custom painters

The EPUB and XTC chapter lists and the Markdown TOC (`EpubReaderChapterSelectionActivity`, `XtcReaderChapterSelectionActivity`, `MdReaderTocSelectionActivity`) derive from `Activity` and call `ButtonNavigator::onNextList`. Footnotes (`EpubReaderFootnotesActivity`) and the KOReader sync result (`KOReaderSyncActivity`) read button events and draw their rows by hand. The button-remap wizard (`ButtonRemapActivity`) draws with `GUI.drawList`.

- Target: all but the remap wizard take input through `ListController` (Footnotes keeps Power for select through `onOtherEvent`) and draw with `fui::list`. Chapter levels are indented with leading spaces in the label, as upstream does. The remap wizard keeps its own input on purpose (it captures whichever button is pressed next) and only moves its rows onto FUI so `drawList` can go.
- Ruled out: SDK changes. FreeInkUI in our pin matches upstream's, which is what lets upstream screens port without a bump.
- Next step: port upstream's FUI chapter selectors, then footnotes and the sync result. USER_GUIDE section 6 (chapter selection) changes with it.

## Cleanup once the last list has moved

- Delete `BaseTheme::drawList` and `LyraTheme::drawList` (and the `std::function` row thunks) after the last caller above moves. `ListTouchBand` stays while the Home carousel records a band.
- Delete `ButtonNavigator`'s list functions (`onNextList` / `onPreviousList`, `onListNav`, `onListPageNav`, the double-tap log). `onPressAndContinuous` and friends stay for the slider, keyboard and frontlight panel.
- Delete the injected-button fallback in `ActivityManager::dispatchListSwipe` once every list screen overrides `Activity::pageList` through its controller.
- Docs: add a developer page for the scheme and how to declare a list (`docs/contributing/architecture.md` only names the bases), check the "Swipe up / down over a list: Page the list" row in `docs/touch-gestures.md` (true only once every list pages on a swipe), and do a final pass on USER_GUIDE 5.2 and 5.7. USER_GUIDE already has "Moving through lists".

## Open questions

- Delete confirmation on Bookmarks and Starred pages. The accidental swipe trigger is gone, but a short Right still deletes the selected entry without asking. Decide whether to add a confirm (file a separate issue).
- Label width. `« ` plus a long translated label may still truncate in a hint box. Check the longest translations of Search, Rename, Delete, Options, Forget and Rescan and shorten them in the YAML if needed.
