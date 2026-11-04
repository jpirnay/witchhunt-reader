# Murphy M3: fixed sizes that assume a bigger panel

Audit of `feat/murphy-support`, 2026-11-03. The M3 is a 416x240 1-bit panel; the UI runs Portrait, so the
logical screen is 240x416. Abbreviations: **P** = Portrait 240x416, **L** = Landscape 416x240. Paths are
relative to the repo root.

**Status.** Fixed and checked on the panel: R1, R2, R3, R4, R5 (portrait and landscape), R8, R10, the slider, the cover
grid (six items a page), Calibre's layout, and System Information. Fixed but never seen on the panel: R9's reader
defaults (they only apply to a fresh settings file), Carousel leaving the theme list (R7), the Finished Book and OTA
text bounds, Weather's moon line, and Calibre's upload bar. The per-area tables further down were worked out before
those fixes, by arithmetic from the code, so their pixel figures are out of date: check a row against the panel before
acting on it.

Landscape needed one fix that was not about size. A virtual front strip now keeps portrait's key roles
(`HalCapabilities::frontStripIsVirtual()`, in the orientation provider in `main.cpp`), because rotating them turned
the physical Up and Down keys into Rename and Delete on the Starred Pages list.

`borrowSecondaryBuffer()` lends a 256 KB PSRAM block on Murphy and the X4 Pro (`HalCapabilities::lendsPsramArena()`).
Murphy's 12,480 byte framebuffer was too small for a ZIP entry's inflate ring, so unopened books got no cover.

**Open, most useful first:**
- Library: the first open after a boot blocks the loop for 43 s on a card with a folder of 999+ books (X4 Pro, with or
  without the arena). A key press aborts the count; serial commands wait for it. Later visits are fast. Not a Murphy
  problem: `fix/library-folder-walk` takes it to 2.4 s.
- XTC: a page encoded for another panel is cropped (`XtcReaderActivity.cpp`, 480x800 shows its top-left quarter). Needs
  a 2:1 reduction and a test file.
- KOReader Sync's comparison screen was reflowed but never seen: it needs credentials and a server. Its arithmetic
  fits in both orientations.
- Reading Stats book detail in landscape overflows (`ReadingStatsBookDetailActivity.cpp`), and `QrDisplayActivity` in
  landscape forces a large payload to 1 px modules.
- Button Actions Overview lists keys Murphy does not have.
- Arena: the T5S3 is not on it. A background build still yields to a render; with the arena it no longer has to, which
  would be the next gain (`recoverSecondaryBufferIfNeeded()`).

The QR pages (`CrossPointWebServerActivity`, `WifiSelectionActivity`) were reworked on `QrUtils::compactLayout()`
and are not covered here. Compact metrics do not make that layout redundant: the full layout puts text beside the
code, and `http://crosspoint.local/` does not fit next to a code in 240 px.

## 0. Ground rules worked out first

**Which screens can be landscape at all.** Only these: the readers and whatever opens over them (reader menu, chapter and footnote lists, percent slider, printed-page input, dictionary, starred pages, quick overrides, QR, keyboard, book info, book stats), plus two screens that force it: `WeatherActivity.cpp:210` and `ButtonActionsOverviewActivity.cpp:69`. Everything else runs in Portrait (`EpubReaderActivity.cpp:856` restores it on reader exit), so "L" does not apply to Home, Settings, Library, network, OPDS. `KOReaderSyncActivity` replaces the reader, so it is Portrait too.

**Fonts on Murphy.** Measured from the generated glyph tables (advances only, kerning ignored, so widths are slightly high). The point sizes are at 150 DPI, so these are bigger than the names suggest.

| Slot | Face | Line height | Average px/char (en) | Chars across 240 |
|---|---|---|---|---|
| SMALL_FONT_ID | notosans_6 | 17 | 6.1 | 39 |
| UI_10_FONT_ID | inter_ui_8 | 20 | 8.2 (bold 8.4) | 29 |
| UI_12_FONT_ID | inter_ui_10 | 25 | 10.2 (bold 10.4) | 23 |

**Content rects with Lyra metrics** (`UITheme::getContentRect`, `UITheme.cpp:139-192`):

| Call | P | L clockwise | L counter-clockwise |
|---|---|---|---|
| (hints, no side) | 0,0,240,376 | 48,0,368,240 | 0,0,368,240 |
| (hints, side) | 0,0,202,376 | 48,0,368,210 | 0,30,368,210 |

Header band y=5..89, body from y=105. Chrome is 145 of 416 px in P (35 %), and the body starts at 44 % of the height in L.

**BoardProfile.uiScale: confirmed no consumers.** Only `lib/hal/HalCapabilities.h:205` reads it, and nothing calls that function. Murphy's value is 1.0 anyway (`BoardConfig.h`, MURPHY_M3). A float multiplier would not help much either: most draw code reads the compile-time `XMetrics::values` tables directly (84 uses: Lyra 36, Base 29, Carousel 13, Lyra3 6), which bypass `UITheme::getMetrics()`.

**No memory hazard from drawing off-panel.** Verified by reading: `fillPhysicalHSpanByte` clamps (`GfxRenderer.cpp:1830`), `drawPixel` bounds-checks (`:1266`), `fillRect`, `fillRoundedRect` and `drawRoundedRect` reject non-positive sizes, glyph blits check rows and columns (`:511ff`), `blitImage` breaks out on the wrapped uint16 coordinate (`FreeInkDisplay.cpp:305`), `copyRegionToBuffer`/`copyBufferToRegion` clamp and size-check (`GfxRenderer.cpp:3058ff`). Everything below is therefore wrong pixels, not corruption, with one S3 logging exception.

---

## 1. Central root causes, in the order I would fix them

| # | Root cause | Where | What one change buys |
|---|---|---|---|
| **R6** | Home is laid out as a fixed tile plus a fixed menu, with floors that exceed the panel. | `HomeActivity.cpp:45-47,80-115`, `LyraTheme.cpp:592-676,775-790`, `BaseTheme.cpp:970-983` | R1 fixes most of the menu; the card still needs a narrow-column path (R6 rows below). |

---

## 2. Hint strips

| Sev | File:line | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S1 | `LyraTheme.cpp:406`, `ButtonHintLayout.h:56-57`, `HintLayout.h:56` | `buttonWidth = 80`, `gap = slack>0 ? ... : 0`, `max(base, min(room/4, W/4))` | P: slack -80, gap 0, x = {0,80,160,240}. `bottomMaxWidth` wants 60, base floors it to 80. Box 4 starts exactly at the panel edge: this is the device screenshot. Its touch rect is recorded at x=240..319, so no tap can reach it. L: same panel frame, box 4 is past the logical bottom. | Central R2 |
| S1 | `BaseTheme.cpp:276` | `buttonWidth = 106` | Classic, P: x = {0,106,212,318}. Box 3 keeps 28 of 106 px with its centred label cut, box 4 is gone. | Central R2 |
| S1 | `LyraTheme.cpp:47,498,555`, `HintLayout.h:145` | `topHintButtonY = 345`, `buttonHeight = 78`, `sideStacked(...)` | Junction 423 is past the panel. room = min(423-89, 376-428) = -52, so `grow` returns base. Up box y=345..422 runs through the bottom strip (376) and off the end. Down box y=428..505 is entirely off-panel, with a dead touch rect. Up is drawn after the bottom strip (`ListController.cpp:298-299`), so it overprints hint 3. Same panel coordinates in L. | Central R3 |
| S1 | `BaseTheme.cpp:412,415` | `topButtonY = 345`, height 80, gap 0 | Classic: Up 345..424, Down 425..504, same result. | Central R3 |
| S2 | `UITheme.cpp:186-187`, `HeaderLayout.h:33` | `columnReserve(30)` = 38 | Every screen using `hasSideHints=true` loses 16 % of its width for a box that is not beside any key: lists, Library, keyboard, reader menu, tab bars, cover grid. L: 30 of 240 px of height. | Central R3 |
| S2 | `ButtonGeometry.h:85-88` | `default:` unmeasured board | Murphy has no entry, so `up`/`down` anchors are absent and hints cannot follow the physical keys. | Central R3 |
| S3 | `LyraTheme.h:28`, `BaseTheme.h:119` | `buttonHintsHeight = 40` | The label is 17 px tall at a 7 px offset, so about 12 px per screen is unused. | Central R1 |

Not a size problem, but it decides what the strip should say: Murphy has physical Confirm, Up, Down only (`BoardConfig.h`, MURPHY_M3), so "Back" and Left/Right exist only as touch targets on this strip. Hence the off-panel fourth box is a lost control, not only a lost label.

## 3. Theme chrome, shared drawing

| Sev | File:line | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S2 | `LyraTheme.h:12-14`, `LyraTheme.cpp:167-222` | `headerHeight = 84`, `batteryBarHeight = 40`, `verticalSpacing = 16` | Body starts at y=105: 25 % of P, 44 % of L. Title sits at y+43 with about 13 px blank beneath it. | Central R1 |
| S2 | `GfxRenderer.cpp:1428` | `x = (W - textWidth)/2`, no bound | Goes negative, both ends cut. en over 240: `KOReaderSyncActivity.cpp:881` 291 px, `KOReaderAuthActivity.cpp:176` 243. Near the limit: `ClearCacheActivity.cpp:104` 236, `EpubReaderPrintedPageInputActivity.cpp:183` 227, `KeyboardEntryActivity.cpp:839` 225. de/fr/ru: about 20 of the static strings exceed 240 (for example `KeyboardEntryActivity.cpp:841` 324-339 px, `DetectTimezoneActivity.cpp:241` de 310). Dynamic strings are unmeasured (`FullScreenMessageActivity.cpp:15`, `BmpViewerActivity.cpp:358`, `KOReaderAuthActivity.cpp:179`, `SyncTimeActivity.cpp:203`, `OpdsBookBrowserActivity.cpp:549`). | Central R4 |
| S2 | `LyraTheme.cpp:840-860` | `w = textWidth + 32`, `x = (W-w)/2`, no truncation | en over 240: `FileBrowserActivity.cpp:1276` 303, `BmpViewerActivity.cpp:438` 284, `:444` 254, `OpdsBookBrowserActivity.cpp:749` 246. de pushes `SleepActivity.cpp:1038` to 294, "Loading font" to 248. The box is wider than the panel and the message loses both ends. L: fine up to 416. | Central R4 |
| S2 | `BaseTheme.cpp:1035-1050` | same with margin 15, bold | Classic equivalent. | Central R4 |
| S2 | `BaseTheme.cpp:605-607` | `rect.width - padding*2 - 40` | Classic header title capped at 116 px (11 chars). "Reading Stats" 141 px is truncated. | Central R1 (padding) |
| S2 | `BaseTheme.cpp:527,567` | `- (rowValue ? 60 : 0)`, value untruncated | Classic: row text 97 px at width 202, and a value over 60 px overprints the title. | Central R1 + local truncate |
| S2 | `LyraTheme.cpp:828` | label drawn with no truncation | Home menu text starts at x=78, 142 px to the tile edge. en fits (max 136). fr "Reading Stats" 220, "File Transfer" 200, ru 191: clipped at the panel edge. | Local: truncate in `drawButtonMenu` |
| S3 | `LyraTheme.cpp:50,326` and `LyraCarouselTheme.cpp` `maxValWidth` | `maxListValueWidth = 200` | Larger than the whole row text area (183 at 240). A value over about 170 px drives `rowTextWidth` <= 0, `truncatedText` returns "", and the title disappears. Only `ButtonRemapActivity` and Wi-Fi pass values, and remap's are short, so low exposure today. | Local: `min(200, rect.width/2)` |
| S3 | `LyraTheme.cpp:764` | `padding = 48`, no truncation | "No open book": en ends at 188, fine. de 270 px runs off. | Central R1 + truncate |
| S3 | `LyraTheme.cpp:46`, `LyraTheme.h:15` | side padding 20 + selection padding 8 | Row text inset 28 px each side: 145 of 202 px usable in `GUI.drawList`. | Central R1 |
| S3 | `GfxRenderer.cpp:1277` with `:1674` | off-panel horizontal `drawLine` goes through `drawPixel`, which `LOG_ERR`s per pixel | Base side boxes (`BaseTheme.cpp` `line()`), and the carousel's off-screen `drawPixel` corners, log `!! Outside range` per pixel on every frame. `LOG_ERR` is compiled in at every level (`Logging.h:70`). Rounded rects go through `fillRect`, so Lyra hints are mostly quiet. Goes away with R3/R7. | Follows R3, R7 |

## 4. Home

Default menu on Murphy is 5 rows (Library, Stats, File Transfer, Weather, Settings), 6 with bookmarks. `useWeather = 1` and all `show*OnHome = 1` (`CrossPointSettings.h:780-789`). Simulated from `computeHomeScreenLayout` + `drawButtonMenu`:

| Theme | Entries | Tile | Menu height | Row height |
|---|---|---|---|---|
| Lyra | 4 / 5 / 6 | 170 | 146 | 36 / 29 / 24 |
| Lyra 3 Covers | 4 / 5 / 6 | 200 | 116 | 29 / 23 / 19 |
| Classic | 4 / 5 / 6 | 280 | 54 | 13 / 10 / 9 |

| Sev | File:line | Constant / expression | What happens at 240x416 | Fix |
|---|---|---|---|---|
| S2 | `LyraTheme.cpp:592,595,676,754` | `coverWidth = coverHeight*0.6` = 92, `textWidth = 200-16-16-92` = 76, status line drawn untruncated | Text column is x=136..212, 76 px: about 7 title chars a line. Status is bold UI_10: "10% · ~27m" 96 px ends at 232, past the card edge at 220 (the reported overrun). "100% · ~2h 15m" 129 px ends at 265, off the panel. | Local: truncate the status, and below a column minimum put the status and history under the cover or shrink the cover. R6 |
| S2 | `LyraTheme.cpp:51,780-787,823`, `HomeActivity.cpp:46` | `mainMenuIconSize = 32` in rows of 29 / 24, floor `minAdaptiveMenuRowHeight = 40` dropped by the `max(1, h/n)` fallback | With the default 5 rows, icons (top at row+5) overrun the next row by 8 px; with 6 rows by about 11 px, and the 25 px text line is taller than the 24 px row. Selection fill is shorter than the icon. | Central R1 (`menuRowHeight` 36) plus 24 px icons in `drawButtonMenu` when the row is under 36. `mainMenuColumns = 2` at `LyraTheme.cpp:53` is defined but unused, if a two-column menu is wanted. |
| S1 | `HomeActivity.cpp:47`, `Lyra3CoversTheme.h:15-45,60` | min tile 200, rows 23 / 19 | Row is shorter than the 25 px text, so labels collide and icons overlap by 10+ px. Tiles are 66 px wide, covers 50x110, titles about 8 chars. Three covers do not work on this width. | Local: `homeRecentBooksCount` 1 or 2 on Murphy, or drop the theme. R6 |
| S1 | `HomeActivity.cpp:45`, `BaseTheme.cpp:980` | `CLASSIC_MIN_RECENT_TILE_HEIGHT = 280` | Menu gets 54 px, rows 9-13 px with 20 px text: labels overprint each other and are unreadable. | Central R1 plus lower floor. R6 |
| S1 | `LyraCarouselTheme.h:32-33,55-58`, `LyraCarouselTheme.cpp:50`, `CarouselCoverLayout.h:51-60` | centre 340x540, side 200x390, overlap 60 | Centre tile x=-50..290, y=66..606: wider and taller than the panel. Left side ends at x=10, right side starts at x=230. Icon row (y=316) and hints draw straight over the cover. Dots, author, title (y>=614) and history are off-panel. Thumbs generated at 340x540 and 200x390 are larger than the panel. | Central R7 |
| S3 | `Lyra3CoversTheme.h:56` | `historyRowHeight = 23` "FIT_SMALL line height" | On Murphy that face is 17 px: 6 px wasted, and the comment is wrong here. | Local: read the ladder |
| S3 | `BookInfoActivity.cpp:126,128,143,145` | `metrics.homeCoverHeight` = 226 | Home uses a 154 px thumb, Book Info generates a second at 226 and draws it scaled to at most 100 px wide, which resamples a dithered 1-bit image (the repo's own rule says never). | Local, or R1 sets 154 |

## 5. FreeInkUI widgets as the firmware calls them

| Sev | File:line | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S1 | `KeyboardEntryActivity.cpp:27-28,125,406,485-487,696` | 5 rows x 40 + gaps = 232, `hintGap` 23, `inputStartY = 105 + 64 - 7` = 162 | P: keyboard top y=121, input field y=162. `frame.target().fill(kbRect, ...)` paints after the text, so **the typed text is covered by the keyboard**. Header survives (ends at 89). Carousel's 50 px keys: top y=71, header covered too. | Central R5 |
| S1 | same | same | L clockwise: content bottom 210, top y=-45: the digit row is off-panel and unreachable, and header and field are covered. L counter-clockwise: top y=-15. Used over the reader for search and renaming a star. | Central R5 |
| S1 | `keyboard.h:596-615`, `KeyboardEntryActivity.cpp:20,24` | gap 6, padding 4, width 202, 20 units | Unit 7 px, so keys are 14 px wide. Label font is UI_12: 'm' 18, 'w' 17, 'W' 20, 'q' 13, 'a' 12. Wide glyphs do not fit their key, and `text()` truncates to the rect, so they should render as an ellipsis (inferred, see section 11). German 11-key rows and Russian 12-key rows are narrower still (about 10 px). 14 px touch targets also miss a lot. | Central R5: label font UI_10, gap 2-3, full width via R3 |
| S2 | `ListTabBar.h:8`, `EpubReaderMenuActivity.h:90`, `FileBrowserActivity.cpp:259` | `HEIGHT = 54`, `54 + growth` | 13 % of P (22 % of L) for one line of text. Settings already uses `metrics.tabBarHeight` (`SettingsActivity.cpp:380`), so the two disagree. | Central R10 |
| S2 | `ListTabBar.cpp` (`props.text`), `tab-bar.h` equal slots | 4 tabs in 202 = 50 px slots, UI_10 | Settings: Display 58, Reader 56, Controls 67, System 59: all ellipsized. Library: Recent 55, Authors 62. Reader menu: Navigation 84, Settings 64. At 240 (R3) slots are 60: most fit, Controls and Navigation do not. | Central R10 (small font) |
| S2 | `list.h:466-484` (`measureListRow`), `UiListActivity.cpp:159` | content = 202-40 = 162, value width unbounded, `maxLines = 2` | Toggle rows leave 114 px for the label: 94 of the 254 strings referenced from `SettingsList.h` need a second line, 4 are ellipsized. A value row with a 100 px value leaves a 52 px label. A value over 152 px gives label width 0 and the value runs left out of its band (`list.h:894-899`). Not truncated. | Central R1 + R3 give +62 px. SDK: cap value width at 60 % |
| S3 | `ConfirmDialog.h:94`, `option-dialog.h` padding 16, gap 8 | width = 4/5 of safe width | 192 px panel, 160 content, two 76 px buttons. en fine, de "Abbrechen" ellipsized. In L only the message gives up lines, so a 3-line headline with buttons can exceed 240. | Local: use 92 % width on compact |

## 6. Settings-side activities

| Sev | File:line | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S1 | `SliderPickerActivity.cpp:21,33,189` | `kBarWidth = 360` | P: `barLeft` = -60, bar spans -60..300. Both end caps are off-panel, and the knob leaves the screen below 17 % and above 83 %. Touch: x only reaches 0..239, so `valueForX` cannot reach the outer sixths; buttons still can. L: fits (52..412). Used by every `SliderSetting` and the reader's percent jump. | Local one-liner: `min(360, contentRect.width - 2*pad)` |
| S1 | `ReadingStatsActivity.cpp:94-227`, `CardLayout.cpp` | cards stacked with no fit check or paging | From y=105: all-time card about 153, 30-day card 78, so y is about 336; Top Books header reaches about 374 and its rows are under the hint strip or off-panel. With a live session card (+88) the 30-day card goes too. | Local: reuse `sparkFits` logic plus paging as `SystemInformationActivity` does |
| S1 | `ReadingStatsBookDetailActivity.cpp:200` | same, only the spark is guarded | L (opened from the reader menu): body 135 px, summary 55 + time 104 already exceed it. History card off-panel. P: fits. | Local: paging |
| S2 | `ReadingStatsActivity.cpp:106`, `ReadingStatsBookDetailActivity.cpp:140` | `outerMarginX = verticalSpacing*2` = 32 | Card 176 wide, inner 156. Stat grid cells 39 px, labels `labelMaxW` 31 px: "Sessions" wraps or ellipsizes. | Central R1 |
| S2 | `CardLayout.cpp:41-45` | label and value both untruncated | Inner 156: "Time to finish" 111 + "12h 34m" about 60 overlap. de "Pages turned" 173 alone exceeds the card. | Central R4 |
| S1 | `CalibreConnectActivity.cpp:202,212-213,219,225` | y stack: 161, +87, Exit button +112 | P: "Calibre setup" heading at y=360 into the hint strip; instructions start about y=410; Status heading and the upload progress bar are off-panel. A user cannot see transfer progress. Instruction 4 is 225 px at x=20, also clipped. | Local: put status first, hide instructions on compact |
| S1 | `ButtonActionsOverviewActivity.cpp:24,102` | forced L, 7 rows x 24 from y=131 | Rows end at 299: Up is cut, Down and Confirm are off-panel. No paging. Columns 73 px, so action names are ellipsized. Rows also name keys Murphy does not have. | Central R8 |
| S2 | `SystemInformationActivity.cpp:140-141,170-171` | `leftX = 48`, `valueX = 120`, value untruncated | Label column 72 px. 10 of 17 labels are wider in en ("App partition" 106, "Panel controller" 129, "Free / Min / Max" 131), so label overprints value. Values run past x=240. Layout paging and the logo guard are fine. | Local: label over value on compact; R1 shrinks `leftX` to 18 |
| S2 | `BootDiagnosticsActivity.cpp:100,103` | `leftX = 48`, `valueX = 42 %` = 100 | Both truncated, so no overlap, but labels get 48 px: about 5 chars. Diagnostic rows become useless. | Central R1 |
| S2 | `ButtonRemapActivity.cpp:143,146` | `topOffset + 4*row + 4*spacing` | Help lines land at y=369 and y=405: first is half under the hint strip (376), second fully under it. Prompt 261 px is truncated in the subheader. | Central R1, then check |
| S2 | `FinishedBookActivity.cpp:810,812` | `drawText` untruncated | "Congratulations, you finished this book." 313 px at x=20: cut at the panel edge. Line 2 235 px also runs through the side column. | Local: `wrappedText` |
| S2 | `OtaUpdateActivity.cpp:170,173` | `drawText` of label + version | At x=20 with 220 px: "Current version: " plus the version string is likely over. | Local: truncate or two lines |
| S3 | `StatusBarSettingsActivity.cpp:95,102` | `previewHeight = 78`, band 130 | List gets 141 px, about 5 rows. Usable. | Central R1 |
| S3 | `ClockSettingsActivity.cpp:109` | warning in subheader | "Uses more battery; Clock may drift" is truncated when no RTC. | Local |
| S3 | `GestureActionsOverviewActivity.cpp` (frame maths) | zones scaled from the screen | Scales correctly, but cell labels get about a third of a roughly 150 px frame, so they are clipped. Read only partly. | Accept |

## 7. Reader and its overlays

| Sev | File:line | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S1 | `KOReaderSyncActivity.cpp:931-932,950,961` | `optionY = 350`, `optionHeight = 30` | P: strip top is y=376. Option 1 (Apply) at 350..380 is visible with its last 4 px cut. Option 2 (Upload) at 380..410 is fully under the strip. Option 3 (conflict policy) at 410..440 is under it and mostly off-panel. Upload and the policy are selected blind. | Local: derive from the content rect, compress the comparison block |
| S2 | `KOReaderSyncActivity.cpp:880-881,899-929,972-999` | y = 120..320 literals; `remoteChapterStr` untruncated | Literals all fit in P, so no L problem since this screen is Portrait. Chapter titles at x=20 run off the right edge. Line 881 is 291 px (R4). | Local truncate |
| S1 | `XtcReaderActivity.cpp:266-267` | `min(pageWidth, displayHeight)`, `min(pageHeight, displayWidth)` | XTC/XTCH pages are pre-rendered bitmaps. A 480x800 page shows only its top-left 240x416, a quarter of the page, and the book cannot be read. Only files encoded for 240x416 work. Same for `drawCurrentPageToBuffer` (sleep overlay). | Local: downscale, or refuse with a message |
| S1 | `TxtReaderActivity.cpp:231`, `MdReaderActivity.cpp:865`, `EpubReaderActivity.cpp:5212,5221,5286` | `drawCenteredText(..., 300, ...)` | L only: y=300 is past 240, so "Empty file/chapter" and "Out of bounds" show a blank page. P fine. | Local: `getScreenHeight()/2` |
| S2 | `CrossPointSettings.h:160,493` | `DEFAULT_FONT_POINT_SIZE = 14`, `hyphenationEnabled = 0` | Viewport 230 wide. Bookerly 14: about 15 chars a line and 10 lines in P; 27 chars and 5 lines in L. Justified text with no hyphenation at 15 chars gives rivers and forced breaks (`ParsedText.cpp:1155-1157`). Bookerly 10: 22 chars and 14 lines. | Central R9 |
| S2 | `UITheme.cpp:252-256`, `LyraTheme.h:33` | status lane = 19+8+4 = 31 | L: 13 % of the height; viewport 204 px. The comment in `UiFontScale.h` warns that moving it invalidates pagination caches, which is no issue on a new board. | Central R1 (`statusBarVerticalMargin`) |
| S2 | `BaseTheme.cpp:1269-1280` | `titleMargin* = cluster + 30` | P defaults: left cluster 53 + 30, right cluster 71 + 30, leaves 230-83-101 = 46 px, about 7 chars of chapter title. With a left clock (+40) about 6 px, so the title is reduced to an ellipsis. No overlap, it truncates cleanly. | Local: drop the 30 on compact |
| S2 | `StarredPagesActivity.cpp:136,145`, `QrDisplayActivity.cpp`, `DictionaryDefinitionActivity.cpp` | full theme header in L | L: 105 px of header leaves 2 rows of 40 in Starred Pages. | Central R1 |
| S2 | `EpubReaderMenuActivity.cpp:62-69`, `.h:90` | title + progress + tabs = 125 | L: list gets 85 px, 3 rows at 28. | Central R10 |
| S1 | `QrDisplayActivity.cpp:40-44`, `QrUtils.cpp:100-105` | `px = max(1, maxDim/size)` | L: bounds height 103. A payload above about version 21 (over 101 modules) is forced to 1 px and exceeds the bounds: it overlaps the header and is clipped at the bottom, so it will not scan. P: bounds 200, fits up to version 40 at 1 px; dense codes at 1 px are probably unscannable (S3). | Local: refuse or truncate payload so `size <= maxDim` |
| S3 | `MdReaderActivity.h:27,69-70`, `.cpp:667,737` | `MIN_COL_INNER_WIDTH = 24`, `LIST_INDENT = 20` | Tables with 7+ columns flatten at 230 px, handled deliberately. Third-level list indent is 60 of 230. | Accept |
| S3 | `SettingsList.h:400` | `screenMargin {5, 40, 5}` | At 40 the column is 160 px: 10 chars at 14 pt. | Murphy: cap at 20 |
| S3 | `EpubReaderActivity.cpp:6036-6039` | hint box height 44, `hintY2 = +20` | Truncated-section hint fits on Murphy; numbers were tuned for a larger line height. | Accept |

## 8. Weather

| Sev | File:line | Constant / expression | What happens at 416x240 (forced) | Fix |
|---|---|---|---|---|
| S1 | `WeatherActivity.cpp:471-472` | `graphHeight = 240`, `topSectionHeight = pageHeight - graphHeight` = **0** | Current-conditions panel, daily cards and the hourly graph all draw from y about 0 on top of each other. Current panel text runs to y about 219 with no clip. Separators have height 0. Whole screen is overprinted. | Central R8 |
| S1 | `WeatherActivity.cpp:470,478`, `lib/Weather/WeatherIconsLarge.h:8` | `leftPanelWidth = 220`, `WEATHER_ICON_SIZE = 64` | Forecast area is 376-220 = 156 px. Split over `numDays` (7 if the API returns a week, inferred), that is 22 px cards holding 64 px icons and untruncated "Tue" / temperature / moon strings. | Central R8 |
| S2 | `WeatherActivity.cpp:468` | `contentX = metrics.buttonHintsHeight` | Ignores the 8 px hint gap in `getContentRect`; panel touches the strip. Minor next to the above. | Local |

## 9. Assets, buffers, sizes

| Sev | File:line | Constant / expression | What happens | Fix |
|---|---|---|---|---|
| S2 | `CoverGridLayout.h:25,35-36,41`, `.cpp:17-44` | cell max 242, min 96, margin 10 | Library covers view: usable 213 px at width 202 gives 1 column, 1 row, cell 182x167: one book per page. Adapts safely, but cover view is close to useless. R1 + R3 give about 240x260, still 1-2 per page. | Accept, or default list view on Murphy |
| S3 | `FinishedBookActivity.cpp:56-57,192` | `kThumbWidth/Height` = 196x240 | Shares no file with the grid thumb here (180x165, the code comment says it only matches on X3/X4), and draws at 72 px tall: extra thumb per book plus 1-bit resampling. | Local |
| S3 | `SleepActivity.cpp:762-800` | custom sleep BMPs | Display-size covers are generated correctly. User BMPs made for 480x800 get scaled, i.e. a dithered 1-bit image resampled. | Accept |
| S3 | `CrossPointSettings.h:440`, `EpubReaderActivity.cpp:5776`, `TxtReaderActivity.cpp:304` | `textAntiAliasing = 1`, no capability gate | Murphy driver overrides no gray methods, so `copyGrayscaleLsb/Msb` are empty (`PanelDriver.h:159-166`). The reader still renders the AA planes, which wastes extra page renders per turn. | Central R9, or gate on capability |
| S3 | `PanelDriver.h:221` | `grayLevels()` default 4 | A 1-bit panel reports 4 levels, so BMP viewer and sleep paths believe in grayscale. Should return 2 for Murphy, in the SDK. | SDK, local |
| S3 | `GfxRenderer.cpp:2157` | `drawIcon` portrait-only transform | L: `physY = 416 - w - x` is past 240 for most x, icon silently dropped. All boards, no current L caller with row icons. | Leave |

**Checked, fine (do not re-audit):**
- Framebuffer and BW chunk sizes come from the profile (`MAX_FRAMEBUFFER_BYTES`, `GfxRenderer.cpp:117-118`). No static array sized by a panel constant in `src/` or `lib/`.
- `FontPreviewCache.cpp` row buffer (128 B) and dictionary snapshot (2048 B) are upper bounds that hold.
- `Logo120` on Boot/Sleep fits P (y=148..268, text to 320). `SystemInformationActivity.cpp:319` guards it. `MoonIcon` 48 px fits.
- Carousel frame cache: region clamps to 11,040 B, fits the 12,480 B lent buffer.
- `lib/Xtc/Xtc/XtcTypes.h:31-32` 480x800 are parser defaults only.
- EPUB image sizing derives from the viewport; `kMinImageSliceH = 64` is fine. `PngToBmpConverter.cpp:47` and `JpegToBmpConverter.cpp:929` caps are source pixels, not panel.
- `UITheme::getNumberOfItemsPerPage` has no callers, so its possible 0 is harmless. The private `getPageItems()` versions clamp with `max(1, ...)`. `drawList` guards `pageItems <= 0`.
- Hint/status primitives do not divide by a dimension that can reach 0 here. Reader viewport stays positive at the 40 px margin.
- OPDS (`OpdsBookBrowserActivity.cpp`): body-relative, truncates, fits at 240. Font download, USB drive, serial transfer, SD firmware update wrap their text.
- `TapTargets`, `ListTouchBand` caps are upper bounds only.

**Trap for whoever tunes list rows.** `makeUiTarget` (`UiAppHelpers.h:13`) builds `GfxRendererTarget` with `hasTouch = false` (`FreeInkUIGfxRenderer.h:44`), so FreeInkUI lists size to 20 + 2x4 = 28 px rows with no gap, although `mappedInput.hasTouch()` is true. That is what makes Settings show about 8 rows. If someone passes `true`, `listTouchMinRowHeight = 56` plus gap 6 (`FreeInkUICore.h:656-658`) cut that to 3 rows, and `uiThemeTokens()` would need to override those tokens.

**Lent framebuffer is 12,480 B on Murphy, not the 48 KB the comments assume.** Sampled consumers guard capacity and fall back to heap: `image_scratch::canServe`, `JpegToBmpConverter.cpp:678,894`, `Section.cpp:1385`, `TtfFont.cpp:465`, `FootnotePreviews.cpp:521`, and `Section.cpp` runs on 10 KB by default so 12 KB is above its floor. I sampled these rather than tracing all 11 borrow sites. The S3's 8 MiB PSRAM makes a heap fallback cheap, so I expect this to be fine.

## 10. QR constants left outside the two excluded files

- `QrUtils.cpp:100-105`: scale logic, see section 7.
- `QrDisplayActivity.cpp:39`: `truncNoticeHeight = 16`, fine.
- `CrossPointWebServerActivity.cpp:32-33` (`QR_CODE_WIDTH/HEIGHT = 198`) and its layout are the excluded file. Noted only.
- `CalibreConnectActivity` has no QR.

## 11. Coverage, and what is inferred

**Not reviewed:** `WifiSelectionActivity.cpp`, `CrossPointWebServerActivity.cpp` (per brief). `FontScalingTestActivity`, `EpubRenderBenchmarkActivity` (developer screens, skimmed). `TxtReaderActivity`/`MdReaderActivity` body layout beyond constants. EPUB CSS absolute margins and table cell minimums (`ChapterHtmlSlimParser.cpp`), so a `margin-left: 40px` on 230 px is unassessed. The web UI. Other SDK components (`publication-page.h`, `cover-grid.h`, `sheet.h`, `tile-grid.h`) were only grepped, and I did not establish which are in use. `FrontlightPanelActivity` geometry was skimmed only and looks fine. `KOReaderSettingsActivity`, `OpdsSettingsActivity`, `WeatherSettingsActivity`, `LanguageSelectActivity`, `EnumSelectionActivity`, `HomeMoreActivity`, `FileContextMenuActivity` are list-based and inherit section 5; not read individually.

**Inferred, not verified by reading or on the device:**
- Keyboard keys showing an ellipsis instead of m/w: from `text()` calling `truncatedText` against a key-sized rect. I did not read `button()`'s label rect, and the exact glyph result needs the device.
- Weather `numDays` = 7 depends on the API request.
- `OtaUpdateActivity` overflow depends on the runtime version string length.
- ReadingStats heights are arithmetic from `CardLayout` with typical row counts.
- Text widths ignore kerning. Translations measured are de, fr, ru only; CJK and the other languages are unmeasured.
- Everything is arithmetic from the code at this working tree, which was in flux. Only the 3-of-4 hints and the status overrun have been seen on the device.
