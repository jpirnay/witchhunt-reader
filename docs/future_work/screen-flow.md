# Screen flow

Status: open items as of 2026-10-22.

## A screen that goes back by itself after its message

**Open.** "Font installed!", "Font installation failed" and other screens that end on a message wait for Back.

**Why it matters.** The maintainer wants a timed return shared by every such screen: one mechanism, not one per screen.

**Where.** `FontDownloadActivity` (the COMPLETE and ERROR states) and the other message-only states. Find them with
`grep -rn "STR_.*_DONE\|COMPLETE" src`.

**Next step.** A design that lists the screens first.

## A quick font menu in the reader

Without a bound key or a pinch, changing the font size means the reader menu and then the font selector. The stock X4
Pro firmware (XT Licorice) has a quick change menu in the reader, and with the same 2 to 2.5 s redraw it is far easier to
use. A reader who wants a size up or down should not have to go through Settings. User request, 2026-11-06.

**What exists.** Keys and gestures can be bound to `BTN_FONT_SIZE_SMALLER`, `BTN_FONT_SIZE_LARGER` and
`BTN_CYCLE_FONT_SIZE`, and a pinch changes the size by default (`CrossPointSettings.h:715-716`). `BTN_QUICK_OVERRIDES`
opens Quick overrides, whose Font Size row sets the book's size (`QuickOverridesActivity.cpp:117-145`).

**Done (2026-11-12).** Licorice's arrangement: reader bars over the page (Contents, Progress, Layout, More), opened by
Confirm, a centre tap or a tap on the status bar (`ReaderBarsActivity`). The Layout tab sets the font (the book's own
fonts as the first choice, Publisher Fonts' All Text), size, line spacing, margin, darkness, paragraph gap, alignment,
hyphenation and the book's style, with "This book only" sending them to the book's own settings or to the global ones;
they apply when the bars close. Margin, paragraph spacing and darkness have per-book overrides for it.

**Done (2026-11-12), live on the PSRAM boards** (`HalCapabilities::laysOutReaderLive`). The reader draws the bars
itself (`EpubReaderActivity::liveBars_`) and keeps running under them: each Layout change lays the chapter out again
(Background-C), the page lands under the sheet as soon as the build reaches its text, black and white (no AA, gray
canvas, pre-render or look-ahead while the bars are open), and a pass with no page to draw paints only the bars. X4 Pro,
2026-11-12: a size, spacing or margin never used for the chapter took 5 to 6.5 s until the build was done (the page
showed 3.6 to 4.2 s after the press), one used before 1.5 s.

**Done (2026-11-13), less waiting.** The bars' repaint lets the render lock go during its refresh, a change is applied
once the bars showing it are painted, and where the card is not on the panel's bus (`decodesThroughRefresh`) the build
runs through refreshes. X4 Pro, cold Size+ to 18 pt: the new value showed after 0.7 s (was 2.5 s), the page after about
3.0 s (about 4.3 s), the build ended after 4.25 s (6.2 s). A size used before now takes 2.05 s (was 1.7 s), because its
page waits behind the bars' refresh.

**Done (2026-11-13), page writes.** A page went to the card as some 150 to 220 writes of a few bytes, one card
command per 512 bytes. The build now gathers them in a heap buffer (2 KB on the C3s, 4 KB on the PSRAM boards) and
writes whole sectors (`Section::BuildState::pageWriter`). Same chapters built cold, parse time before and after: X3
1,961 -> 1,816 ms (37 pages), X4 Pro 2,100 -> 1,760 ms (48 pages); the section files are byte-identical.

**Done (2026-11-13), a layout already built in one refresh.** The bars remember the layouts this chapter has complete on
the card (the one they opened on, each build that ended, each page drawn from a complete chapter); a change back to one
goes in at once and its page comes with the bars in one refresh (`EpubReaderActivity::liveLayoutChangeIsBuilt`). X4 Pro:
1.94 s back to 1.54 s, the page on the panel 1.22 s after the press. A layout built in an earlier session is not known to
them and still shows the bars first. The whole series on the X4 Pro, press to idle: a new layout 5.6 s (#749) to 3.8 s,
one built before 1.5 s.

**Open.** Speed: the first build pass of a new layout starts only after the bars' refresh; the book's settings are saved
(about 0.2 s) before the font is looked up, also for a layout already built. The X3 and X4 keep laying the book out on
closing; going live there depends on a heap measurement with the sheet open during a rebuild. The Murphy held sideways has no room for the sheet; its Layout tab
opens the reader menu.
