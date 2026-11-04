# Murphy M3: gray

Four-level gray runs in `env:murphy_gray`, off everywhere else. How it was found and the measurements
are in [tools/murphy-gray-lab/README.md](../../tools/murphy-gray-lab/README.md). All of it comes from
one panel, at room temperature.

## Before it can be the default

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

Not measured as such: this is read from the Test 2 curves and the LUTs, and it may be why B/W UI
screens ghost under FAST, which is why their refresh counter came down.

FAST gives black to white and white to black the same 15 frames, but they are far from equal. VSH reaches
15 % in 4 frames and about 1 % in 8, while VSL needs 16 frames for 98 %, so at 15 white lands a few
points short and black is driven about twice as long as it needs. `WW` then rests, so nothing corrects
it. The recipe's 32 frames of VSL read 103 % of the clean white, so HALF does not reach full white either:
one HALF from black read 94-97 %, a second 96-99 %.

The timing matches the vendor's own LUT, which runs at +-15 V with VCOM near 0. The Murphy runs at
VSL -14.2 V and VCOM -1.6 V, nominally +16.6 V against -12.6 V across the ink.

Next: with the lab, measure the blank page after `t4-pages bw` for `KW` at 20 and 24 frames, for VSL at
0x3F, and for a small VSL touch on `WW` like the VSH one `KK` already has. Nominally 20 frames of VSL also
balances 15 of VSH in volt-frames, but the true centre is unknown with no factory VCOM.
