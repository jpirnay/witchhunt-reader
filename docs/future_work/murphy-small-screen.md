# Murphy M3: fixed sizes that assume a bigger panel

Audit of the Murphy port, 2026-11-03. The M3 is a 416x240 panel with four-level gray
([grayscale-aa-rendering.md](../grayscale-aa-rendering.md#murphy-m3-gray-and-its-bw-waveforms)); the UI runs Portrait,
so the logical screen is 240x416. **P** = Portrait 240x416, **L** = Landscape 416x240. Paths are relative to the repo
root.

**Status.** The audit's root causes R1-R5, R8 and R10 are fixed and were checked on the panel, with the slider, the
cover grid (six items a page, c247ba86c) and Calibre's layout. R1 is the compact theme metrics (`forPanel()` in
`BaseTheme.h`, switched by `PanelMetrics.h`) and R2-R3 the hint strips, both 2910c82ff; R8 is the screens laid out at
absolute positions (21cdf87f7); long text (7db7d086d) and the keyboard (4f63bb36f) were fixed in the same round. Fixed
but never seen on the panel: the reader defaults (they only apply to a fresh settings file), Carousel leaving the theme
list, the Finished Book and OTA text bounds, Weather's moon line, and Calibre's upload bar. How a compact panel is handled, and what was accepted as it is, is in
[board-support.md](../contributing/board-support.md#gotchas).

The tables below keep only the rows still open. Their pixel figures are arithmetic from the code before the compact
metrics, so they are out of date: check a row against the panel before acting on it. Text widths ignore kerning, and of
the translations only de, fr and ru were measured. Not audited at all: EPUB CSS absolute margins and table cell
minimums on a 230 px column, the TXT and Markdown body layout beyond its constants, and the SDK components
`publication-page.h`, `cover-grid.h`, `sheet.h` and `tile-grid.h` (grepped only). The QR pages
(`CrossPointWebServerActivity`, `WifiSelectionActivity`) have their own compact layout (`PanelMetrics`,
`QrUtils::compactSpacing()`) and are not covered here: the full layout puts text beside the code, and
`http://crosspoint.local/` does not fit next to a code in 240 px.

**Open, most useful first:**
- System Information in P truncates its labels and values (`SystemInformationActivity::render`, `drawRow`); the user
  called the screen unreadable. Serial screenshots were the only way to read the light-sleep figures.
- XTC: a page encoded for another panel is cropped (`XtcReaderActivity::renderPage`, and
  `drawCurrentPageToBuffer` for the sleep overlay: the source is clamped to the display, so 480x800 shows its top-left
  quarter). Needs a 2:1 reduction and a test file.
- KOReader Sync's comparison screen (`KOReaderSyncActivity`) was reflowed (21cdf87f7) but never seen: it needs
  credentials and a server. Its arithmetic fits in both orientations.
- Reading Stats book detail in landscape overflows (`ReadingStatsBookDetailActivity::render`; the same screen at the
  Large UI font is in [screen-layout.md](screen-layout.md), fix both together), and `QrDisplayActivity` in landscape
  forces a large payload to 1 px modules (`QrUtils::drawQrCode`).
- Button Actions Overview lists all 7 keys, some of which Murphy does not have (`kButtonRows` in
  `ButtonActionsOverviewActivity.cpp`).

A background build on the S3 boards still gives way to a render, although the PSRAM arena it builds in holds nothing
the page needs: in [reader-priority-scheme.md](reader-priority-scheme.md), "A page turn on an S3 board discards
Background-B's live build".

## Ground rules

**Which screens can be landscape at all.** Only these: the readers and whatever opens over them (reader menu, chapter
and footnote lists, percent slider, printed-page input, dictionary, starred pages, quick overrides, QR, keyboard, book
info, book stats), plus two screens that force it: Weather (`WeatherActivity::onEnter`) and Button Actions Overview
(`ButtonActionsOverviewActivity::onEnter`). Everything else runs in Portrait (`EpubReaderActivity::onExit` restores
it), so "L" does not apply to Home, Settings, Library, network, OPDS. `KOReaderSyncActivity` replaces the reader, so it
is Portrait too.

**Fonts on Murphy.** Measured from the generated glyph tables (advances only, kerning ignored, so widths are slightly
high). The point sizes are at 150 DPI, so these are bigger than the names suggest.

| Slot | Face | Line height | Average px/char (en) | Chars across 240 |
|---|---|---|---|---|
| SMALL_FONT_ID | notosans_6 | 17 | 6.1 | 39 |
| UI_10_FONT_ID | inter_ui_8 | 20 | 8.2 (bold 8.4) | 29 |
| UI_12_FONT_ID | inter_ui_10 | 25 | 10.2 (bold 10.4) | 23 |

Content rects come from `UITheme::getContentRect` with the compact metrics.

## Root cause left: Home (R6)

Home is laid out as a fixed tile plus a fixed menu, with floors that exceed the panel: `HomeActivity.cpp`
(`CLASSIC_MIN_RECENT_TILE_HEIGHT` and the other tile floors, `computeHomeScreenLayout`),
`LyraTheme::drawRecentBookCover` and `LyraTheme::drawButtonMenu`, `BaseTheme::drawRecentBookCover` and
`BaseTheme::drawButtonMenu`. The compact metrics
fix most of the menu (`forPanel()` caps `menuRowHeight` at 36); the card still needs a narrow-column path (the Home
rows below).

## Theme chrome, shared drawing

| Sev | Where | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S2 | `BaseTheme::drawList` | `- (rowValue != nullptr ? 60 : 0)`, value drawn untruncated | Classic: row text 97 px at width 202, and a value over 60 px overprints the title. | Local: truncate the value |
| S3 | `maxListValueWidth` in `LyraTheme.cpp` (`LyraTheme::drawList`), `maxValWidth` in `LyraCarouselTheme::drawList` | `= 200` | Larger than the whole row text area (183 at 240). A value over about 170 px drives `rowTextWidth` <= 0, `truncatedText` returns "", and the title disappears. Only `ButtonRemapActivity` and Wi-Fi pass values, and remap's are short, so low exposure today. | Local: `min(200, rect.width/2)` |

## Home

Default menu on Murphy is 5 rows (Library, Stats, File Transfer, Weather, Settings), 6 with bookmarks: `useWeather`
and every `show*OnHome` default to 1 (`CrossPointSettings.h`). Simulated from `computeHomeScreenLayout` +
`drawButtonMenu`, before the compact metrics:

| Theme | Entries | Tile | Menu height | Row height |
|---|---|---|---|---|
| Lyra | 4 / 5 / 6 | 170 | 146 | 36 / 29 / 24 |
| Lyra 3 Covers | 4 / 5 / 6 | 200 | 116 | 29 / 23 / 19 |
| Classic | 4 / 5 / 6 | 280 | 54 | 13 / 10 / 9 |

| Sev | Where | Constant / expression | What happens at 240x416 | Fix |
|---|---|---|---|---|
| S2 | `LyraTheme::drawRecentBookCover` | `coverWidth = coverHeight * 3 / 5` = 92, `textWidth = 200-16-16-92` = 76 | Text column is x=136..212, 76 px: about 7 title chars a line. The status line is truncated since 7db7d086d. | Below a column minimum put the status and history under the cover or shrink the cover. R6 |
| S1 | `CLASSIC_MIN_RECENT_TILE_HEIGHT` in `HomeActivity.cpp`, `BaseTheme::drawRecentBookCover` | `= 280` | Menu gets 54 px, rows 9-13 px with 20 px text: labels overprint each other and are unreadable. | Lower floor. R6 |
| S3 | `historyRowHeight` in `Lyra3CoversTheme.h` | `= 23` "FIT_SMALL_FONT_ID line height" | On Murphy that face is 17 px: 6 px wasted, and the comment is wrong here. | Local: read the ladder |

## FreeInkUI widgets as the firmware calls them

| Sev | Where | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S2 | `measureListRow` in the SDK's `list.h`, `UiListActivity::listProps` | content = 202-40 = 162, value width unbounded, `maxLines = 2` | Toggle rows leave 114 px for the label: 94 of the 254 strings referenced from `SettingsList.h` need a second line, 4 are ellipsized. A value row with a 100 px value leaves a 52 px label. A value over 152 px gives label width 0 and the value runs left out of its band (the value draw in `list.h`). Not truncated. The 3/5 cap there (`balanceWrappedLabelWithValue`) narrows a wrapping label, not the value. | SDK: cap the value width at 60 % |
| S3 | `ConfirmDialog::draw` (`ConfirmDialog.h`), `option-dialog.h` padding 16, gap 8 | width = 4/5 of safe width | 192 px panel, 160 content, two 76 px buttons. en fine, de "Abbrechen" ellipsized. In L only the message gives up lines, so a 3-line headline with buttons can exceed 240. | Local: use 92 % width on compact |

## Settings-side activities

| Sev | Where | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S1 | `ReadingStatsBookDetailActivity::render` | cards stacked, only the 30-day spark checks the room left (`sparkFits`) | L (opened from the reader menu): body 135 px, summary 55 + time 104 already exceed it. History card off-panel. P: fits. | Local: paging |
| S2 | `kButtonRows` in `ButtonActionsOverviewActivity.cpp` | all 7 keys | Rows name keys Murphy does not have (it has Confirm, Up and Down). The layout fits since 21cdf87f7. | Local: list the board's keys |
| S1 | `SystemInformationActivity::render` (`drawRow`) | label and value columns at half the width each | P: labels ("App parti...", "Display S...") and values ("ESP32-S3 re...", the light-sleep decline reasons) end in an ellipsis; the user called the screen unreadable. | Local: value under its label on a compact panel |

## Reader and its overlays

| Sev | Where | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S1 | `XtcReaderActivity::renderPage`, `XtcReaderActivity::drawCurrentPageToBuffer` | `min(pageWidth, getDisplayHeight())`, `min(pageHeight, getDisplayWidth())` | XTC/XTCH pages are pre-rendered bitmaps. A 480x800 page shows only its top-left 240x416, a quarter of the page, and the book cannot be read. Only files encoded for 240x416 work. The sleep overlay crops the same way. | Local: downscale, or refuse with a message |
| S2 | `UITheme::getStatusBarItemsHeight`, `statusBarVerticalMargin` in `LyraTheme.h` | status lane = 19+8+4 = 31 | L: 13 % of the height; viewport 204 px. The comment in `UiFontScale.h` warns that moving it invalidates pagination caches, which is no issue on a new board. | Central: cap `statusBarVerticalMargin` in `forPanel()` (not capped yet) |
| S2 | `BaseTheme::drawStatusBar` | `titleMarginLeft/Right = cluster + 30` | P defaults: left cluster 53 + 30, right cluster 71 + 30, leaves 230-83-101 = 46 px, about 7 chars of chapter title. With a left clock (+40) about 6 px, so the title is reduced to an ellipsis. No overlap, it truncates cleanly. | Local: drop the 30 on compact |
| S1 | `QrDisplayActivity::render`, `QrUtils::drawQrCode` | `px = max(1, maxDim/size)` | L: bounds height 103. A payload above about version 21 (over 101 modules) is forced to 1 px and exceeds the bounds: it overlaps the header and is clipped at the bottom, so it will not scan. P: bounds 200, fits up to version 40 at 1 px; dense codes at 1 px are probably unscannable (S3). | Local: refuse or truncate payload so `size <= maxDim` |
| S3 | `screenMarginHorizontal` in `SettingsList.h` | `{SCREEN_MARGIN_MIN, SCREEN_MARGIN_MAX, SCREEN_MARGIN_STEP}` = `{5, 40, 5}` (`CrossPointSettings.h`) | At 40 the column is 160 px: 10 chars at 14 pt. | Murphy: cap at 20 |
