# X3 sleep-screen ghosting — observations and open hypotheses

**Status: cause established by test T0 and a code deduction; fix built, awaiting device
confirmation — see §6.** The earlier fix derived from code reading alone was flashed, made no
difference, and was reverted in full. This document separates what is *observed* from what is
*inferred*, so each step is chosen by evidence rather than by narrative.

Nothing below is a fix proposal. Section 5 lists candidate causes, each with the cheapest
test that would discriminate it.

---

## 1. The symptom

Sleeping from inside the EPUB reader shows a **text-shaped ghost** — the outline of the page
that was being read — over the sleep cover. Reproduces every time. Reported to exist on other
devices too, most visible on X3 (jens's unit is a UC8253, not a UC8279 — see
`docs/` notes on telling them apart from the `X3_DRF` vs `8279_gray_DRF` log tag).

---

## 2. Device observations (serial capture, 2026-09-23 20:47, X3)

Facts, in the order they appear:

| Line | What it establishes |
|---|---|
| `[ERS] BG work: ... B runs=0 ... borrow=0` | **No background borrow was active.** Background A had never run this session. |
| `[ACT] Exiting activity: EpubReader` → `[ACT] Entering activity: Sleep` | Ordinary reader → sleep transition. |
| `[FBUF] singleBufferFastDiff=0 (hasSecondary=1 ...)` | The secondary framebuffer was **resident** at reader exit. |
| `[FBUF] releaseSecondary -> 1 (hasSecondary=0 ...)` | The sleep cover path released it, as designed. |
| `[SLP] Rendering sleep cover: .../cover.bmp`, `bitmap 528 x 792, screen 528 x 792` | COVER mode, full-screen cover, drawn at 0,0. |
| `[SLP] Grayscale planes: absolute` | The **absolute** plane path ran, not the differential one. |
| `Wait complete: X3_DRF (485 ms)` then `X3_DRF (228 ms)` | Base push ≈485 ms, grayscale refresh ≈228 ms. |

**These timings are identical to the 2026-09-16 measurements** (485 ms base, 227 ms absolute).
Nothing unusual is happening: this is the ordinary path, behaving as it always has.

### What the log does NOT tell us

- **`redSynced=0` carries no information here.** It is `FreeInkDisplay`'s own `_redRamSynced`,
  and `displayBuffer()` only maintains it via `if (_panelSel != PanelSel::X3)`. On X3 it is
  never set true. It is *not* the `Uc8253X3Driver::_redRamSynced` that decides
  `cleanBaseNeeded`. I misread this once already; it is called out here so nobody repeats it.
- **Which bank the 485 ms base actually ran** — full sync, half scrub, or fast differential.
  `displayFinish()` logs one untagged ` X3_DRF` for all three.
- **Whether the last page used anti-aliasing.** The capture window starts ~3 s before the
  power press; the page render was earlier. No `[ERS] Deferred AA` line is in scope.

---

## 3. Code observations

These are facts about the code, established by reading it. They are **not** claims about the
cause. The right-hand column is what matters: most were not exercised in the captured run.

| # | Observation | Exercised in this scenario? |
|---|---|---|
| A | `cleanupGrayscaleWithPreviousBuffer()` falls back to `frameBuffer` when the secondary is released. Every caller is the tail or abort point of a grayscale plane pass, which leaves `frameBuffer` holding a **plane**. So the fallback is never a valid baseline. | **No.** Requires the secondary to be absent *during* a plane pass; the log shows `borrow=0`, `hasSecondary=1`, and the reader gates AA on `hasSecondaryBuffer()`. |
| B | `returnSecondaryBuffer()` / `reallocSecondaryBuffer()` reseed the secondary **from the write buffer**, so it can be resident and report resident while holding a frame that was never displayed. | **No.** No borrow/return occurred. |
| C | The pre-rendered next page lives in the **write** buffer, not the secondary. `EpubReaderActivity.cpp:1211` and `ActivityManager.cpp:697` both say the opposite. | Documentation defect only. |
| D | `ActivityManager::goToSleep()` performs no framebuffer preparation, so OVERLAY and QUICK_RESUME composite onto the previously displayed frame. | **No.** COVER mode calls `clearScreen()` before drawing. |
| E | `saveSleepFrameBuffer()` runs after the sleep screen's `displayBuffer()` has swapped, so it persists the other slot. | **No.** Quick Resume only. |
| F | `beginAbsoluteGrayPass()` and `displayGrayBuffer()` are the only display entry points that never call `consumeRefreshOverride()`. A reader-armed `enforceExitFullRefresh()` HALF is therefore **dropped** on the absolute gray path. | **Plausibly yes** — untested. |
| G | `beginAbsoluteGrayPass()` defaults its base fallback to `HALF_REFRESH`. | Yes, by default. |
| H | On X3, HALF loads the `_half` bank, documented as *"WW==BW, WB==BB → drive every pixel to target ignoring DTM1"* — a **single-pass drive to target**, not a clear-then-write. | Only if the base ran as HALF. |
| I | After a completed AA page, `cleanupGrayscaleBuffers()` rebases **both** DTM planes from the B/W frame and sets `_redRamSynced = true`. The glass physically holds that page *plus grey at every anti-aliased glyph edge*; the controller's baseline describes a pure B/W frame. | Yes, if AA ran on the last page. |
| J | `TxtReaderActivity`, `MdReaderActivity` and `XtcReaderActivity` never call `enforceExitFullRefresh()` on exit — every hit in those files is inside a submenu-launch path. Xtc runs real grayscale plane passes. | Unrelated to the EPUB→sleep case; a separate latent issue. |

**A, B, C, D, E and J are real defects worth fixing on their own merits. None of them explains
the observed symptom.** That distinction is the thing that was lost the first time round.

---

## 4. Withdrawn hypotheses

**H1 — "the plane pass poisoned the controller's DTM baseline" (A above).**
*Refuted by device.* The fix was built, flashed and changed nothing; the log then showed its
precondition never held (no borrow, secondary resident). A real bug, not this bug.

**H2 — "the gray base ran as a gentle differential against a stale baseline".**
*Withdrawn, not refuted.* The evidence I offered for it (`redSynced=0`) was a misreading of the
facade's flag. It remains untested — see §5.

---

## 5. Open hypotheses, cheapest discriminating test first

### T0 — Costs nothing, needs no new firmware: change the sleep screen mode

The device already on the bench can answer the biggest question. Set **Sleep screen = Dark**
(or Blank) and sleep from the reader again. That path is `clearScreen()` +
`displayBuffer(HALF_REFRESH)` — no grayscale, no planes, no cover.

- **Ghost still visible** → the grayscale path is innocent entirely. The cause is the HALF
  drive itself (observation H): a single-pass drive to target does not erase the previous
  image, and only a full, inverting refresh does. This would also explain "we already had it
  for other devices", since nothing about it is X3-specific.
- **Ghost gone** → the cause is inside the grayscale cover path, and H3/H4 below are next.

A second free variant: sleep onto the same cover **from the Home screen** rather than from
the reader. If the ghost only appears when coming from a reader page, the reader's final
state (observation I — greys on glass, B/W in the baseline) is implicated; if it appears from
Home too, it is not.

### H3 — the base push is a single-pass scrub, and that is simply not enough

`beginAbsoluteGrayPass()` defaults to HALF (G), and X3's HALF drives every pixel to target
without clearing (H). Nothing in the entire sleep path ever requests `FULL_REFRESH`.
*Test:* tag the bank in `displayFinish()` (`X3_DRF_full` / `_half` / `_fast`) so the log says
which ran. Diagnostic only, no behaviour change.

### H4 — the base runs as a differential against a baseline that is wrong at the glyph edges

`displayGrayscaleBase()` takes its gentle `preBwMid` path when `cleanBaseNeeded == false`,
which is exactly what `cleanupGrayscaleBuffers()` arranges (I).
*Test:* tag the two branches (`X3_GRAYBASE_clean` / `X3_GRAYBASE_diff`). Same diagnostic build
as H3.

### H5 — the dropped refresh override (F)

The reader arms a HALF that the absolute gray path never consumes.
*Test:* log `hasRefreshOverridePending()` at the top of the gray pass. Same diagnostic build.
Note this is only *interesting* if H3 shows the base was weaker than a HALF; if the base
already runs HALF or FULL, the dropped override changes nothing.

### H6 — the absolute grayscale nudge itself is too weak

Independent of the base: the 228 ms absolute pass may not have the drive to overwrite a text
page.
*Test:* open the same cover in the BMP viewer (which has its own BW/Gray toggle) from Home and
from the reader, and compare.

---

## 6. Result

**T0 ran: a Dark sleep screen from the same reading state shows no ghosting.** That is one
`_half` pass from the anti-aliased page — `clearScreen()` + `displayBuffer(HALF_REFRESH)` —
and it is clean. So a single `_half` clears the AA residue. **H3 is refuted.**

What that lets us deduce about the cover path, from the code alone:

1. The absolute gray pass (S6) cannot produce reader-text shapes. Its planes are derived from
   the cover, and `absoluteGc` idles WW and BB, so it never drives an endpoint pixel. **H6 is
   refuted.** Whatever text outline is on the cover was left by the base push (S4).
2. If S4 had taken the *clean* branch it would have run `display(cover, nullptr, HALF)` — the
   same `_half` T0 just showed clears the residue — followed by the `_aa_pre_bw_mid` settle
   with DTM1 == DTM2, which drives nothing anywhere strongly and cannot bring shapes back.
   A clean-branch cover would therefore be clean.
3. So S4 took the **differential** branch: `_aa_pre_bw_mid` as an old→new transition against
   DTM1 = the B/W reader page. An AA-edge pixel — grey on the glass, white in DTM1, white on
   the cover — lands in that bank's gentle WW cell, is not cleared, and the absolute pass
   then leaves it alone forever. **H4 is the mechanism.**
4. It took that branch because `cleanBaseNeeded` was false, and it was false because
   `cleanupGrayscaleBuffers()` sets `_redRamSynced = true` after every AA pass. That is a true
   statement about controller RAM and a false one about the glass — the third state in
   `display-baseline-audit-2026-09-23.md` §9, which the UC8253 driver did not track.

H5 (the dropped `enforceExitFullRefresh` override) remains a true fact about the facade and is
still worth closing on its own, but it is not what this fix rests on.

**The fix**, in the UC8253 driver only (the controller with evidence): a `_grayOnGlass` state,
set by the gray waveform, cleared only by a waveform that drives every pixel from a known
state (`_full`, `_half`, `grayscaleRevert`), explicitly *not* cleared by the RAM restore, and
folded into `cleanBaseNeeded`. The differential base is now taken only when the start state is
genuinely 1-bit. A host test in `run_uc8253_power.py` fails without the consuming term
(differential, 1 refresh) and passes with it (clean, 2).

Why `_half` and not a targeted correction of the known grey pixels: the correction needs the
grey mask alive at the *next* push (52,272 B on X3, against `free=42992` in the log), rests on
the unproven premise that `_fast`'s 4-frame BW cell clears grey where `_half`'s 6-frame cell
is what T0 proved, and would make every host-copy producer in the audit a mask-invalidation
site. The `_half` bank is the LUT header's own named tool for this ("scrub bank … to reset
after AA grayscale"). Decided 2026-09-23.

**Still to confirm on the device**, with the label trace now in the build: a cover sleep from
an AA page should log `X3_GRAYBASE_clean` and `X3_DRF_half` where the previous firmware would
have logged `X3_GRAYBASE_diff`, and the cover should be clean. A Dark sleep should log
`X3_DRF_half`, confirming T0 ran the bank this reasoning assumes. If the labels say `clean`
and the ghost persists, points 1–3 above are wrong somewhere and the search reopens.

The same omission exists, untouched, on UC8279d (X3 newer) and SSD1677 — see the audit §5.
Not changed here: no unit to test on, and no report.

## 8. Device verification plan (2026-09-23)

Hardware on the bench: X3 (UC8253), X4 (SSD1677), X4 Pro (UC8179 or UC8279 — the log says
which: `8179_DRF` vs `8279x4_DRF`), LilyGo T5S3. Builds: X3 and X4 share `-e default`; X4 Pro
is `-e x4pro`; T5S3 is `-e lilygo_t5s3`. For every test, send the ~40 serial lines before
`Entering deep sleep` plus a yes/no on the ghost. All `[SLP]`/`X3_*` lines are `LOG_DBG`.

### X3 — the fix under test

| # | Do | Expect in the log | Expect on the glass |
|---|---|---|---|
| T1 | Read 2–3 pages with AA on, then **Cover** sleep | `[SLP] Grayscale planes: absolute`, `Gray base: overridePending=1`, an `X3_DRF_half`, then `X3_GRAYBASE_clean` | Clean cover |
| T2 | Same reading state, **Dark** sleep | `X3_DRF_half` | Clean (confirms T0 ran the bank the reasoning assumes) |
| T3 | Just read: page turns, and past the periodic scrub | turns `X3_DRF_fast`; the scrub `X3_DRF_half`; **no** `X3_DRF_full` | No new flashes; sleep entry not noticeably slower — report the two `Wait complete` times |
| T4 | Open a grayscale BMP/JPEG in the viewer **from a reading session** | `X3_GRAYBASE_clean` (it is the other `beginAbsoluteGrayPass` consumer) | Clean |

If T1 logs `X3_GRAYBASE_diff`, the flag did not engage: check for an earlier `[ERS] Deferred AA`
line — if AA never ran on the last page there was nothing to clear and the test is void. If it
logs `clean` and the ghost is still there, §6 points 1–3 are wrong and the search reopens.

### X3 — the two device-independent open findings (one minute each)

| # | Do | Finding | Question |
|---|---|---|---|
| T5 | Sleep screen = **Overlay**; turn forward twice quickly, then sleep | D | Is the page under the overlay the one you were reading, or the previous one? |
| T6 | Sleep screen = **Quick Resume**; sleep, then wake | E | Does the restored frame during the wake show the moon icon? |

### The other boards — does the symptom exist there at all?

Same procedure as T1 on each. The controller code is unchanged on all three; only the
`[SLP] Gray base:` line is new.

| Board | Build | Prediction from the audit | Report |
|---|---|---|---|
| X4 (SSD1677) | `default` | Overlay branch pushes a `BYPASS_RED` HALF base before the planes → likely clean. If `_cfg.absoluteGrayscale` is set the absolute branch skips the base entirely → ghost plausible | ghost y/n, `[SLP] Grayscale planes:` line |
| X4 Pro | `x4pro` | Carries `_redriveAfterGray`; base behaviour depends on which silicon | ghost y/n, the DRF tag prefix, `[SLP]` lines |
| T5S3 | `lilygo_t5s3` | Cover goes through `displayGray8Canvas(FULL)` = clean bank → clean | ghost y/n |

A ghost on X4 or X4 Pro would mean the same class (the driver forgetting greys on the glass)
needs the same treatment there; a clean result closes those boards for this symptom.

## 7. Suggested next step (as written before T0, kept for the record)

Run **T0** first. It needs no build, no flash and no code change, and it splits the search in
half: grayscale path vs. refresh drive. Everything in §5 after it assumes T0's answer.

If a diagnostic build is wanted after that, H3/H4/H5 share one — three log labels, no
behaviour change — and it should be flashed and read *before* any fix is written.
