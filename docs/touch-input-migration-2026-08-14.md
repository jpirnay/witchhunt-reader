# Touch input migration — investigation and plan

Date: 2026-08-14 (last updated 2026-08-16)
Status: **phases 1–3 implemented** (2026-08-16), phase 4 onward still proposal.
**Nothing has run on hardware.** Phases 1–2 met their gates (build deltas, host
tests); phase 3's gate is a device test and is NOT met — see the phase 3 note.
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

Gate: **NOT MET — on device, the reader is fully navigable by touch alone.**
Nothing here has run on hardware, and it cannot until workstream B makes an S3
board boot. Treat all of phase 3 as unvalidated.

Known gap: `wasHomeKeyTapped()` / `wasHomeKeyLongPressed()` are exposed through
`HalGPIO` and reachable via `wasHomeGesture()` / `wasHomeKeyHold()`, but nothing
consumes them yet. Open question 4 wanted phase 3 to cover the X4 Pro Home key;
that is still outstanding and matters, because the board has no back button.

### Phase 4 — Lists and chrome (the decision point)

Two ways to spend this phase. Both are legitimate; pick one deliberately.

**4a — bolt-on (`rowTouch`).** Fastest path to a touch-navigable device, no new
UI concepts, no conflict with in-flight reader work. Interim by construction: the
tap-feedback answer (P2) is ours to invent, and it is superseded when a screen
converts.

**4b — start the FUI conversion here.** Port `UiAppHost` (37 lines) +
`UiListActivity` + `UIThemeTokens`, convert the highest-traffic lists first,
using the 42 shared activities as reference diffs. Slower to first light, but
every converted screen is finished rather than revisited.

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

7. **Multi-touch** (SDK `410d0ab`) — recommendation in §0.3 is to take the free
   robustness and plan no feature around it. Confirm that is acceptable, or say
   what a two-finger gesture would be *for* on a reader.

Phase 0 also no longer needs to add `env:x4pro` or fix the S3 build — workstream A
did both, and the `freeink-sdk` submodule is now at **`cc89c653`** (was `76e61c4`;
bumped when master merged in on 2026-08-16). What remains of phase 0 is the GT911
bring-up itself and the P1 decision.
