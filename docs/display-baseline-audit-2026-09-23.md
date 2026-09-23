# Where the "previous frame" lives — per-controller baseline audit (2026-09-23)

Companion to `docs/x3-sleep-ghosting-hypotheses.md` (the symptom and the open hypotheses).
This document answers the question that should have been answered before any fix was
attempted: **for each controller this firmware drives, where does the differential baseline
physically live, what does each refresh mode do with it, and which paths can leave it
describing something other than what is on the glass?**

Everything here is read from the drivers and the facade, with file references. Where a
conclusion depends on waveform physics rather than code, it says so and stops.

Controllers covered — the six this repo builds for:

| Board | Controller | Driver |
|---|---|---|
| Xteink X3 (original) | UC8253 | `Uc8253X3Driver` — jens's unit; logs `X3_DRF` |
| Xteink X3 (newer batch) | UC8279d | `Uc8279Driver` — logs `8279_DRF` |
| Xteink X4 | SSD1677 | `Ssd1677Driver` |
| X4 Pro | UC8179 | `Uc8179Driver` |
| X4 Pro (variant) | UC8279 800×480 | `Uc8279X4Driver` |
| LilyGo T5S3 | ED047TC2 via LovyanGFX | `LgfxEpdDriver` |

---

## 1. Vocabulary — four things that all get called "the previous frame"

They are different objects, and the bug reports in this repo have repeatedly conflated
two of them.

| Name | What it is | Who owns it |
|---|---|---|
| **Glass** | The physical particle state. The only truth. After an anti-aliased page it holds grey at every glyph edge. | Physics |
| **Controller baseline** | What the controller diffs a new frame against. 1-bit on every part except the T5S3. | Controller RAM (UltraChip DTM1, SSD1677 RED) or the panel library (LGFX `Panel_EPD`) |
| **Host copy** (`frameBufferActive`) | The facade's second framebuffer, holding the last frame the host *pushed*. | `FreeInkDisplay` |
| **Advisory flags** | Bits that claim the baseline is in sync: facade `_redRamSynced`; driver `_redRamSynced` / `_oldPlaneValid` / `_inGrayscaleMode` / `_needFullClear` / `_redriveAfterGray` / `_grayState.lsbValid`. | Facade and each driver separately |

The single most important fact in this document: **the host copy is the controller baseline
on exactly one of the six controllers** (SSD1677, via `prev`). The other five ignore `prev`
and keep their own. Every firmware path that borrows, lends, releases or reseeds the
secondary buffer moves the *host copy* only.

---

## 2. Per controller: baseline location, what each mode does, what grayscale leaves behind

### 2.1 UC8253 (X3, original) — `Uc8253X3Driver.cpp`

| | |
|---|---|
| Baseline | Controller **DTM1** ("old" plane). `prev` is `(void)` (`:187`). |
| Post-refresh sync | `displayFinish()` writes DTM1 ← `fb` after every refresh (`:296`), then a settle pass after a full sync. |
| FAST | `_fast` bank, differential against DTM1. |
| HALF | `_half` bank: **WW==BW and WB==BB** — every pixel is driven toward its *target* regardless of DTM1. Same short timing as the page-turn bank. Not an inverting clear. (`Luts.h:38`) |
| FULL | DTM1 ← white, `_full` OEM bank, long timing (`0x18/0x04/0x0E/0x0A` vs the others' `0x06/0x01/0x06/0x06`). Absolute-from-white. |
| Which runs | `doFullSync = (!fast && !half) \|\| !_redRamSynced \|\| _initialFullSyncsRemaining > 0 \|\| _forceFullSyncNext` (`:207`). Half only if not promoted. |
| Grayscale overlay (reader AA) | LSB → DTM1, MSB → DTM2 (`copyGrayscaleLsb/Msb`), `_gc` nudge bank. Afterwards `_inGrayscaleMode = true`, `_redRamSynced = false`, `lsbValid = false` (`:513-534`). |
| Grayscale absolute (sleep cover) | `beginGrayscale(Absolute)` **first runs `displayGrayscaleBase()` — a real B/W base push** — then planes go out under `absoluteGc = {gc.vcom, ww←gc.bb, bw←gc.bw, wb←gc.ww, bb←gc.bb}` (`:508`). With `gc.bb`'s lead byte `0x00`: **WW and BB idle.** Under (DTM1,DTM2) 11=white / 00=black / 10=dark / 01=light, white and black pixels receive no drive. The driver's own comment: *"The B/W base already supplies the endpoints … idle both endpoints."* |
| `displayGrayscaleBase` | If `_inGrayscaleMode` → `grayscaleRevert()` (both planes white + `_half` → a visible flash to white). Then `cleanBaseNeeded = !_redRamSynced \|\| lsbValid \|\| _forceFullSyncNext \|\| _initialFullSyncsRemaining > 0` (`:370`). **True** → `display(fb, nullptr, fallback)` + `_aa_pre_bw_mid` settle. **False** → DTM2 ← fb, `_aa_pre_bw_mid` **differential against DTM1**, then DTM1 ← fb. |
| `_aa_pre_bw_mid` drives | WW `0x20` (gentle), BW `0xAA`, WB `0x55`, BB `0x10` (gentle). OEM comment: *"With DTM1 == DTM2 == displayed BW frame, all pixels sit in WW/BB and get a gentle settle."* (`Luts.h:133-165`) |
| `cleanupGrayscaleBuffers(bw)` | DTM2 ← bw, DTM1 ← bw; `lsbValid=false`, **`_redRamSynced=true`**, `_inGrayscaleMode=false` (`:536-566`). |
| `cleanupGrayscaleBuffers(nullptr)` | `lsbValid=false`, `_redRamSynced=false` → next push is `_full`. `_inGrayscaleMode` untouched. |

**Steady-state consequence.** After a completed AA page the driver's state is *"in sync,
B/W"* (`_redRamSynced=true`, `_inGrayscaleMode=false`) while the glass carries grey at every
AA edge. `_redRamSynced` is false only for the instant between `displayGray()` and
`cleanupGrayscaleBuffers()` — never at a moment the reader pushes anything. The reader never
requests `FULL_REFRESH`; its periodic scrub is `HALF`. **So the `_full` bank is unreachable
in normal reading on this controller.** Every page turn is `_fast`, every scrub is `_half`,
and the AA greys are never driven from-white.

### 2.2 UC8279d (X3, newer batch) — `Uc8279Driver.cpp`

| | |
|---|---|
| Baseline | DTM1, synced in `displayFinish()` inside the PTL window (`:203-222`). `prev` ignored. |
| FAST | DU bank against real DTM1, only with `_oldPlaneValid`. |
| HALF / FULL | **GC bank against the real previous frame** — not white-seeded. The driver comment records that a white seed *caused* ghosting (old==new==white → WW → no drive). |
| Grayscale | XTF_AA nudge (overlay) or **XTH4** (absolute — the long stock waveform). Afterwards `_oldPlaneValid=false` (planes overwrote both DTMs), `_forceFullSyncNext = absolute`, `_inGrayscaleMode = !absolute` (`:318-348`). |
| `displayGrayscaleBase` | Same two-branch shape as UC8253: revert if in gray mode; `cleanBaseNeeded = !_oldPlaneValid \|\| _lsbValid \|\| _forceFullSyncNext \|\| _initialFullsRemaining > 0`. |
| `cleanup(bw)` | Both planes ← bw, `_oldPlaneValid=true`, `_inGrayscaleMode=false`. |
| `cleanup(nullptr)` | `_lsbValid=false`, `_oldPlaneValid=false`, `_forceFullSyncNext=true`. |

**The two X3 controllers disagree about what HALF is.** UC8253's HALF is a single-pass
target drive that ignores the old frame; UC8279's is a GC diff against the real old frame.
Anything tuned on one does not automatically transfer to the other.

### 2.3 SSD1677 (X4) — `Ssd1677Driver.cpp`

| | |
|---|---|
| Baseline | **RED RAM, host-managed.** The one controller that consumes `prev`. |
| FAST | BW ← fb; RED ← `prev` (dual-buffer) or retained (single-buffer). |
| HALF / FULL | `CTRL1_BYPASS_RED`; both planes ← fb. Absolute. Stock's only clean primitive is the single-pass HALF `0xD7` (`:411-440`). |
| Post-refresh | Blocking single-buffer only: BW,RED ← fb again (`:478-487`). **The async `displayStart` path skips this.** |
| `seedPreviousFrame` | RED ← buf, no refresh (`:548`). This is what `syncRedRamFromFrameBuffer()` reaches. |
| Grayscale | **LSB → BW RAM, MSB → RED RAM** (`:581-591`). After an AA pass RED physically holds the MSB plane. `displayGray` sets `_inGrayscaleMode = !factoryMode`. |
| `cleanup(bw)` | RED ← bw, `_inGrayscaleMode=false`. |
| `cleanup(nullptr)` | No-op → `_inGrayscaleMode` stays true → next `displayImpl` promotes FAST → HALF (`:445-452`). |
| Absolute (if `_cfg.absoluteGrayscale`) | `GrayscaleBase::Combined` — **no base push at all**; `_needsGrayClear=true` afterwards promotes the next FAST. |
| Boot/wake | `_needsInitialFull` promotes the first FAST. |

### 2.4 UC8179 (X4 Pro) — `Uc8179Driver.cpp`

| | |
|---|---|
| Baseline | DTM1 = OLD plane, synced in `displayFinish()` (`:452-476`). `prev` ignored. |
| FAST | DU partial against real OLD, only if `_oldPlaneValid && !_needFullClear`. |
| HALF | **Charge scrub: OLD ← ~target**, GC bank — every pixel is forced through a transition cell (`:365-395`). |
| FULL | OLD ← white, GC. |
| Grayscale | `_grayBase` (PSRAM) snapshots the B/W base at `displayStart`. After `displayGray`, both planes are restored from `_grayBase` and **`_redriveAfterGray = true`**: the next FAST `display()` is intercepted into `transitionGrayscaleBase()` (XTF_PRE_BW_MID, non-flashing) instead of a DU (`:236-247`, `:740-766`). |
| `cleanup(bw)` | Returns early if `_bwPlanesSynced && _oldPlaneValid` (already restored from `_grayBase`); else both planes ← bw. |
| `cleanup(nullptr)` | `_needFullClear=true`, `_oldPlaneValid=false`. |

### 2.5 UC8279 800×480 (X4 Pro variant) — `Uc8279X4Driver.cpp`

Same model as UC8179. Difference: `_redriveAfterGray` makes the next FAST seed OLD ← ~target
(a DU scrub) rather than intercept (`:420-447`). `cleanup(bw)` writes DTM1 only.
`cleanup(nullptr)` → `_needFullClear=true`, `_oldPlaneValid=false`.

### 2.6 ED047TC2 / LovyanGFX (T5S3) — `LgfxEpdDriver.cpp`

| | |
|---|---|
| Baseline | **Inside LovyanGFX**: `Panel_EPD`'s own 4-bpp buffer + step buffer. The host-visible object is an 8-bit PSRAM canvas rebuilt from `fb` by `fillCanvasBW()` on *every* push (`:240`, `:763`). `prev` ignored. |
| FULL / HALF | `epd_text` clean bank + `normalizeForCleanBank()` (`:201-209`). |
| FAST | `epd_fast` differential. **Every** push — B/W included — goes through the same graded canvas, which is why a B/W page leaves the canvas holding the page exactly as a grey one does (`EpubReaderActivity.cpp:691-697`). |
| Grayscale two-push | `overlayCanvasGray()` on the canvas the base left, refreshed under the base's mode (`:902-925`). |
| Grayscale single-push | `displayGrayFrame()`: base + greys in one push. The facade deliberately does **not** swap afterwards (`FreeInkDisplay.cpp:838-850`). |
| Native gray8 | `borrowGray8Canvas()` / `displayGray8Canvas(FULL)` — the sleep cover uses this. |
| `cleanup(bw)` | `fillCanvasBW(bw)` — **the canvas is the next diff source**, so a wrong `bw` here drives wrong pixels one refresh later. This is 817be7759. |
| `cleanup(nullptr)` | No-op; the next push rebuilds the canvas from `fb` anyway. |

---

## 3. The facade layer — what `frameBufferActive` is and is not

`FreeInkDisplay.cpp` unless noted.

- Dual-buffer swap after `displayBuffer` / `triggerDisplay` / `displayAsyncImpl`
  (`:342-352`). `prev = frameBufferActive` (`consumePrevFrameFor`, `:513`) — consumed by
  **SSD1677 only**.
- `_redRamSynced` (facade, `:496`): advisory, X4-only. `isRedRamSynced()` hard-returns false
  on X3 (`.h:242`). **The `[FBUF] … redSynced=N` log field is this flag**, and on X3 it is
  always 0. It says nothing about `Uc8253X3Driver::_redRamSynced`.
- `_redBaselineAuthoritative` one-shot (`:475-489`): armed by `reallocSecondaryBuffer()` and
  `returnSecondaryBuffer()`; the next FAST passes `prev=nullptr` so SSD1677 diffs against
  retained RED rather than the unproven reseed.
- `_singleBufferFastDiff`: without it, FAST → HALF while the secondary is away, X4 only
  (`resolveReleasedMode`).
- `syncPendingAsync()` → `displayFinish(frameBufferActive ? frameBufferActive : frameBuffer)`
  (`:576`). Matters for the four UltraChip drivers whose finish writes DTM1 from that pointer.
- `displayGrayBuffer()` hands `frameBuffer` to the driver (`:895`) — at that moment it holds
  the last plane. SSD1677, UC8253, UC8279 `(void)fb`; LGFX overlays its canvas instead;
  UC8179/X4 use `_grayBase`. The HAL prepends an LGFX-only `syncWriteBufferFromActive()`
  that its own comment marks as possibly dead (`HalDisplay.cpp:489-524`).
- `cleanupGrayscaleWithPreviousBuffer()` (dee9eebf, current):
  `baseline = frameBufferActive ? frameBufferActive : (_secondaryLent ? nullptr : frameBuffer)`.
- `returnSecondaryBuffer()` / `reallocSecondaryBuffer()` reseed the restored secondary **from
  the write buffer** (`:551`, `:504`) — the facade's own comment calls this "UNPROVEN".
- `lendBuildStorage()` lends the *primary* (`frameBuffer = nullptr`, `:434-445`).
  `releaseBuffers()` frees both.

---

## 4. Every firmware path that moves the host copy

| # | Site | Action | Controller baseline touched? |
|---|---|---|---|
| 1 | `HomeActivity.cpp:231` cover loading | release + `setSingleBufferFastDiff(true)`; realloc at `:627`, explicitly **no** RED reseed | No |
| 2 | `ReaderActivity.cpp:835` first-open indexing | release after popup + fastDiff; realloc `:847` | No |
| 3 | `EpubReaderActivity.cpp:3796-3818` Background-C | `syncRedRamFromFrameBuffer()` (X4), then **borrow** (fallback release), fastDiff, `secondaryBufferDegraded_` | **X4 RED only** |
| 4 | `EpubReaderActivity.cpp:3492/3517` blocking section build | release (or in-place); realloc `:3564`, explicitly no RED reseed | No |
| 5 | `EpubReaderActivity.cpp:1246` Background-B look-ahead | **borrow**, deliberately no RED seed (the write buffer may hold the pre-rendered next page) | No |
| 6 | `EpubReaderActivity.cpp:4457-4469` image warm pass | borrow (fallback release) *inside a render*; returned after `clearScreen()` (`:4522`). `aaEnabledForThisRender` was computed before it (`:4402`) | No |
| 7 | `EpubReaderActivity.cpp:739-761` `onExit` | return C's borrow / realloc C's release; `setSingleBufferFastDiff(false)` | No |
| 8 | `NetworkMemoryTrim.cpp` | return-if-lent, RED seed (X4), release, fastDiff; no realloc (session reboots) | X4 RED only |
| 9 | `SerialTransferActivity.cpp:69` | release + fastDiff under low heap | No |
| 10 | `CrossPointWebServerActivity.cpp:303` | `releaseFrameBuffers()` — both | No |
| 11 | `EpubReaderActivity.cpp:4331` pre-reboot warm | `releaseFrameBuffersWithScratch()` | No |
| 12 | `SleepActivity.cpp:980` cover | release; no realloc (deep sleep follows) | No |
| 13 | `return`/`realloc` themselves | reseed host copy from write buffer; arm `_redBaselineAuthoritative` | No |

**On X3 and T5S3, none of these thirteen paths alters what the controller diffs against.**
They alter what the host *believes* is on the glass — which is what every partial repaint,
overlay, screenshot and Quick-Resume save reads. That distinction is the one the release-site
comments ("baseline in controller, no display downside on X3") get right about the panel and
silent about the host.

---

## 5. What a grayscale pass leaves behind, per controller

After the reader's AA pass on page N, in every case: **glass = page N + grey at every AA
edge.** What each controller *believes*:

| Controller | Baseline after cleanup | Flags after cleanup | Remembers grey is on the glass? |
|---|---|---|---|
| UC8253 | DTM1 = DTM2 = B/W page N | `_redRamSynced=true`, `_inGrayscaleMode=false`, `lsbValid=false` | **No** — until this branch; now `_grayOnGlass` (see below) |
| UC8279 | DTM1 = DTM2 = B/W page N | `_oldPlaneValid=true`, `_inGrayscaleMode=false` | **No** |
| SSD1677 | RED = B/W page N | `_inGrayscaleMode=false` | **No** |
| UC8179 | both planes = `_grayBase` (B/W page N) | `_redriveAfterGray=true` | **Yes** — next FAST becomes XTF_PRE_BW_MID |
| UC8279X4 | DTM1 = `_grayBase` | `_redriveAfterGray=true` | **Yes** — next FAST seeds OLD = ~target |
| LGFX | canvas = B/W page N (`fillCanvasBW`); Panel_EPD's step buffer = what it pushed | — | The panel library knows what it pushed; the host canvas does not |

**Only the two UltraChip X4-Pro drivers carried a flag that means "the glass has grey the
baseline does not describe."** UC8253, UC8279 and SSD1677 declared a clean B/W sync the moment
`cleanupGrayscaleBuffers()` ran. Whether that omission is *visible* depends on whether the
next push's waveform can move a grey pixel to its rail — a LUT question this audit could not
answer from code, and which test T0 then answered for UC8253: one `_half` clears it, the
differential `_aa_pre_bw_mid` transition does not. UC8253 now tracks the state as
`_grayOnGlass`, device-confirmed 2026-09-23 (`x3-sleep-ghosting-hypotheses.md` §6). UC8279 and
SSD1677 still do not.

---

## 6. The reader → sleep-cover sequence as state transitions

Anchored on the captured X3 UC8253 log (`x3-sleep-ghosting-hypotheses.md` §2). Columns:
glass / DTM1 / DTM2 / host write buffer / host secondary / driver flags.

| Step | Glass | DTM1 | DTM2 | write buf | secondary | Driver (UC8253) |
|---|---|---|---|---|---|---|
| S0 page N shown FAST after AA + cleanup | N + greys | B/W N | B/W N | page N−1 (post-swap) | B/W N | `_redRamSynced=1`, `_inGray=0`, `lsbValid=0` |
| S1 `onExit` | — | — | — | N−1, or N if a pre-render was pending | — | facade: HALF override armed (`enforceExitFullRefresh`) |
| S2 `SleepActivity`: HAL `releaseSecondaryBuffer()` | — | — | — | ← secondary (HAL syncs first) = B/W N | **freed** | untouched |
| S3 `clearScreen()`, draw cover 1-bit | — | — | — | cover B/W | — | — |
| S4 `beginAbsoluteGrayPass()` (fallback HALF; **override not consumed**) → `displayGrayscaleBase` | see below | | | | | `_inGray=0` → no revert; `cleanBaseNeeded = !1 ∨ 0 ∨ 0 ∨ 0` = **false in steady state** |
| S4-diff (steady state) | cover (via `_aa_pre_bw_mid` diff vs B/W N) | cover | cover | cover | — | `_redRamSynced=1` |
| S4-clean (only if something had dropped `_redRamSynced`) | cover (via `_full` from white, since `!_redRamSynced` forces `doFullSync`) | cover | cover | cover | — | `_redRamSynced=1` |
| S5 planes | — | LSB(cover) | MSB(cover) | MSB plane | — | `lsbValid=1` |
| S6 `displayGrayBuffer()` → `absoluteGc` | cover + greys; **white/black pixels not driven** | | | | | `_inGray=1`, `_redRamSynced=0` |
| S7 deep sleep | | | | | | chip reset |

Two things this table settles from code alone:

1. **The base push in S4 is not a from-white drive in steady state.** The steady-state branch
   is the gentle `_aa_pre_bw_mid` differential; even the other branch, entered only when
   `_redRamSynced` is false, would run `_full` only because that flag is false — the HALF
   fallback itself would be the single-pass `_half`. The reader-armed HALF override is
   dropped unconsumed either way.
2. **S6 cannot repair anything S4 left.** The absolute gray pass idles WW and BB. Whatever
   the base push left at the endpoints stays.

What each S4 branch does to a pixel that is *grey on the glass, white in DTM1* (an AA edge
of page N) whose cover target is white: diff branch → WW `0x20`, gentle, short; clean branch
→ `_half` WW==BW `0xAA`, short. Neither is `_full`'s long from-white drive. **Whether either
leaves that pixel visibly grey is physics, not code**, and is exactly what test T0 in the
hypotheses document isolates (Dark sleep screen = `clearScreen` + HALF = `_half` alone).

Which S4 branch the captured sleep took is **unknown**: the log's `redSynced=0` is the
facade flag (§3), and `displayFinish` logs one untagged `X3_DRF` for all three banks. A
label-only trace answers it in one flash.

The same sequence elsewhere, briefly:

- **UC8279 X3**: identical structure; its clean-branch HALF is a GC diff (stronger), and its
  absolute pass is XTH4 with `_forceFullSyncNext` afterwards.
- **SSD1677**: takes the absolute branch only with `_cfg.absoluteGrayscale`; otherwise the
  overlay branch runs `triggerDisplayAsync(HALF)` = `BYPASS_RED` single pass, then planes to
  BW/RED RAM, then the custom-LUT gray refresh.
- **T5S3**: `nativeGray` branch — paint the cover into the borrowed gray8 canvas,
  `displayGray8Canvas(FULL)` → `epd_text` clean bank. The reader's last page went out
  single-push, so no plane-pass cleanup ever ran there; 817be7759 affects only the two-push
  fallback on that board.

---

## 7. Cross-controller findings (facts, not causes)

- **F1.** `prev` is consumed by one driver in six. On the other five the host copy is never
  the differential baseline. The firmware's thirteen borrow/release/realloc paths therefore
  change what the host believes, not what the controller diffs against (§4).
- **F2.** Only UC8179 and UC8279X4 remember that greys are on the glass after an AA pass
  (`_redriveAfterGray`). UC8253, UC8279 and SSD1677 report a clean B/W sync after cleanup;
  LGFX rebuilds its canvas from a B/W frame (§5).
- **F3.** UC8253's absolute gray pass drives dark/light only; WW and BB idle (§2.1). It can
  add greys to a base; it cannot remove what the base left.
- **F4.** `beginAbsoluteGrayPass()` defaults its base to HALF and, like `displayGrayBuffer()`,
  never calls `consumeRefreshOverride()`. The reader-armed HALF is dropped on the sleep-cover
  path. On UC8253 the `_full` bank is unreachable in normal reading (§2.1).
- **F5.** HALF means different things on the two X3 controllers: single-pass target drive
  (UC8253) vs GC diff against the real old frame (UC8279). Fixes do not transfer by name.
- **F6.** `cleanupGrayscaleBuffers(nullptr)` is honoured everywhere but differently: UC8253
  drops `_redRamSynced` (→ `_full`); UC8279 sets `_forceFullSyncNext`; UC8179/X4 set
  `_needFullClear`; SSD1677 leaves `_inGrayscaleMode` (→ FAST promoted to HALF); LGFX no-op.
- **F7.** The facade's `_redRamSynced` is X4-only and the FBUF log prints it on X3 as
  `redSynced=0` unconditionally. Already misread once in this investigation.
- **F8.** `displayGrayBuffer()` hands every driver a buffer that holds a plane. Four ignore
  it; LGFX overlays; UC8179/X4 use their snapshot. The HAL's LGFX-only reseed before it is
  documented as a deletion candidate.
- **F9.** On UC8253, `_redRamSynced` is false only between `displayGray()` and the cleanup
  that immediately follows — never when the reader pushes a page.
- **F10.** SSD1677's async `displayStart` skips the post-refresh BW/RED resync that its
  blocking path performs; in single-buffer async use, RED holds whatever the last *start*
  wrote until something reseeds it.

---

## 8. What code cannot decide, and the tests that can

| Question | Why code can't answer it | Test |
|---|---|---|
| Does UC8253's `_half` (single pass) leave AA-edge grey visible over a light target? | Waveform physics | **T0**: Dark/Blank sleep from the reader = `clearScreen` + HALF, nothing else. No build needed. |
| Is it the AA residue on the glass at all, or the cover render? | — | Sleep onto the same cover from **Home** (no AA on glass). No build needed. |
| Which S4 branch ran on the captured sleep? | Driver flag not logged; `X3_DRF` untagged | Label-only trace: tag `displayFinish` by bank and the two `displayGrayscaleBase` branches. One flash, no behaviour change. |
| Does the "other devices" report share the mechanism? | §5: three different memories of grey | T0 per board before generalising anything. |

---

## 9. On the question of state flags

Asked mid-audit: *would helper flags that say where the primary (B/W) and secondary
(grayscale/AA) buffers currently are — "glass", "next page", "lent" — help?*

The audit says yes, and says which — and it is not two flags on two buffers. There are
**three** states that matter, and today they live in three different places under different
names, or nowhere:

1. **Host write buffer content** — `{Glass, NextPage, Plane, Stale, Scratch}`. Today implicit:
   "NextPage" is `preRenderedPage.ready`, "Plane" is *nothing* (the plane passes leave it
   there silently), "Stale" is the post-swap default nobody names, "Scratch" is the image
   warm pass. Every compositing consumer in `docs/framebuffer-content-audit` (§3 there)
   needs exactly this bit and re-derives it.
2. **Host secondary content** — `{Glass, Unproven, Lent, Released}`. Today: `frameBufferActive
   != nullptr` conflates Glass with Unproven (the realloc/return reseed), and
   `_secondaryLent` is private. This is the one-bit `hasDisplayedFrame()` from the reverted
   branch, generalised.
3. **Glass carries grey beyond the B/W baseline** — a *controller-side* state, and the one
   this bug actually turns on. Before this branch it existed on exactly two drivers as a
   private flag (`_redriveAfterGray`) and was absent from the other four (§5). UC8253 now
   has it as `_grayOnGlass`; the decision it feeds is which base bank a grayscale pass gets,
   which is the granularity a 1-bit old plane can express. A finer, per-pixel correction was
   costed and declined on memory grounds — see the hypotheses document §6. Any consumer that is about
   to push a differential after a grayscale page — the sleep cover, the home screen after the
   reader, a T5S3 FAST — needs it, and none can ask for it.

The first two are host-side and cheap; they would not have found this bug, because on five
of six controllers the host buffers are not the baseline (F1). The third is the one worth
designing carefully, because it has to be answered by each driver and it decides refresh
*mode*, not buffer *content*. That is a different kind of flag from the other two and should
not be folded into them.

None of this is a proposal yet. It is what the state space looks like once the four
"previous frame" objects in §1 are kept apart.
