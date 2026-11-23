# Screen flow

Status: open items as of 2026-11-23.

## A screen that goes back by itself after its message

**Open.** "Font installed!", "Font installation failed" and other screens that end on a message wait for Back.

**Why it matters.** The maintainer wants a timed return shared by every such screen: one mechanism, not one per screen.

**Where.** `FontDownloadActivity` (the COMPLETE and ERROR states) and the other message-only states. Find them with
`grep -rn "STR_.*_DONE\|COMPLETE" src`.

**Next step.** A design that lists the screens first.

## The reader bars: a layout change still waits

The reader bars and their live Layout tab shipped (#747-#764); what they do and why is in
[../design/reader-bars.md](../design/reader-bars.md).

**Open.**
- **The first build pass of a new layout starts only after the bars' refresh.** `serviceLiveBars()` applies a change
  once the bars showing it are painted (`liveLayoutOwed_`). Overlapping the two would start the build up to that
  refresh earlier (about 0.7 s on the X4 Pro, #761; inference, not tried).
- **The book's settings are saved before the font is looked up,** about 0.2 s, also for a layout already built:
  `applyReaderLayout()` saves the settings (`SETTINGS.saveToFile()`) or the book's overrides
  (`RECENT_BOOKS.setReaderOverrides()`) before `relayoutWithChangedSettings()` loads the font.
- **A Font step on the X4 Pro takes 4 to 15 s, by family,** live under the bars (the PSRAM boards do not defer the font,
  `HalCapabilities::defersLiveFontChange()`). Where that time goes is not measured.
- **The X4 has not run the live bars** on a device.

**Next step (proposed).** Profile a Font step on the X4 Pro, as the size changes were profiled for #751, before
choosing what to overlap.
