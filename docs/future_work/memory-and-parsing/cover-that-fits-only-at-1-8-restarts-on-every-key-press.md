# A cover that fits only at 1/8 restarts on every key press

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** When a cover's progressive JPEG fits in memory only at 1/8, `JpegThumbSession` refuses it and the one-shot `convertResized` draws it. That conversion is not sliced: a key press aborts it (`CooperativeAbort`), and the next attempt starts again from byte 0. Before `ProgressiveJpegDc` was retired, this case was sliced; a cover that fits at no scale was one-shot before too.

**Why it matters.** Input stays responsive, but a large cover appears only once the reader pauses for one whole conversion. Small covers still finish between key presses.

**Where.** `JpegToBmpConverter.cpp`: `convertResized` (`:369`, its abort poll `progressiveFullShouldAbort` at `:284`),
`JpegThumbSession::begin` (`:1095`), which returns no session when `chooseProgressiveShift` (`:785`) leaves the cover to
the resize (`:1111`); `ProgressiveJpeg::decodeResized` (`ProgressiveJpeg.cpp:1525`).

**Next step.** Make the resized decode steppable in `ProgressiveJpeg`: its resampler state in the session, about 60 lines. Then give `JpegThumbSession` a third mode, with the per-output setup shared with the other two, about 120-150 lines with tests.
