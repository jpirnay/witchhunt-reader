# Screen flow

Status: open items as of 2026-10-22.

## A screen that goes back by itself after its message

**Open.** "Font installed!", "Font installation failed" and other screens that end on a message wait for Back.

**Why it matters.** The maintainer wants a timed return shared by every such screen: one mechanism, not one per screen.

**Where.** `FontDownloadActivity` (the COMPLETE and ERROR states) and the other message-only states. Find them with
`grep -rn "STR_.*_DONE\|COMPLETE" src`.

**Next step.** A design that lists the screens first.

## A quick font menu in the reader

Changing the font size means the reader menu and then the font selector. The stock X4 Pro firmware (XT Licorice) has a
quick change menu in the reader, and with the same 2 to 2.5 s redraw it is far easier to use. A reader who wants a size
up or down should not have to go through Settings. User request, 2026-11-06.
