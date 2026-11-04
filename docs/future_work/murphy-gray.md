# Murphy M3: gray

Murphy builds have four-level gray, through `-DFREEINK_MURPHY_GRAY` in `[murphy_board]`. How it was found
and the measurements are in [tools/murphy-gray-lab/README.md](../../tools/murphy-gray-lab/README.md). All
of it comes from one panel, at room temperature.

## Still open

**Long-term DC.** A fringe pixel does the ordinary cycle, white to black, a net-zero nudge, black to
white. Gray that stays black takes 15 frames toward black that nothing pays back, through the marks
`Uc8253MurphyDriver::writeOldPlane()` adds. A short session cannot show what thousands of pages do.
Next: read for some weeks, then compare a clean white and black with the shots kept from the lab.

**BMP viewer.** It uses the same gray pass as sleep covers, which work, but has not been run.

**Temperature.** The waveforms were measured around 20-25 °C. The OTP is blank, so the controller has
no temperature ranges to fall back on, and the driver does not read the sensor.

**Stored overrides.** A book with `textAntiAliasingOverride` 0 stays B/W whatever the setting says. Books
opened on a Murphy before gray may carry one, which is how the first reader test came out B/W.

## One refresh instead of two

The gray lands about 330 ms after the page. Four levels cannot be one refresh without a flash: a page
turn needs rest, to white, to black, light and dark, and a refresh has four LUT slots. Merging rest with
to white would drive the whole background one way on every page.

Three levels fit: rest, to white, to black, and one gray such as VSL 4, VSH 8, VSL 4, which nets to zero
and takes 16 frames, as long as FAST. The reader already has a one-push path for it
(`supportsGrayFrame()`). Text darkness Lighter shows roughly how one gray looks. Not built, not measured.

## UI ghosting

B/W UI screens ghost, which is why their refresh counter came down. FAST now gives black to white 20
frames instead of 15, as VSL needs about twice as long as VSH. On a real Settings list the band a selection
bar leaves went from 4.2 to 2.5 points below a row that never held it (`tools/murphy-gray-lab/ui_ghost.py`,
four moves each, one panel). Patches agree, and 24 frames overshoots on black that FAST drew. VSL at
-15 V barely helped, and a VSL touch on `WW` made things much worse.

**The refresh counter.** The driver no longer promotes FAST to a full refresh on its own, so the
setting means what it says, and "Never" means no periodic full refresh at all. It had capped every
setting at 9 while the log still said FAST. Ghosting over runs longer than 9 has not been measured.

**HALF may leave more than FAST.** The same band read 5.7 points below the untouched row after the next
HALF, on both FAST builds. Not like for like: by then the row had also been toggled four more times. But a
HALF from black reads 94-97 %, and its white gets the same 15 frames of VSL, so it may under-drive white in
the same way. If so, more HALFs do not clear this ghost, and `MURPHY_LUT_21_DEFAULT` is the place to look.

**DC.** 20 frames against 15 nets 5 toward white per round trip in frames, though nominally 20 x 12.6 V
about balances 15 x 16.6 V. The true centre is unknown with no factory VCOM.
