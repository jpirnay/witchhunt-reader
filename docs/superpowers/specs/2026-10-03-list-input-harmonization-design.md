# One button scheme for every list — design

> **Status.** Agreed in conversation on 2026-10-03, section by section; this document is the
> written form for review before an implementation plan. Started from issue **#374** (OPDS
> front buttons labelled "Up"/"Down" that did nothing). The interim fix on
> `fix/374-opds-front-buttons` (commits `e8d974a7e`, `6e45c08fd`, unpushed) is superseded by
> this design and is not to be merged; #374 is closed by the OPDS pilot (PR 3 below).

## Problem

There are about 36 list screens and five ways of binding buttons to them. The same physical
button means different things depending on which base class a screen happens to use:

| Pattern | Screens | Up / Down | Left / Right | Hold |
|---|---|---|---|---|
| `ButtonNavigator::onNextList` default | `MenuListActivity` family, chapter/TOC lists, OPDS format picker, 8 older settings/network screens | step | page | Up/Down 1.5 s → ends; Left/Right repeat page |
| Step-only navigator + own actions | Bookmarks, Starred pages, Wi-Fi, OPDS catalog, file browser | step | Rename/Delete, Forget/Rescan, Search/Info, Options | per screen |
| `UiListActivity` base | Enum, Font, Dictionary, Keyboard-layout, Language pickers | step (on release) | step | any arrow 0.5 s → repeat page |
| `TabbedUiListActivity` | Settings, reader menu | step | step | any arrow → switch tab |
| Hand-rolled | Footnotes, KOReader sync result, Home list, … | varies | varies | varies |

What that costs the reader:

- **Labels lie.** About a dozen screens label front Left/Right "Up"/"Down" while those buttons
  page once the list is longer than a screen; OPDS labelled them "Up"/"Down" while they did nothing (#374); the file context menu
  pages under blank labels, which on touch boards makes the boxes untappable.
- **"Page" is not a page.** Eight screens pass no page size and page by a hard-coded 10 rows,
  snapped to multiples of 10, while the theme draws 15–22 rows; a "page" often stays on the
  same screen.
- **Holds are unreachable by touch.** Every hold feature reads the global held time
  (`MappedInputManager::getHeldTime`), which an injected press never sets, so a long tap on a
  hint box — the only way to hold Left/Right/Back/Confirm on the X4 Pro and the T5S3 — triggers
  none of them.
- **A 300 ms lag on every Left/Right press outside the reader.** The default double-press
  bindings on Left/Right (`PAGE_BACK_10` / `PAGE_FORWARD_10`) are reader-scoped, but
  `ButtonEventManager::hasDoubleAction` still makes every Short wait out the double window, and
  two quick presses become a `Double` that lists ignore — the file browser pages zero times.
- **A swipe fires actions.** `ActivityManager::dispatchListSwipe` injects a raw Left/Right press
  because no screen overrides `Activity::pageList()`. On Bookmarks and Starred pages a swipe up
  **deletes the selected entry without confirmation**; on Wi-Fi it rescans, on OPDS it opens
  Info or Search, on the button-remap wizard it is captured as an assignment.
- **The OPDS catalog cannot page at all**, though a feed holds up to 2000 entries.
- **Unlocked render-state writes.** `MenuListActivity::handleNavigation` calls `nav.follow()`
  and the file browser writes `nav.top` from the loop task; `ListNav` documents both as
  render-owned.

The device matrix the scheme has to serve:

| | X3 / X4 | X4 Pro | LilyGo T5S3 |
|---|---|---|---|
| Up / Down | physical | physical | Down physical; **no Up** |
| Left / Right | physical | hint boxes only | hint boxes only |
| Back / Confirm | physical | Home key: hold / tap; hint boxes | Home key: hold / tap; hint boxes |
| Hold of Left/Right/Back/Confirm | physical | hint-box long tap only | hint-box long tap only |
| Touch | — | yes | yes |

## Goals

1. One button scheme for every single-column selectable list, identical across screens.
2. Hint labels generated from the same declaration as the behaviour, so they cannot drift.
3. Every action reachable on every board: by physical key on X3/X4, by hint-box tap or long
   tap on the X4 Pro and the T5S3.
4. One input implementation for lists (`ListGrammar` + `ListController`) in place of five.
5. The user guide describes the scheme once and every screen that changed.

## Non-goals

- **Row rendering.** Moving every list's row drawing onto `fui::list` is a later project. This
  one replaces input handling and hints only; screens keep drawing their rows as today.
- **Two-dimensional screens** keep geometric arrows: the file browser's cover grid, the Home
  screen (carousel and list layouts), the keyboard, the dictionary word selector, the Wi-Fi
  yes/no prompts, value pickers (slider, printed-page input).
- **The SDK.** No FreeInkUI change is needed for this project.
- **Reader controls** (page turns, chapter skip) and the global button-action settings keep
  their current meaning.
- **Delete without confirmation** on Bookmarks and Starred pages is a separate issue. This
  design removes the swipe that triggers it by accident; it does not add a confirmation.

## Design

### 1. The scheme (normative)

| Button | Short press | Long press (1 s) |
|---|---|---|
| **Up / Down** | step one row; two quick taps jump a page | first / last selectable row; **on tabbed lists**, previous / next tab |
| **Left / Right** | step one row (**default pair**), or the screen's **declared pair** | page back / forward; repeats every 500 ms while still held |
| **Confirm** | open / select | the screen's second action if declared, otherwise the same as short |
| **Back** | the screen's back | Home |

| Touch | Effect |
|---|---|
| Tap a row | point-then-confirm via `ListRowTap`, unchanged |
| Swipe up / down over the list | page forward / back — the same page as a long Left/Right, called directly, never as an injected button |
| Tap / long tap a hint box | the same as a short / long press of that button (unchanged mechanism) |

Rules:

- **R1 — Step** moves to the next/previous selectable row and wraps at the ends, as today.
- **R2 — The declared pair.** A screen may replace the short Left/Right pair, as a unit, with
  `none / action`, `action / none` or `action / action`. Declaring either side overloads the
  pair: **neither side steps any more**. The declaration is made once per screen; an action
  that does not apply to the selected row or the current feed (no search link, a folder rather
  than a book, an unsaved network) makes that side **none** for the moment — it never falls
  back to stepping. A button therefore never changes between moving and acting as the
  selection moves.
- **R3 — Page** is the number of rows the screen actually draws, reported by the renderer. It
  moves to the first row of the previous / next page and **clamps** at the ends (a held Right
  stops on the last page instead of wrapping). On a list that fits one screen it moves to the
  first / last row. Hold-repeat needs the live key level, so on the X4 Pro and the T5S3 a long
  tap pages once.
- **R4 — Long Up / Down** jumps to the first / last selectable row. On a tabbed list (Settings,
  reader menu) it switches to the previous / next tab instead, wrapping; tabbed lists use the
  default Left/Right pair.
- **R5 — Events, not levels.** All six buttons act on `ButtonEventManager` events: Short at
  release, Long while held at `LONG_PRESS_MS` (1 s). Each event carries the time its key went
  down on the sampler's clock (`ButtonEvent::pressMs`). This is lossless (events are classified
  from the sampler's edge queue) and treats a physical key and an injected long tap
  identically. The jump to the ends moves from 1.5 s to 1 s.
- **R5a — Double-tap Up/Down pages.** Two taps of the same Up/Down key whose presses land
  within `DOUBLE_WINDOW_MS` (300 ms) jump a page, as `ButtonNavigator` did on `master`. The
  first tap steps at once (single steps never wait); the second undoes that step and pages from
  where the pair started, so the pair moves exactly one page. On a list that fits one page the
  two taps stay two steps. The window is measured between the presses, not between the loop
  ticks that handle them, so a redraw between the taps does not break the pair. A `Double`
  event (only when a double action is bound to the page-turn keys and falls through) is the
  same double-tap; elsewhere a `Double` counts as one press (an action, Confirm, Back) or as
  two steps on the default Left/Right pair.
- **R6 — No needless double-press wait.** `ButtonEventManager::hasDoubleAction(btn)` returns
  false when the key's configured double action is reader-scoped
  (`CrossPointSettings::isReaderScopedAction`) and no reader activity is on top. The lookup
  of "is a reader on top" is injected (a function pointer set from `main.cpp`), so the button
  manager does not depend on `ActivityManager`.
- **R7 — Global bindings win.** An event consumed by a user-bound global action never reaches
  the list (existing behaviour of the dispatch in `main.cpp`). So a user who binds a global
  action to long Left loses hold-to-page on lists; documented, not worked around.
- **R8 — Labels.** Resolved through `MappedInputManager::mapHints`, so they follow the
  orientation:
  - Front Left / Right: the page glyph first, then the short-press label —
    `« Up` / `» Down` (default pair), `« Search` / `» Info` (declared), `«` / `»` alone for a
    side that is none at the moment. Hint boxes truncate at the end ("Downlo…"), so the glyph
    goes first to survive long and translated labels. «/» consistently means "hold to page".
  - Side Up / Down: `Up` / `Down`, **drawn and active on every list**, so a none front box
    never leaves the T5S3 without a way to step. List content rects reserve the side gutter.
  - Confirm and Back: the screen's labels (Open, Select, Download, Home, …).
- **R9 — Known limitation.** Up/Down are the same physical keys as PageBack/PageForward. A
  user-bound global *Short* action on PageBack/PageForward and the list step both act on one
  press. Rare configuration; documented only.

### 2. Architecture

```
ButtonEventManager ──events──▶ ListController ──command──▶ screen callbacks
                                  │   ▲                      (activate, back, actions)
                                  ▼   │
                              ListGrammar (pure)  ◀── declaration + list state
                                  │
                                  └──▶ hint labels ──▶ mapHints ──▶ GUI.drawButtonHints /
                                                                  drawSideButtonHints
touch: dispatchListTap ─▶ selectListRow ─▶ ListController::tapRow
       dispatchListSwipe ─▶ pageList     ─▶ ListController::page
```

**`ListGrammar`** (`src/util/ListGrammar.{h,cpp}`) — pure logic, no Arduino, no hardware.

- `ListCommand commandFor(Button, PressType, const ListDeclaration&, const ListState&)` returns
  one of: `StepPrev`, `StepNext`, `PagePrev`, `PageNext`, `First`, `Last`, `TabPrev`, `TabNext`,
  `Activate`, `ActivateLong`, `Back`, `Home`, `LeftAction`, `RightAction`, `None`.
- `ListLabels labelsFor(const ListDeclaration&, const ListState&)` returns the six labels.
- Index arithmetic: `stepIndex`, `pageIndex` (clamping, page-aligned), `firstSelectable`,
  `lastSelectable`, all taking an optional selectable predicate. Replaces the list half of
  `ButtonNavigator`'s static helpers.
- Both outputs derive from the one declaration; host tests pin them together.

**`ListDeclaration`** — what a screen states once, as a `static constexpr` value:

```cpp
struct ListAction {
  bool declared;                                 // short Left/Right runs a screen action
  StrId label;                                   // short-press label, e.g. STR_SEARCH
};
struct ListDeclaration {
  ListAction left;                               // neither declared = default pair (step)
  ListAction right;
  bool confirmLong;                              // long Confirm has an action of its own
  bool tabbed;                                   // long Up/Down switch tabs
};
```

The screen implements the virtual `ListHost` interface rather than registering function-pointer
slots with a `ctx`: `listCount`, `listPageRows`, `listSelectable`, `listActionAvailable`,
`onListSelectionChanged`, `onListActivate(row, longPress)`, `onListBack`, `onListHome`,
`onListAction`, `onListTab` and `onListOtherEvent`. A screen with two lists gives each its own
host. A virtual interface costs one vtable pointer, not a `std::function` (heap discipline).

**List state** — per tick, the controller asks the host for the row count, rows per page, the
selectable predicate, and whether each declared action is available on the selected row.
`listPageRows()` returns what the screen's last render published: it is called on the loop task,
and the renderer's live orientation must not be measured there.

**`ListController`** (`src/activities/ListController.{h,cpp}`) — a member of each list screen,
not a base class.

- `update()`, called from the screen's `loop()`: the **only** consumer of button events on a
  list screen. Walks the pending events, maps each through `ListGrammar`, applies movement, runs
  callbacks, and stops at the first event the screen acts on, leaving later presses for the next
  tick. Every state of a list screen must therefore read events, never levels. Matches Up/Down and
  discards their PageBack/PageForward alias events. Events the grammar does not use go to an
  optional `onOtherEvent` callback (Footnotes uses Power to select).
- Hold-repeat for Left/Right paging after a Long, while `isPressed` stays true.
- `page(int dir)` for swipes, `tapRow(int row)` for row taps (wraps `ListRowTap`).
- `drawHints(renderer, backLabel, confirmLabel)` composes the labels into stack buffers per call
  (no allocation per render) and draws both strips.
- Selection access: in PR 3 an `int&` for screens that keep their own index; the `ListNav`
  adapter arrives with PR 4. For a `freeink::ui::ListNav&`, writes go through `requestSelection()` and page size
  comes from `inputPageRows()`. The loop task never writes render-owned `ListNav` fields, which
  removes the unlocked writes in `MenuListActivity` and the file browser.
- Under 100 bytes per instance.

**Base classes.** `UiListActivity`, `MenuListActivity` and `TabbedUiListActivity` each hold a
`ListController`; their own `navigateButtons` / `handleButtons` go. Subclasses that customised
input (the file browser) express it as a declaration instead.

**Touch hooks.** `ListController` backs `Activity::selectListRow` and `Activity::pageList` for
the screen that owns it. Once every list screen overrides `pageList`, the injected-button
fallback in `dispatchListSwipe` is deleted. `UiListActivity`'s own swipe handling stops
scrolling the viewport and pages the selection, like every other list.

**`ButtonNavigator`.** Its list functions (`onNextList` / `onPreviousList`, `onListNav`,
`onListPageNav`, the press-log double-tap logic) are deleted once the last list has moved.
`onPressAndContinuous` and friends stay for the slider, keyboard and frontlight panel.

### 3. Screen mapping

| Screen | Left / Right short | Long Confirm | Notes |
|---|---|---|---|
| `MenuListActivity` family: file context menu, Home "More", settings submenus, clock, quick overrides, KOReader settings, weather menu and city results | default | — | |
| Pickers: Enum, Font, Dictionary, Keyboard layouts, Language | default | — | |
| Settings, reader menu | default | — | `tabbed` |
| OPDS server list, OPDS settings, network mode, status bar, reading-stats book list, font download, finished book | default | — | |
| Chapter / TOC lists (EPUB, Markdown, XTC), footnotes, KOReader sync result | default | — | footnotes: Power via `onOtherEvent` |
| OPDS format picker | default | — | |
| **OPDS catalog** | Search (feed has a search link) / Info (book selected) | — | closes #374; gains paging |
| **Bookmarks, Starred pages** | Rename / Delete | — | |
| **Wi-Fi networks** | Options (saved network; opens the forget prompt) / Rescan | — | |
| **File browser**, list views | none / Options | KOReader pull, then open (Books mode) | Back: parent folder; cover grid out of scope |
| **File browser**, folder picker | New folder / Move here | — | |

The button-remap wizard is not a list; its `pageList` becomes a no-op so a swipe is not
captured as an assignment.

### 4. User-visible changes

Each of these goes into `USER_GUIDE.md` and `RELEASE_NOTES.md` in the PR that makes it:

- Menus and settings lists: front Left/Right **step** on a short press (they paged); hold to
  page.
- Pickers: holding an arrow no longer repeats paging on all four; hold Left/Right to page,
  hold Up/Down to jump to the ends.
- Settings and the reader menu: tabs switch with a **long Up/Down** (any held arrow did).
- File browser: **short Right opens Options** (it was a hold, or a short press only on a
  one-screen folder); hold Right pages; short Left does nothing.
- Bookmarks, Starred pages, Wi-Fi, OPDS: Left/Right actions unchanged; hold now pages; a
  swipe pages instead of triggering the action.
- OPDS catalog: can be paged (hold Left/Right or swipe); front buttons show `«` / `»` when
  Search or Info does not apply.
- Jump to the first / last row: 1 s instead of 1.5 s, and reachable by a long tap on the side
  hint boxes.
- Every list shows the side Up/Down hint boxes (a slightly narrower list on X3/X4).
- Lists no longer lag 300 ms behind Left/Right presses.

### 5. User documentation (deliverable)

User documentation is part of the implementation, not a follow-up: **each PR that changes
behaviour updates `USER_GUIDE.md` in the same PR**, and the series is not done until all of
the following hold.

- A new subsection **"Moving through lists"** under §1 Hardware Overview, after Button Layout:
  the scheme table from §1 of this design in reader terms, how hints show it (`«`/`»` = hold to
  page, `short` label after the glyph), and the per-device row (on the X4 Pro and T5S3 tap or
  long-tap the hint boxes; the side boxes are the T5S3's Up).
- Per-screen sections then describe only their declared pair and anything special, linking to
  that subsection instead of restating the arrows:
  - §1 Hardware Overview: the T5S3 note ("paging backward … by tapping the scroll bar or
    swiping") — replace with the side hint box and hold/swipe paging.
  - §3.3 Browse Files: *Navigate List* and *Options menu* bullets; *Move to folder* (New /
    Move here on short Left/Right).
  - §3.4 Recent Books: the keys sentence.
  - §3.7 Settings, and §4 *System Navigation* (reader menu): tabs on long Up/Down.
  - §3.7.5 OPDS: browsing keys (Search / Info, paging) — the section covers servers only
    today, so this adds a short "Browsing a catalog" paragraph.
  - §5.2 Touch: swipe pages; remove the stale **Tap the scroll bar** and **Swipe right from the
    left edge → Back** rows (neither exists in the firmware); fix the T5S3 note under the table.
  - §5.7 Per-device differences: hint boxes as the Left/Right/Up keys.
  - §6 Chapter Selection: Up/Down step, hold Left/Right to page.
- `RELEASE_NOTES.md`: one entry per PR describing the change in reader terms.
- Developer docs: a new `docs/list-input.md` (the scheme, how to declare a list, the
  `ListController` contract); `docs/touch-gestures.md` stale rows removed; the class comments of
  `TabbedUiListActivity` and the OPDS format picker corrected.
- No new translatable strings are expected: labels are composed from existing `StrId`s plus
  the `«` / `»` glyphs. If one is needed it is added to the English YAML and regenerated per
  `docs/i18n.md` (never a hand-run `gen_i18n.py` outside the build).

### 6. Rollout

One concern per PR; each builds `default`, `x4pro` and `lilygo_t5s3` and passes the host suite.

1. **Swipe fix.** `pageList()` overrides that consume the swipe without effect on Bookmarks,
   Starred pages, Wi-Fi, OPDS and the button-remap wizard. Paging by swipe on the first four
   arrives when each moves to `ListController` (OPDS in PR 3, the rest in PR 6); none of them
   pages by button today either. Standalone; ships first. User guide: §5.2 swipe row.
2. **Double-press wait (R6).** Standalone.
3. **`ListGrammar` + `ListController` + hints, piloted on OPDS** (catalog and format picker).
   Closes #374. User guide: new "Moving through lists" subsection, §3.7.5.
4. **`MenuListActivity` family + the 8 default-navigator screens.** Fixes the lying labels and
   the 10-row page. User guide: §3.7.
5. **`UiListActivity` pickers + `TabbedUiListActivity`.** User guide: §3.7, §4.
6. **Screens with declared pairs:** Bookmarks, Starred pages, Wi-Fi, file browser list views
   and folder picker. User guide: §3.3, §3.4.
7. **The rest:** chapter/TOC lists, footnotes, KOReader sync result. User guide: §6.
8. **Cleanup:** delete `ButtonNavigator`'s list functions and the `dispatchListSwipe` injection
   fallback; `docs/list-input.md`, `docs/touch-gestures.md`, §5.2/§5.7 final pass.

### 7. Testing

- **Host (new `test/list_grammar/`):** every button × press type × declaration shape (default,
  none/action, action/none, action/action, tabbed, long-Confirm) × list shape (empty, one page,
  several pages, selectable predicate with headers at the ends) → expected command **and**
  expected labels. Index arithmetic: wrap on step, clamp on page, page alignment, first/last
  selectable.
- **Host:** R6 — the double-wait decision, extracted into a pure function if
  `ButtonEventManager` cannot run in the host suite.
- **Device, per PR:** a checklist for the screens it touches:
  - X3 / X4: short and long on all six buttons, hold-repeat paging.
  - T5S3 / X4 Pro: hint-box tap and long tap, side boxes, swipe, row tap.
  - Lists opened from the reader (chapter list, reader menu, quick overrides) in portrait and
    both landscapes.

### 8. Costs

- **Flash.** The C3 partition is about 95 % full. PR 3 may add at most ~2 KB; the series must
  end below today's size once per-screen handlers and `ButtonNavigator`'s list code are gone.
  Each PR records its measured size delta.
- **Heap.** No `std::function`; labels in fixed buffers inside the controller; under 100 bytes
  per screen; nothing allocated per tick or per render.

## Open questions

- **Delete confirmation** on Bookmarks / Starred pages: separate issue to file once PR 1 has
  removed the accidental swipe trigger.
- **Label width.** `« ` plus a long translated label may still truncate in a hint box; check
  the longest translations of Search, Rename, Delete, Options, Forget, Rescan during PR 3/6 and
  shorten in the YAML if needed.
