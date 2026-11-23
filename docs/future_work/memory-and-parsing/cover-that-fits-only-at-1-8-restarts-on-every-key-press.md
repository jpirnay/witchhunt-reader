# A cover that fits only at 1/8 restarts on every key press

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** When a cover's progressive JPEG fits in memory only at 1/8, `JpegThumbSession` refuses it and the one-shot
`convertResized` draws it. That conversion is not sliced: a key press aborts it (`CooperativeAbort`), and the next
attempt starts again from byte 0. Before `ProgressiveJpegDc` was retired, this case was sliced; a cover that fits at no
scale was one-shot before too.

**Why it matters.** Input stays responsive, but a large cover appears only once the reader pauses for one whole
conversion. Small covers still finish between key presses. How often a cover takes this path is not measured. Read
from the code (triage spec §6): with the secondary buffer lent, the case needs a cover about 8,000 px wide or a starved
heap; on the S3 boards the region is 256 KB and it does not arise.

**Where.** `JpegToBmpConverter.cpp`: `convertResized` (its abort poll `progressiveFullShouldAbort`),
`JpegThumbSession::begin`, which returns no session when `chooseProgressiveShift` leaves the cover to the resize;
`ProgressiveJpeg::decodeResized` (`ProgressiveJpeg.cpp`).

**Next step (a device measurement).** On the X3, dev build, a library of covers 1,400 px wide and wider: open Home and
the file browser's cover view and count the covers that take `convertResized`. It has no log line of its own:
`logCoverReads` is called from it and from the sliced decodes alike, so the measurement adds one. Build the change only
if a cover takes that path on a normal open with the buffer lent; if it shows only with the buffer released, fix the
lend, not the decoder. The change: make the resized decode steppable in `ProgressiveJpeg`, its resampler state in the
session, about 60 lines; then give `JpegThumbSession` a third mode, with the per-output setup shared with the other
two, about 120-150 lines with tests.
