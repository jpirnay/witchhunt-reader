# Touch input migration — investigation and plan

Date: 2026-08-14
Status: proposal, nothing implemented
Reference: `upstream/feat-x4-papermono-support` (crosspoint-reader), `upstream/develop`

## Summary

This is **not** a greenfield design. Upstream has already built a complete touch
stack on `feat-x4-papermono-support`, and the FreeInk SDK carries the drivers and
geometry underneath it. Our fork consumes none of it — `grep -ril touch src/ lib/`
returns nothing.

So the real question is not *what to build* but *how much of upstream's stack to
take*, because their touch layer sits on top of a UI refactor
(**"convert all lists and tabs to FUI" #2957**, 89 files, +5649/−3570) that is
already merged to their **develop** mainline and that our fork has not taken.

Our divergence from `upstream/develop` is **2944 ahead / 1142 behind**.

The recommendation below is: **port upstream's input layers verbatim (phases 0–3),
then adopt FUI incrementally from phase 4 onward.** The input layers are not an
alternative to FUI — `UiAppHost::routeTouch()` takes a `MappedInputManager`, so
they are its prerequisite under any strategy. FUI itself is already proven on the
C3, is a per-screen opt-in mixin rather than a cutover, and solves the e-paper
tap-feedback problem a bolt-on approach would leave us to invent. See §3.

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
- Adopting FUI aligns *one layer*. It does not by itself close a 2944/1142
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

- Port `env:x4pro` (and `papermono` if the board is in play) from upstream's
  `platformio.ini`. We already have `env:lilygo_t5s3`.
- Add `FreeInkUI=symlink://freeink-sdk/libs/ui/FreeInkUI` to `lib_deps`.
- Verify GT911 enumerates on T5S3 and X4 Pro; log raw contacts.
- **Resolve I2C ownership (see §5, P1).** Cross-cutting — deciding it late means
  reworking phases 1–2.
- Confirm the primary bring-up board (X4 Pro or T5S3).

Deliverable: raw contacts in the serial log. No UI.

### Phase 1 — `HalGPIO` touch passthrough

Port upstream's ~10 methods verbatim. Everything inside `#if FREEINK_CAP_TOUCH`
with inert `false`-returning stubs otherwise, so callers need no ifdefs and C3
builds pay nothing.

Gate: `pio run -e default` (C3) flash/RAM delta is **zero**.

### Phase 2 — `MappedInputManager` touch layer (the core port)

Port upstream's header and implementation:

- Constructor gains `const GfxRenderer&` (mechanical change in `main.cpp`).
- `Button` enum extension — additive, but audit `ButtonEventManager::NUM_BUTTONS`
  / `ALL_BUTTONS` (currently 9): the new `Screen*`/`Nav*` names are touch-derived
  and must **not** join the press-type FSM.
- Full touch API + `SwipeDir` + `RowTouch` + the named semantic gestures.
- Geometry from FreeInkUI (`edgeSwipe`, `touchToLogical`, `Rect`) — linked, not
  copied.

Gate: host unit tests for the orientation transform in all four orientations, and
for `rowTouch` band arithmetic. This is pure logic; it should not need hardware.

### Phase 3 — Reader touch

Port `ReaderUtils::detectTouchPageTurn` / `isTouchMenuGesture`, the
`TOUCH_READER_CONTROLS` enum, and `tapForReaderMenu`, including the later fixes on
the branch (`2a3f08dd` vertical bounds, `6bf0dd74` inverted tap, `c0b2d3c2`
menu-tap toggle). Wire the settings screen with `tr(STR_*)` keys via
`scripts/gen_i18n.py`.

Also: route `wasTouchActivity()` into whatever path `wasAnyPressed()` feeds, so
touch resets the idle/sleep timer.

Gate: on device, the reader is fully navigable by touch alone. This is the phase
that makes the two touch boards genuinely usable.

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

**P2 — No hover, no free feedback.** A finger on glass gives no confirmation, and
on e-paper acknowledgement costs a refresh (~200 ms fast, 1–2 s full). Upstream
answers this with FUI's tap-flash (`clearTapFlash`, `setFlash`) — which is *inside*
the layer we are deferring. Under Option B we need our own answer in phase 4.
This is the most likely place for Option B to feel worse than upstream.

**P3 — Accidental touch while reading.** A thumb resting on the panel cannot
misfire a button but can misfire a capacitive panel. Upstream's answer is
`touchReaderControls = TOUCH_READER_OFF` plus zone restriction;
`suppressTouchContact()` is available for consuming a contact.

**P4 — Drift.** Every phase we implement differently from upstream widens a fork
that is already 2944/1142 diverged. Hence the rule: **names and signatures copied
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

1. **P1** — answered: upstream has no sampler, so there is nothing to copy. We must
   pick resolution 1 or 2 in §5 ourselves. Needs a read of `InputManager` to confirm
   button/touch servicing can be split across tasks.
2. **P2** — only live if phase 4a is chosen for lists. Under 4b, FUI's
   `setFlash`/`clearTapFlash` answers it.
3. **Phase 4 split** — 4a or 4b for the lists? This is the real decision in the
   document; everything before it is unconditional.
4. Primary bring-up board — X4 Pro or LilyGo T5S3? Phase 0 ordering follows.
5. Are `papermono` / `sticky` in scope for us at all, or X4 Pro + T5S3 only?
6. Does the FUI conversion wait for the paused reader/Stage-1 work to resolve?
   Phases 0–3 do not conflict with it; phase 4b touches many of the same files.
