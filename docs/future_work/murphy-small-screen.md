# Murphy M3: fixed sizes that assume a bigger panel

Audit of `feat/murphy-support`, 2026-11-03. The M3 is a 416x240 panel, 1-bit at the time and four-level gray since ([grayscale-aa-rendering.md](../grayscale-aa-rendering.md#murphy-m3-gray-and-its-bw-waveforms)); the UI runs Portrait, so the
logical screen is 240x416. Abbreviations: **P** = Portrait 240x416, **L** = Landscape 416x240. Paths are
relative to the repo root.

**Status.** Fixed and checked on the panel: R1, R2, R3, R4, R5 (portrait and landscape), R8, R10, the slider, the cover
grid (six items a page), Calibre's layout, and System Information. Fixed but never seen on the panel: R9's reader
defaults (they only apply to a fresh settings file), Carousel leaving the theme list (R7), the Finished Book and OTA
text bounds, Weather's moon line, and Calibre's upload bar. The per-area tables further down keep only the rows still
open on 2026-11-09, each checked against its file. Their pixel figures are arithmetic from the code before those fixes,
so they are out of date: check a row against the panel before acting on it. Text widths ignore kerning, and of the
translations only de, fr and ru were measured. Not audited at all: EPUB CSS absolute margins and table cell minimums on
a 230 px column, the TXT and Markdown body layout beyond its constants, and the SDK components `publication-page.h`,
`cover-grid.h`, `sheet.h` and `tile-grid.h` (grepped only). What was checked and found fine by reading is in
[board-support.md](../contributing/board-support.md#gotchas).

Landscape needed one fix that was not about size. A virtual front strip now keeps portrait's key roles
(`HalCapabilities::frontStripIsVirtual()`, in the orientation provider in `main.cpp`), because rotating them turned
the physical Up and Down keys into Rename and Delete on the Starred Pages list.

`borrowSecondaryBuffer()` lends a 256 KB PSRAM block on every S3 board (`HalCapabilities::keepsSecondaryBuffer()`).
Murphy's 12,480 byte framebuffer was too small for a ZIP entry's inflate ring, so unopened books got no cover.

**Open, most useful first:**
- XTC: a page encoded for another panel is cropped (`XtcReaderActivity.cpp:268-269`, 480x800 shows its top-left
  quarter). Needs a 2:1 reduction and a test file.
- KOReader Sync's comparison screen was reflowed but never seen: it needs credentials and a server. Its arithmetic
  fits in both orientations.
- Reading Stats book detail in landscape overflows (`ReadingStatsBookDetailActivity.cpp:200`), and `QrDisplayActivity`
  in landscape forces a large payload to 1 px modules (`QrUtils.cpp:104`).
- Button Actions Overview lists all 7 keys, some of which Murphy does not have (`ButtonActionsOverviewActivity.cpp:25`).
- Arena: a background build still yields to a render; with the arena it no longer has to, which would be the next gain
  (`recoverSecondaryBufferIfNeeded()`, `EpubReaderActivity.cpp:4001-4022`).

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

Nothing open. R1, R2 and R3 fixed every row: the bottom boxes share the panel's width, and a compact panel has no
side column (`BaseTheme.h`, `forPanel`).

## 3. Theme chrome, shared drawing

| Sev | File:line | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S2 | `BaseTheme.cpp:528,566-569` | `- (rowValue ? 60 : 0)`, value untruncated | Classic: row text 97 px at width 202, and a value over 60 px overprints the title. | Local truncate (R1 done) |
| S3 | `LyraTheme.cpp:50,336` and `LyraCarouselTheme.cpp` `maxValWidth` | `maxListValueWidth = 200` | Larger than the whole row text area (183 at 240). A value over about 170 px drives `rowTextWidth` <= 0, `truncatedText` returns "", and the title disappears. Only `ButtonRemapActivity` and Wi-Fi pass values, and remap's are short, so low exposure today. | Local: `min(200, rect.width/2)` |
| S3 | `LyraTheme.cpp:798` | `padding = 48`, no truncation | "No open book": en ends at 188, fine. de 270 px runs off. | Truncate (R1 done) |

## 4. Home

Default menu on Murphy is 5 rows (Library, Stats, File Transfer, Weather, Settings), 6 with bookmarks. `useWeather = 1` and all `show*OnHome = 1` (`CrossPointSettings.h:780-789`). Simulated from `computeHomeScreenLayout` + `drawButtonMenu`:

| Theme | Entries | Tile | Menu height | Row height |
|---|---|---|---|---|
| Lyra | 4 / 5 / 6 | 170 | 146 | 36 / 29 / 24 |
| Lyra 3 Covers | 4 / 5 / 6 | 200 | 116 | 29 / 23 / 19 |
| Classic | 4 / 5 / 6 | 280 | 54 | 13 / 10 / 9 |

| Sev | File:line | Constant / expression | What happens at 240x416 | Fix |
|---|---|---|---|---|
| S2 | `LyraTheme.cpp:606,687` | `coverWidth = coverHeight*0.6` = 92, `textWidth = 200-16-16-92` = 76 | Text column is x=136..212, 76 px: about 7 title chars a line. The status line is truncated since (`7db7d086d`). | Below a column minimum put the status and history under the cover or shrink the cover. R6 |
| S1 | `HomeActivity.cpp:44`, `BaseTheme::drawRecentBookCover` | `CLASSIC_MIN_RECENT_TILE_HEIGHT = 280` | Menu gets 54 px, rows 9-13 px with 20 px text: labels overprint each other and are unreadable. | Lower floor (R1 done). R6 |
| S3 | `Lyra3CoversTheme.h:56` | `historyRowHeight = 23` "FIT_SMALL line height" | On Murphy that face is 17 px: 6 px wasted, and the comment is wrong here. | Local: read the ladder |

## 5. FreeInkUI widgets as the firmware calls them

| Sev | File:line | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S2 | `list.h:458-484` (`measureListRow`), `UiListActivity.cpp:159` | content = 202-40 = 162, value width unbounded, `maxLines = 2` | Toggle rows leave 114 px for the label: 94 of the 254 strings referenced from `SettingsList.h` need a second line, 4 are ellipsized. A value row with a 100 px value leaves a 52 px label. A value over 152 px gives label width 0 and the value runs left out of its band (`list.h:894-899`). Not truncated. | SDK: cap value width at 60 % (R1 and R3 done) |
| S3 | `ConfirmDialog.h:94`, `option-dialog.h` padding 16, gap 8 | width = 4/5 of safe width | 192 px panel, 160 content, two 76 px buttons. en fine, de "Abbrechen" ellipsized. In L only the message gives up lines, so a 3-line headline with buttons can exceed 240. | Local: use 92 % width on compact |

## 6. Settings-side activities

| Sev | File:line | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S1 | `ReadingStatsBookDetailActivity.cpp:200` | cards stacked, only the spark is guarded | L (opened from the reader menu): body 135 px, summary 55 + time 104 already exceed it. History card off-panel. P: fits. | Local: paging |
| S2 | `ButtonActionsOverviewActivity.cpp:25` | `kButtonRows`, all 7 keys | Rows name keys Murphy does not have (it has Confirm, Up and Down). The layout fits since R8. | Local: list the board's keys |
| S3 | `GestureActionsOverviewActivity.cpp` (frame maths) | zones scaled from the screen | Scales correctly, but cell labels get about a third of a roughly 150 px frame, so they are clipped. Read only partly. | Accept |

## 7. Reader and its overlays

| Sev | File:line | Constant / expression | What happens at 240x416 / 416x240 | Fix |
|---|---|---|---|---|
| S1 | `XtcReaderActivity.cpp:268-269` | `min(pageWidth, displayHeight)`, `min(pageHeight, displayWidth)` | XTC/XTCH pages are pre-rendered bitmaps. A 480x800 page shows only its top-left 240x416, a quarter of the page, and the book cannot be read. Only files encoded for 240x416 work. Same for `drawCurrentPageToBuffer` (sleep overlay). | Local: downscale, or refuse with a message |
| S2 | `UITheme.cpp:254`, `LyraTheme.h:33` | status lane = 19+8+4 = 31 | L: 13 % of the height; viewport 204 px. The comment in `UiFontScale.h` warns that moving it invalidates pagination caches, which is no issue on a new board. | Central R1 (`statusBarVerticalMargin`, not capped yet) |
| S2 | `BaseTheme.cpp:1282-1283` | `titleMargin* = cluster + 30` | P defaults: left cluster 53 + 30, right cluster 71 + 30, leaves 230-83-101 = 46 px, about 7 chars of chapter title. With a left clock (+40) about 6 px, so the title is reduced to an ellipsis. No overlap, it truncates cleanly. | Local: drop the 30 on compact |
| S1 | `QrDisplayActivity.cpp:40-44`, `QrUtils.cpp:103-104` | `px = max(1, maxDim/size)` | L: bounds height 103. A payload above about version 21 (over 101 modules) is forced to 1 px and exceeds the bounds: it overlaps the header and is clipped at the bottom, so it will not scan. P: bounds 200, fits up to version 40 at 1 px; dense codes at 1 px are probably unscannable (S3). | Local: refuse or truncate payload so `size <= maxDim` |
| S3 | `MdReaderActivity.h:27,69`, `.cpp:667,737` | `MIN_COL_INNER_WIDTH = 24`, `LIST_INDENT = 20` | Tables with 7+ columns flatten at 230 px, handled deliberately. Third-level list indent is 60 of 230. | Accept |
| S3 | `SettingsList.h:407` | `screenMargin {5, 40, 5}` (now `screenMarginHorizontal`) | At 40 the column is 160 px: 10 chars at 14 pt. | Murphy: cap at 20 |
| S3 | `EpubReaderActivity.cpp:6053-6065` | hint box height 44, `hintY2 = +20` | Truncated-section hint fits on Murphy; numbers were tuned for a larger line height. | Accept |

## 8. Weather

| Sev | File:line | Constant / expression | What happens at 416x240 (forced) | Fix |
|---|---|---|---|---|
| S2 | `WeatherActivity.cpp:469` | `contentX = metrics.buttonHintsHeight` | Ignores the 8 px hint gap in `getContentRect`; panel touches the strip. | Local |

## 9. Assets, buffers, sizes

| Sev | File:line | Constant / expression | What happens | Fix |
|---|---|---|---|---|
| S3 | `SleepActivity.cpp:762-800` | custom sleep BMPs | Display-size covers are generated correctly. User BMPs made for 480x800 get scaled, i.e. a dithered 1-bit image resampled. | Accept |
| S3 | `GfxRenderer.cpp:2302-2303` | `drawIcon` portrait-only transform | L: `physY = 416 - w - x` is past 240 for most x, icon silently dropped. All boards, no current L caller with row icons. | Leave |

Two rows are gone since gray landed: the reader's AA planes now reach the panel, and `grayLevels()` of 4 is true. A build without `FREEINK_MURPHY_GRAY` hides the settings through `HalCapabilities::panelShowsGray()`.

**Trap for whoever tunes list rows.** `makeUiTarget` (`UiAppHelpers.h:12`) builds `GfxRendererTarget` with `hasTouch = false` (`FreeInkUIGfxRenderer.h:44`), so FreeInkUI lists size to 20 + 2x4 = 28 px rows with no gap, although `mappedInput.hasTouch()` is true. That is what makes Settings show about 8 rows. If someone passes `true`, `listTouchMinRowHeight = 56` plus gap 6 (`FreeInkUICore.h:656-658`) cut that to 3 rows, and `uiThemeTokens()` would need to override those tokens.

**The lent buffer is no longer Murphy's 12,480 B framebuffer.** Murphy lends a 256 KB PSRAM arena instead since #605, and every S3 board does since #614 (2026-11-05; `HalDisplay.cpp:375`, `404-416`).

## 10. QR constants left outside the two excluded files

- `QrUtils.cpp:103-104`: scale logic, see section 7.
- `QrDisplayActivity.cpp:39`: `truncNoticeHeight = 16`, fine.
- `CrossPointWebServerActivity.cpp:32-33` (`QR_CODE_WIDTH/HEIGHT = 198`) and its layout are the excluded file. Noted only.
- `CalibreConnectActivity` has no QR.
