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

## FAST under-drives white

B/W UI screens ghost under FAST, which is why their refresh counter came down. FAST gives black to white
and white to black the same 15 frames, but they are far from equal: VSH reaches 15 % in 4 frames and
about 1 % in 8, while VSL needs 16 for 98 %. So black is driven twice as long as it needs, white lands
short, and `WW` rests, so nothing corrects it.

Measured with `lab.py t6`, one panel, production voltages. Ghost is a patch that went black and back,
minus a strip beside it that never changed, in points of the black-to-white range. Text is the line-scale
stddev of a blank page after five text pages, with a noise floor of 0.07.

| `KW` frames | Black from FAST | Black from a full refresh | After 60 s | Text | FAST waveform |
|---|---|---|---|---|---|
| 15, today | -0.7, -0.5 | -1.5, -2.4 | -2.3 | 0.42, 0.44 | 363 ms |
| 18 | | -0.8, -1.4 | | | |
| 20 | +0.7, then +0.3 | -0.5, -1.0 | -0.8 | 0.22 | 463 ms |
| 24 | +1.2, then +0.5 | 0.0, -0.6 | | 0.18 | 543 ms |

Black from a full refresh lifts less, so it is the case that matters: UI black is drawn that way.

20 frames looks like the right trade: under a point of ghost either way, for 100 ms. VSL at -15 V alone
barely helped (text 0.34), and with 20 frames it added nothing. A VSL touch on `WW` made things much worse
(-3.4 to -4.9): it keeps whitening the background, which raises the contrast with anything that moved.

Not done. `MURPHY_LUT_22_FAST` would go to 20 frames, with the VCOM LUT stretched to match. Then measure
real UI screens, and see whether their refresh counter can go back up. 20 frames against 15 nets 5 toward
white per round trip in frames, though nominally 20 x 12.6 V about balances 15 x 16.6 V. The true centre
is unknown with no factory VCOM.
