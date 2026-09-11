# Touch input migration — investigation and plan

Date: 2026-08-14 (last updated 2026-09-01)
Status: **phases 1–3 done and DEVICE-VALIDATED** on the T5S3 (touch page turns,
centre-tap menu). **Phase 4a — the button-hint strip — done and DEVICE-VALIDATED**
(2026-08-17); the rest of 4a (tab bar, central back/home gestures, P2 tap feedback)
and **phase 4b (lists)** are not started. Phases 5–6 still proposal.
**Phase 7 — bindable gestures and the reading light — code complete, NOT device
validated** (2026-09-01); see §8, which also answers open question 7.

The "nothing has run on hardware" caveat that stood here through 2026-08-16 is
retired: the T5S3 boots, and touch is the primary way it is navigated. Phases 1–2
met their build/host gates; phase 3's device gate is now met.
Scope: **workstream C** of
[multi-board-bringup-2026-08-14.md](multi-board-bringup-2026-08-14.md) — read that
first for sequencing.
Reference: **`upstream/develop`** (crosspoint-reader) — the touch stack is now
mainline there — plus **`upstream/feat-touch`** for the in-flight board work.

> **2026-08-16 revision.** Three premises of the 2026-08-14 draft have changed.
> Read §0 before the rest of the document.
>
> 1. `upstream/feat-x4-papermono-support` **no longer exists**. The whole touch
>    stack merged to `upstream/develop`; the active branch is now `feat-touch`.
> 2. The SDK bump to `cc89c653` added **multi-touch** and **list layout
>    feedback + row rectangles**. The latter materially weakens **P2**.
> 3. **Touch is on the critical path for X4 Pro after all** — the board has no
>    back and no confirm button. The line struck from the scope note above was
>    wrong.

## Summary

This is **not** a greenfield design. Upstream has already built a complete touch
stack (now merged to `upstream/develop` — see §0.1), and the FreeInk SDK carries the drivers and
geometry underneath it. Our fork consumes none of it — `grep -ril touch src/ lib/`
returns nothing.

So the real question is not *what to build* but *how much of upstream's stack to
take*, because their touch layer sits on top of a UI refactor
(**"convert all lists and tabs to FUI" #2957**, 89 files, +5649/−3570) that is
already merged to their **develop** mainline and that our fork has not taken.

Our divergence from `upstream/develop` is **2973 ahead / 1153 behind**
(measured 2026-08-16; was 2944/1142 on 08-14).

The recommendation below is: **port upstream's input layers verbatim (phases 0–3),
then adopt FUI incrementally from phase 4 onward.** The input layers are not an
alternative to FUI — `UiAppHost::routeTouch()` takes a `MappedInputManager`, so
they are its prerequisite under any strategy. FUI itself is already proven on the
C3, is a per-screen opt-in mixin rather than a cutover, and solves the e-paper
tap-feedback problem a bolt-on approach would leave us to invent. See §3.

---

## 0. What changed since 2026-08-14

### 0.1 The reference branch moved to mainline

`upstream/feat-x4-papermono-support` has been **deleted upstream**. Every file
this plan names as a port target now lives on `upstream/develop`:

```
src/MappedInputManager.{h,cpp}      src/activities/UiListActivity.{h,cpp}
src/activities/reader/ReaderUtils.h  (detectTouchPageTurn, isTouchMenuGesture)
```

Verified on `upstream/develop` (2026-08-16): the constructor is
`MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer)`, and
`SwipeDir`, `RowTouch`, `rowTouch`/`colTouch`, `wasTapInRect`, `wasScreen*`,
`wasBackGesture`/`wasHomeGesture`/`wasMenuGesture`/`wasLightPanelGesture`/
`wasHomeKeyHold` are all present exactly as §1 describes. **§1 remains accurate —
only the branch name in it is stale.**

FUI adoption on develop: **41** files use `UiListActivity`/`UiTabListActivity`,
**21** use `UiAppHost`.

*Consequence:* the port is now against a maintained mainline rather than a
side branch that could be rebased or abandoned. This is a straight improvement
to the risk profile of phases 1–3, and it removes the "reference diff may
disappear" concern from the phase-4b argument.

### 0.2 `upstream/feat-touch` — 42 commits ahead of develop

This is where the board work is happening, and it overlaps our **workstream B**
far more than it overlaps workstream C. Reference commits for the B table in
[the handover](multiboard-bringup-handover-2026-08-15.md):

| Commit | Subject | Our B-table site |
|---|---|---|
| `28d8ad56` | Fix SPI bus initialization for non-C3 boards | `HalGPIO.cpp:150` |
| `c812091a` | Make USB detection pin configurable | `HalGPIO.cpp:531` |
| `6acecd8e` | Use board config for battery monitoring setup | `HalPowerManager.cpp:344` |
| `06ff5aa4` | Add SDK RTC and IMU hardware abstraction support | `HalClock.cpp:19` |
| `a5109872` | Support multi-I2C + ESP32-S3 USB logging | see P1 |
| `61cb4946` | Fix I2C init ordering for the dual X3+X4 binary | C3 regression risk |
| `9b7b9e90` | Skip X3 fingerprint probe on non-C3 boards | `HalGPIO::begin()` |
| `526cf1b6` | Add touch gesture support **and LilyGo T5 S3 board** | our second board |
| `e103cdfd` | UI scaling for touch vs button devices | phase 5 |
| `57c389c0` | Add touch-down visual feedback to settings menus | **P2** |

Most of these are 10–30 line commits against the exact call sites the handover
lists as unported. **Workstream B is substantially cheaper than the handover
assumes** — it is now largely a review-and-adapt job, not original work.

`61cb4946` deserves separate attention: it is a **C3 fix**, not board work.
Upstream found that the dual X3+X4 binary left the X4 profile active through
`powerManager`/`clock`/`tilt` `begin()`, so `Wire` was never re-initialised
after the detection probe and every X3 I2C read failed with `lock == NULL`.
Whether our fork has the same latent bug is worth checking on its own merits,
independently of any board work.

### 0.3 SDK `76e61c4` → `cc89c653` — two touch-relevant additions

**Multi-touch (`410d0ab`).** Purely additive; the single-contact contract is
unchanged.

```cpp
static constexpr uint8_t MAX_TOUCH_CONTACTS = 4;
bool supportsMultiTouch() const;            // GT911 only
TouchSnapshot getTouchSnapshot() const;     // fixed-size, allocation-free
bool wasMultiTouchSwipe(uint8_t& contactCount, float& nxStart, …, unsigned long& durationMs) const;
bool popMultiTouchSwipe(…);                 // dedicated queue — popSwipe() never sees these
```

Two behaviours matter more than the API:

- A `touchMultiContactSequence` latch **suppresses the single-contact
  classifiers until full release**, so a two-finger gesture cannot emit a
  spurious tap or swipe. This is defensive work we would otherwise have had to
  do ourselves in `MappedInputManager`.
- `FreeInkUIInputManager::snapshotFrom()` now gates `touchHeld` on
  `isTouchHeldAt()` so a staggered multi-contact sequence cannot become a UI
  drag.

Pinches, rotations, diagonal motion and ambiguous contact matching are
explicitly **rejected** by the SDK — it reports centroid translation only.

*Recommendation:* **do not plan a feature around multi-touch.** It changes
nothing in phases 0–5, and the SCOPE.md test does not obviously pass for a
reading device. Note it, take the free robustness, revisit only if a concrete
need appears. It does not belong in this plan's phases.

**List layout feedback + row rectangles (`bb48403`, `cc89c65`).** This is the
consequential one.

> **Superseded 2026-08-17.** Upstream `310ec61` ("Remove row rectangle tracking
> from list component") **deleted `rowRectFor()` and `MAX_ROW_RECTS`** again, and
> `cc15c5a` replaced them with windowed list rendering. Both are in
> `Free-Ink/freeink-sdk` main as of the #47 merge, which this repo now tracks. So
> the P2 downgrade below rests on a primitive that no longer exists: bounded
> partial-refresh tap flash is **not** available from the SDK today, and P2 is back
> to "port upstream `57c389c0`'s approach" under 4a or "inherit whatever FUI does"
> under 4b. The rest of this subsection is kept as the record of why the phase-4
> recommendation was written the way it was.

```cpp
struct ListNav {
  int  pageRows() const;                       // MEASURED page size, not the estimate
  bool rowRectFor(int index, Rect* out) const; // rect of a drawn row
  static constexpr uint8_t MAX_ROW_RECTS = 16;
  bool consumeRebuildNeeded();
  void onListRendered(uint16_t effectiveTop, int drawn, bool selectedDrawn, …);
};
```

`list()` now reports its actual layout back through `props.nav`, so
variable-height rows (wrapped labels, subtitles) no longer let the selection
sit on a never-drawn row, and `scrollBy` clamps against the measured page size
rather than the fixed-height estimate. Two new host tests cover it
(`testListNavLayoutFeedback`, `testListNavConvergesThroughRealList`).

**Why this matters to the phase-4 decision:** `rowRectFor()` gives the caller
the exact rectangle of a row, which is precisely the primitive needed to paint
and refresh *only* the rows a selection change touched. Combined with upstream's
`57c389c0` (touch-down visual feedback), the "we would have to invent tap
feedback ourselves" objection in **P2** is now answered inside the layer phase 4b
adopts — and answered with something bounded to a partial refresh rather than a
full-screen one. See the revised P2 in §5.

### 0.4 X4 Pro has no back and no confirm button

From the SDK's `XTEINK_X4_PRO` profile (hardware-confirmed, not RE guesswork):

```
// {back, confirm, left, right, up, down, power, powerActiveHigh}
{PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 7, 3, false},
```

Two physical nav keys (Up=GPIO0, Down=GPIO7) plus Power=GPIO3. The profile
comment is explicit: *"back/confirm come from the GT911 (touch + the capacitive
Home key)."*

**So on the lead bring-up board, touch is not an enhancement — it is the only
way to confirm or go back.** The board can reach the handover's milestone
("boots, mounts SD, renders a page") without touch, but it cannot be *navigated*
without it. Phase 3 is therefore the point at which X4 Pro becomes a usable
device, and workstream C stops being parallel-optional work.

This does not reorder B before C — B still gates booting at all — but it does
mean **C must complete for the board to ship**, and the two should be planned as
one delivery rather than as a critical path plus a nice-to-have.

---

## 1. Upstream's architecture

Four layers, cleanly separated:

```
InputManager (SDK)          raw contacts, tap/swipe/long-press/drag, async queues
      ↓ normalized 0..1 panel-native coords
HalGPIO                     passthrough only — no interpretation
      ↓ still normalized
MappedInputManager(gpio, renderer)
                            orientation mapping → LOGICAL screen px,
                            gesture semantics, hit-test helpers
      ↓ logical px + named gestures
Activities                  either FUI interaction tables (UiAppHost),
                            or direct rowTouch/colTouch/wasTapInRect calls
```

### Layer 2 — `HalGPIO` (raw passthrough)

Upstream added, all normalized coords, no orientation logic:

`hasTouch()`, `wasTouchTap(nx,ny)`, `wasTouchDown(nx,ny)`, `wasTouchReleased()`,
`isTouchTapCandidate(nx,ny,heldMs)`, `isTouchHeldAt(nx,ny)`,
`wasTouchLongPress(nx,ny)`, `suppressTouchContact()`, `lastTouchHeldMs()`,
`wasTouchActivity()`, plus the GT911 capacitive home key.

This matches the repo's own layering rule (expose an SDK capability as a HAL
method, never reach around it) and is a near-mechanical port.

### Layer 3 — `MappedInputManager` (the interesting one)

Upstream changed the constructor to `MappedInputManager(HalGPIO&, const GfxRenderer&)`.
The renderer is held for **live** orientation — the same discipline our
`setStripReversedPredicate` already uses, so the touch transform can never go
stale against a rotated reader.

**`Button` enum extended, purely additively** (ours ends at `PageForward`):

```
… PageBack, PageForward, NavNext, NavPrevious,
ScreenLeft, ScreenRight, ScreenUp, ScreenDown
```

`Screen*` are direction-in-what-the-user-sees buttons; `mapScreenDirection()`
resolves them through the live orientation.

**Touch API:**

| Method | Purpose |
|---|---|
| `hasTouch()` | board capability |
| `wasScreenTapped(x, y)` | tap in **logical** px |
| `wasScreenTouchDown(x, y)` | press edge |
| `wasScreenLongPress(x, y)` | consuming it suppresses the rest of the contact |
| `isScreenTouchHeld(x, y)` | drag |
| `wasScreenTouchReleased()` | raw release, including swipe/drag-off |
| `wasSwipe()` → `SwipeDir::{None,Left,Right,Up,Down}` | direction, orientation-mapped |
| `wasTapInRect(x, y, w, h)` | single-rect hit test |
| `rowTouch(row, top, rowStep, rowCount, xStart, xEnd, rowHeight)` → `RowTouch::{None,Down,Tap}` | **row-band hit test for lists** |
| `colTouch(col, left, colStep, colCount, yStart, yEnd, colWidth)` | horizontal variant (prompts) |

**Named semantic gestures** — the app-meaning layer:
`wasBackGesture()` (left-edge L→R swipe), `wasHomeGesture()` (bottom-edge up-swipe),
`wasMenuGesture()`, `wasLightPanelGesture()` (top-edge down-swipe → frontlight panel),
`wasHomeKeyHold()`.

Upstream links FreeInkUI for the geometry (`freeink::ui::ScreenEdge`,
`edgeSwipe`, `touchToLogical`, `Rect`, `TapZone`) rather than reimplementing it:
`FreeInkUI=symlink://freeink-sdk/libs/ui/FreeInkUI` in `lib_deps`.

**Note what is *not* here: there is no double-tap anywhere in upstream's stack, and
none in the SDK.** If we want it, it is ours to add — and it should be added as an
extension of this API, not a parallel one.

### Layer 4 — two consumption paths

Upstream supports **both**, deliberately:

1. **FUI path** — `UiAppHost` (owns a `freeink::ui::FreeInkApp<24,6>`, a render
   target, and a `uiReady` atomic handshake so the loop task only routes touch
   against a fully published interaction table). `UiListActivity` /
   `UiTabListActivity` layer the list protocol on top. This is what #2957
   converted every list and tab screen to.

2. **Direct path** — `rowTouch` / `colTouch` / `wasTapInRect`, documented in
   upstream's own header as *"the shared hit-test for lists the theme helpers
   above do not cover (custom row heights, option prompts, menus)"*.

**Path 2 is our escape hatch.** It is upstream's supported API for screens that are
not FUI, which is currently all of ours.

### Reader integration

Factored into shared helpers in `src/activities/reader/ReaderUtils.h` —
`detectTouchPageTurn(renderer, input)` and `isTouchMenuGesture(renderer, input)` —
and driven by two settings:

```cpp
enum TOUCH_READER_CONTROLS {
  TOUCH_READER_OFF = 0,
  TOUCH_READER_ON = 1,           // outer-third tap zones
  TOUCH_READER_SWIPE = 2,        // horizontal swipe turns pages
  TOUCH_READER_INVERTED_TAP = 3, // mirrored zones
};
uint8_t touchReaderControls = TOUCH_READER_ON;
uint8_t tapForReaderMenu = 1;    // center-third tap opens the reader menu
```

Tap zones are outer thirds; the centre column is reserved for the menu tap, with
vertical bounds (`2a3f08dd`) so it does not swallow the whole column. These
helpers are header-only and among the most portable pieces on the branch.

---

## 2. Where our fork actually differs

| Concern | Upstream | Us | Cost to align |
|---|---|---|---|
| SDK touch drivers | in use | unused | none — already present |
| `HalGPIO` touch | ~10 passthrough methods | **absent** | low, mechanical |
| `MappedInputManager` | `(gpio, renderer)`, +6 Buttons, touch API | `(gpio)`, 9 Buttons, no touch | **medium — the core port** |
| FreeInkUI linked | yes (`lib_deps`) | **no** | low (header-only geometry) |
| List base class | `UiListActivity` on `UiAppHost` (FUI) | `MenuListActivity` + `BaseTheme::drawList` | **high — 89-file refactor** |
| `UIScale.h` / `uiScale` | reads `BoardConfig::ACTIVE.uiScale` (1.2 on touch) | never read | low–medium |
| Touch envs | `x4pro`, `papermono`, `sticky` | only `lilygo_t5s3` | low |
| Reader touch | `ReaderUtils` helpers + 2 settings | none | low, portable |
| Input sampling | **none** — `update()` polls from the loop task | background `btnsample` task + edge queue + `ButtonEventManager` | see P1 |

That last row is the one to watch. Upstream's `HalGPIO::update()` is three lines:
`inputMgr.update(); updateUsbState(millis());`. They have no sampler task, no
`ButtonEdge` queue, and no `ButtonEventManager` — **all of that is fork-local**,
added so presses survive long sliced background builds. It is also the single
place where upstream's touch design does not transplant cleanly onto ours.

Our UI layer has also diverged independently (Lyra carousel themes, our own
`MenuListActivity`), so #2957 is **not** a clean cherry-pick — it would be a
re-implementation against our themes.

Relevant local geometry for path 2: `BaseTheme::drawList(renderer, rect,
itemCount, selectedIndex, …)` has 20 call sites across 18 activities, and
8 activities derive from `MenuListActivity`. Rows are uniform and
`UITheme::getNumberOfItemsPerPage` already bounds them — which is exactly the
`rowTouch(top, rowStep, rowCount)` shape. The fit is good.

---

## 3. Strategy

**Recommended: adopt FUI as the destination, reached incrementally. Phases 0–3
are unconditional; the FUI decision only bites at phase 4.**

### The reframe

The input layers are **not** an alternative to FUI — they are its foundation.
Upstream's own signature is:

```cpp
TouchRoute UiAppHost::routeTouch(const MappedInputManager& input, ...);
```

FUI consumes `MappedInputManager`. So porting `HalGPIO` + `MappedInputManager`
touch (phases 1–2) is required work under *either* strategy, and none of it is
throwaway. The genuine fork in the road is phase 4 — lists — where the choice is
`rowTouch` bolted onto `MenuListActivity`, or conversion to `UiListActivity`.

### Why FUI is the right destination

1. **It is already proven on our exact hardware.** Upstream's `env:default` is
   X3/X4 — ESP32-C3, 380 KB, no PSRAM — and it links FreeInkUI unconditionally.
   The RAM-ceiling objection is empirically dead; `UiAppHost` documents ~300 bytes
   per live host, bounded by activity stack depth.

2. **The migration is incremental, not a cutover.** `upstream/develop`'s
   `Activity.h` contains **zero** FUI references. FUI is a per-screen opt-in
   mixin — `class UiListActivity : public Activity, protected UiAppHost` — so
   our `Activity` / `ActivityManager` lifecycle survives untouched and screens
   convert one at a time. 50 of their activity files use it; the rest do not.

3. **It solves the problem this plan could not.** P2 (tap feedback on e-paper) is
   the weakest point of the bolt-on approach, and FUI already answers it with
   `setFlash` / `clearTapFlash`. Building our own is re-deriving a solved,
   maintained thing.

4. **Touch is native there, bolted on here.** FUI's interaction table *is* the
   hit-test model: register a rect once, routing handles it. `rowTouch` makes
   every screen re-derive its own geometry.

5. **Host testability.** FreeInkUI is freestanding C++17 with no Arduino/ESP-IDF
   dependency and ships 2,806 lines of host tests. Given how much of this
   codebase's debugging history is device-only, a UI layer that runs on the host
   is a compounding win.

6. **It is where the SDK is going** — 80 commits in the last six months, and the
   recent ones are exactly touch, list selection styles, home-key detection, list
   layout. Every month we don't adopt, we re-implement what lands there for free.

7. **Themes survive.** `BaseTheme`, `LyraTheme`, `Lyra3CoversTheme` all still
   exist on `upstream/develop` after #2957. FUI renders through tokens and a
   `GfxRendererTarget` bridge; `UiAppHost.cpp` is 37 lines. The plumbing is thin —
   the weight is in screen conversion.

### What it honestly costs

- **34 of our 76 activities have no upstream counterpart** (42 shared, 34
  ours-only). For those, conversion is hand work with no reference diff. #2957's
  89 files / +5649−3570 is upstream's number for *their* 56 activities; ours is
  comparable or larger.
- **Timing.** A 90-file UI conversion running concurrently with the paused
  reader/Stage-1 work and the master cherry-pick queue is a merge-conflict
  machine. Sequencing matters more than speed here.
- **Flash — and this is worse than first stated.** Capacity-templated types
  instantiate per-capacity; upstream already had to consolidate to a single
  `FreeInkApp<24,6>` to stop the bloat. An earlier draft of this doc claimed we
  had headroom, citing ~2.22 MB against a 6.4 MB partition. That figure is *text
  only*. **Measured 2026-08-14 on a passing C3 build: Flash 97.3 % — 6,376,829 of
  6,553,600 bytes, ~177 KB free.** So the single-instantiation discipline is
  mandatory, not advisory, and FUI adoption needs a flash budget agreed up front.
  See the flash section in
  [multi-board-bringup](multi-board-bringup-2026-08-14.md).
- Adopting FUI aligns *one layer*. It does not by itself close a 2973/1153
  divergence, and shouldn't be sold as if it does.

### The rule that makes this safe

Keep upstream's names and signatures **verbatim** throughout. A screen that gets
`rowTouch` in phase 4 and `UiListActivity` in phase 6 changes its base class and
drops one call — nothing else it touches moves. That is what lets us start
delivering touch before committing to the full conversion, without the interim
work becoming waste.

---

## 4. Plan

### Phase 0 — Bring-up and decisions (blocking)

- ~~Port `env:x4pro`~~ — **done** (workstream A, `b4b94068`).
- ~~Confirm the primary bring-up board~~ — **X4 Pro**, and §0.4 makes that
  binding: it has no back/confirm button.
- Add `FreeInkUI=symlink://freeink-sdk/libs/ui/FreeInkUI` to `lib_deps`.
- Verify GT911 enumerates on T5S3 and X4 Pro; log raw contacts.
- **Resolve I2C ownership (see §5, P1).** Cross-cutting — deciding it late means
  reworking phases 1–2. §5 now rules out bus separation on X4 Pro, so this is a
  straight choice between resolution 1 and 2.

Deliverable: raw contacts in the serial log. No UI.

**Build state (2026-08-16, after merging master into `fix/s3-build-config`):**

| Env | Result | RAM | Flash |
|---|---|---|---|
| `default` (C3) | SUCCESS | 56,692 | 6,349,593 — 96.9 % |
| `x4pro` | SUCCESS | 66,624 | 6,172,882 — 94.2 % |
| `lilygo_t5s3` | SUCCESS | 66,628 | 6,174,219 — 94.2 % |

All three envs link. C3 flash is **~204 KB free**, better than the 177 KB the
08-14 draft budgeted against — master's font regeneration recovered ~27 KB. The
flash caution in §3 still applies, but with slightly more room than stated.

### Phase 1 — `HalGPIO` touch passthrough ✅ DONE (`cfd00bf8`)

Port upstream's ~10 methods verbatim. ~~Everything inside `#if FREEINK_CAP_TOUCH`
with inert `false`-returning stubs otherwise~~ — **not needed**: every
`InputManager` touch method is already `#if FREEINK_CAP_TOUCH` guarded *inside
the SDK* and compiles to an inert `false`/`0`, so plain passthroughs cost the C3
nothing and callers still need no ifdefs.

Gate: `pio run -e default` (C3) flash/RAM delta is **zero**. **Met exactly** —
RAM 56,692 / Flash 6,349,593, byte-identical; x4pro +4 bytes.

### Phase 2 — `MappedInputManager` touch layer ✅ DONE (`4ab2c188`, `7d59e493`, `82be8458`, `6990c4b1`)

- ✅ Constructor gains `const GfxRenderer&`; `main.cpp` declares `renderer`
  before `mappedInputManager` (globals in one TU construct in declaration order).
- ✅ `GfxRenderer::tapToLogical()` — the transform itself, which our fork
  lacked. Math extracted to an Arduino-free `TouchTransform.h` so it is
  host-testable, with a `static_assert` keeping the orientation orders in step.
- ✅ Full touch API + `SwipeDir` + `RowTouch` + back/menu/home/home-key gestures.
- ✅ Geometry from FreeInkUI (`edgeSwipe`, `swipeDirection`) — linked via
  `lib_deps`, not copied.
- ✅ P1: `HalI2cBus` mutex + sampler stack 2048 → 4096 under `FREEINK_CAP_TOUCH`.
- ⚠️ **`Button` enum extension deliberately deferred.** Upstream's
  `Screen*`/`Nav*` names and `mapScreenDirection()` key on a
  `frontButtonFollowOrientation` setting our fork does not have (we use
  `setStripReversedPredicate`). The touch API does not need them, and adding a
  setting is a scope call. `ButtonEventManager::NUM_BUTTONS` therefore stays 9
  and the press-type FSM is untouched — which is what the audit was protecting.

Also not ported, with reasons: `wasLightPanelGesture` (no `HalFrontlight` in this
fork — belongs with the frontlight work), `wasPowerConfirmClick` (depends on
`SETTINGS.shortPwrBtn`), and the `rememberTouchHeldTime`/`getHeldTime` override
(would change button held-time behaviour on the shipped C3).

Gate: host unit tests for the orientation transform in all four orientations, and
for `rowTouch` band arithmetic. **Met** — `test/touch_transform`, 15 tests,
470/470 suite green. `rowTouch`/`colTouch` now share one `bandHit()` helper
(same question on perpendicular axes), which is what made the arithmetic
testable without HalGPIO or a renderer.

Final sizes: C3 RAM 56,692 / Flash 6,349,601 (**+8 bytes** over the merge
baseline, from the reordered globals); x4pro RAM 66,640 / Flash 6,173,358.
The touch code is unreferenced until phase 3 wires it up, so the linker still
drops most of it.

### Phase 3 — Reader touch ⚙️ CODE COMPLETE, UNVALIDATED (`968a8493`, `013f343c`)

- ✅ `ReaderUtils::detectTouchPageTurn` / `isTouchMenuTap` / `isTouchMenuGesture`.
- ✅ `TOUCH_READER_CONTROLS` (off / tap / swipe / inverted) + `tapForReaderMenu`,
  with `tr(STR_*)` keys through `scripts/gen_i18n.py`. Includes the later
  branch fixes: menu-zone vertical bounds, inverted tap, menu-tap toggle.
- ✅ Page turns wired into all three readers next to `detectTiltPageTurn()`;
  EPUB reader menu on centre tap / top-edge swipe.
- ✅ `wasTouchActivity()` feeds the inactivity timer, so a tapping reader
  cannot fall asleep mid-chapter.

**Settings visibility became capability-based here.** `SettingDeviceTarget
{BOTH, X3, X4}` resolved through `deviceIsX3()`, which cannot answer "has a
touch panel" and silently mis-answers on any third board. Replaced with
`SettingRequires {Nothing, TouchPanel, TiltSensor, SelectableGrayscaleLut}`,
resolved once in `getSettingsList()` from the HAL and `BoardConfig::ACTIVE`.
Confirmed to reproduce shipped behaviour exactly — X3 (UC8253 + Qmi8658) keeps
fast-AA and tilt, X4 (SSD1677, no IMU) keeps neither. This is B0's
capability-predicate idea applied to the one place phase 3 forced the issue;
the remaining 49 `deviceIsX3()` call sites are still workstream B0's job.

Gate: **MET (2026-08-17).** On the T5S3 the reader is navigable by touch: outer-third
taps turn pages and a centre tap opens the menu, both confirmed in device logs
(`[TCH] contact DOWN at nx=… ny=…` followed by `PageTurn summary: hit=1`). Combined
with the phase-4a hint strip and the capacitive home key, the board is usable
without ever pressing a physical nav key.

Known gap: `wasHomeKeyTapped()` / `wasHomeKeyLongPressed()` are exposed through
`HalGPIO` and reachable via `wasHomeGesture()` / `wasHomeKeyHold()`, but nothing
consumes them yet. Open question 4 wanted phase 3 to cover the X4 Pro Home key;
that is still outstanding and matters, because the board has no back button.

### Phase 4 — Lists and chrome (the decision point)

**4a chrome: hint strip DONE and DEVICE-VALIDATED (2026-08-17).** `drawButtonHints` now records
the geometry it painted (`ButtonHintStrip.h`), and `ActivityManager::dispatchHintStripTap()`
turns a tap on a box into the button press it depicts. Notes:

- **No per-screen work.** `mapLabels()` emits its four labels in
  `{BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT}` order and permutes only the *text*, so hint
  box *i* is raw button *i* on every board and under every remapping. All ~95
  `drawButtonHints` call sites became tappable without being touched.
- **Synthesis, not a parallel path.** The tap injects a raw press through
  `HalGPIO::injectPress()` — the same accumulator+edge mechanism the capacitive home key
  already uses (device-validated) — so both the `wasPressed()` bitmask consumers and
  `ButtonEventManager`'s press-type FSM see an ordinary Short press.
- **Dispatch runs AFTER `currentActivity->loop()`**, because `wasScreenTapped()` consumes.
  That makes the strip a strict fallback: a screen handling touch itself (reader page turns;
  the list rows of 4b) claims the tap first and this finds nothing.
- **Invalidated on every activity transition** (`Activity::onEnter`/`onExit`) so a screen
  drawing no hints cannot inherit the previous screen's boxes.
- Gate: host tests for the band arithmetic — `test/button_hint_strip`, 9 tests, 479/479 green.
  They caught that the tuned X4 positions overlap by 1 px (box 0 spans 25..130, box 1 starts
  at 130); the tie-break to the lower index is now pinned rather than accidental.
- Cost on C3: **+40 bytes RAM, +888 bytes flash.**
- Still open in 4a: the **tab bar** (`colTouch`), central `wasBackGesture()`/`wasHomeGesture()`
  dispatch, and P2 tap feedback (port upstream `57c389c0`'s approach).
- **Orientation-independent.** The strip is recorded in the Portrait frame (drawButtonHints
  forces Portrait for its draw) whatever the screen is rotated to, so the dispatcher resolves
  the tap into the *Portrait* frame too rather than the live one. `touchtransform::tapToLogical`
  always took orientation as a parameter; only `GfxRenderer`'s convenience wrapper pinned it to
  the live value, so this added `tapToLogical(Orientation, ...)` and
  `MappedInputManager::wasScreenTappedIn(...)` beside it. An earlier draft carried a
  "bail unless live orientation is Portrait" guard instead — wrong fix, removed.

**4b lists, covers and home: DONE, unvalidated (2026-08-31).** Built the way 4a was rather than
the way the plan below assumed, and the difference is worth recording because the plan's
`rowTouch`-per-screen framing made 4b look more expensive than it is.

The draw records what it painted; the input side matches against that. Three recorders now, each
for a shape the others cannot express:

| Recorder | Shape | Resolves to |
|---|---|---|
| `ButtonHintStrip` (4a) | four equal boxes in a fixed strip | a raw button index |
| `ListTouchBand` | a contiguous vertical run of full-width rows, heights may differ | an item index |
| `TapTargets` | arbitrary rects, each with a value | that value |

- **Lists.** `BaseTheme::drawList`, `LyraTheme::drawList` and the shared `drawWrappedList`
  record their rows — that is all of them. `rowTouch()` could not have done the wrapped lists at
  all: its arithmetic is a uniform step, and those rows are one, two or three lines tall.

  **Recording is only half of it, and the first attempt shipped only that half.** A recorded row
  is *matchable*; something still has to read the band and act. `ActivityManager::dispatchListTap()`
  now does the reading, asks the screen to move its selection via `Activity::selectListRow()`,
  and then **synthesizes a Confirm press** — the hint-strip pattern. Synthesizing matters: each
  of these screens already has a Confirm path several branches deep, and a tap must run that
  exact code rather than a parallel copy. One small override per screen instead of a
  restructured `loop()`. Coverage: **26 list screens, 25 wired, 1 excluded on purpose**
  (`ButtonRemapActivity` — a wizard that assigns roles from the physical button pressed, so a
  synthesized Confirm would record itself as the reader's choice).
- **Recent books.** The list view rides the band. The grid view instead got
  `CoverGridLayout::hitTest()`, the inverse of its own placement, sitting next to `compute()`.
  No recording there and none wanted: the geometry already comes from `computeGridLayout()`,
  which the loop task calls anyway, and `pageStartRow` follows `selectorIndex`, which that task
  owns — so there is no render-task staleness to reason about.
- **Home.** `TapTargets`, two named instances (covers, menu), recorded by all four themes. Two
  traps found here: the cover draw is skipped on a cached repaint, so the targets are recorded
  ahead of it; and `LyraCarouselTheme::tryFastHomeRender()` restores the covers from a cached
  region without calling `drawRecentBookCover()` at all, so the slot geometry is factored and
  recorded from both paths.

**Two sweeps were needed, because the first used the wrong criterion.** Looking for callers of
`GUI.drawList` misses every screen that draws its own rows, and six do: the three chapter/TOC
selectors, the footnote picker, the OPDS browser (two lists) and the KOReader conflict prompt.
They all share one shape, so `ListTouchBand::recordUniformRows()` records it and each gets the
same `selectListRow()` hook. The lesson for the next screen: **"does it have a selection index"
is the question, not "does it call drawList"**.

Current coverage, by that question: **33 selection screens, 32 wired, 1 excluded on purpose**
(`ButtonRemapActivity`).

Still untouched, and each a different shape rather than more of the same:

| Screen | Shape | Why it is not just another band |
|---|---|---|
| `KeyboardEntryActivity` | key grid (`selectedRow`/`selectedCol`) | A grid, not a row run. The plan already calls this "the single best tap payoff in the firmware". |
| `DictionaryWordSelectActivity` | word boxes over a page of text | Hundreds of targets — far past `TapTargets`' capacity. It already holds per-word boxes, so it should hit-test its own data directly, like the RecentBooks grid does. |
| Settings tab bar | horizontal strip | `colTouch`, still open from 4a. |

`ConfirmationActivity` needs nothing: it has no selection state, and its Back/Confirm are already
tappable through the 4a hint strip.

**Sliders: DONE (2026-08-31), pulled forward from phase 5.** One `SliderPickerActivity` backs
every slider in the firmware — go-to-percent, the KOReader settings, `SliderSettingPicker` — so
one change covers them all. Tap and drag both work.

Drag on e-paper needed a decision rather than a port. A repaint is a panel refresh of several
hundred ms, so the slider cannot track a finger the way an LCD one does. The split that makes it
usable: the **value** follows the finger every loop pass, the **repaint** happens only when the
value changes, and the render task supersedes a pending update when a newer one arrives. The
number lands where the finger stopped even though the screen shows only some intermediate
positions. `setValue()`'s early return on an unchanged value is load-bearing —
`isScreenTouchHeld()` reports on every pass while the finger is down.

A tap does not confirm; it moves the value and leaves committing to Confirm, matching the
buttons. Confirm is already reachable through the hint strip.

The mapping lives in `SliderGeometry`, used by both the draw and the hit test, with a test that
walks every value, draws it, and hit-tests the pixel it was drawn at. That round-trip caught two
things: `valueForX` must ROUND rather than truncate (truncating makes the top of a 0..100 range
untappable), and `fillWidthFor` returned a negative width for a degenerate bar where its inverse
already guarded the same trap.

**Invalidation belongs to the render pass, not to the screens.** A screen can stop drawing its
list *without* changing activity — `WifiSelection` swaps the network list for a "no networks"
message, `FontDownload` for a progress screen — and `drawList` is then simply not called, so a
band recorded earlier kept answering taps against rows nothing paints any more. Clearing on
activity transitions never covered it. All three recorders are cleared at the top of every render
pass instead, so what they hold is exactly what the current frame painted.

**`SettingsActivity` is off by one**, and would have been wrong under any blanket edit: `render()`
passes `drawList` `selectedSettingIndex - 1`, because index 0 is the category tab row, which the
list does not paint. Band row *i* is `selectedSettingIndex` *i+1*. A tap therefore cannot reach
the tab row — correct, since the tab bar is its own (still unbuilt) `colTouch` target.

**Interaction rule: point then confirm.** The first tap on a row moves the selection there and
stops; only a tap on the row that is already selected runs the action. The rule lives in
`ListRowTap::apply()` and is exercised on the host, so nineteen screens share one definition of
it. Reaching a row you did not mean costs one more tap and can never cost an action.

This took two goes to get right, and the wrong turn is worth keeping. The first version activated
on a single tap, and it also acted on `RowTouch::Down` — move the selection, repaint — before
activating on `Tap` and repainting again. `wasScreenTouchDown()` fires once a contact has been
held `TOUCH_DOWN_SELECT_DELAY_MS` (90 ms), which an ordinary finger tap comfortably exceeds, so
**one gesture cost two full refreshes** and the first was feedback arriving after the finger had
gone. Dropping the `Down` half was right. Concluding from it that two-step feedback was too
expensive was **not**: two deliberate taps, each doing one thing, are a completely different
trade from one contact firing twice. `Down` remains claimed-but-inert; the two-step is between
separate taps.

That first repaint is the only feedback e-paper affords until a localised invert exists
(upstream's `setFlash`/`clearTapFlash`). **P2 remains the highest-value remaining touch item** —
it would let the first tap flash the row instead of repainting the screen.

Cost of the touch surface on C3: **+416 bytes RAM, +8,132 bytes flash.** Gates:
`test/list_touch_band` (12), `test/tap_targets` (11), 6 more in `test/cover_grid_layout`; host
suite 689/689. **No touch board has run any of it.**

Still unconverted, and each still keyed on the board name rather than a capability:
`UITheme.cpp:155-182`, `KeyboardEntryActivity.cpp:363`, and the recents gesture-hint predicate
(`gridShowsGestureHint()`, now at least asked in one place instead of three).

What follows is the original plan, kept for the 4b-vs-FUI argument in it.

---


Two ways to spend this phase. Both are legitimate; pick one deliberately.

**4a — bolt-on (`rowTouch`).** Fastest path to a touch-navigable device, no new
UI concepts, no conflict with in-flight reader work. Interim by construction: the
tap-feedback answer (P2) is ours to invent, and it is superseded when a screen
converts.

**4b — start the FUI conversion here.** Port `UiAppHost` (37 lines) +
`UiListActivity` + `UIThemeTokens`, convert the highest-traffic lists first,
using the 42 shared activities as reference diffs. Slower to first light, but
every converted screen is finished rather than revisited.

> **Scouting notes, 2026-08-17.** A first pass at 4b was started and stopped
> before any code was written. Three facts worth having before the next attempt:
>
> 1. **The port targets are small and fetchable.** `git fetch upstream develop`
>    works; the pieces are `src/components/UiAppHost.{h,cpp}` (~77 lines),
>    `UiAppHelpers.h` (202), `UIThemeTokens.h` (40), `UIScale.h` (24),
>    `src/activities/UiListActivity.{h,cpp}` (93 + 167). That is the whole
>    infrastructure — the weight is all in screen conversion, as §3 says.
>
> 2. **`UIThemeTokens.h` does not fit our themes.** It reads twelve `ThemeMetrics`
>    fields; **eleven do not exist in this fork.** Only `headerHeight` does.
>    Missing: `listRowGap`, `listRowRadius`, `listInset`, `listSidePadding`,
>    `listSelectionStyle`, `listScrollWidth`, `listScrollSide`,
>    `headerSidePadding`, `headerUnderlineSize`, `headerTitleAlign`,
>    `listTitleBold`. So 4b does not begin with "port a header" — it begins with
>    adding a list-shape vocabulary to `BaseMetrics`/`LyraMetrics` and choosing
>    values for **each** of our four themes. This is the concrete form of §3's
>    "not a clean cherry-pick — a re-implementation against our themes".
>
> 3. **The flash budget is still unmeasured, and it is the real gate.** C3 sits at
>    **97.0 %** (6,355,463 of 6,553,600 — ~198 KB free) as of `7040ef1d`. §3 calls
>    the single-`FreeInkApp<24,6>`-instantiation discipline mandatory for exactly
>    this reason. **Measure one converted screen before converting eighteen** — a
>    spike that instantiates the app and one list screen, built for `env:default`,
>    answers whether 4b is possible at all on the shipped board.
>
> Nothing was committed from this pass.

Recommendation: **4a for the button-hint strip and tab bar** (they are trivial
`colTouch` targets and will likely never justify a FUI screen), **4b for the
lists** — `MenuListActivity`'s 8 subclasses are precisely what `UiListActivity`
replaces, and doing them twice is the one genuinely wasted outcome available here.

- `MenuListActivity::loop()` calls `rowTouch(...)` against the geometry
  `drawMenuList` already computes: `Down` moves the selection highlight, `Tap`
  activates. That covers its 8 subclasses in one change.
- The 10 direct `BaseTheme::drawList` callers opt in individually.
- `drawButtonHints` (an already-labelled 4-button strip) and `drawTabBar` become
  tappable via `colTouch`.
- Global gestures — `wasBackGesture()`, `wasHomeGesture()` — dispatched centrally
  in `ActivityManager`/`main.cpp`, as upstream does.

Gate: no per-frame heap allocation; hit tests are arithmetic over existing rects.

### Phase 5 — Touch-native chrome

Port `UIScale.h` so `BoardConfig::ACTIVE.uiScale` (1.2 on touch boards) reaches
`ThemeMetrics`, and preserve upstream's `ee4db8e1` behaviour (denser rows retained
on non-touch devices). Enforce a minimum touch target. Drag support in
`SliderPickerActivity` via `isScreenTouchHeld` (upstream's `UiSliderDialog.h` is
the reference).

### Phase 6 — Complete the FUI conversion

Convert the remaining screens, shared-with-upstream first (reference diffs
available), then our 34 originals. Keyboard entry is the single best tap payoff
in the firmware and FUI already has a `keyGrid` component. This is a long tail and
should be run as background work between features, not as a blocking project.

Explicitly deferred:

- **Double-tap.** Exists nowhere upstream or in the SDK. Only add it if a real
  need appears; adding it makes us the divergent party.
- Fling/momentum scrolling — on e-paper each frame is a refresh; paged swipe
  navigation from phase 3 is very likely the better answer.

---

## 5. Risks

**P1 — I2C lands on our sampler task, and upstream has no answer to copy.**
This is the one genuinely fork-specific problem, and it is not opt-in.

`HalGPIO::sampleOnce()` calls `inputMgr.update()`
([HalGPIO.cpp:184](../lib/hal/HalGPIO.cpp#L184)), and on a touch board
`InputManager::update()` internally runs `serviceTouch()` — an I2C transaction.
So the moment `FREEINK_CAP_TOUCH` is on, touch I2C **automatically** starts running
on our `btnsample` task: priority 2, 10 ms cadence, and a **2048-byte stack sized
against a measured ~380-byte `analogRead` path**. Two hazards:

- *Stack* — the GT911 read path is deeper than `analogRead`; re-measure with
  `samplerStackHighWater()`.
- *Bus contention* — [`HalClock.cpp`](../lib/hal/HalClock.cpp#L276-L306) drives the
  RTC over `Wire` from the **loop** task, and on X4 Pro the GT911, BM8563 RTC and
  CW2017 gauge share one bus. Concurrent I2C from two tasks with no mutex is a
  corruption risk.

Upstream never hits this because they poll input synchronously from the loop task
and let the SDK's own async queues (`popTouchTap`, `popSwipe`) carry gestures
across e-paper refreshes. Our sampler exists for a real reason — presses surviving
long sliced background builds — so removing it is not on the table.

Two workable resolutions, in preference order:

1. **Keep touch off the sampler.** Split servicing so `btnsample` does buttons only
   and touch is serviced from the loop task, relying on `popTouchTap`/`popSwipe`
   for refresh survival — the mechanism the SDK provides precisely for this. Closest
   to upstream, no new mutex.
2. **HAL-owned I2C mutex**, the same discipline `HalStorage` applies to SPI, if
   touch must stay on the sampler.

Option 1 needs a check that `InputManager` tolerates button and touch servicing on
different tasks; if it does not, option 2 is the fallback.

**Update 2026-08-16 — there is no third option on X4 Pro.** Upstream *does* now
have a multi-I2C mechanism (`a5109872`): `BoardProfile::batteryGauge.i2cBus`
selects `Wire` or `Wire1`, and on **Sticky** the fuel gauge is moved to `Wire1`
"so it doesn't fight the GT911 touch, which owns `Wire`". That looks like a clean
escape from bus contention — but it does not apply to our lead board.

The SDK's `XTEINK_X4_PRO` profile puts **all three peripherals on bus 0**:

| Device | Addr | Bus | Pins |
|---|---|---|---|
| GT911 touch | `0x5D` (alt `0x14`) | 0 (`Wire`) | SDA 39 / SCL 38 @ 400 kHz |
| CW2017 fuel gauge | `0x63` | 0 (`Wire`) | SDA 39 / SCL 38 @ 400 kHz |
| BM8563 RTC (PCF8563-compatible) | `0x51` | 0 (`Wire`) | SDA 39 / SCL 38 @ 400 kHz |

The profile comments say so directly — the RTC is "on the shared touch bus", and
the gauge is on "the SHARED touch/RTC bus … Wire". The X4 Pro silicon exposes a
second I2C controller, but the *board* wires all three devices to one pair of
pins, so `i2cBus` cannot separate them.

Therefore **P1 must be resolved by resolution 1 or 2 — bus separation is not
available on X4 Pro.** Resolution 1 remains preferred. The `i2cBus` field is
still worth carrying when we port the battery work (`6acecd8e`), because LilyGo
and Sticky can use it; it just does not rescue the lead board.

Note also that with three devices on one 400 kHz bus, the *combined* traffic
matters: a GT911 contact read at the sampler's 10 ms cadence is far more bus
traffic than the RTC and gauge polls put together. Whichever resolution is
chosen, the touch poll interval should be a tuned number, not inherited from the
button sampler's 10 ms by accident.

### P1 resolved — 2026-08-16

Reading `InputManager` (the check open question 1 asked for) settles this, and
corrects two claims in the draft above.

**Correction 1 — "upstream has no sampler, so there is nothing to copy" is
wrong about the SDK.** Upstream has no *HalGPIO-level* sampler, but the SDK ships
one:

```cpp
void InputManager::beginAsync(uint8_t taskPriority, uint32_t pollMs, uint8_t queueLen);
// creates task "fi_input", stack 4096, loops: update() → popPress/popTouchTap/
// popSwipe/popMultiTouchSwipe queues → vTaskDelay(pollMs)
```

That is the same shape as our `btnsample`, at **4096 bytes** rather than 2048.
Upstream simply never calls it. So the stack hazard has an authoritative number:
**the SDK sizes a task that runs `update()` at 4096.** Our 2048 was measured
against `analogRead` only, and is not safe once `update()` also walks the GT911
path.

**Correction 2 — resolution 1 is not implementable against today's SDK.**
`serviceTouch()` is **private** ([InputManager.h:279]) and `update()` is the only
public entry point, at line 26. `update()` *always* services touch. There is no
public way to run buttons on one task and touch on another, so "keep touch off
the sampler" cannot be done in our fork alone — it needs an SDK change (making
`serviceTouch()` public, or an `update(bool includeTouch)` overload).

Relatedly, the draft's phrasing "service touch from the loop task, relying on
`popTouchTap`/`popSwipe`" is self-defeating: those queues are filled *only* by
the `beginAsync` task. If touch is serviced from the loop task, `beginAsync` is
not running and the queues are always empty.

**`beginAsync()` is also not a drop-in replacement for our sampler.**
`popPress()` reports a button id on the press edge only — no release edge, no
timestamps. Our `ButtonEventManager` FSM classifies `PressType {Short, Double,
Long}` over 9 buttons and needs both edges plus timing, which is precisely why
`HalGPIO`'s `ButtonEdge` queue exists. Running `beginAsync` *and* `btnsample`
together is not an option either: both call `update()`, so they would
double-service the machine and race its one-shot edge flags.

**Conclusion.** The bus hazard has to be solved in our fork regardless of where
touch is serviced, because `HalClock` drives the RTC over `Wire` from the loop
task ([HalClock.cpp:276-306](../lib/hal/HalClock.cpp#L276-L306)) and the gauge
will too. So:

> **Adopt resolution 2 — a HAL-owned I2C mutex — and raise the `btnsample`
> stack from 2048 to 4096 when `FREEINK_CAP_TOUCH` is on.**

Rationale: resolution 2 is the only one implementable entirely in our fork; it
is required anyway for the loop-task RTC/gauge traffic on X4 Pro's shared bus;
and it keeps the `ButtonEdge`/`ButtonEventManager` press-type FSM intact. The
mutex follows the discipline `HalStorage` already applies to SPI.

Resolution 1 stays on the table as a **later SDK contribution** (split
`update()` so touch can be serviced independently). It would let touch run at
its own cadence instead of the button sampler's 10 ms — worth doing, but it is
an optimisation, not a prerequisite, and it should not block phases 1–2.

Two follow-ups this creates:

- Re-measure `samplerStackHighWater()` on an X4 Pro once the GT911 path runs;
  4096 is the SDK's number for its own task, not a measurement of ours.
- The mutex must cover `HalClock`, the battery gauge, *and* `sampleOnce()`'s
  `inputMgr.update()`. Note `sampleOnce()` deliberately keeps `update()` outside
  its `portENTER_CRITICAL(&inputMux_)` section — the I2C mutex is a separate
  lock and must not be taken inside that critical section.

**P2 — No hover, no free feedback.** A finger on glass gives no confirmation, and
on e-paper acknowledgement costs a refresh (~200 ms fast, 1–2 s full). Upstream
answers this with FUI's tap-flash (`clearTapFlash`, `setFlash`) — which is *inside*
the layer we are deferring. Under Option B we need our own answer in phase 4.
This is the most likely place for Option B to feel worse than upstream.

**Downgraded 2026-08-16.** Two things landed that make this cheaper on both
paths:

- SDK `cc89c653` adds `ListNav::rowRectFor(index, &rect)` — the exact rectangle
  of any drawn row, up to `MAX_ROW_RECTS = 16`, with an honest `false` when a row
  wasn't tracked so the caller can fall back to a full repaint. That is the
  primitive a tap-flash needs: invert one row's rect and issue a partial refresh
  bounded to it, instead of a full-screen update.
- Upstream `57c389c0` ("Add touch-down visual feedback to settings menus") is a
  worked example of the same idea applied to non-FUI screens.

So P2 is no longer "invent it ourselves" under 4a — it is "port `57c389c0`'s
approach", and under 4b it is free. **P2 is no longer a serious argument for
choosing 4b over 4a.** The remaining arguments for 4b (doing `MenuListActivity`'s
8 subclasses once rather than twice, and staying on a maintained mainline) are
unaffected and still stand.

**P3 — Accidental touch while reading.** A thumb resting on the panel cannot
misfire a button but can misfire a capacitive panel. Upstream's answer is
`touchReaderControls = TOUCH_READER_OFF` plus zone restriction;
`suppressTouchContact()` is available for consuming a contact.

**P4 — Drift.** Every phase we implement differently from upstream widens a fork
that is already 2973/1153 diverged. Hence the rule: **names and signatures copied
verbatim**, differences confined to *which* layers we take, never *what they look
like*.

---

## 6. Scope note

Per [SCOPE.md](../SCOPE.md): phases 0–3 cost nothing on C3 (all behind
`FREEINK_CAP_TOUCH`) and make two touch-only boards usable — clearly in scope.
Phase 4 is arithmetic against rects we already compute. Phase 5 is a metrics
change.

Phase 6 is where this could become a general-purpose touch UI framework. Stop at
"the reader and its lists work by finger".

## 7. Open questions

1. ~~**P1**~~ — **CLOSED 2026-08-16.** The `InputManager` read is done; see
   "P1 resolved" in §5. `serviceTouch()` is private and `update()` always
   services touch, so resolution 1 needs an SDK change; bus separation is
   unavailable on X4 Pro (all three devices on bus 0). **Decision: resolution 2
   (HAL I2C mutex) + `btnsample` stack 2048 → 4096 under `FREEINK_CAP_TOUCH`.**
   Phases 1–2 are unblocked.
2. ~~**P2**~~ — **downgraded 2026-08-16.** `ListNav::rowRectFor()` (SDK
   `cc89c653`) plus upstream `57c389c0` give a bounded partial-refresh tap flash
   on *either* path. No longer a decision driver.
3. **Phase 4 split** — 4a or 4b for the lists? Still the real decision, but the
   grounds have shifted: P2 no longer favours 4b, while the touch stack landing
   on mainline `develop` (§0.1) *does* favour it — reference diffs are now stable
   rather than hostage to a feature branch. Net: unchanged recommendation
   (4a for chrome, 4b for lists), stronger reason.
4. ~~Primary bring-up board?~~ **Answered: X4 Pro**, which also has the capacitive
   Home key the T5S3 lacks — so phase 3 should cover `wasHomeKeyTapped()` /
   `wasHomeKeyLongPressed()`, not just tap/swipe.
5. Are `papermono` / `sticky` in scope for us at all, or X4 Pro + T5S3 only?
6. ~~Does the FUI conversion wait for the reader/Stage-1 work?~~ **No** — that work
   is parked (2026-08-14), so nothing needs sequencing around it. The phase 4
   decision is now purely about long-term maintainability and flash budget.

7. ~~**Multi-touch**~~ — **ANSWERED 2026-09-01, and the §0.3 recommendation
   reversed.** A two-finger gesture is for the two things every user already has
   muscle memory for: pinch resizes the text, and a two-finger turn rotates the
   page. Both needed an SDK addition (pinch was explicitly rejected there) and
   both now ship bound by default. See §8.

Phase 0 also no longer needs to add `env:x4pro` or fix the S3 build — workstream A
did both, and the `freeink-sdk` submodule is now at **`cc89c653`** (was `76e61c4`;
bumped when master merged in on 2026-08-16). What remains of phase 0 is the GT911
bring-up itself and the P1 decision.

---

## 8. Phase 7 — bindable gestures and the reading light (2026-09-01)

Not in the original plan. Open question 7 asked what a two-finger gesture would
be *for* on a reader; this answers it, and generalises the answer.

### 8.1 The model: a gesture is a button

Eighteen gestures — four swipes, five tap zones, the same five as long taps,
pinch in/out, rotation either way — each carry a `CrossPointSettings::
BUTTON_ACTION`, chosen from the identical option list a physical key offers.
`GestureEventManager` is the counterpart of `ButtonEventManager`: it classifies
the contact, resolves the action, and hands it to the *same* `runAction()` ladder
in `main.cpp` that buttons use. That ladder was extracted rather than copied, so
a gesture bound to "Reader Menu" cannot drift from a button bound to it.

`BTN_DEFAULT` keeps its usual meaning — "whatever this input already did" — and
that is what makes the layer safe to add on top of touch handling that is already
device-validated:

* **Unbound is untouched.** `consumeAction()` peeks; it never claims a contact
  whose gesture is `BTN_DEFAULT`. The reader then interprets that contact exactly
  as it did before this existed.
* **Acting claims.** When a gesture *is* bound and fires, `suppressTouchContact()`
  stops the same contact also reaching the reader, so one tap cannot be both a
  gesture action and a page turn.

This only works because the SDK's tap and swipe reads are pure `const` peeks at
latched state rather than consuming calls. `peekScreenLongPress()` was added to
complete the set — `wasScreenLongPress()` suppresses as it reads, which is right
for a caller that has already decided.

### 8.2 Scope: everything in the reader, swipes everywhere

In the reader every gesture is live — the page has no touch targets of its own to
compete with.

Everywhere else **only swipes and two-finger gestures are**. A tap on a list row
is that row, and no arbitration between the two would be predictable, so taps and
long taps stay reader-only. Swipes are safe to open up because nothing outside
the reader consumes one (the sole `wasSwipe()` consumer in the tree is
`detectTouchPageTurn`) — and opening them is what puts the reading light within
reach on the home screen in the dark, which the reader-only rule left with no
answer at all.

Outside the reader a gesture bound to a **reader-scoped** action is declined
rather than swallowed, so the screen underneath still gets its touch. The
predicate is `CrossPointSettings::isReaderScopedAction()`, shared with the button
path, which needs the same answer for the same reason.

### 8.3 The zones are a superset, not a replacement

`TapZones::zoneFor()` splits the screen with the **horizontal** thirds tested
first, because that is what the reader already did: the outer thirds have been
the page-turn zones over their whole height, and the reader menu has been the
centre third of the centre column. Top and Bottom are the two remaining cells of
the centre column, which nothing had ever used. `test/tap_zones` asserts the
Centre band point-by-point against the rectangle `isTouchMenuTap()` accepted, so
a centre tap cannot stop opening the menu.

Binding a zone to `BTN_IGNORE` is therefore how a tap zone gets switched off.

### 8.4 Tap zones stay Built-in, deliberately

No shipped default binds a tap zone, and this is not timidity. The reader's own
tap handling is what implements Touch Reading Controls (Off / Tap / Swipe /
Inverted tap) **and** the end-of-book flow, neither of which
`dispatchButtonAction(BTN_PAGE_BACK)` goes through. A tap zone bound to
`BTN_PAGE_BACK` would look identical on a normal page and quietly bypass both.
"Tap left = previous page" is already true through the built-in path.

### 8.5 Shipped defaults

Light-forward, following Kobo/Kindle for the vertical swipes, CrossInk
(`upstream/feature/crossink-controls-port`, Julia Nguyen) for chapter-skip and
lookup on a hold, and universal convention for pinch and rotate:

| Gesture | Default | Gesture | Default |
|---|---|---|---|
| Swipe down, left half | Reader Menu | Long tap left | Previous Chapter |
| Swipe down, right half | Light Dimmer | Long tap right | Next Chapter |
| Swipe up, right half | Light Brighter | Long tap centre | Dictionary |
| Swipe up, left half | **Toggle Reading Light** | Long tap top | Toggle Reading Light |
| Swipe left/right | Built-in | Long tap bottom | Star Page |
| All five tap zones | Built-in | Pinch in / out | Smaller / Larger Text |
| Rotate clockwise | Change Orientation | Rotate anticlockwise | Change Orientation Back |

The two rotations are a **pair**, and needed `BTN_CYCLE_ORIENTATION_BACK` to be
one: turning two fingers one way and then back must undo, not advance three more
steps. Same reasoning as the directional font sizes — a single cycling action is
fine on a button and useless on a reversible gesture. Which visual direction
"forward" is depends on the `ORIENTATION` enum's order, which its names do not
make obvious; if it reads inverted on hardware, swapping the two defaults is a
one-line change.

**The quick light on/off is swipe up on the left half**, and it has to be a swipe
rather than a tap or a hold for two reasons: swipes are the only gestures live
outside the reader (§8.2), and reaching the light from the home screen at night
is most of what a quick toggle is for. It also has to exist at all — dimming
clamps at `MIN_BRIGHTNESS`, so there is deliberately no way to reach "off" by
swiping down, while turning the light *on* needs no toggle (adjusting brightness
while it is off lights it, the "obvious I want light" inference both reference
firmwares make). Long tap top keeps the same action as the deliberate,
hard-to-hit-by-accident version inside the reader.

Brightness steps by 5, matching CrossInk's panel. Ten was too coarse: the SDK's
gamma-1.6554 curve puts most of the usable range in the bottom third, where a
10-point step is a large visible jump.

**These take nothing away** — see §8.6.

### 8.6 Left half is the menu, right half is the light

Binding a vertical swipe to the light collides with the reader menu, which owns
the top-edge down-swipe. The answer is the one every phone already uses: a
downward swipe means different things depending on **which half of the screen it
starts in** — notification shade on the left, quick settings on the right.

So the two vertical swipes are four gestures, split on the start point:

| | left half | right half |
|---|---|---|
| swipe down | Reader Menu | Light Dimmer |
| swipe up | *(free)* | Light Brighter |

The **start** point, not the end: a downward swipe travels, and where the finger
ends up says nothing about which control the reader reached for. Horizontal
swipes are deliberately not split — they are the page turn in Swipe mode, and a
page turn does not care which half of the page it began on.

Swipe-up-left is left unbound rather than filled in for symmetry. There is no
obvious counterpart to "open the menu", and an unused gesture is better than a
surprising one.

Two earlier attempts at this collision are worth recording, because both looked
reasonable and both were worse:

* **Move the menu to the bottom edge on boards with a light** (what CrossInk
  does). It only relocates the collision — the defaults bound *both* vertical
  directions, so whichever edge the menu sat on would be claimed first.
* **Have the gesture layer decline the menu's own edge swipe.** That worked, but
  the rule "your bound swipe does nothing if you happen to start it at one
  particular edge" is not something a user can predict from the settings screen.

The split beats both because it is visible in the settings rows themselves: the
row is called "Swipe down, left half", so what it does and where is on the
screen in front of you. `wasMenuGesture()` is therefore back to the top-edge
down-swipe on every board, which is where it has always been — it remains the
built-in for both down-swipe rows, and being edge-anchored it is *narrower* than
the rows that supersede it.

### 8.7 Two ordering rules that are easy to break

**Board-gated actions come in NARROWING order.** There are two gated blocks now:
the light (`BTN_LIGHT_TOGGLE/BRIGHTER/DIMMER`) and, after it, the warm/cool pair
(`BTN_LIGHT_WARMER/COOLER`) which needs a second LED channel most lit boards do
not have. Warm is a strict subset of lit, so it must come last — dropping it on a
single-channel light then leaves the wider block's values untouched.
`dropUnsupportedActions()` takes both capabilities for the same reason.

**`BUTTON_ACTION` is positional.** `SettingsList` builds each option list by
position — index *i* is action value *i* — and drops board-gated actions on
boards that cannot perform them. Those actions must therefore be the **last**
entries of the enum, or dropping them renumbers everything after them and a
settings file written on one board is misread on another. There is an `assert`
in `buildSettingsList()` holding the list to `BUTTON_ACTION_COUNT`, because the
failure mode is not a crash but every stored mapping silently shifting by one.

**A default must never name a board-gated action** without
`dropUnsupportedActions()` to clean up after it. Three of the shipped defaults do
name one (the light), so `main.cpp` calls that immediately after
`Frontlight.begin()` — which is also what rescues an SD card moved from a board
with a frontlight to one without, where the stored action would otherwise be both
inert and uneditable.

### 8.8 Every touch event is latched

The SDK reports tap, swipe and long press as one-shot flags cleared by its next
`update()` — a ~10 ms life at our 10 ms sampler cadence. Buttons have been
latched into a ring for that reason since the sampler existed; touch was left as
a live passthrough because it was device-validated as it stood.

**Device data settled it.** In one session 1 tap in 15 was dropped: the sampler
line reported `tap=1` and no `[TCH] list tap` line followed, i.e. the loop never
saw it. The dropped one came 2.6 s into an idle stretch, and
`IDLE_LIGHT_SLEEP_MS` is 1000 — so the loop was almost certainly light-sleeping
between polls. That makes the drops likeliest **while someone sits on a menu
deciding what to tap**, not during a busy render, which is the worse of the two.

So `HalGPIO` latches single-contact events the same way, and `update()` moves
ONE into a loop-side snapshot. Three properties matter:

* **Replaced, not merged.** Each event is visible for exactly one drain cycle,
  the contract `snapPressed_` already has.
* **Non-consuming within the cycle.** Several layers read the same tap in turn —
  the gesture classifier looks, decides, and only then suppresses it before the
  screen underneath reads it (§8.1). A pop-per-reader queue would have broken
  that outright, which is why this is a snapshot and not a queue at the API.
* **Suppression drops what is queued.** `suppressTouchContact()` now also clears
  the ring and the snapshot. The SDK latch can only stop events it has not
  produced yet, and after a slow tick the tap that follows a long press may
  already be queued — without this, "ignore the rest of this contact" would not.

Level states (`isTouchTapCandidate`, `isTouchHeldAt`) stay live reads: "where is
the finger now" has no meaning latched.

One consequence worth expecting: swipes made during a page refresh now queue and
replay one per tick instead of being dropped. That is deliberate and matches what
the button edge ring already does for a burst of presses during a slow slice.

Activity transitions flush the ring (`flushTouchEvents()`, from
`ButtonEventManager::drain()`), which matters more than it did — an event now
outlives the tick it happened in.

### 8.9 Orientation must be sampled once

`getScreenWidth()` and `tapToLogical()` both read the **live draw** orientation,
which the themes flip to Portrait mid-pass to put the hint strips on the panel
edge. A loop-task caller that reads width, then height, then maps a tap can
resolve each against a different frame and zone a landscape tap as though it were
portrait — the same class of bug as issue #87. `GestureEventManager` reads
`getHeldOrientation()` **once** and passes it to orientation-explicit overloads
(`getScreenWidth(o)`, `wasSwipeIn(o)`, `wasScreenTappedIn(o)`,
`peekScreenLongPressIn(o)`). `wasEdgeSwipe()` was fixed the same way, and also
switched from the live orientation to the held one: every caller of it is on the
loop task, so it had the same exposure.

### 8.9b Single-push grayscale (T5S3)

SDK PR #51 gives the ED047TC2 a fast bank whose grey columns **self-normalize**:
each grey drive saturates at the white rail, then walks down to its level. That
is only correct when nothing has driven the pixel first — i.e. under one push.

Adopting the panel without adopting that path produced a visible fault: the B/W
base drove the anti-aliased fringes BLACK, the overlay then saturated them to
WHITE (a flash), and they settled too light, so the anti-aliasing read as having
vanished. Reported from hardware as "three updates instead of two".

The reader now has a third AA strategy beside inline (X4) and deferred (X3),
chosen on `renderer.supportsGrayFrame()`. Its one structural difference is
**order**: the planes are staged BEFORE the page is rendered, not after.

    warm image caches            (unchanged, and what makes this legal)
    stage LSB plane, stage MSB   <- was after the page
    clearScreen
    render page + status bar
    displayGrayscaleFrame(mode)  <- ONE waveform

Both the planes and the page use the framebuffer as scratch, so whichever runs
last is the one left in it — and `displayGrayFrame()` needs the intact B/W page
there. The reorder costs nothing: three renders happen either way, and it removes
the whole second refresh plus the restore write.

**The planes are captured by the page render, not rendered separately.** Adopted
from jetaudio's crosspoint-aurora (`GfxRenderer::beginGrayCapture` / `captureGray`),
which is the reference consumer of this waveform.

Aurora hooks a per-pixel callback in its glyph loop. This fork's 2-bit path is a
fused gather+threshold that builds a row/column mask per 8-pixel chunk, so a
per-pixel callback would undo the thing it was optimised for. Same effect is had
one level up: while capture is armed, the BW dispatch runs the glyph blit twice
more with each plane's own draw mask, into caller-owned panel-native planes. The
page walk, the layout and the glyph decode — where the cost actually is — still
happen exactly once.

What that removed: two full page renders per page (~80 ms), the plane-before-page
reorder, and the abort predicate (there is no separate pass left to abort). Costs
two full-page plane buffers, ~130 KB together here, allocated per render with
`makeUniqueNoThrow` and falling back to the staged path on OOM.

Two guards come from aurora as well: image pages stay on the staged path, because
the capture rides the GLYPH blit and an image's own greys would simply not be in
the planes; and night mode falls back, since inverted output inverts the planes'
meaning (the SDK's `displayGrayscaleFrame` already checks `_inverted`).

**The pre-rendered path had to follow.** Background-A pre-renders the next page's
B/W into the back framebuffer, and a page turn hits it almost every time — so
leaving that path on the old two-push AA replay meant the single push was the
exception, not the rule, and a pre-rendered page looked different from a freshly
rendered one. It now stages the planes during the pre-render and displays in one
push too.

The planes must be staged during the PRE-RENDER, not at display time, for the
same reason the order flipped above: both they and the page use the framebuffer
as scratch, and the page has to be the survivor. At display time the pre-rendered
page is already sitting in that buffer and staging would destroy it.

`preRenderedPlanesStaged_` is therefore cleared everywhere `preRenderedPage.ready`
is — a stale plane pair belongs to a page no longer in the framebuffer. The
status bar carries no anti-aliasing on either path, because it is superimposed at
display time and the plane lambda is page content only.

Two consequences worth knowing:

* `aaPreemptedByNavigation()` is consulted earlier than the inline path consults
  it. `stageGrayscalePlanes()` still re-checks between planes, so a turn arriving
  during the page render aborts the pass; it just cannot un-stage a plane already
  written, which costs nothing — an unused staged plane is never displayed.
* `completeDisplay()` after a single push is a no-op (`syncPendingAsync()` with
  nothing pending), so the existing trigger/complete structure needed no change.

`FreeInkDisplay::displayGrayscaleFrame()` was missing `swapBuffers()`, which
`displayBuffer()` does. Fixed in the SDK branch: it displays the frame, so the
buffer just shown has to become the previous one, or every consumer that tracks
what is on the panel works from the page before this one.

### 8.9c Leaving the reader needs a clean baseline

Every other reader (Txt, Md, Xtc, the BMP viewer) calls
`ReaderUtils::enforceExitFullRefresh()` on the way out. The EPUB reader never did,
and got away with it because the two-push AA ended by reseeding the differential
baseline (`cleanupGrayscaleWithPreviousBuffer`).

The single push deliberately does no such cleanup — aurora's comment says why:
*"the panel's canvas holding the finished page IS the baseline the next push diffs
against"*. So the greys are still on the glass when the reader closes, and the
home screen's FAST diff drives them from a level the differential bank's columns
do not start from. That showed on device as heavy ghosting across Home and
Settings.

Armed only when anti-aliasing actually ran: a plain B/W page leaves the panel on
its rails already, and a clean-bank refresh costs about a second and a half.

### 8.10 The reading light

The SDK has carried `FrontlightManager` and a backlight entry in the T5S3 profile
all along; this fork simply never linked it. `HalFrontlight` is ported verbatim
from upstream (crosspoint-reader#2983). Upstream's `FrontlightPanelActivity` is
**not** ported — it is built on `UiAppHost` / `fui::sheet` / `sliderRow` /
`tileGrid`, none of which this fork has adopted (see §3). The controls are a
Display → Reading light submenu plus three bindable actions instead.

`frontlightOn` is a `DynamicToggle` because the live hardware, not the setting, is
the authority for "is the light on" — after a wake with Restore off they
legitimately disagree. That means `JsonSettingsIO`'s generic loop skips it (no
`valuePtr`), so it is saved and loaded by hand, next to the `fontFamily`
precedent.

### 8.11 Silencing touch outside the reader

`touchUiControls` gates every touch **event** query on `MappedInputManager` —
taps, long presses, drags, swipes, edge gestures, multi-touch — through private
`raw*()` wrappers, so a query added later cannot forget the gate. `hasTouch()` is
deliberately *not* gated: screens ask it to decide layout, and a board does not
stop having a digitiser because its owner turned touch navigation off.

`main.cpp` refreshes the gate every tick from the setting and the activity on top,
leaving the reader always enabled — it has `touchReaderControls`, and one
behaviour with two switches is a support question waiting to happen. Reader
*sub*-screens (contents, dictionary, the menu) are UI screens and are gated with
the rest. `BTN_TOGGLE_TOUCH_UI` flips it from a button or gesture; because the
reader is never gated, a gesture bound to it can always turn touch back on.

Nobody is stranded by turning it off: on a board whose Back/Confirm come from the
capacitive Home key, `HalGPIO` synthesises those as **button** edges below this
layer.

### 8.12 Diagnostics, and the first device attempt

The first hardware attempt (2026-09-01) produced no gesture actions at all. The
firmware under test predated §8.5, so every gesture was still `BTN_DEFAULT` and
`consumeAction()` returned on `anyBound()` before reading the contact — which is
correct behaviour, and from the outside indistinguishable from a broken layer.

That is a bad failure mode to have twice, so the build now traces both ends of
the path under `BUTTON_TRACE` (already on for `lilygo_t5s3`):

* `[TCH] release: held=… tap=… swipe=… travel=…,… px` — printed from the
  **sampler** task at the release edge, where the SDK's one-shot flags are still
  fresh. Travel is measured by the trace itself in panel-native pixels, because
  the SDK only fills in its own endpoints once it has already decided the contact
  *was* a swipe, which is no use when the question is why it decided otherwise.
* `[GEST] <gesture> -> action=… (claimed | unbound…)` — printed from the **loop**,
  saying what it did about it. `no gesture is bound to an action` covers the
  all-default case above.

A gesture that appears in the first line but not the second was missed by a busy
loop; one that appears in both as "unbound" is a settings question. Those want
completely different fixes, which is the point of having both.

Thresholds worth knowing when reading a trace (`InputManager`, private, so the
trace states them as literals): tap slop 28 px, swipe ≥ 60 px within 700 ms, long
press at 500 ms — all in panel-native pixels.

**Two-finger swipes do nothing, by design.** The SDK routes multi-touch
translation to its own queue, which nothing here reads; only pinch and rotation
are wired. A two-finger swipe also sets `touchMultiContactSequence`, which
suppresses the single-contact classifiers until full release — so it will not
fall back to being a one-finger swipe either.

### 8.13 A changed default does not reach a device that has saved its settings

`BTN_DEFAULT` is 0, which makes "the user chose Built-in" and "this key was
written before that default existed" the same byte on disk. `fromJson()` prefers
the stored value, so a default changed after a device has saved its settings once
reaches nobody — and the symptom is a gesture that silently does nothing, which
looks exactly like a firmware bug. Two device sessions went on that before it was
understood.

`CrossPointSettings::GESTURE_DEFAULTS_VERSION` fixes it: the file carries a
`gestureDefaultsV` stamp, and when it is older than the constant the loader
IGNORES every gesture key and keeps the compiled defaults. Absent counts as
older, so files written before the stamp existed are covered.

A bump discards gesture customisation, deliberately — while the defaults are
still being tuned on hardware that is the useful trade. **Stop bumping it once
they settle** and the mechanism goes inert on its own.

The general lesson is not specific to gestures: any setting whose "unset" value
is also a meaningful choice needs something like this before its default can ever
be changed. The existing precedent in the tree is renaming the JSON key
(`fastAntiAliasing` → `fastAntiAliasingV2`), which works for one flag and does
not scale to twenty.

### 8.14 A list is only tappable if its screen says so

`ActivityManager::dispatchListTap()` resolves a tap to an item index from the
band the render published, then hands it to `Activity::selectListRow()` — whose
**base implementation returns `Rejected`**. A screen that draws through
`GUI.drawList` but never overrides it therefore records a perfectly good band,
resolves the right row, and does nothing, on every page.

That is how `EnumSelectionActivity` — the full-screen enum picker, i.e. every
gesture and button action row — shipped. It was reported as "cannot tap a list
item after scrolling to the second page", which is where it was noticed rather
than where it began: the screen had never been tappable, and the second page is
simply where a 30-option list makes you look.

The audit is mechanical — every file that calls `drawList` / `drawWrappedList` /
`recordUniformRows` must either override `selectListRow` or inherit
`MenuListActivity`. Two files legitimately do neither and say so:
`ButtonRemapActivity` (a wizard, not a picker) and `RecentBooksActivity` (routes
`listTouch()` itself, because its grid view is not a band).

The `Rejected` branch now logs. A silent return there is indistinguishable from a
tap that never arrived and from a band recorded for the wrong rows, and those
three want completely different fixes.

### 8.15 Status

Builds on `default` (C3), `x4pro` and `lilygo_t5s3`; 766 host tests green,
including the new `test/tap_zones` and `test/font_size_ladder`. **Not yet device
validated** — the first attempt is written up in §8.12 and did not reach the
code under test. The gesture classification, the suppression handshake and the
backlight all still need hardware.

---

## 9. Phase 8 — closing the remaining gaps (2026-09-07)

An audit of every activity against the three recorders and the reader gesture path
found six things touch could not do; tappable footnote markers (§9.10) were asked for
on top of them. All seven are now closed. **Code complete, not device validated.**

### 9.1 The carousel drew a cover you could not tap

`LyraCarouselTheme`'s side covers **wrap** — the slot left of the first book shows
the last one, the way Left/Right wrap — and are gated on the book count (`>= 3`
for a left neighbour, `>= 2` for a right one). `recordCarouselCoverTargets` did
neither: it clamped at `centerIdx - 1 >= 0` / `centerIdx + 1 < bookCount`. Three
consequences, the first of them on the position the home screen opens at:

| Case | Drawn | Recorded |
|---|---|---|
| `centre == 0`, 3+ books | left cover = last book | nothing — **dead tap** |
| `centre == n-1`, 3+ books | right cover = book 0 | nothing — **dead tap** |
| exactly 2 books, `centre == 1` | no left cover; right cover = book 0 | a LEFT target for book 0 — **phantom** |

The comment above the recorder claimed "no wrap-around, matching the draw", which
was simply false about the code below it.

Fixed by giving both sides one source: `CarouselCoverLayout::compute()` returns the
three slots with their geometry and the book each one shows (`-1` when a slot is not
drawn), and the draw and the recorder both read it. Header-only and dependency-free,
so `test/carousel_cover_layout` round-trips it — compute a slot, hit-test the pixel
it was drawn at, assert the book that comes back is the one painted there — across
every centre position for 3..12 books. That is the gate `test/cover_grid_layout` and
`test/slider_geometry` already apply, and the one this code did not have because the
arithmetic lived in an anonymous namespace inside the theme.

### 9.2 The on-screen keyboard was untappable

Every WiFi password, OPDS URL and KOReader login was typed by walking a cursor over
forty-odd keys — the last screen in the firmware with a selection and no touch. The
grid is uniform arithmetic, so it needs a hit test rather than a recorder
(`TapTargets` holds ten; the keyboard has ~45): `KeyboardGrid::hitTest()` in
`src/components/`, next to the other layout inverses, with `test/keyboard_grid`
round-tripping every key on a spaced and a tight theme.

Two decisions worth keeping:

- **Single tap types.** Point-then-confirm exists because activating the wrong row
  can be expensive to undo; a mistyped letter costs one Del, and two taps per letter
  would make the keyboard slower by finger than by the cursor it replaces. Same
  argument as the settings tab bar.
- **The grids are published by the draw**, not recomputed on the loop task. The
  character block is bottom-aligned on all four shipped themes, but a theme that is
  not derives its origin from the wrapped height of the entered text — and that wrap
  only happens inside `render()`.

### 9.3 `DictionaryWordSelectActivity` hit-tests its own words

Hundreds of selectable words per page, far past `TapTargets`' capacity, and the
boxes are already there — so it walks its own fragments, as the RecentBooks grid
inverts its own layout. Point-then-confirm here earns its keep: a lookup searches
the index off the SD card and opens an activity.

The touch path deliberately falls THROUGH to the existing tail (`lastMoveMs`,
`speculateForSelection()`) rather than growing a second copy of it.

### 9.4 Long presses were unreachable by touch — on X4 Pro, unreachable at all

`injectPress()` shared one timestamp between its press and release edges, so
everything the hint strip synthesized classified Short. On X4 Pro that closed the
door completely: no Back or Confirm pin, and the capacitive home key also emits
press and release in the same pass, so **nothing on that board could produce a
hold**. Unreachable: the file browser's context menu (long Right, in any folder big
enough to page), the recents view toggle (long Up), remove-book (long Left) and
book-info (long Right).

`injectPress(index, longPress)` now backdates the press edge by
`HalGPIO::INJECTED_LONG_PRESS_MS`, and a **long tap on a hint box** produces it.
The classifier is never told the press came from a finger. Notes:

- The backdate is saturated: `millis()` is small for the first second after boot and
  an underflowed edge would sit ~49 days in the future and never classify.
- `INJECTED_LONG_PRESS_MS` lives in the HAL, which cannot include
  `ButtonEventManager.h`; `ActivityManager` `static_assert`s it against
  `LONG_PRESS_MS` where both are visible.
- peek + `suppressTouchContact()` rather than the consuming `wasScreenLongPress()`,
  so a long press that misses every box still degrades into the tap it would have
  been.

### 9.5 A vertical swipe pages a list

A tap reaches only the rows `ListTouchBand` recorded, i.e. the ones on screen;
everything below the fold was behind the hint strip's Left/Right boxes and nothing
else. `dispatchListSwipe()` injects the list PAGE button — logical Left/Right, which
`ButtonNavigator::onListPageNav` already implements for `MenuListActivity`'s eight
subclasses, and which the browser and the chapter selectors implement themselves —
so no screen learns a new verb and no `BUTTON_ACTION` is added.

Ordering matters twice, and both are load-bearing:

- **Before `dispatchListTap()`**, because the SDK reports a tap *and* a swipe for the
  same contact when the travel sits near the threshold; the swipe suppresses the
  contact when it claims one. Same ordering, same reason, as `GestureEventManager`.
- **After main.cpp's gesture dispatch**, so a swipe the user has BOUND to an action
  wins and this never doubles up with it.

Direction follows the content, not the finger: dragging the page up brings the rows
below into view.

### 9.6 The non-EPUB readers had no touch route to their own navigation

`detectTouchPageTurn` was wired in all four readers but `isTouchMenuGesture` only in
`EpubReaderActivity`, and the readers draw no hint strip — so in TXT, MD and XTC you
could page and nothing else. Worse, `LineReaderActivity` had **no `onButtonAction`
at all** while reporting `isReaderActivity() == true`, so main.cpp routed every
reader-scoped action to it and it dropped the lot: a gesture bound to "Next page"
worked in EPUB and did nothing in MD.

Both are fixed. The centre-third tap and the top-edge swipe now open whatever
navigation the reader has — MD's heading list, XTC's chapter list, nothing in plain
TXT — reached through the screen's own Confirm path so the two cannot diverge. That
also retires a small lie in `TouchGestures::builtinLabelFor`, which told the settings
screen a centre tap meant "Reader menu" on screens where it meant nothing.

### 9.7 Tap-to-page on the paged text views

`BookInfoActivity`'s description and `DictionaryDefinitionActivity`'s entry both page
with Left/Right and had no gesture. They now split their text band in half —
`TapZones::halfOfBand()`, tested in `test/tap_zones`.

**Bounded to the band, deliberately.** The reader's zones run the full height of the
panel, which is right for the reader (it draws no chrome) and wrong for these: they
draw a hint strip, and full-height zones would swallow the taps meant for it and
break Back. The split is also live only while there is more than one page, so a
short description leaves every tap alone.

### 9.8 Tapping a footnote marker or cross-reference

A tap on a marker in the reading text now follows the link, exactly as picking it out of the
footnote list does (`navigateToHref` with `savePosition`, so page-back returns to the caller).

**The scope is wider than "footnotes", and for free.** `ChapterHtmlSlimParser` collects EVERY
internal `<a href>` into `FootnoteEntry` — its own comment says "footnotes, cross-references" —
so `currentPageFootnotes` was already the page's link list. Nothing in the parser or the page
cache format had to change.

**Why the boxes have to be found again.** The parser attaches a link to a page by word COUNT and
keeps only its display text and href; where that text landed on screen is never recorded. Nor can
the reader defer the question to the tap: `renderContents()` takes the `Page` **by value** and
destroys it when the render ends, which is why `currentPageFootnotes` is `std::move`d out of the
page in the first place. So the markers are located during the render, by scanning the words the
page is about to draw.

The scan is cheap by construction: it compares stored word text (plain memory reads) and calls
`wordBox()` only for a word that matched — and `wordBox` reads a precomputed x from the block
rather than laying anything out. Nothing is allocated.

**Matching is a token walk, not `strcmp`** (`LinkMarkerMatch`, host-tested in
`test/link_marker_match`). Two reasons, both from how the text is stored:

- the parser NORMALISES a noteref on the way in — `"[12]"` is stored as `"12"` while the page
  still paints the brackets, so the page word has to be normalised by the same rule or every
  bracketed marker in the corpus misses;
- link text is not always one word (`"turn to 256"`), so the run spans several page words and
  their boxes are unioned.

Links are matched **in page order**, which is the invariant that keeps a marker `"1"` from being
claimed by an unrelated `"1"` earlier in the paragraph.

**Published from all three paths that put a page on screen** — the full render, the pre-rendered
fast display, and a page drawn from an in-progress Background-C build. Each shows a different
`Page` object; publishing from only one of them would be §9.1 over again.

**`readerLinks` is the one recorder NOT cleared per render pass.** The reader has passes that draw
without displaying — above all the pre-render, which fills the frame buffer with the NEXT page
while the current one is still on the panel — and a per-pass clear would leave the visible page's
links dead. It is cleared on activity transitions like the rest, every display path republishes
it (with an empty set when the page has no links), and the two passes that COVER the page (the
indexing popup, the empty-chapter message) clear it explicitly.

Three smaller decisions:

- **Single tap, not point-then-confirm.** Two steps would need somewhere to show the highlight,
  which a full page of text does not have; and the jump is the cheapest action in the reader to
  undo, because coming back is already a first-class gesture.
- **Tested before the page-turn zones and the centre-tap menu**, because a marker can sit
  anywhere on the page including inside one of them, and it is the more specific target.
- **The rect is padded and floored to 30 px.** A marker is often a single superscript digit a few
  pixels wide. Kept modest deliberately: these rects win over the page-turn zones, so an
  over-generous marker would make page turns unreliable in the text around it.

Gated on `touchReaderControls` like the reader's other touch paths, so turning touch reading
controls off still silences the reading surface completely.

### 9.9 Screen edge margin (per device, user-settable)

Not a touch gap, but it arrived with the same board: on the T5 S3 the plastic cover comes close
enough to the live pixels that text at the board profile's own inset is hard to read.

`BoardProfile::viewableInsets` already describes where the CASE sits over the glass, measured per
board. What it cannot describe is how much clearance a given reader wants beyond that. So a new
Display setting, **Screen Edge Margin: Narrow / Medium / Large**, adds 0 / 5 / 10 px uniformly on
top of whatever the profile declares. Narrow is the default and is exactly today's behaviour, so
no shipped device moves until someone asks it to.

Applied in `GfxRenderer::getOrientedViewableTRBL()`, which is the one place the profile insets are
read and rotated — so the reader's page margins (EPUB, TXT, MD) and the status bar all follow from
one change. Pushed from main.cpp's loop beside `setTextDarkness()`, the existing precedent, so a
change in Settings takes effect on the next render rather than at the next reboot.

Two things fall out for free and are worth stating:

- **Pagination re-flows by itself.** `layout.viewportWidth/Height` are derived from these margins
  and are part of the section cache key, so widening the inset rebuilds the section rather than
  showing stale page breaks.
- **It is inherently per device**, because settings live in the device's own SPIFFS. The X3/X4
  keep Narrow; only the board that needs it gets widened.

General UI chrome is unaffected: `UITheme::getContentRect()` does not consult the viewable insets
at all today, so only the reading surface and the status bar respect the bezel. Whether the rest
of the chrome should is a separate question this does not answer.

Cost: **+410 bytes flash, +24 bytes RAM.**

### 9.10 What is still open

- **P2, the localised tap flash.** Still the highest-value remaining item: the first
  tap of a two-step still costs a full-screen repaint, which is the only feedback
  e-paper affords. `grep` finds no `setFlash`/`clearTapFlash` in the tree.
- **Discoverability of the long press.** It is now reachable, but nothing on screen
  says a hint box can be held — and `FileBrowserActivity` drops its "Options" label
  entirely in a folder large enough to page, which was already true for buttons.
- **Device validation.** None of §9 has run on hardware.

### 9.11 Corrections to earlier sections

- §4 lists the settings **tab bar** as still open under 4a. It shipped:
  `SettingsActivity::loop()` hit-tests `TapTargets::tabBar()`, single-tap by design.

---

## 10. Phase 9 — resource gating, zone rework, self-documentation (2026-09-11)

Eleven commits on `perf/gate-touch-ui-on-non-touch-boards`. Prompted by three
questions: what do non-touch boards pay for this, should the vertical swipes be split
by where they end, and what should the defaults actually be. **Code complete, NOT
device validated.**

### 10.1 The app-side layer was never gated (`b7826239`)

`FREEINK_CAP_TOUCH` has always gated the SDK's touch backend. The half this fork owns
never was: the tap/row/target recorders, the gesture classifier, the three touch
dispatchers and the twenty gesture settings rows were compiled, linked and in two
places executed on X3/X4.

Measured on `env:default`, both builds against the same tree:

| | baseline | gated | delta |
|---|---|---|---|
| text | 6275269 | 6262441 | **-12828 B** |
| bss | 2542833 | 2542097 | **-736 B** |

Two costs show in neither figure: `BaseTheme::drawList` built a 280-byte
`ListTouchBand::Builder` on the **render stack** on every list draw, and
`buildSettingsList()` pushed twenty `SettingInfo` rows — each with its own ~66-byte
option vector — that `getSettingsList()` then deleted with `remove_if`, on every
settings **save and load**, not only when the settings screen opened.

`CP_TOUCH_UI` is **asserted in platformio.ini, not derived** from
`FREEINK_CAP_TOUCH`. The recorder headers are dependency-free so the host tests can
include them, and a macro that only exists once `BoardConfig.h` has been included
would give different TUs different answers about `sizeof(ListTouchBand::Band)` — an
ODR violation, not a missed optimisation. It defaults to 1 (keep touch) and
`MappedInputManager.cpp` static_asserts the two against each other.

### 10.2 The vertical swipes were split by screen half, and that was wrong

Three problems, all fixed by anchoring to the **edge columns** (`c8af60f2`):

1. **A half is not a place.** KOReader puts these in the outer eighths
   (`DSWIPE_ZONE_LEFT_EDGE`), Kobo on the left edge. Half the screen is most of the
   page, so a thumb resting mid-page could dim the display.
2. **The halves shadowed the reader menu.** `gestSwipeDownRight` shipped as Light
   Dimmer and fired anywhere in the right half — including the top edge, which is the
   built-in menu swipe. `GestureEventManager` runs before the activity loop and calls
   `suppressTouchContact()`, which zeroes the snapshot `wasSwipe()` reads
   (`HalGPIO::clearTouchEventState`), so **the menu gesture silently did nothing on
   half the screen.** A live bug, found by re-reading the interaction rather than by
   testing.
3. **`builtinLabelFor()` had to lie about it**, reporting "Reader Menu" as the
   built-in for a row that overlapped the real one.

The original request was to split by where the swipe **ends**. Declined, with reason:
end-position is a function of swipe *length* rather than of a place the reader picks,
and e-paper cannot animate a drag, so there is nothing to aim at while the finger
moves. All three reference firmwares anchor at the start.

`TapZones::edgeColumnFor()` excludes the top/bottom bands, without which a swipe
starting in a bottom corner would be both "open the menu" and "brighten the light" —
an overlap the half-split never had because it had no bands to collide with.

### 10.3 The defaults, and where each came from

| Gesture | Default | Source |
|---|---|---|
| Left edge column, up/down | Brightness ± | KOReader `defaults.lua`, Kobo |
| Right edge column, up/down | Warmth ± | KOReader (`RIGHT_EDGE` = warmth) |
| Top edge, swipe down | Light panel | Kindle quick settings, CrossInk |
| Bottom edge, swipe up | Reader menu | CrossInk (X4 Pro), phone bottom-sheet |
| Outward swipe in outer thirds | -/+10 pages | KOReader `double_tap_*_side = page_jmp` |
| Hold top-left corner | Toggle light | KOReader `tap_left_bottom_corner` |
| Pinch in/out | Font size -/+ | all three |
| Double-press Power | Toggle light | the one control no overlay can cover |

Rotate stays bound to orientation. It was proposed for unbinding (KOReader ships both
`rotate_cw`/`ccw` nil) and that was **rejected by the maintainer as useful** — recorded
because the role-model argument was the weaker one here.

### 10.4 Scope is decided by size, not by screen

Taps and the five long-press **zones** stay reader-only; a zone is a third of the glass
and would sit on whatever a screen has drawn. The four **corners** are live everywhere,
because an audit found that outside the reader the only other long-press consumer is
the button hint strip along the bottom — lists, covers, the tab bar and the keyboard
all resolve on taps.

That distinction exists to serve one requirement: **the light toggle means the same
thing on every screen.** A control that works in a book and silently does nothing on
the home screen is worse than one that does not exist. The alternative — a separate
home-screen gesture — was considered and rejected on those grounds.

### 10.5 Self-documentation

`GestureActionsOverviewActivity` (`ec8fab36`) draws the zones with each region's bound
action inside it, in three pages. Every string comes from `getSettingsList()` via
`getDisplayValue()`, looked up by the gesture's label out of `BINDINGS` — so there is
no parallel table to drift, "Built-in" rows show what `builtinLabelFor()` really does,
and board variance handles itself because filtered-out rows render empty.

`docs/touch-gestures.md` is the user-facing companion, and names the on-device page as
the authority over itself. Its 26 documented defaults were cross-checked against the
struct initialisers programmatically, which caught two wrong setting names.

### 10.6 Audit: nine unwired accessors (`6a84ee76`)

Eight deleted, one wired. `wasBackGesture()` — a rightward swipe from the left edge —
now goes Back outside the reader (excluded inside it, where a rightward swipe is
already previous-page; CrossInk documents the same carve-out). It earns its keep on the
T5S3, where Back was otherwise only a home-key hold.

Deleted: `wasScreenLongPress`, `peekScreenLongPress`, `wasTapInRect`, `rowTouch`,
`colTouch`, `getRenderer`, `touchEventsEnabled`, and `wasHomeKeyHold` — the last
replaced by a comment, because its absence is load-bearing: a Home-key hold **is**
handled, at the HAL layer, arriving as an ordinary Back press.

Five of the eight were upstream API taken across signature-for-signature during phases
3-4 to keep the diff comparable. That was right then; this is the cleanup it owed.

**Method note.** The first pass of this audit was wrong three times, always the same
way: reading `BoardConfig`'s pin tables and concluding something was unwired when the
wiring lived a layer below — the HAL's home-key routing, `dropUnsupportedActions()`,
and `InputManager::setButtonHook()` (which is how the T5S3's expander button becomes
`BTN_DOWN`). A scan that excludes the defining file also reports internal helpers as
dead. **Treat `BoardConfig` as necessary but not sufficient, and check the hook and HAL
layers before calling anything dead.**

### 10.8 Device test on a LilyGo T5S3 (2026-09-11)

The first hardware run of §10. It found more than the whole build-and-host-test chain
did, and two of the four findings were pre-existing bugs this work merely walked into.

**1. The corner light toggle worked in the reader.** The one thing that needed a thumb
to confirm — is a 67x67 px corner findable without looking — is confirmed on a 540 px
panel.

**2. A press on the carousel home screen opened a book, from outside every drawn
cover.** Two distinct faults behind one symptom.

The first was that the corner gestures were still reader-only in the flashed build, so
the press fell through unclaimed and its lift became an ordinary tap.

The second is the real bug, and it predates this branch.
`CarouselCoverLayout::compute()` puts the left tile at `centreX - sideW + overlap`,
which is **-40** on a 540 px panel — the tile is DRAWN clipped, but
`recordCarouselCoverTargets()` recorded the full unclipped rect. So
`x in [0,160), y in [141,531)` was tappable empty margin beside a partly-drawn cover,
and a press there opened a book that was not under the finger. Fixed by clamping on
the recording side (`clampToScreen`), with a test pinning the -40 geometry.

Worth noting how this was missed: §9.1 fixed the carousel's *wrap* logic and added
`test/carousel_cover_layout` to round-trip compute-then-hit-test. Every one of those
tests used a 480 px screen, where the left tile's x is +20 and nothing hangs off the
edge. The bug needed a NARROWER panel to appear, and the test suite had no narrow
panel in it.

**3. The ten-page swipe worked on the right and not the left.** Diagnosed wrongly
twice before the cause was accepted as ergonomic rather than logical: every code path
here is symmetric, so nothing in the source explains it. The original design required
the swipe to travel OUTWARD — leftward in the left third — which means starting inside
the third and still finding 60 px (`TOUCH_SWIPE_MIN_PX`) of room toward the bezel. That
is a narrow window, and which hand holds the device decides how reliably a thumb lands
in it.

Made direction-agnostic within the zone: begin anywhere in an outer third and flick
either way. `SwipeLeftInLeft`/`SwipeRightInRight` were renamed to
`SwipeInLeftZone`/`SwipeInRightZone`, since the old names described a direction
requirement that no longer exists.

That in turn collided with the left-edge back swipe wired in §10.6 — a rightward swipe
in the left third would have been both "Back" and "back ten pages". The left edge had
three claimants (Back, the ten-page jump, the brightness column), and **the back swipe
was dropped** as the weakest: it could never run in the reader anyway, Back is already
the capacitive home-key hold on both touch boards, and it had only been wired because
the accessor was lying around unused. Wiring a dead accessor because it exists is not a
reason to add a gesture, and it is how the contention arose.

**4. The gesture overview's later pages were unreachable.** `mapLabels(back, "", "", "")`
left the Prev/Next hint boxes empty, and an empty hint label marks its box INACTIVE — so
it is neither drawn nor tappable. On a board whose only physical keys are Down and the
capacitive home key, that left the later pages with no route and nothing on screen
saying they existed. Both fixed by labelling the boxes (which also makes them touch
targets, via `dispatchHintStripTap`) and putting "2 / 3" in the header subtitle.

Also from the same session: the page no longer forces landscape (a MAP of the screen has
to be the shape of the screen, unlike the button TABLE next door, so the frame now
carries the panel's own aspect ratio), page 1 gained a "bold = tap, below = hold" key,
and `tapForReaderMenu` was removed in favour of binding Tap centre to "Ignore".

**What this says about the verification here.** Eleven commits passed two board builds
and 880 host tests; the host tests cover the geometry helpers thoroughly and could not
see any of the four findings above. Three of them are about what a screen shows or what a
thumb can reach, and one needed a panel narrower than any the tests use. Build-green is
not evidence about an input layer.

### 10.9 What is still open

- **X4 Pro has not been tested at all.** Only the T5S3 has run this.
- Whether the ten-page swipe is now reliable, and whether the overview's labels fit
  legibly, both need another pass on hardware.
- **`BTN_PAGE_BACK_10`/`FORWARD_10` stop at a chapter boundary.** Crossing a spine does
  `section.reset()`, so the next iteration of the ten-step loop returns false and breaks:
  "skip 10 pages" silently means "skip up to 10 within this chapter". Pre-existing, and it
  affects the `btnDoubleLeft`/`btnDoubleRight` button defaults too. Investigated as a
  candidate cause of finding 3 and ruled out (the report was from page 30+), but it is a
  real defect and still unfixed.
- **`GESTURE_DEFAULTS_VERSION` 2 -> 3** discards existing gesture customisation.
- **The overflow-arrow lists have no tappable scroll bar** (`ListScrollBar` covers only
  the two wrapped-list draws).
- **P2, the localised tap flash**, still unaddressed (see §9.10).
