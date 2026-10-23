# Screen flow

Status: open items as of 2026-10-22.

## A screen that goes back by itself after its message

**Open.** "Font installed!", "Font installation failed" and other screens that end on a message wait for Back.

**Why it matters.** The maintainer wants a timed return shared by every such screen: one mechanism, not one per screen.

**Where.** `FontDownloadActivity` (the COMPLETE and ERROR states) and the other message-only states. Find them with
`grep -rn "STR_.*_DONE\|COMPLETE" src`.

**Next step.** A design that lists the screens first.
