# LilyGo T5S3: the ED047TC2 waveform, ghosting and the gray state

Handover, 2026-11-17. The owner saw ghosting build up on the T5S3 when switching Settings tabs a few times (fast
refreshes); a half refresh (Home, then Settings again) wiped it. Display stack: [LilyGo T5S3 display
stack](../lilygo-t5s3-display-stack.md).

## No firmware regression behind the report

An audit compared the owner's earlier build (398b21b78, the charger-bolt fix) with master 29fd523a3 on the device
(serial test API, debug log): both ask the panel for the same refreshes in the same order with the same durations,
boot onto the same waveform (24 °C, 21 drive frames), and the waveform tables, `LgfxEpdDriver` and LovyanGFX are
unchanged between them. Light sleep and the panel power-off never run on the T5S3 (frontlight on GPIO11, no GT911 wake
pin; Boot Diagnostics: 0 s slept). The bench had driven the panel hard that afternoon (71 HALF and 52 FAST refreshes
in ten minutes); in the evening the same repro showed fainter ghosting.

## Waveform balance: branch, not merged

- SDK fork `fix/t5s3-waveform-balance` (from `witchhunt` ea9883e): c467d83 balances the clean bank and the fast-bank
  greys in `tools/gen_ed047tc2_waveform.py` (`clean_phases()`/`clean_rows()`, `fast_rows()`) and regenerates
  `ED047TC2Waveform.cpp`; a894cae makes the re-tag before a back-to-back HALF keep each pixel's level
  (`LgfxEpdDriver.cpp`, `normalizeForCleanBank()`). The wrong "charge neutral" comments are corrected.
- Firmware `fix/t5s3-waveform-balance` 312e58180 pins it and updates the display-stack doc. Not on `witchhunt`, no PR.
- Net drive per pixel, frames: a HALF on a pixel that keeps its level 0 (was −19 on black, −23 behind a clean push),
  a change at most 4 (LovyanGFX's fixed eraser spans +2..−2 and a destination-only column cannot see the old level);
  a fast-bank grey drive 0 (was +9/+10). Host check: `NetDrive` in `tools/test_gen_ed047tc2_waveform.py`, all seven
  temperature ranges; it fails on the old tables.
- Cost on the device (24 °C): HALF 2.81 → 2.96 s, HALF behind a clean push 3.02 → 3.09 s, a Settings tab change
  0.89 → 0.97 s, an AA page turn about +0.1 s. LUT rows 104 → 113 of 128 at worst.

**Owner's eye check (2026-11-17), balance build:** after 8 Settings tab switches the ghosting is very faint. On EPUB
page turns there is "a bit of pumping: text, then darker, then lighter". That is the new black lead-in of the fast
greys (9-10 frames toward black before the grey).

Next:
1. Try the lead-in that fits the existing 21 fast rows (6-7 frames, about +3 net per grey drive instead of 0): a
   smaller dark swing, no added FAST time. Or drop the grey lead-in and keep only the HALF balance, which is what
   the Settings screens use. Owner eye check on both, then decide what goes into a PR.
2. Long-term effect of the balance: days of use or the camera; not measured.
3. The darkest levels of the 16-level canvas now get up to 6 more white frames first; by the measured curves up to
   about 0.03 darker. Not measured.

## The tab-switch build-up itself (not built)

FAST drives a changing pixel 21 frames toward its new rail and leaves unchanged pixels alone. But LovyanGFX drives
pixel pairs: an unchanged pixel whose neighbour changes is driven again with its full rail column, one-sided ±21 per
FAST (about 12k pixels per tab switch along the changed text, by a model from screenshots, not measured). The first
FAST after a HALF also re-drives the whole screen, because each pixel is tagged with the bank that last drove it.

- **Hold levels:** the fast bank has 12 columns that never drive (levels 1-12 at 24 °C). The driver could write a hold
  level for every unchanged pixel and the real level only for the changed ones. Needs the previous true frame on the
  host (about 518 KB PSRAM, or the 1-bit frame plus planes), a compare pass per FAST, FAST frames in the graded write
  mode, and per-range hold levels from the generator. Side effect: a kept white nets +1 per HALF. The most likely fix
  for the build-up; benefit on the glass not measured.
- **A kick** (a short opposite pulse at the start of a change): balanced over a round trip and free at 24-27 °C, but
  the unchanged pair partners would flicker at glyph edges. The vendor DU has none.

## Gray state kept for this panel (review only, nothing removed)

| Item | Memory | Recommendation |
|---|---|---|
| 8-bit canvas `g_canvas` | 518,400 B PSRAM | Keep. A 4-bit canvas would save 259 KB; not worth it now |
| Planes `g_lsb`/`g_msb` and the reader's capture planes | 129,600 B PSRAM + 2 × 64.8 KB per render | Shrink later: the reader paints the canvas directly, −130 KB and two copies per page, same on the glass |
| LovyanGFX per-pair record | 259,200 B + 1,036,800 B PSRAM, LUT 50,688 B internal DMA | Keep. Half the LUT block (about 25 KB internal DMA) is LovyanGFX slack: a one-line upstream fix |
| 16-level gray8 canvas (sleep image, pictures) | shares `g_canvas` | Keep: without it covers dither and come out far too dark |
| Fast-bank AA greys (L13/L14) | none | Keep: single-push AA, one waveform per page |
| Two-pass grey after a HALF base | — | Check which master paths still take it (about 2 s, a second clean waveform) and use the single push |

## Device state

T5S3 (MAC DC:B4:D9:2B:EE:D4): app0 = the balance build 312e58180, app1 = master d81e79e07. The charger build that ran
before 2026-11-17 is no longer on the chip; its image and the slot switch script are in the maintainer's backup
folders (`2026-11-17-t5s3-popup/`, `2026-11-17-t5s3-ghost-audit/`). The owner's settings and card files are as found.
