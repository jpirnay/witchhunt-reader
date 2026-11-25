# Screen layout

Status: open items from the header/body audit of #543. Screens lay out their body with
`src/components/themes/HeaderLayout.h` and `UITheme::getBodyRect()`; the items below were found in the
audit and left out of that PR.

## Overlaps

- **Wi-Fi legend:** overlaps the last network row when the list is full (pre-existing).
  `WifiSelectionActivity::renderNetworkList` ends the list rect `2 × verticalSpacing` above the body's
  foot (20 px on Base, 32 px on Lyra) and starts the legend (`GUI.drawHelpText`)
  `contentSidePadding + 15` above it (35 px).
- **Reading stats in landscape at the Large UI font:** the cards do not fit; the history card can
  reach the Base subtitle line in landscape CW. Only the optional 30-day card is dropped today.
  Where: `ReadingStatsBookDetailActivity::render` (the History card has no fit check; the 30-day card
  is dropped by `sparkFits`). The Murphy's landscape overflow of the same screen is in
  [murphy-small-screen.md](murphy-small-screen.md); fix both together.
- **KOReader Sync screen:** rows on a 25 px pitch overlap the 30 px lines of the Large UI font.
  `KOReaderSyncActivity::render` (the result state) takes `rowStep` from `PanelMetrics::capped(25, …)`,
  which caps only on a compact panel, so the other boards keep 25 px whatever the line height.
- **Dictionary definition:** the headword and the status text collide inside the header band.
  `DictionaryDefinitionActivity::render` trims the status (dictionary name, page) to half the width but
  draws it mid-band beside a centred headword that has no width limit of its own.
- **Detect Timezone:** its two lines sit 20 px apart (`DetectTimezoneActivity::render`, the success
  state: the zone and the DST line).

## Wrong arithmetic

- The Markdown and XTC chapter lists count rows per page from a different rect than they draw in:
  `getPageItems()` takes `getContentRect(renderer, true, false)`, `render()` draws in
  `getContentRect(renderer, true, true)` (`MdReaderTocSelectionActivity`,
  `XtcReaderChapterSelectionActivity`). Moving both onto `UiListActivity`
  ([list-input-harmonization.md](list-input-harmonization.md), "The custom painters") removes this.
- System Information and Boot Diagnostics subtract the hint strip twice: their `render()` takes a
  content rect that already leaves out the hints, then subtracts `buttonHintsHeight` again for
  `hintsTop`.
- BaseTheme's title-width budget mixes absolute and relative x (`BaseTheme::drawHeader`: `batteryX`
  includes `rect.x`, `rect.width` does not).

## Cleanup

- Screens that already lay out correctly still compute the body top themselves
  (`metrics.topPadding + metrics.headerHeight`); switching them to `HeaderLayout` removes the
  duplicates (no visible change). These 20 files do so: in `settings/` FontScalingTest,
  KOReaderSettings, ButtonActionsOverview, ButtonRemap, OpdsSettings, SettingsSubmenu, DetectTimezone,
  ClockSettings, BootDiagnostics, ReadingStats, SyncTime; `weather/WeatherSettings`;
  `util/FrontlightPanel`; in `home/` HomeMore, GlobalBookmarks, FileContextMenu; in `network/`
  SerialTransfer, CalibreConnect, CrossPointWebServer, UsbDrive (each `<Name>Activity.cpp` under
  `src/activities/`). Some of them also carry an item above. The chapter lists could also use
  `overlayTitleY`.
- OtaUpdate has hard-coded English strings (not `tr(STR_*)`): the failure reasons in
  `OtaUpdateActivity::render`, except the two already translated.
- Wrong comment: `ActivityManager::begin()` says CPU 0 runs the Arduino loop task. On the S3 it runs
  on core 1 (`CONFIG_ARDUINO_RUNNING_CORE=1` in the framework's esp32s3 `sdkconfig`).
- The SDK widget `freeink::ui::header()` overlaps title and subtitle (`subtitle` starts at
  `y + titleLineH` while the title is centred). This repo does not use it; for freeink-sdk.
