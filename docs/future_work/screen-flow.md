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

**Decided (2026-11-12).** Licorice's arrangement: reader bars over the page (Contents, Progress, Layout, More), opened
by Confirm, a centre tap or a tap on the status bar. The bars without Layout are `ReaderBarsActivity`.

**Open.** The Layout tab: a sheet above the tab bar with the font (including the book's own fonts as one entry, which is
Publisher Fonts' All Text), size, line spacing, margin, paragraph spacing, alignment, hyphenation, darkness and the
book's style, and a "This book only" switch that sends the changes to the book's own settings or to the global ones.
Changes apply when the sheet is left on every board first. The page laid out again live under the sheet comes after,
on the S3 boards: it needs the sheet to be part of the reader (a pushed screen stops the reader's loop, so nothing
could be rebuilt under it) and the gray pass kept off the sheet. Whether the X3 and X4 can have it too depends on a
heap measurement with the sheet open during a rebuild. Margin and paragraph spacing then need per-book overrides; both
are already in the section file's header, so no cache version changes for them.
