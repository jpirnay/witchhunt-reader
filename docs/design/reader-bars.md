# The reader bars

The reader bars are the reader's controls inside a book: two bands over the page, which stays readable between them.
This record says why they are shaped as they are and what the live layout under them costs. The code is in
`src/activities/reader/`: `ReaderBarsController` (state, input, drawing), `ReaderBarsLayout.h` and
`ReaderLayoutSheet.h` (the layout, FreeInkUI only), `ReaderLayoutControls.h`, `ReaderLayoutValues.h`, and the live
path in `EpubReaderActivity` (`liveBars_`, `serviceLiveBars()`, `applyLiveLayout()`). Open work is in
[../future_work/screen-flow.md](../future_work/screen-flow.md).

## Why

A user asked for a quick font change in the reader (#747). Without a bound key or a pinch, a size up or down meant
the reader menu and then the font selector. Keys and gestures could already be bound to `BTN_FONT_SIZE_SMALLER`,
`BTN_FONT_SIZE_LARGER` and `BTN_CYCLE_FONT_SIZE`, a pinch changes the size by default (`gestPinchIn`, `gestPinchOut`),
and `BTN_QUICK_OVERRIDES` opens Quick overrides with a Font Size row. The stock X4 Pro firmware (XT Licorice) has a
quick change menu over the page, and with the same 2 to 2.5 s redraw it is far easier to use. The bars follow its
arrangement, the idea only; none of its code.

## The bars (#747, #762)

- **Top bar:** back out of the book, the book's title (opens the book's details), the page's bookmark.
- **Tab bar,** an icon each: Navigation (the chapter list, the bookmarks, a printed page, leaving the book), Progress
  (where the book and the chapter stand, the time spent in the book, previous chapter / Go to % / next chapter, and
  KOReader sync where it is set up), Layout (below), Orientation (the four ways to hold it, and Auto-Rotate on a board
  with an IMU), More (the reader menu, so every feature stays one step away).
- **Opened** by Confirm, a centre tap and, on touch boards, a tap on the status bar (`TapZones::statusBandFor`); any
  key or gesture can be bound to them (`BTN_READER_BARS`). A key bound to the reader menu still opens the menu.
- **Keys reach everything on every board,** the X3 and X4 included. On touch boards the cursor stays hidden until a key
  is pressed.
- **Plain text, not `fui::header` / `settingRow`:** their battery chrome alone cost 2.5 KB of flash on the C3.
- **Three actions** for the bars and their panels, each value naming its target: FreeInkApp keeps 8 action handlers a
  screen and drops the rest without a sign. `test/reader_bars_layout` holds them to three and lays the bars out at
  every board's size, upright and sideways.
- **No copy of the page:** a C3 cannot spare 48 KB. The bars draw over the frame the reader prepares.

## The Layout tab (#748)

A sheet above the tab bar: font, size, line spacing, margin, darkness, paragraph gap, alignment, hyphenation, the
book's style, and **This book only**. On, the values are the book's own settings (`overrides.bin`, #746). Off, they are
the global settings, and the book drops its own, its font included. Margin, paragraph spacing and darkness gained
per-book overrides for it, appended to `overrides.bin`; margin and paragraph spacing were already in the section header,
so no `SECTION_FILE_VERSION` bump. The font cell goes round the fonts, the book's own first (Publisher Fonts' All
Text), then the built-in and the card's families: no second picker screen.

**Where it does not fit** (`ReaderBarsLayout::layoutFits()`, `panelFits()`). The sheet balances its rows as wide as
the screen allows, uses one-line cells where two lines are too tall, and covers the page where no strip of it would be
left. The Murphy held sideways (240 px high) has room for none of that: its Layout tab opens the reader menu instead.
On the same screen Progress puts its values on one line (`stacksRows()`) and KOReader sync stays in the reader menu
(`showsSyncRow()`). This is by design, not open work.

## The book laid out live under the bars (#749, #764)

The reader draws the bars itself (`EpubReaderActivity::liveBars_`) and keeps running under them. Each Layout change
lays the chapter out again (Background-C), and the page lands under the sheet as soon as the build reaches its text, in
black and white: no AA, gray canvas, pre-render, look-ahead or Indexing popup while the bars are open. A pass with no
page to draw paints only the bars, and only when they changed. Reader actions on keys and gestures go to the bars
(`Activity::takesReaderControls()`). Darkness only picks the gray levels, so it shows once the bars close.

The PSRAM boards did this first (#749). The C3s followed (#764) after a measurement on the X3, the binding board: a
cold Size+ (39 pages) with the sheet open bottomed out at 29.6 KB free against 32.2 KB for the same build after the bars
close, with 15 KB of ballast held; the build completed either way, with no failed allocation. Live costs about 2.6 KB at
the low point; the bars hold about 3.5 KB while open. The pushed bars screen of the first version went with it.

**The font waits on the C3s** (`HalCapabilities::defersLiveFontChange()`). Each Font step there copies the family into
the flash font area before the chapter is laid out again: a 3 MB erase (5.3 s) and the file (1.8 s for 608 KB), 10-12 s
a press with the build, and 3 MB of flash erased each time someone goes round the fonts. So a Font press only updates
the cell, which reads "Font \*"; the Size cell steps along that font's sizes; every other change is laid out live in
the font the book is in (`ReaderBarsController::liveLayoutValues()`); and when the bars close
`ReaderBarsResult::layoutChanged` carries the font, which the reader applies once. The PSRAM boards lay a new font out
at once.

## Less waiting (#751, #753, #761)

Profiled on the X4 Pro, a cold Size+ to 18 pt took 6.2 s to the end of the build, of which the build did 2.7 s of work.
The rest was waiting on the panel:

- **The bars' repaint** held the render lock through its 0.95 s refresh. It now triggers its refresh, lets the lock go
  and completes it afterwards, as a page does (`paintLiveBars()`).
- **The build skipped its slices** while a refresh was pending. Where the card is not on the panel's bus and the build's
  memory is PSRAM (`HalCapabilities::decodesThroughRefresh()`) it now runs through them.
- **The new value reached the panel late,** because the rebuild started before the bars were repainted. A change is now
  applied once the bars showing it are painted (`liveLayoutOwed_`); presses in between go in as one change, and closing
  the bars applies one still waiting.

X4 Pro, cold Size+ to 18 pt: the new value on the panel 2.5 -> 0.7 s after the press, the page about 4.3 -> 3.0 s, the
build's end 6.2 -> 4.25 s.

**A layout the chapter already has** came 0.4 s later after that change, its page waiting behind the bars' own refresh.
So the bars remember the layouts this chapter has complete on the card (`builtLiveLayouts_`, 8 entries): the one they
opened on, each build that ended under them, each page drawn from a complete chapter. A change back to one goes in at
once, and its page comes with the bars in one refresh (`liveLayoutChangeIsBuilt()`): X4 Pro 1.94 -> 1.54 s, the page on
the panel 1.22 s after the press. A wrong entry costs only that early repaint. A layout built in an earlier session is
not known and keeps the bars-first order.

**Page writes.** A chapter build wrote each page to the card in some 150 to 220 writes of a few bytes, one card command
per 512 bytes. It now gathers them and writes whole sectors (`Section::BuildState::pageWriter`, a `BufferedFileWriter`
of 2 KB on the C3s and 4 KB on the PSRAM boards; [../memory-allocation-strategy.md](../memory-allocation-strategy.md)).
The same chapters built cold: X3 1,961 -> 1,816 ms (37 pages), X4 Pro 2,100 -> 1,760 ms (48 pages); the section files
are byte-identical.

Over the series, X4 Pro, press to idle: a new layout 5.6 s (#749) -> 3.8 s; a layout built before 1.5 s.
