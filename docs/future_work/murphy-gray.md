# Murphy M3: gray

Murphy builds have four-level gray, through `-DFREEINK_MURPHY_GRAY` in `[murphy_board]`. How it was found
and the measurements are in [tools/murphy-gray-lab/README.md](../../tools/murphy-gray-lab/README.md). All
of it comes from one panel, at room temperature.

## Still open

**Long-term DC.** A fringe pixel does the ordinary cycle, white to black, a net-zero nudge, black to
white. Gray that stays black takes 15 frames toward black that nothing pays back, through the marks
`Uc8253MurphyDriver::writeOldPlane()` adds. A short session cannot show what thousands of pages do.
Next: read for some weeks, then compare a clean white and black with the shots kept from the lab.

**Temperature.** The waveforms were measured at room temperature, which was not recorded. The OTP is
blank, so the controller has no temperature ranges to fall back on, and the driver does not read the
sensor.

**Stored overrides.** A book with `textAntiAliasingOverride` 0 stays B/W whatever the setting says. Books
opened on a Murphy before gray may carry one, which is how the first reader test came out B/W.

## One refresh instead of two

The gray lands about 330 ms after the page. Four levels cannot be one refresh without a flash: a page
turn needs rest, to white, to black, light and dark, and a refresh has four LUT slots. Merging rest with
to white would drive the whole background one way on every page.

Three levels fit: rest, to white, to black, and one gray such as VSL 4, VSH 8, VSL 4, which nets to zero
and takes 16 frames, as long as FAST. The reader already has a one-push path for it
(`supportsGrayFrame()`). Text darkness Lighter shows roughly how one gray looks. Not built, not measured.

## Images

BMP, JPG and PNG all show in one gray pass, and Confirm's gray and B/W toggle works. Midtones come out dark:
the 4-level dither assumes light gray at 67 %, and ours is 50-58 %. Measured on an even step ramp, in percent
of white:

| Ramp | 14 | 29 | 43 | 57 | 71 | 86 |
|---|---|---|---|---|---|---|
| BMP, gray | 9 | 23 | 36 | 52 | 64 | 88 |
| JPG or PNG, gray | 7 | 20 | 30 | 41 | 55 | 73 |
| JPG, B/W | 3 | 21 | 40 | 57 | 77 | 92 |

JPG and PNG also read 8-15 points below BMP on a soft test image. This was left as it is. Light gray at 5
frames instead of 4 would be about 70 %, but it would change text as well. Not tried.

## UI ghosting

B/W UI screens ghost, which is why their refresh counter came down. FAST now gives black to white 20
frames instead of 15, as VSL needs about twice as long as VSH. On a real Settings list the band a selection
bar leaves went from 4.2 to 2.5 points below a row that never held it (`tools/murphy-gray-lab/ui_ghost.py`,
four moves each, one panel). Patches agree, and 24 frames overshoots on black that FAST drew. VSL at
-15 V barely helped, and a VSL touch on `WW` made things much worse.

**The refresh counter.** The driver no longer promotes FAST to a full refresh on its own, so the
setting means what it says, and "Never" means no periodic full refresh at all. It had capped every
setting at 9 while the log still said FAST. At 20 the UI and the reader looked fine by eye. Ghosting over
runs longer than 9 has not been measured.

**HALF stays as the manufacturer set it.** HALF leaves more ghost than FAST, and more VSL on `WW` in
`MURPHY_LUT_21_DEFAULT` would clean better at no cost in time, as its last 16 frames idle. But that whitens
the background, and every FAST after it then lands short of the new white. So the two have to be judged as
a pair. Points below an untouched patch, three interleaved `./lab.py t8` runs on a blank panel:

| HALF + FAST | After FAST | After HALF |
|---|---|---|
| **Unchanged + 20 frames** | **-0.5** | -2.4 |
| 5 extra + 20 | -1.8 | -1.5 |
| 10 extra + 20 | -2.2 | -1.1 |
| 10 extra + 25 | -1.3 | -1.3 |

FAST runs far more often than HALF, and on a real list 10 extra frames came out as a net loss, so HALF was
left alone. If the ghost after HALF ever matters more, 10 extra + 25 is the balanced choice, at 100 ms more
per FAST.

**DC.** 20 frames against 15 nets 5 toward white per round trip in frames, though nominally 20 x 12.6 V
about balances 15 x 16.6 V. The true centre is unknown with no factory VCOM.

## Upstream

Offered to Free-Ink/freeink-sdk, all open: gray as #174, the FAST change and the limit removal as #176 (stacked
on #174), and the lent block as #175. Until they merge, the firmware pins our fork. HALF was not offered, as it
did not change. #175 is not in use here: the firmware still lends its block from `HalDisplay`.
