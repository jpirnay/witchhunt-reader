# List Input Harmonization (PRs 1–3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the first three PRs of the list button scheme: stop swipes firing list actions, drop the needless 300 ms double-press wait outside the reader, and introduce `ListGrammar` + `ListController` with the OPDS browser as the pilot (closes #374).

**Architecture:** `ListGrammar` is pure, host-tested logic that turns a button event plus a screen's one-time declaration into a command, and derives the hint labels from the same declaration. `ListController` is a per-screen member that drains `ButtonEventManager` events, applies `ListGrammar` commands to the screen's selection, runs hold-to-repeat paging, and draws both hint strips. Screens implement the small `ListHost` interface. Row drawing is untouched.

**Tech Stack:** C++ (gnu++17 / ESP-IDF Arduino), PlatformIO, GoogleTest host suite (CMake + Ninja, MSYS2 UCRT64 gcc).

**Spec:** `docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md`

**Scope:** PRs 1–3 of the spec's rollout (§6). PRs 4–8 (MenuList family, pickers + tabbed, declared-pair screens, chapter lists, cleanup) get their own plan once the controller API is proven on OPDS. The `freeink::ui::ListNav` selection adapter in spec §2 arrives with PR 4, the first screens that need it; PR 3's controller takes an `int&` selection only.

## Global Constraints

- Up/Down short = step one row, everywhere. Up/Down long = first/last selectable row (tabbed lists: previous/next tab). Long Up/Down never pages.
- Left/Right short = step by default; a screen may overload the pair as none/action, action/none, action/action. Declaring either side means neither side steps; an action that does not apply is **none**, never a fallback step.
- Left/Right long = page back/forward, always; repeats every 500 ms while the physical key stays held.
- Page = rows the screen actually draws; clamps at the ends; on a one-page list it moves to the first/last row. Step wraps.
- Labels: page glyph first: `« Up` / `» Down`, `« <action>` / `» <action>`, `«` / `»` alone when a declared side does not apply. Side Up/Down hint boxes drawn and active on every list screen.
- Lists act on `ButtonEventManager` events only (Short/Double/Long), never on polled levels.
- A vertical swipe pages through `Activity::pageList()`, never as an injected Left/Right press.
- No `std::function` in list declarations; nothing allocated per tick or per render; controller < 100 bytes.
- C3 flash: PR 3 may add at most ~2 KB to the `default` env; record the measured delta in each PR description.
- Every behaviour-changing PR updates `USER_GUIDE.md` and `RELEASE_NOTES.md` in the same PR.
- User-facing strings via `tr(STR_*)`; input via `MappedInputManager` logical buttons/directions (skill `hal-and-abstractions`).
- Never hand-run `scripts/gen_i18n.py` (it bloats the strings ~31 KB); no new strings are expected in these PRs.
- Format every touched C++ file with `"/c/Program Files/LLVM/bin/clang-format.exe" -i <file>`.
- Firmware builds run in **PowerShell** with `$env:PLATFORMIO_CORE_DIR = 'C:\pio'`; host tests build in **Git Bash** (UCRT64 gcc is only on Bash's PATH).

## Review Focus

1. **A press handled in one OPDS state reaches the catalog again as an event.** Example: Back closes Book Detail, then the catalog reads the same Back and leaves the feed. Another: the Back that aborts a download is replayed after it. Expected: every press acts exactly once. Pinned by Task 5 (all OPDS states read events; `drain()` after downloads) and its device checklist items 6–8.
2. **An empty catalog** (a feed with no entries) with Left/Right/Up/Down/Confirm pressed. Expected: nothing moves, Info is unavailable (no `getEntry` on an empty feed), Search still works. Pinned by grammar tests `EmptyListDoesNotMove` (Task 3) and `CatalogHost::listActionAvailable`'s bounds check (Task 5), plus device item 9.
3. **Long Left/Right on a list that fits one screen.** Expected: the selection goes to the first/last row rather than nowhere or wrapping. Pinned by `PageOnOnePageListGoesToTheEnds` (Task 3).
4. **Headers or other unselectable rows at a page boundary or at the list ends.** Expected: paging and First/Last settle on the nearest selectable row and never on a header. Pinned by `PageSkipsUnselectableRows` and `FirstAndLastSkipUnselectableRows` (Task 3).
5. **A Double event reaching a list.** This happens with a forced or global double action. Expected: a Double on a step moves two rows, and a Double on an action runs it once. Pinned by `DoubleStepsTwiceButRunsActionsOnce` (Task 3).

---

## Before you start

- [ ] **Read the spec** `docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md` §1–§2.
- [ ] **Baseline the host suite** (Git Bash):

```bash
cd /c/_development/witchhunt-reader
cmake --build test/build -j 8 -- -k 0
ctest --test-dir test/build --output-on-failure -j 8 | tail -5
```

Expected: all tests pass (about 1046). `epub_build_inventory` fails to build on Windows (`<dlfcn.h>`) and that is pre-existing; `-k 0` keeps the rest building. Record the count.

- [ ] **Baseline firmware sizes** (PowerShell), one env at a time. A cold build takes about 15 minutes, so run it in the background:

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e default 2>&1 | Select-String "Flash:|RAM:|SUCCESS|FAILED"
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e x4pro 2>&1 | Select-String "Flash:|RAM:|SUCCESS|FAILED"
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e lilygo_t5s3 2>&1 | Select-String "Flash:|RAM:|SUCCESS|FAILED"
```

Record each env's Flash bytes. If an env fails on `master` already, note it in the PR description and keep going with the others; do not fix it in these PRs.

---

## PR 1 — A swipe no longer fires a list's Left/Right action

Branch: `git switch -c fix/list-swipe-no-actions origin/master`

### Task 1: Consume the swipe on the five screens whose Left/Right are actions

`ActivityManager::dispatchListSwipe` (`src/activities/ActivityManager.cpp`, near line 874) calls `currentActivity->pageList(dir)`. When that returns false, it injects a raw logical Left/Right press. No screen overrides `pageList` today, so on these screens the swipe runs their Left/Right action:

| Screen | Effect of the swipe |
|---|---|
| Bookmarks, Starred pages | Delete (swipe up) or Rename (swipe down) |
| Wi-Fi | Rescan / Options |
| OPDS | Info / Search |
| Button remap wizard | captured as an assignment |

The fix returns true without effect. Real swipe paging on the first four arrives when each moves to `ListController` (OPDS in Task 5, the rest in PR 6). Today those four have no button paging either, so nothing is lost.

There is no host test: these are activity overrides with no pure logic. Verification is the build and the device checklist.

**Files:**
- Modify: `src/activities/home/GlobalBookmarksActivity.h` (after line 29), `src/activities/home/GlobalBookmarksActivity.cpp` (after `selectListRow`, line ~289)
- Modify: `src/activities/reader/StarredPagesActivity.h` (after line 30), `src/activities/reader/StarredPagesActivity.cpp` (after `selectListRow`, line ~159)
- Modify: `src/activities/network/WifiSelectionActivity.h` (after line 156), `src/activities/network/WifiSelectionActivity.cpp` (after `selectListRow`, line ~1402)
- Modify: `src/activities/settings/ButtonRemapActivity.h` (after the comment ending line 19), `src/activities/settings/ButtonRemapActivity.cpp` (before `loop`, line ~36)
- Modify: `src/activities/browser/OpdsBookBrowserActivity.h` (after `selectListRow`), `src/activities/browser/OpdsBookBrowserActivity.cpp` (after `selectListRow`, end of file)
- Modify: `USER_GUIDE.md` §5.2, `RELEASE_NOTES.md`
- Modify: `docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md` §6 step 1 (record the no-op decision)

**Interfaces:**
- Consumes: `virtual bool Activity::pageList(ListPageDirection)` (`src/activities/Activity.h:89`), `enum class Activity::ListPageDirection : uint8_t { Back, Forward }`.
- Produces: nothing new.

- [ ] **Step 1: Declare the override in each header**

In `GlobalBookmarksActivity.h`, after `ListRowTap::Result selectListRow(int index) override;`:

```cpp
  // A swipe must not reach Left/Right, which are Rename and Delete here. Consumed without effect.
  bool pageList(ListPageDirection direction) override;
```

In `StarredPagesActivity.h`, after `ListRowTap::Result selectListRow(int index) override;`:

```cpp
  // A swipe must not reach Left/Right, which are Rename and Delete here. Consumed without effect.
  bool pageList(ListPageDirection direction) override;
```

In `WifiSelectionActivity.h`, after `ListRowTap::Result selectListRow(int index) override;`:

```cpp
  // A swipe must not reach Left/Right, which are Options and Rescan here. Consumed without effect.
  bool pageList(ListPageDirection direction) override;
```

In `ButtonRemapActivity.h`, after the comment block that ends `...so a tap would record itself as the user's chosen button.`:

```cpp
  // Same reason for swipes: an injected Left/Right press would be captured as an assignment.
  bool pageList(ListPageDirection direction) override;
```

In `OpdsBookBrowserActivity.h`, after `ListRowTap::Result selectListRow(int index) override;`:

```cpp
  // A swipe must not reach Left/Right, which are Search and Info here. Consumed without effect.
  bool pageList(ListPageDirection direction) override;
```

- [ ] **Step 2: Define the overrides**

Append to each `.cpp` at the anchor given in **Files**:

```cpp
bool GlobalBookmarksActivity::pageList(ListPageDirection /*direction*/) { return true; }
```

```cpp
bool StarredPagesActivity::pageList(ListPageDirection /*direction*/) { return true; }
```

```cpp
bool WifiSelectionActivity::pageList(ListPageDirection /*direction*/) { return true; }
```

```cpp
bool ButtonRemapActivity::pageList(ListPageDirection /*direction*/) { return true; }
```

```cpp
bool OpdsBookBrowserActivity::pageList(ListPageDirection /*direction*/) { return true; }
```

- [ ] **Step 3: Update the user guide (§5.2)**

In `USER_GUIDE.md`, under the §5.2 gesture table, replace the row

```markdown
| Swipe up / down over a list | Page the list |
```

with

```markdown
| Swipe up / down over a list | Page the list (on Bookmarks, Starred pages, Wi-Fi networks and the OPDS catalog, a swipe does nothing for now) |
```

- [ ] **Step 4: Add the release note**

At the top of `RELEASE_NOTES.md`, directly under the intro line `User-facing changes only. Full commit history is in git log.`, add an `## Unreleased` section if there is none, then:

```markdown
## Unreleased

### Touch

- **Fix: a swipe on Bookmarks, Starred pages, Wi-Fi or the OPDS catalog no longer acts on the selected row.** A vertical swipe over these lists was passed on as a Left/Right press. Swiping up on Bookmarks or Starred pages deleted the highlighted entry, and on Wi-Fi it started a rescan. On these screens a swipe now does nothing. On other lists it still pages.
```

- [ ] **Step 5: Record the decision in the spec**

In `docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md` §6, replace step 1:

```markdown
1. **Swipe fix.** `pageList()` overrides on Bookmarks, Starred pages, Wi-Fi, OPDS (page) and the
   button-remap wizard (no-op). Standalone; ships first. User guide: §5.2 swipe row.
```

with

```markdown
1. **Swipe fix.** `pageList()` overrides that consume the swipe without effect on Bookmarks,
   Starred pages, Wi-Fi, OPDS and the button-remap wizard. Paging by swipe on the first four
   arrives when each moves to `ListController` (OPDS in PR 3, the rest in PR 6); none of them
   pages by button today either. Standalone; ships first. User guide: §5.2 swipe row.
```

- [ ] **Step 6: Format, build, check sizes**

```bash
cd /c/_development/witchhunt-reader
for f in src/activities/home/GlobalBookmarksActivity.{h,cpp} src/activities/reader/StarredPagesActivity.{h,cpp} src/activities/network/WifiSelectionActivity.{h,cpp} src/activities/settings/ButtonRemapActivity.{h,cpp} src/activities/browser/OpdsBookBrowserActivity.{h,cpp}; do "/c/Program Files/LLVM/bin/clang-format.exe" -i "$f"; done
```

Then build `default`, `x4pro` and `lilygo_t5s3` in PowerShell with the baseline commands. Expected: SUCCESS for each env that built at baseline; Flash delta within a few hundred bytes.

- [ ] **Step 7: Commit**

```bash
git add src/activities USER_GUIDE.md RELEASE_NOTES.md docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md
git commit -m "fix(touch): a swipe no longer runs a list's Left/Right action

dispatchListSwipe injects a logical Left/Right press when a screen does not
override pageList(), and none did. On Bookmarks and Starred pages that made a
swipe up delete the highlighted entry without confirmation; on Wi-Fi it
rescanned, on OPDS it opened Info or Search, and on the button-remap wizard it
was captured as an assignment. These five screens now consume the swipe
without effect; real swipe paging arrives as each moves to ListController."
```

- [ ] **Step 8: Device checklist (T5S3 or X4 Pro), then open the PR**

1. Bookmarks with at least 2 entries: swipe up and down over the list. Nothing is deleted, nothing is renamed.
2. Starred pages: the same.
3. Wi-Fi network list: a swipe does not rescan and does not open the forget prompt.
4. OPDS catalog: a swipe opens neither Info nor Search.
5. Settings → Controls → button remap: a swipe is not recorded as a button.
6. A settings submenu longer than one screen: a swipe still pages (regression check).

```bash
git push -u origin fix/list-swipe-no-actions
gh pr create --title "fix(touch): a swipe no longer runs a list's Left/Right action" --body "<summary, the checklist above with results, the flash delta>"
```

---

## PR 2 — No double-press wait outside the reader for reader-only double actions

Branch: `git switch -c fix/no-reader-double-wait-outside-reader origin/master`

### Task 2: `hasDoubleAction` ignores reader-scoped double actions when no reader is on top

By default Left/Right have the double actions `PAGE_BACK_10` / `PAGE_FORWARD_10`, which are reader-scoped. `ButtonEventManager::hasDoubleAction` still answers true everywhere. So every Short on Left/Right waits 300 ms, and two quick presses become a `Double`, which no list understands. The decision moves into a pure, host-tested function that takes "is a reader on top" as an input. `main.cpp` supplies that through a function pointer, so the button manager does not depend on `ActivityManager`.

**Files:**
- Create: `src/util/DoubleActionWait.h`
- Create: `test/double_action_wait/CMakeLists.txt`, `test/double_action_wait/DoubleActionWaitTest.cpp`
- Modify: `test/CMakeLists.txt` (add the subdirectory next to `add_subdirectory(card_text_fit)`)
- Modify: `src/ButtonEventManager.h` (public setter near `hasDoubleAction`, private members near `forcedDoubleMask`)
- Modify: `src/ButtonEventManager.cpp:31-63` (`hasDoubleAction`)
- Modify: `src/main.cpp` (next to `ButtonNavigator::setMappedInputManager(mappedInputManager);`, line ~1298)
- Modify: `RELEASE_NOTES.md`

**Interfaces:**
- Consumes: `CrossPointSettings::isReaderScopedAction(uint8_t)` (`src/CrossPointSettings.cpp:58`), `ActivityManager::currentIsReaderActivity()`, globals `buttonEventManager` (`main.cpp:76`) and `activityManager` (`main.cpp:107`).
- Produces: `inline bool doubleActionNeedsWait(bool configured, bool readerScoped, bool readerOnTop)`; `using ButtonEventManager::ReaderOnTopQuery = bool (*)();` and `void ButtonEventManager::setReaderOnTopQuery(ReaderOnTopQuery)`.

- [ ] **Step 1: Write the failing test**

`test/double_action_wait/DoubleActionWaitTest.cpp`:

```cpp
// When a Short press has to wait out the double-click window: only for a double action that can do
// something where the press lands. A reader-only action falls through to the screen anywhere else,
// so waiting for it there only delays every press and merges two quick presses into one Double.

#include <gtest/gtest.h>

#include "util/DoubleActionWait.h"

TEST(DoubleActionWait, NoConfiguredActionNeverWaits) {
  EXPECT_FALSE(doubleActionNeedsWait(false, false, false));
  EXPECT_FALSE(doubleActionNeedsWait(false, false, true));
  EXPECT_FALSE(doubleActionNeedsWait(false, true, true));
}

TEST(DoubleActionWait, GlobalActionWaitsEverywhere) {
  EXPECT_TRUE(doubleActionNeedsWait(true, false, false));
  EXPECT_TRUE(doubleActionNeedsWait(true, false, true));
}

TEST(DoubleActionWait, ReaderActionWaitsOnlyInTheReader) {
  EXPECT_TRUE(doubleActionNeedsWait(true, true, true));
  EXPECT_FALSE(doubleActionNeedsWait(true, true, false));
}
```

`test/double_action_wait/CMakeLists.txt`:

```cmake
add_executable(DoubleActionWaitTest
  DoubleActionWaitTest.cpp
)

target_include_directories(DoubleActionWaitTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(DoubleActionWaitTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DoubleActionWaitTest)
```

In `test/CMakeLists.txt`, after `add_subdirectory(card_text_fit)`:

```cmake
add_subdirectory(double_action_wait)
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd /c/_development/witchhunt-reader
cmake --build test/build --target DoubleActionWaitTest
```

Expected: compile error, `util/DoubleActionWait.h: No such file or directory`.

- [ ] **Step 3: Write the helper**

`src/util/DoubleActionWait.h`:

```cpp
#pragma once

// Whether a Short press must wait out the double-click window before it is reported.
//
// Only when a double action is configured AND it can do something where the press lands. An
// action that works in the reader only (CrossPointSettings::isReaderScopedAction) falls through to
// the screen anywhere else, so waiting for it there only delays every press, and turns two quick
// presses into one Double that no list understands.
inline bool doubleActionNeedsWait(const bool configured, const bool readerScoped, const bool readerOnTop) {
  return configured && (!readerScoped || readerOnTop);
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build test/build --target DoubleActionWaitTest && ctest --test-dir test/build -R DoubleActionWait --output-on-failure
```

Expected: 3 tests PASS.

- [ ] **Step 5: Use it in `ButtonEventManager`**

In `src/ButtonEventManager.h`, directly after the `hasDoubleAction` declaration:

```cpp
  // Whether a reader activity is on top. Set once from main.cpp; until it is, every configured
  // double action is assumed to apply and its key waits out the double-click window.
  using ReaderOnTopQuery = bool (*)();
  void setReaderOnTopQuery(ReaderOnTopQuery query) { readerOnTopQuery = query; }
```

and in the private section, directly after `uint32_t forcedDoubleMask = 0;`:

```cpp
  ReaderOnTopQuery readerOnTopQuery = nullptr;

  // The double action configured for a key. Up/Down answer with the PageBack/PageForward settings,
  // so both names of a physical key share one wait policy.
  static uint8_t configuredDoubleAction(Button button);
```

In `src/ButtonEventManager.cpp`, add `#include "util/DoubleActionWait.h"` after `#include "CrossPointSettings.h"`. Then replace the whole body of `hasDoubleAction` (the `switch` that returns `SETTINGS.btnDouble* != BA::BTN_DEFAULT`) with:

```cpp
bool ButtonEventManager::hasDoubleAction(const Button button) const {
  const uint32_t pairMask = (1u << static_cast<int>(button)) | (1u << static_cast<int>(pairedAlias(button)));
  if (forcedDoubleMask & pairMask) {
    return true;
  }
  const uint8_t action = configuredDoubleAction(button);
  // Outside the reader a reader-only double action falls through to the screen, so there is
  // nothing to wait for (the default PAGE_BACK_10 / PAGE_FORWARD_10 on Left/Right).
  const bool readerOnTop = readerOnTopQuery == nullptr || readerOnTopQuery();
  return doubleActionNeedsWait(action != CrossPointSettings::BTN_DEFAULT,
                               CrossPointSettings::isReaderScopedAction(action), readerOnTop);
}

uint8_t ButtonEventManager::configuredDoubleAction(const Button button) {
  switch (button) {
    case Button::Back:
      return SETTINGS.btnDoubleBack;
    case Button::Confirm:
      return SETTINGS.btnDoubleConfirm;
    case Button::Left:
      return SETTINGS.btnDoubleLeft;
    case Button::Right:
      return SETTINGS.btnDoubleRight;
    case Button::Up:
    case Button::PageBack:
      return SETTINGS.btnDoublePageBack;
    case Button::Down:
    case Button::PageForward:
      return SETTINGS.btnDoublePageForward;
    case Button::Power:
      return SETTINGS.btnDoublePower;
  }
  return CrossPointSettings::BTN_DEFAULT;
}
```

Keep the comment block above `hasDoubleAction` (aliased pairs must answer the same) as it is.

- [ ] **Step 6: Wire the query in `main.cpp`**

Directly after `ButtonNavigator::setMappedInputManager(mappedInputManager);`:

```cpp
  buttonEventManager.setReaderOnTopQuery([] { return activityManager.currentIsReaderActivity(); });
```

(A captureless lambda converts to `bool (*)()`; both objects are globals.)

- [ ] **Step 7: Release note**

Under `## Unreleased` in `RELEASE_NOTES.md` (create the section as in Task 1 if this PR lands first), add a `### Buttons` heading if absent, then:

```markdown
- **Menus and lists respond to Left and Right without a delay.** Outside a book, every Left/Right press waited 0.3 s to see whether a double press would follow, because the double press is bound to a reading action by default. Two quick presses then moved nothing at all in the file browser. The wait now happens only in the reader, where the double press means something.
```

No user-guide section describes the wait, so `USER_GUIDE.md` is unchanged in this PR.

- [ ] **Step 8: Format, run the suite, build**

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/util/DoubleActionWait.h src/ButtonEventManager.h src/ButtonEventManager.cpp src/main.cpp test/double_action_wait/DoubleActionWaitTest.cpp
cmake --build test/build -j 8 -- -k 0 && ctest --test-dir test/build --output-on-failure -j 8 | tail -3
```

Expected: baseline count + 3, all passing. Then build the three envs in PowerShell. Expected: SUCCESS.

- [ ] **Step 9: Commit**

```bash
git add src/util/DoubleActionWait.h src/ButtonEventManager.h src/ButtonEventManager.cpp src/main.cpp test/CMakeLists.txt test/double_action_wait RELEASE_NOTES.md
git commit -m "fix(input): no double-press wait outside the reader for reader-only actions

Left/Right carry reader-scoped double actions by default (PAGE_BACK_10 /
PAGE_FORWARD_10), yet hasDoubleAction() answered true on every screen, so
each Short waited out the 300 ms window and two quick presses became one
Double that lists ignore. The decision is now a pure, host-tested function of
(configured, reader-scoped, reader on top); main.cpp supplies the last."
```

- [ ] **Step 10: Device checklist (X3 or X4), then open the PR**

1. File browser in a folder longer than one screen: press Right twice quickly. It pages twice, where it used to page zero times.
2. In a book: a double press of Right still jumps 10 pages (the reader keeps the wait).
3. In the reader menu or the chapter list opened from a book: Left/Right respond at once.
4. Settings → Controls → set double Left to a global action (e.g. Go Home): a double Left on the Home screen still runs it.

```bash
git push -u origin fix/no-reader-double-wait-outside-reader
gh pr create --title "fix(input): no double-press wait outside the reader for reader-only actions" --body "<summary, checklist results, flash delta>"
```

---

## PR 3 — `ListGrammar` + `ListController`, piloted on the OPDS browser (closes #374)

Branch: stack on PR 1, which also edits `OpdsBookBrowserActivity::pageList`. After PR 1 merges, rebase onto `master`. The spec and this plan travel with this PR:

```bash
git switch -c feat/list-controller-opds fix/list-swipe-no-actions
git cherry-pick $(git rev-list --reverse origin/master..docs/list-input-harmonization)
```

### Task 3: `ListGrammar`, the pure rules with host tests

**Files:**
- Create: `src/util/ListGrammar.h`, `src/util/ListGrammar.cpp`
- Create: `test/list_grammar/CMakeLists.txt`, `test/list_grammar/ListGrammarTest.cpp`
- Modify: `test/CMakeLists.txt` (add the subdirectory after `add_subdirectory(card_text_fit)`)
- Modify: `docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md` §1 R5 (Double semantics, see Step 6)

**Interfaces:**
- Consumes: nothing (no Arduino, no HAL).
- Produces (namespace `ListGrammar`):
  - `enum class Key : uint8_t { Up, Down, Left, Right, Confirm, Back };`
  - `enum class Press : uint8_t { Short, Double, Long };`
  - `enum class Side : uint8_t { Left, Right };`
  - `enum class Command : uint8_t { None, StepPrev, StepNext, PagePrev, PageNext, First, Last, TabPrev, TabNext, Activate, ActivateLong, Back, Home, LeftAction, RightAction };`
  - `struct Shape { bool leftDeclared, rightDeclared, confirmLong, tabbed; };` (all default false)
  - `struct Availability { bool left, right; };` (default false)
  - `struct Result { Command command; uint8_t times; };`
  - `Result commandFor(Key, Press, const Shape&, const Availability&);`
  - `enum class FrontLabel : uint8_t { Step, Action, PageOnly };` `struct Labels { FrontLabel left, right; };`
  - `Labels labelsFor(const Shape&, const Availability&);`
  - `using Selectable = bool (*)(const void* ctx, int row);` `struct Rows { int count; int pageRows; Selectable selectable; const void* ctx; };`
  - `int step(const Rows&, int from, int direction);` `int page(const Rows&, int from, int direction);` `int first(const Rows&);` `int last(const Rows&);`

- [ ] **Step 1: Write the failing tests**

`test/list_grammar/ListGrammarTest.cpp`:

```cpp
// The list button scheme (docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md §1):
// which command each button event means, what the front hint boxes say, and the row arithmetic.

#include <gtest/gtest.h>

#include <vector>

#include "util/ListGrammar.h"

namespace {

using ListGrammar::Availability;
using ListGrammar::Command;
using ListGrammar::FrontLabel;
using ListGrammar::Key;
using ListGrammar::Press;
using ListGrammar::Rows;
using ListGrammar::Shape;

Shape shape(bool left, bool right, bool confirmLong = false, bool tabbed = false) {
  Shape s;
  s.leftDeclared = left;
  s.rightDeclared = right;
  s.confirmLong = confirmLong;
  s.tabbed = tabbed;
  return s;
}

Availability available(bool left, bool right) {
  Availability a;
  a.left = left;
  a.right = right;
  return a;
}

const Shape kDefault = shape(false, false);
const Shape kBoth = shape(true, true);
const Availability kNone = available(false, false);
const Availability kAll = available(true, true);

void expectCommand(Key key, Press press, const Shape& s, const Availability& a, Command command, int times) {
  const auto result = ListGrammar::commandFor(key, press, s, a);
  EXPECT_EQ(static_cast<int>(result.command), static_cast<int>(command));
  EXPECT_EQ(result.times, times);
}

struct Mask {
  std::vector<bool> selectable;
};

bool maskSelectable(const void* ctx, const int row) { return static_cast<const Mask*>(ctx)->selectable[row]; }

Rows plainRows(int count, int pageRows) {
  Rows rows;
  rows.count = count;
  rows.pageRows = pageRows;
  return rows;
}

Rows maskedRows(const Mask& mask, int pageRows) {
  Rows rows;
  rows.count = static_cast<int>(mask.selectable.size());
  rows.pageRows = pageRows;
  rows.selectable = &maskSelectable;
  rows.ctx = &mask;
  return rows;
}

}  // namespace

// --- Commands -------------------------------------------------------------------------------

TEST(ListGrammarCommand, UpDownStepOnEveryShape) {
  for (const Shape& s : {kDefault, kBoth, shape(false, false, false, true)}) {
    expectCommand(Key::Up, Press::Short, s, kAll, Command::StepPrev, 1);
    expectCommand(Key::Down, Press::Short, s, kAll, Command::StepNext, 1);
  }
}

TEST(ListGrammarCommand, UpDownLongJumpsToTheEnds) {
  expectCommand(Key::Up, Press::Long, kDefault, kNone, Command::First, 1);
  expectCommand(Key::Down, Press::Long, kDefault, kNone, Command::Last, 1);
  expectCommand(Key::Down, Press::Long, kBoth, kAll, Command::Last, 1);
}

TEST(ListGrammarCommand, UpDownLongSwitchesTabsOnTabbedLists) {
  const Shape tabbed = shape(false, false, false, true);
  expectCommand(Key::Up, Press::Long, tabbed, kNone, Command::TabPrev, 1);
  expectCommand(Key::Down, Press::Long, tabbed, kNone, Command::TabNext, 1);
}

TEST(ListGrammarCommand, LeftRightStepByDefault) {
  expectCommand(Key::Left, Press::Short, kDefault, kNone, Command::StepPrev, 1);
  expectCommand(Key::Right, Press::Short, kDefault, kNone, Command::StepNext, 1);
}

TEST(ListGrammarCommand, LeftRightLongAlwaysPages) {
  for (const Shape& s : {kDefault, kBoth, shape(false, true), shape(false, false, false, true)}) {
    expectCommand(Key::Left, Press::Long, s, kNone, Command::PagePrev, 1);
    expectCommand(Key::Right, Press::Long, s, kAll, Command::PageNext, 1);
  }
}

TEST(ListGrammarCommand, DeclaredPairRunsItsActions) {
  expectCommand(Key::Left, Press::Short, kBoth, kAll, Command::LeftAction, 1);
  expectCommand(Key::Right, Press::Short, kBoth, kAll, Command::RightAction, 1);
}

TEST(ListGrammarCommand, DeclaringOneSideTakesTheStepOffBoth) {
  const Shape rightOnly = shape(false, true);
  expectCommand(Key::Left, Press::Short, rightOnly, kAll, Command::None, 0);
  expectCommand(Key::Right, Press::Short, rightOnly, kAll, Command::RightAction, 1);
  const Shape leftOnly = shape(true, false);
  expectCommand(Key::Left, Press::Short, leftOnly, kAll, Command::LeftAction, 1);
  expectCommand(Key::Right, Press::Short, leftOnly, kAll, Command::None, 0);
}

TEST(ListGrammarCommand, ActionThatDoesNotApplyIsNoneNeverAStep) {
  expectCommand(Key::Left, Press::Short, kBoth, kNone, Command::None, 0);
  expectCommand(Key::Right, Press::Short, kBoth, available(true, false), Command::None, 0);
  expectCommand(Key::Left, Press::Short, kBoth, available(true, false), Command::LeftAction, 1);
}

TEST(ListGrammarCommand, DoubleStepsTwiceButRunsActionsOnce) {
  expectCommand(Key::Down, Press::Double, kDefault, kNone, Command::StepNext, 2);
  expectCommand(Key::Up, Press::Double, kBoth, kAll, Command::StepPrev, 2);
  expectCommand(Key::Right, Press::Double, kDefault, kNone, Command::StepNext, 2);
  expectCommand(Key::Right, Press::Double, kBoth, kAll, Command::RightAction, 1);
  expectCommand(Key::Confirm, Press::Double, kDefault, kNone, Command::Activate, 1);
  expectCommand(Key::Back, Press::Double, kDefault, kNone, Command::Back, 1);
}

TEST(ListGrammarCommand, ConfirmLongActivatesUnlessDeclared) {
  expectCommand(Key::Confirm, Press::Short, kDefault, kNone, Command::Activate, 1);
  expectCommand(Key::Confirm, Press::Long, kDefault, kNone, Command::Activate, 1);
  expectCommand(Key::Confirm, Press::Long, shape(false, false, true), kNone, Command::ActivateLong, 1);
}

TEST(ListGrammarCommand, BackShortGoesBackLongGoesHome) {
  expectCommand(Key::Back, Press::Short, kDefault, kNone, Command::Back, 1);
  expectCommand(Key::Back, Press::Long, kBoth, kAll, Command::Home, 1);
}

// --- Labels ---------------------------------------------------------------------------------

TEST(ListGrammarLabels, DefaultPairShowsTheStep) {
  const auto labels = ListGrammar::labelsFor(kDefault, kNone);
  EXPECT_EQ(labels.left, FrontLabel::Step);
  EXPECT_EQ(labels.right, FrontLabel::Step);
}

TEST(ListGrammarLabels, DeclaredActionsShowWhenTheyApply) {
  const auto labels = ListGrammar::labelsFor(kBoth, kAll);
  EXPECT_EQ(labels.left, FrontLabel::Action);
  EXPECT_EQ(labels.right, FrontLabel::Action);
}

TEST(ListGrammarLabels, DeclaredSideThatDoesNotApplyShowsTheGlyphAlone) {
  auto labels = ListGrammar::labelsFor(kBoth, available(false, true));
  EXPECT_EQ(labels.left, FrontLabel::PageOnly);
  EXPECT_EQ(labels.right, FrontLabel::Action);
  labels = ListGrammar::labelsFor(shape(false, true), kAll);
  EXPECT_EQ(labels.left, FrontLabel::PageOnly);  // undeclared side of an overloaded pair
  EXPECT_EQ(labels.right, FrontLabel::Action);
}

TEST(ListGrammarLabels, LabelsAgreeWithCommands) {
  // A side labelled Step steps, Action acts, PageOnly does nothing on a short press.
  const Shape shapes[] = {kDefault, kBoth, shape(true, false), shape(false, true)};
  const Availability avails[] = {kNone, kAll, available(true, false), available(false, true)};
  for (const Shape& s : shapes) {
    for (const Availability& a : avails) {
      const auto labels = ListGrammar::labelsFor(s, a);
      const auto left = ListGrammar::commandFor(Key::Left, Press::Short, s, a).command;
      const auto right = ListGrammar::commandFor(Key::Right, Press::Short, s, a).command;
      EXPECT_EQ(labels.left == FrontLabel::Step, left == Command::StepPrev);
      EXPECT_EQ(labels.left == FrontLabel::Action, left == Command::LeftAction);
      EXPECT_EQ(labels.left == FrontLabel::PageOnly, left == Command::None);
      EXPECT_EQ(labels.right == FrontLabel::Step, right == Command::StepNext);
      EXPECT_EQ(labels.right == FrontLabel::Action, right == Command::RightAction);
      EXPECT_EQ(labels.right == FrontLabel::PageOnly, right == Command::None);
    }
  }
}

// --- Row arithmetic -------------------------------------------------------------------------

TEST(ListGrammarRows, StepWraps) {
  const Rows rows = plainRows(3, 10);
  EXPECT_EQ(ListGrammar::step(rows, 0, 1), 1);
  EXPECT_EQ(ListGrammar::step(rows, 2, 1), 0);
  EXPECT_EQ(ListGrammar::step(rows, 0, -1), 2);
}

TEST(ListGrammarRows, StepSkipsUnselectableRows) {
  const Mask mask{{false, true, true, false}};
  const Rows rows = maskedRows(mask, 10);
  EXPECT_EQ(ListGrammar::step(rows, 2, 1), 1);
  EXPECT_EQ(ListGrammar::step(rows, 1, -1), 2);
}

TEST(ListGrammarRows, StepWithNothingSelectableStays) {
  const Mask mask{{false, false}};
  EXPECT_EQ(ListGrammar::step(maskedRows(mask, 10), 0, 1), 0);
}

TEST(ListGrammarRows, EmptyListDoesNotMove) {
  const Rows rows = plainRows(0, 10);
  EXPECT_EQ(ListGrammar::step(rows, 0, 1), 0);
  EXPECT_EQ(ListGrammar::page(rows, 0, 1), 0);
  EXPECT_EQ(ListGrammar::page(rows, 0, -1), 0);
  EXPECT_EQ(ListGrammar::first(rows), 0);
  EXPECT_EQ(ListGrammar::last(rows), 0);
}

TEST(ListGrammarRows, PageForwardGoesToTheNextPageAndClampsOnTheLast) {
  const Rows rows = plainRows(50, 23);
  EXPECT_EQ(ListGrammar::page(rows, 0, 1), 23);
  EXPECT_EQ(ListGrammar::page(rows, 10, 1), 23);
  EXPECT_EQ(ListGrammar::page(rows, 23, 1), 46);
  EXPECT_EQ(ListGrammar::page(rows, 46, 1), 49);
  EXPECT_EQ(ListGrammar::page(rows, 49, 1), 49);
}

TEST(ListGrammarRows, PageBackGoesToThePreviousPageAndClampsOnTheFirst) {
  const Rows rows = plainRows(50, 23);
  EXPECT_EQ(ListGrammar::page(rows, 49, -1), 23);
  EXPECT_EQ(ListGrammar::page(rows, 30, -1), 0);
  EXPECT_EQ(ListGrammar::page(rows, 23, -1), 0);
  EXPECT_EQ(ListGrammar::page(rows, 5, -1), 0);
  EXPECT_EQ(ListGrammar::page(rows, 0, -1), 0);
}

TEST(ListGrammarRows, PageOnOnePageListGoesToTheEnds) {
  const Rows rows = plainRows(5, 23);
  EXPECT_EQ(ListGrammar::page(rows, 2, 1), 4);
  EXPECT_EQ(ListGrammar::page(rows, 2, -1), 0);
}

TEST(ListGrammarRows, PageTreatsANonPositivePageAsOneRow) {
  EXPECT_EQ(ListGrammar::page(plainRows(5, 0), 0, 1), 1);
}

TEST(ListGrammarRows, PageSkipsUnselectableRows) {
  // Rows 4 and 8 are headers; four rows per page.
  const Mask mask{{true, true, true, true, false, true, true, true, false, true}};
  const Rows rows = maskedRows(mask, 4);
  EXPECT_EQ(ListGrammar::page(rows, 0, 1), 5);   // lands on header 4, walks on
  EXPECT_EQ(ListGrammar::page(rows, 5, 1), 9);   // lands on header 8, walks on
  EXPECT_EQ(ListGrammar::page(rows, 9, -1), 3);  // lands on header 4, walks back
}

TEST(ListGrammarRows, PageAtAHeaderAtTheEndSettlesBesideIt) {
  const Mask mask{{true, true, true, true, true, false}};
  EXPECT_EQ(ListGrammar::page(maskedRows(mask, 3), 3, 1), 4);
  const Mask leading{{false, true, true, true}};
  EXPECT_EQ(ListGrammar::page(maskedRows(leading, 2), 3, -1), 1);
}

TEST(ListGrammarRows, FirstAndLastSkipUnselectableRows) {
  const Mask mask{{false, true, true, false}};
  const Rows rows = maskedRows(mask, 10);
  EXPECT_EQ(ListGrammar::first(rows), 1);
  EXPECT_EQ(ListGrammar::last(rows), 2);
  const Mask none{{false, false}};
  EXPECT_EQ(ListGrammar::first(maskedRows(none, 10)), 0);
  EXPECT_EQ(ListGrammar::last(maskedRows(none, 10)), 0);
}
```

`test/list_grammar/CMakeLists.txt`:

```cmake
add_executable(ListGrammarTest
  ListGrammarTest.cpp
  ${REPO_ROOT}/src/util/ListGrammar.cpp
)

target_include_directories(ListGrammarTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ListGrammarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ListGrammarTest)
```

In `test/CMakeLists.txt`, after `add_subdirectory(card_text_fit)`:

```cmake
add_subdirectory(list_grammar)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build test/build --target ListGrammarTest
```

Expected: FAIL, `util/ListGrammar.h: No such file or directory` (and `ListGrammar.cpp` missing).

- [ ] **Step 3: Write `src/util/ListGrammar.h`**

```cpp
#pragma once

#include <cstdint>

// The button scheme every list screen follows, as pure logic: which command a button event means,
// and what the front hint boxes say, both derived from one declaration so they cannot disagree.
// No hardware and no Arduino here, so every rule is pinned by host tests (test/list_grammar).
// The rules are docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md §1.
namespace ListGrammar {

// Up/Down/Left/Right are screen directions (MappedInputManager::Direction), already resolved for
// the orientation by the caller.
enum class Key : uint8_t { Up, Down, Left, Right, Confirm, Back };
enum class Press : uint8_t { Short, Double, Long };
enum class Side : uint8_t { Left, Right };

enum class Command : uint8_t {
  None,
  StepPrev,
  StepNext,
  PagePrev,
  PageNext,
  First,
  Last,
  TabPrev,
  TabNext,
  Activate,
  ActivateLong,
  Back,
  Home,
  LeftAction,
  RightAction,
};

// What the screen declared, reduced to what the rules read.
struct Shape {
  bool leftDeclared = false;   // short Left runs a screen action instead of stepping
  bool rightDeclared = false;  // short Right runs a screen action instead of stepping
  bool confirmLong = false;    // long Confirm has an action of its own
  bool tabbed = false;         // long Up/Down switch tabs instead of jumping to the ends
};

// Whether each declared action applies to the selected row right now.
struct Availability {
  bool left = false;
  bool right = false;
};

// A command and how often to apply it: a Double press of a step is two steps.
struct Result {
  Command command = Command::None;
  uint8_t times = 0;
};

Result commandFor(Key key, Press press, const Shape& shape, const Availability& available);

// What a front Left/Right box shows after its page glyph: the default step, the declared action,
// or nothing (the glyph alone: the pair is overloaded but this side does not apply right now).
enum class FrontLabel : uint8_t { Step, Action, PageOnly };
struct Labels {
  FrontLabel left = FrontLabel::Step;
  FrontLabel right = FrontLabel::Step;
};

Labels labelsFor(const Shape& shape, const Availability& available);

// Row arithmetic. `selectable` may be null (every row is selectable); `ctx` is handed back to it.
using Selectable = bool (*)(const void* ctx, int row);
struct Rows {
  int count = 0;
  int pageRows = 1;  // rows the screen draws per page
  Selectable selectable = nullptr;
  const void* ctx = nullptr;
};

// One selectable row back (-1) or forward (+1), wrapping round the ends.
int step(const Rows& rows, int from, int direction);
// To the first row of the previous / next page, clamped at the ends: the first row from page one,
// the last row from the last page, so a list that fits one page pages to its ends. A header there
// is passed over in the direction of travel, or else settled beside.
int page(const Rows& rows, int from, int direction);
int first(const Rows& rows);
int last(const Rows& rows);

}  // namespace ListGrammar
```

- [ ] **Step 4: Write `src/util/ListGrammar.cpp`**

```cpp
#include "ListGrammar.h"

namespace ListGrammar {
namespace {

bool isSelectable(const Rows& rows, const int row) {
  return rows.selectable == nullptr || rows.selectable(rows.ctx, row);
}

Result once(const Command command) { return {command, 1}; }

// Short Left/Right: the default step, or, once the screen has overloaded the pair, the declared
// action — none when it does not apply. Overloading either side takes the step off both.
Result shortSide(const Side side, const Press press, const Shape& shape, const Availability& available) {
  if (!shape.leftDeclared && !shape.rightDeclared) {
    const Command stepCommand = side == Side::Left ? Command::StepPrev : Command::StepNext;
    return {stepCommand, static_cast<uint8_t>(press == Press::Double ? 2 : 1)};
  }
  const bool declared = side == Side::Left ? shape.leftDeclared : shape.rightDeclared;
  const bool applies = side == Side::Left ? available.left : available.right;
  if (!declared || !applies) return {};
  return once(side == Side::Left ? Command::LeftAction : Command::RightAction);
}

}  // namespace

Result commandFor(const Key key, const Press press, const Shape& shape, const Availability& available) {
  const bool isLong = press == Press::Long;
  const uint8_t steps = press == Press::Double ? 2 : 1;
  switch (key) {
    case Key::Up:
      if (isLong) return once(shape.tabbed ? Command::TabPrev : Command::First);
      return {Command::StepPrev, steps};
    case Key::Down:
      if (isLong) return once(shape.tabbed ? Command::TabNext : Command::Last);
      return {Command::StepNext, steps};
    case Key::Left:
      if (isLong) return once(Command::PagePrev);
      return shortSide(Side::Left, press, shape, available);
    case Key::Right:
      if (isLong) return once(Command::PageNext);
      return shortSide(Side::Right, press, shape, available);
    case Key::Confirm:
      return once(isLong && shape.confirmLong ? Command::ActivateLong : Command::Activate);
    case Key::Back:
      return once(isLong ? Command::Home : Command::Back);
  }
  return {};
}

Labels labelsFor(const Shape& shape, const Availability& available) {
  if (!shape.leftDeclared && !shape.rightDeclared) return {};
  const auto label = [](const bool declared, const bool applies) {
    return declared && applies ? FrontLabel::Action : FrontLabel::PageOnly;
  };
  Labels labels;
  labels.left = label(shape.leftDeclared, available.left);
  labels.right = label(shape.rightDeclared, available.right);
  return labels;
}

int step(const Rows& rows, const int from, const int direction) {
  if (rows.count <= 0) return from;
  int row = from;
  for (int tried = 0; tried < rows.count; ++tried) {
    row = ((row + direction) % rows.count + rows.count) % rows.count;
    if (isSelectable(rows, row)) return row;
  }
  return from;
}

int page(const Rows& rows, const int from, const int direction) {
  if (rows.count <= 0) return from;
  const int perPage = rows.pageRows > 0 ? rows.pageRows : 1;
  const int currentPage = from / perPage;
  const int lastPage = (rows.count - 1) / perPage;
  int target = 0;
  if (direction > 0) {
    target = currentPage < lastPage ? (currentPage + 1) * perPage : rows.count - 1;
  } else {
    target = currentPage > 0 ? (currentPage - 1) * perPage : 0;
  }
  const int onward = direction > 0 ? 1 : -1;
  for (int row = target; row >= 0 && row < rows.count; row += onward) {
    if (isSelectable(rows, row)) return row;
  }
  for (int row = target; row >= 0 && row < rows.count; row -= onward) {
    if (isSelectable(rows, row)) return row;
  }
  return from;
}

int first(const Rows& rows) {
  for (int row = 0; row < rows.count; ++row) {
    if (isSelectable(rows, row)) return row;
  }
  return 0;
}

int last(const Rows& rows) {
  for (int row = rows.count - 1; row >= 0; --row) {
    if (isSelectable(rows, row)) return row;
  }
  return 0;
}

}  // namespace ListGrammar
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/util/ListGrammar.h src/util/ListGrammar.cpp test/list_grammar/ListGrammarTest.cpp
cmake --build test/build --target ListGrammarTest && ctest --test-dir test/build -R ListGrammar --output-on-failure
```

Expected: all `ListGrammar*` tests PASS (26 tests).

- [ ] **Step 6: Record the Double refinement in the spec**

In the spec §1, rule **R5**, replace the sentence

```
A `Double` event — produced only when a
  double action is configured for that key and was not consumed globally — counts as two
  Shorts.
```

with

```
A `Double` event — produced only when a
  double action is configured for that key and was not consumed globally — counts as two
  steps where the Short is a step, and as one press otherwise (an action, Confirm, Back).
```

- [ ] **Step 7: Commit**

```bash
git add src/util/ListGrammar.h src/util/ListGrammar.cpp test/list_grammar test/CMakeLists.txt docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md
git commit -m "feat(lists): ListGrammar, the list button scheme as pure host-tested rules

One declaration per screen decides both what each button event does and what
the front hint boxes say, so labels cannot drift from behaviour again (#374).
Up/Down step, long Up/Down jumps to the ends or switches tabs; Left/Right step
by default or carry a declared action pair, and page on a long press. Row
arithmetic: step wraps, page clamps and settles beside headers."
```

### Task 4: `ListController` and `ListHost`

Glue between `ButtonEventManager`, `ListGrammar`, the screen and the hint strips. It depends on the HAL (`millis`, `MappedInputManager`), so it has no host test. `ListGrammar`'s tests cover its decisions, and Task 5's device checklist covers the glue.

**Files:**
- Create: `src/activities/ListController.h`, `src/activities/ListController.cpp`

**Interfaces:**
- Consumes: everything from Task 3. Also:
  - `ButtonEventManager::consumeEvent(ButtonEvent&)`, `ButtonEvent{Button button; PressType type;}`, `PressType{Short, Double, Long}`
  - `MappedInputManager::isDirection(Button, Direction)` (static), `isPressed(Button)`, `mapHints(back, confirm, left, right, up, down) -> Hints{Labels front; SideLabels side}`
  - `GUI.drawButtonHints(GfxRenderer&, b1, b2, b3, b4)`, `GUI.drawSideButtonHints(GfxRenderer&, up, down)`
  - `I18n::getInstance().get(StrId)`, `ListRowTap::apply(int, int, int&)`
- Produces:
  - `class ListHost` with virtual `listCount() const`, `listPageRows() const`, `listSelectable(int) const` (default true), `listActionAvailable(ListGrammar::Side, int) const` (default true), `onListSelectionChanged()`, `onListActivate(int row, bool longPress)`, `onListBack()`, `onListHome()`, `onListAction(ListGrammar::Side, int)` (default no-op), `onListTab(int direction)` (default no-op), `onListOtherEvent(const ButtonEventManager::ButtonEvent&)` (default no-op)
  - `struct ListAction { bool declared; StrId label; }`, `struct ListDeclaration { ListAction left, right; bool confirmLong; bool tabbed; }`
  - `class ListController(MappedInputManager&, ButtonEventManager&, ListHost&, int& selection, const ListDeclaration&)` with `void update()`, `void page(int direction)`, `ListRowTap::Result tapRow(int row)`, `void drawHints(GfxRenderer&, const char* backLabel, const char* confirmLabel) const`

- [ ] **Step 1: Write `src/activities/ListController.h`**

```cpp
#pragma once

#include <I18n.h>

#include "ButtonEventManager.h"
#include "ListRowTap.h"
#include "MappedInputManager.h"
#include "util/ListGrammar.h"

class GfxRenderer;

// The screen's half of a ListController: what the controller asks of the list, and what it tells
// the screen. A screen with two lists (OPDS: catalog and format picker) gives each its own host.
class ListHost {
 public:
  virtual int listCount() const = 0;
  // Rows the screen draws per page, from the same geometry its render uses.
  virtual int listPageRows() const = 0;
  virtual bool listSelectable(int /*row*/) const { return true; }
  // Whether a declared Left/Right action applies to `row` right now. Asked for declared sides only.
  virtual bool listActionAvailable(ListGrammar::Side /*side*/, int /*row*/) const { return true; }

  virtual void onListSelectionChanged() = 0;
  virtual void onListActivate(int row, bool longPress) = 0;
  virtual void onListBack() = 0;
  virtual void onListHome() = 0;
  virtual void onListAction(ListGrammar::Side /*side*/, int /*row*/) {}
  virtual void onListTab(int /*direction*/) {}
  // A button the scheme does not use on a list (Power).
  virtual void onListOtherEvent(const ButtonEventManager::ButtonEvent& /*event*/) {}

 protected:
  ~ListHost() = default;
};

// A short Left or Right press the screen takes over from the default step.
struct ListAction {
  bool declared = false;
  StrId label = StrId::STR_DIR_UP;  // shown after the page glyph; unused when not declared
};

// What a list screen states once: its Left/Right pair, whether long Confirm has an action of its
// own, whether long Up/Down switch tabs.
struct ListDeclaration {
  ListAction left;
  ListAction right;
  bool confirmLong = false;
  bool tabbed = false;
};

// Runs one list's buttons, hint strips and touch through ListGrammar, so every list answers the
// same way (docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md). A member of the
// screen, not a base class; it holds no heap and allocates nothing per tick or per render.
class ListController {
 public:
  ListController(MappedInputManager& input, ButtonEventManager& events, ListHost& host, int& selection,
                 const ListDeclaration& declaration);

  // Call from the screen's loop() while this list is on screen. It is the only reader of button
  // events there, and it consumes every pending event so none is left for a later state to read.
  void update();
  // A vertical swipe over the list (Activity::pageList): -1 back, +1 forward.
  void page(int direction);
  // A tap on a row (Activity::selectListRow).
  ListRowTap::Result tapRow(int row);
  // Draws the bottom and the side hint strips; `backLabel` and `confirmLabel` are the screen's own.
  void drawHints(GfxRenderer& renderer, const char* backLabel, const char* confirmLabel) const;

 private:
  using Button = MappedInputManager::Button;

  MappedInputManager& input;
  ButtonEventManager& events;
  ListHost& host;
  int& selection;
  ListDeclaration declaration;

  // Hold-to-repeat paging after a long Left/Right, while that key stays down.
  bool repeating = false;
  Button repeatButton = Button::Right;
  int8_t repeatDirection = 1;
  unsigned long repeatSinceMs = 0;

  ListGrammar::Shape shape() const;
  ListGrammar::Availability availability() const;
  ListGrammar::Rows rows() const;
  static bool keyFor(Button button, ListGrammar::Key& key);
  // Applies one command. False once the screen has acted (it may have left this list): stop
  // reading events until the next tick.
  bool apply(ListGrammar::Result result);
  void moveTo(int row);
  void continuePageRepeat();
};
```

- [ ] **Step 2: Write `src/activities/ListController.cpp`**

```cpp
#include "ListController.h"

#include <Arduino.h>
#include <GfxRenderer.h>

#include <cstdio>

#include "components/UITheme.h"

namespace {

// How often a held Left/Right keeps paging once its long press has fired.
constexpr unsigned long kPageRepeatMs = 500;

// "«" and "»": the page glyphs that open the front Left/Right labels.
constexpr const char* kPrevGlyph = "\xC2\xAB";
constexpr const char* kNextGlyph = "\xC2\xBB";

bool hostSelectable(const void* ctx, const int row) { return static_cast<const ListHost*>(ctx)->listSelectable(row); }

ListGrammar::Press pressFor(const ButtonEventManager::PressType type) {
  switch (type) {
    case ButtonEventManager::PressType::Long:
      return ListGrammar::Press::Long;
    case ButtonEventManager::PressType::Double:
      return ListGrammar::Press::Double;
    case ButtonEventManager::PressType::Short:
      break;
  }
  return ListGrammar::Press::Short;
}

// "« Search", "» Down", or the glyph alone for a declared side that does not apply. The glyph goes
// first because hint boxes cut long labels off at the end.
void composeFront(char* out, const size_t size, const char* glyph, const ListGrammar::FrontLabel label,
                  const StrId action, const StrId stepLabel) {
  switch (label) {
    case ListGrammar::FrontLabel::PageOnly:
      snprintf(out, size, "%s", glyph);
      return;
    case ListGrammar::FrontLabel::Action:
      snprintf(out, size, "%s %s", glyph, I18n::getInstance().get(action));
      return;
    case ListGrammar::FrontLabel::Step:
      snprintf(out, size, "%s %s", glyph, I18n::getInstance().get(stepLabel));
      return;
  }
}

}  // namespace

ListController::ListController(MappedInputManager& input, ButtonEventManager& events, ListHost& host, int& selection,
                               const ListDeclaration& declaration)
    : input(input), events(events), host(host), selection(selection), declaration(declaration) {}

ListGrammar::Shape ListController::shape() const {
  ListGrammar::Shape s;
  s.leftDeclared = declaration.left.declared;
  s.rightDeclared = declaration.right.declared;
  s.confirmLong = declaration.confirmLong;
  s.tabbed = declaration.tabbed;
  return s;
}

ListGrammar::Availability ListController::availability() const {
  ListGrammar::Availability a;
  a.left = declaration.left.declared && host.listActionAvailable(ListGrammar::Side::Left, selection);
  a.right = declaration.right.declared && host.listActionAvailable(ListGrammar::Side::Right, selection);
  return a;
}

ListGrammar::Rows ListController::rows() const {
  ListGrammar::Rows r;
  r.count = host.listCount();
  r.pageRows = host.listPageRows();
  r.selectable = &hostSelectable;
  r.ctx = &host;
  return r;
}

bool ListController::keyFor(const Button button, ListGrammar::Key& key) {
  using Direction = MappedInputManager::Direction;
  if (button == Button::Confirm) {
    key = ListGrammar::Key::Confirm;
  } else if (button == Button::Back) {
    key = ListGrammar::Key::Back;
  } else if (MappedInputManager::isDirection(button, Direction::Up)) {
    key = ListGrammar::Key::Up;
  } else if (MappedInputManager::isDirection(button, Direction::Down)) {
    key = ListGrammar::Key::Down;
  } else if (MappedInputManager::isDirection(button, Direction::Left)) {
    key = ListGrammar::Key::Left;
  } else if (MappedInputManager::isDirection(button, Direction::Right)) {
    key = ListGrammar::Key::Right;
  } else {
    return false;
  }
  return true;
}

void ListController::update() {
  continuePageRepeat();

  ButtonEventManager::ButtonEvent event;
  while (events.consumeEvent(event)) {
    // PageBack/PageForward are the reader's names for the Up/Down keys: one press emits an event
    // under each name, and a list answers to Up/Down only.
    if (event.button == Button::PageBack || event.button == Button::PageForward) continue;

    ListGrammar::Key key = ListGrammar::Key::Confirm;
    if (!keyFor(event.button, key)) {
      host.onListOtherEvent(event);
      continue;
    }

    const bool side = key == ListGrammar::Key::Left || key == ListGrammar::Key::Right;
    const auto result = ListGrammar::commandFor(key, pressFor(event.type), shape(),
                                                side ? availability() : ListGrammar::Availability{});
    if (side && event.type == ButtonEventManager::PressType::Long) {
      repeating = true;
      repeatButton = event.button;
      repeatDirection = key == ListGrammar::Key::Left ? -1 : 1;
      repeatSinceMs = millis();
    }
    if (!apply(result)) return;
  }
}

bool ListController::apply(const ListGrammar::Result result) {
  using ListGrammar::Command;
  switch (result.command) {
    case Command::None:
      return true;
    case Command::StepPrev:
    case Command::StepNext: {
      const auto r = rows();
      int row = selection;
      for (int i = 0; i < result.times; ++i) row = ListGrammar::step(r, row, result.command == Command::StepNext ? 1 : -1);
      moveTo(row);
      return true;
    }
    case Command::PagePrev:
    case Command::PageNext:
      moveTo(ListGrammar::page(rows(), selection, result.command == Command::PageNext ? 1 : -1));
      return true;
    case Command::First:
      moveTo(ListGrammar::first(rows()));
      return true;
    case Command::Last:
      moveTo(ListGrammar::last(rows()));
      return true;
    default:
      break;
  }

  // The rest hands control to the screen, which may leave this list.
  repeating = false;
  switch (result.command) {
    case Command::TabPrev:
      host.onListTab(-1);
      break;
    case Command::TabNext:
      host.onListTab(1);
      break;
    case Command::Activate:
      host.onListActivate(selection, false);
      break;
    case Command::ActivateLong:
      host.onListActivate(selection, true);
      break;
    case Command::Back:
      host.onListBack();
      break;
    case Command::Home:
      host.onListHome();
      break;
    case Command::LeftAction:
      host.onListAction(ListGrammar::Side::Left, selection);
      break;
    case Command::RightAction:
      host.onListAction(ListGrammar::Side::Right, selection);
      break;
    default:
      break;
  }
  return false;
}

void ListController::moveTo(const int row) {
  if (row == selection) return;
  selection = row;
  host.onListSelectionChanged();
}

void ListController::continuePageRepeat() {
  if (!repeating) return;
  // An injected long press (a long tap on a hint box) never holds the live level, so it pages once.
  if (!input.isPressed(repeatButton)) {
    repeating = false;
    return;
  }
  const unsigned long now = millis();
  if (now - repeatSinceMs < kPageRepeatMs) return;
  repeatSinceMs = now;
  moveTo(ListGrammar::page(rows(), selection, repeatDirection));
}

void ListController::page(const int direction) {
  repeating = false;
  moveTo(ListGrammar::page(rows(), selection, direction));
}

ListRowTap::Result ListController::tapRow(const int row) {
  const int count = host.listCount();
  if (row >= 0 && row < count && !host.listSelectable(row)) return ListRowTap::Result::Rejected;
  return ListRowTap::apply(row, count, selection);
}

void ListController::drawHints(GfxRenderer& renderer, const char* backLabel, const char* confirmLabel) const {
  const auto labels = ListGrammar::labelsFor(shape(), availability());
  char left[48];
  char right[48];
  composeFront(left, sizeof(left), kPrevGlyph, labels.left, declaration.left.label, StrId::STR_DIR_UP);
  composeFront(right, sizeof(right), kNextGlyph, labels.right, declaration.right.label, StrId::STR_DIR_DOWN);
  const auto hints = input.mapHints(backLabel, confirmLabel, left, right, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);
}
```

- [ ] **Step 3: Format and compile**

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/activities/ListController.h src/activities/ListController.cpp
```

Build `default` in PowerShell (baseline command). Expected: SUCCESS. `ListController` is compiled but nothing uses it yet, so the linker may drop it and the flash delta may be ~0.

- [ ] **Step 4: Commit**

```bash
git add src/activities/ListController.h src/activities/ListController.cpp
git commit -m "feat(lists): ListController runs a list's buttons, hints and touch via ListGrammar

A member each list screen holds, behind a small ListHost interface. It is the
only reader of button events on its list and drains them all each tick; it
pages on long Left/Right with hold-to-repeat, routes swipes and row taps, and
draws both hint strips from the same declaration the rules read."
```

### Task 5: OPDS browser on `ListController` (catalog + format picker), with docs

The catalog declares the pair **Search / Info**. Search applies when the feed has a search template, Info when a book is selected; neither side ever steps. The format picker keeps the default pair. Both lists page by the rows they actually draw, and the catalog's row count now comes from the content area instead of a fixed 23, which also keeps rows on screen in landscape.

Every other OPDS state moves to events too. That is required, not tidying: the controller drains events, so a state that still read levels would let its press reach the catalog a second time (Review Focus 1). For the same reason the browser drains the event queue after each blocking download, whose callback reads Back by level to abort.

**Files:**
- Modify: `src/activities/browser/OpdsBookBrowserActivity.h` (whole class body: members, hosts, controllers)
- Modify: `src/activities/browser/OpdsBookBrowserActivity.cpp`:
  - constants block, lines 35-56
  - `onEnter`, lines ~228-245
  - `loop`, lines 290-411
  - `render`, lines 435-645 (BROWSING and FORMAT_SELECTION parts)
  - `fetchFeed`'s reserve, line ~666
  - `downloadBook`, after each `HttpDownloader::downloadToFile` call
  - `launchSearch`, line ~988
  - `selectListRow`, `pageList` (end of file)
- Modify: `USER_GUIDE.md` (TOC line 7, new subsection after §1 Button Layout, §3.7.5), `RELEASE_NOTES.md`

**Interfaces:**
- Consumes: Task 4's `ListHost`, `ListDeclaration`, `ListAction`, `ListController`; Task 3's `ListGrammar::Side`.
- Produces: no new public API. Private additions are `CatalogHost`, `FormatHost`, `catalogList`, `formatList`, `catalogRowsPerPage()`, `showBookDetail(const OpdsEntry&)`, `closeFormatPicker()` and `retryAfterError()`.

- [ ] **Step 1: Header changes**

In `OpdsBookBrowserActivity.h`:

1. Replace `#include "util/ButtonNavigator.h"` with `#include "../ListController.h"`.
2. In the constructor's init list, delete `buttonNavigator(),` so it reads:

```cpp
      : Activity("OpdsBookBrowser", renderer, mappedInput), server(std::move(server)), initialQuery_(std::move(initialQuery)) {}
```

3. Delete these members: `ButtonNavigator buttonNavigator;`, `bool consumeConfirm = false;`, `bool consumeBack = false;`, and `ButtonNavigator formatNavigator;` with its two-line comment.
4. After `bool preventAutoSleep() override;` (end of the private section), add:

```cpp
  int catalogRowsPerPage() const;
  void showBookDetail(const OpdsEntry& entry);
  void closeFormatPicker();
  void retryAfterError();

  // Short Left/Right on the catalog: Search where the feed offers one, Info on a book, and never a
  // step (an action that does not apply leaves its side doing nothing). The format picker keeps
  // the default pair.
  static constexpr ListDeclaration kCatalogDeclaration{{true, StrId::STR_SEARCH}, {true, StrId::STR_INFO}};
  static constexpr ListDeclaration kFormatDeclaration{};

  struct CatalogHost final : ListHost {
    explicit CatalogHost(OpdsBookBrowserActivity& browser) : browser(browser) {}
    int listCount() const override;
    int listPageRows() const override;
    bool listActionAvailable(ListGrammar::Side side, int row) const override;
    void onListSelectionChanged() override;
    void onListActivate(int row, bool longPress) override;
    void onListBack() override;
    void onListHome() override;
    void onListAction(ListGrammar::Side side, int row) override;
    OpdsBookBrowserActivity& browser;
  };

  struct FormatHost final : ListHost {
    explicit FormatHost(OpdsBookBrowserActivity& browser) : browser(browser) {}
    int listCount() const override;
    int listPageRows() const override;
    void onListSelectionChanged() override;
    void onListActivate(int row, bool longPress) override;
    void onListBack() override;
    void onListHome() override;
    OpdsBookBrowserActivity& browser;
  };

  // Declared after selectorIndex / formatSelectorIndex, which they hold references to.
  CatalogHost catalogHost{*this};
  FormatHost formatHost{*this};
  ListController catalogList{mappedInput, buttonEvents, catalogHost, selectorIndex, kCatalogDeclaration};
  ListController formatList{mappedInput, buttonEvents, formatHost, formatSelectorIndex, kFormatDeclaration};
```

- [ ] **Step 2: Constants and row geometry**

In the `.cpp` anonymous namespace, replace `constexpr int PAGE_ITEMS = 23;` with:

```cpp
// The catalog draws CATALOG_ROW_HEIGHT rows from CATALOG_LIST_TOP down to the foot of the content
// area; catalogRowsPerPage() turns that into the page both the render and paging use.
constexpr int CATALOG_LIST_TOP = 60;
constexpr int CATALOG_ROW_HEIGHT = 30;
// Entries reserved before a fetch: about one portrait page, plus the prev/next navigation entries.
constexpr size_t FEED_RESERVE_ENTRIES = 25;
```

In `fetchFeed`, replace `entryOffsets.reserve(PAGE_ITEMS + 2);  // +2 for prev/next nav entries` with:

```cpp
  entryOffsets.reserve(FEED_RESERVE_ENTRIES);
```

The `.cpp` needs no new include; the header brings in `ListController.h`. Add this definition after `preventAutoSleep`:

```cpp
int OpdsBookBrowserActivity::catalogRowsPerPage() const {
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  const int rows = (contentRect.y + contentRect.height - CATALOG_LIST_TOP) / CATALOG_ROW_HEIGHT;
  return std::max(1, std::min(rows, ListTouchBand::kMaxRows));
}
```

- [ ] **Step 3: Small extracted actions**

Add after `catalogRowsPerPage`:

```cpp
void OpdsBookBrowserActivity::showBookDetail(const OpdsEntry& entry) {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();
  fetchCoverForEntry(entry);
  state = BrowserState::BOOK_DETAIL;
  requestUpdate();
}

void OpdsBookBrowserActivity::closeFormatPicker() {
  state = BrowserState::BROWSING;
  selectedBookIndex = -1;
  formatSelectionLabels.clear();
  requestUpdate();
}

void OpdsBookBrowserActivity::retryAfterError() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
  } else {
    launchWifiSelection();
  }
}
```

- [ ] **Step 4: The hosts**

Add after the actions from Step 3:

```cpp
int OpdsBookBrowserActivity::CatalogHost::listCount() const { return static_cast<int>(browser.entryOffsets.size()); }

int OpdsBookBrowserActivity::CatalogHost::listPageRows() const { return browser.catalogRowsPerPage(); }

bool OpdsBookBrowserActivity::CatalogHost::listActionAvailable(const ListGrammar::Side side, const int row) const {
  if (side == ListGrammar::Side::Left) return !browser.searchTemplate.empty();
  // Bounds first: an empty feed has no entry to read.
  return row >= 0 && row < listCount() && browser.getEntry(row).type == OpdsEntryType::BOOK;
}

void OpdsBookBrowserActivity::CatalogHost::onListSelectionChanged() { browser.requestUpdate(); }

void OpdsBookBrowserActivity::CatalogHost::onListActivate(const int row, bool /*longPress*/) {
  if (row < 0 || row >= listCount()) return;
  const auto entry = browser.getEntry(row);
  entry.type == OpdsEntryType::BOOK ? browser.chooseBookFormat(entry) : browser.navigateToEntry(entry);
}

void OpdsBookBrowserActivity::CatalogHost::onListBack() { browser.navigateBack(); }

void OpdsBookBrowserActivity::CatalogHost::onListHome() { browser.onGoHome(); }

void OpdsBookBrowserActivity::CatalogHost::onListAction(const ListGrammar::Side side, const int row) {
  if (side == ListGrammar::Side::Left) {
    browser.launchSearch();
    return;
  }
  browser.showBookDetail(browser.getEntry(row));
}

int OpdsBookBrowserActivity::FormatHost::listCount() const {
  return static_cast<int>(browser.formatSelectionLabels.size());
}

int OpdsBookBrowserActivity::FormatHost::listPageRows() const {
  return formatItemsPerPage(UITheme::getContentRect(browser.renderer, true, true));
}

void OpdsBookBrowserActivity::FormatHost::onListSelectionChanged() { browser.requestUpdate(); }

void OpdsBookBrowserActivity::FormatHost::onListActivate(const int row, bool /*longPress*/) {
  const auto entry = browser.getEntry(browser.selectedBookIndex);
  if (row < 0 || row >= static_cast<int>(entry.acquisitionLinks.size())) return;
  browser.downloadBook(entry, entry.acquisitionLinks[row]);
}

void OpdsBookBrowserActivity::FormatHost::onListBack() { browser.closeFormatPicker(); }

void OpdsBookBrowserActivity::FormatHost::onListHome() { browser.onGoHome(); }
```

(`formatSelectionLabels` holds one label per acquisition link, built by `buildOpdsFormatSelectionLabels(book.acquisitionLinks, …)` in `chooseBookFormat`. Counting it avoids an SD read per event.)

- [ ] **Step 5: Replace `loop()`**

Replace the whole of `OpdsBookBrowserActivity::loop()` with:

```cpp
void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (state == BrowserState::BROWSING) {
    catalogList.update();
    return;
  }

  if (state == BrowserState::FORMAT_SELECTION) {
    if (selectedBookIndex < 0 || selectedBookIndex >= static_cast<int>(entryOffsets.size()) ||
        getEntry(selectedBookIndex).acquisitionLinks.empty()) {
      closeFormatPicker();
      return;
    }
    formatList.update();
    return;
  }

  // The states below are not lists, but they read button events too, never levels: the lists drain
  // the event queue, so a press read here by level would reach the catalog a second time as an
  // event once it is back on screen (Back closing Book Detail would also leave the feed).
  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    const auto button = event.button;
    if (state == BrowserState::ERROR) {
      if (button == MappedInputManager::Button::Confirm) {
        retryAfterError();
        return;
      }
      if (button == MappedInputManager::Button::Back) {
        navigateBack();
        return;
      }
    } else if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
      if (button == MappedInputManager::Button::Back) {
        state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
        return;
      }
    } else if (state == BrowserState::BOOK_DETAIL) {
      if (button == MappedInputManager::Button::Back ||
          MappedInputManager::isDirection(button, MappedInputManager::Direction::Right)) {
        state = BrowserState::BROWSING;
        requestUpdate();
        return;
      }
      if (button == MappedInputManager::Button::Confirm) {
        const auto entry = getEntry(selectorIndex);
        state = BrowserState::BROWSING;
        requestUpdate();
        chooseBookFormat(entry);
        return;
      }
    }
    // DOWNLOADING: the download reads Back by itself; any other press is dropped.
  }
}
```

The `consumeConfirm`/`consumeBack` guards go away. They swallowed the level-release of the key that closed the search keyboard. That release never becomes an event: `ButtonEventManager::drain()` runs on every activity transition, and a release with no press-down after it emits nothing.

- [ ] **Step 6: Remove the remaining `consumeConfirm`/`consumeBack` uses**

In `onEnter()`, delete the lines `consumeConfirm = false;` and `consumeBack = false;`. In `launchSearch()`, delete `consumeConfirm = true;`. Then run:

```bash
grep -n "consumeConfirm\|consumeBack\|buttonNavigator\|formatNavigator\|PAGE_ITEMS" src/activities/browser/OpdsBookBrowserActivity.*
```

Expected: no output.

- [ ] **Step 7: Drain the queue after each blocking download**

In `downloadBook`, directly after the statement that ends the book download (`const auto result = HttpDownloader::downloadToFile(...);`, the call whose callback returns `!mappedInput.wasPressed(MappedInputManager::Button::Back)`), add:

```cpp
  // The download read Back by level to abort. Drop the events it left behind, or that same press
  // would reach the catalog afterwards as an event and leave the feed.
  buttonEvents.drain();
```

Do the same directly after the cover download's `const auto coverDlResult = HttpDownloader::downloadToFile(...);` statement:

```cpp
        buttonEvents.drain();
```

- [ ] **Step 8: Render: content rect, catalog rows, both hint strips**

In `render()`:

1. Replace

```cpp
  // Only the browsing list labels its side buttons (see below); every other state uses Back and
  // Confirm alone, so it keeps the full width.
  const Rect contentRect = UITheme::getContentRect(renderer, true, state == BrowserState::BROWSING);
```

with

```cpp
  // The two lists, the catalog and the format picker, label their side buttons; every other state
  // uses Back and Confirm alone, so it keeps the full width.
  const bool showsList = state == BrowserState::BROWSING || state == BrowserState::FORMAT_SELECTION;
  const Rect contentRect = UITheme::getContentRect(renderer, true, showsList);
```

2. In the `FORMAT_SELECTION` block, replace

```cpp
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
```

with

```cpp
    formatList.drawHints(renderer, tr(STR_BACK), tr(STR_DOWNLOAD));
```

3. Replace the browsing section from `// Browsing state` through the end of the row loop (before the final `renderer.displayBuffer();`) with:

```cpp
  // Browsing state. The selected entry's type decides the Confirm label; the Left/Right labels
  // come from the catalog's declaration (Search / Info, or the page glyph alone).
  const bool selectedIsBook = !entryOffsets.empty() && getEntry(selectorIndex).type == OpdsEntryType::BOOK;
  catalogList.drawHints(renderer, tr(STR_BACK), selectedIsBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN));

  if (entryOffsets.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_NO_ENTRIES));
    renderer.displayBuffer();
    return;
  }

  const int rowsPerPage = catalogRowsPerPage();
  const auto pageStartIndex = selectorIndex / rowsPerPage * rowsPerPage;
  // Entry rows published for touch. The top is a bare CATALOG_LIST_TOP, NOT contentRect.y + it as
  // the chapter selectors use — matched to the fill below, which is where the row visibly is.
  ListTouchBand::recordUniformRows(contentRect.x, contentRect.width - 1, CATALOG_LIST_TOP - 2, CATALOG_ROW_HEIGHT,
                                   pageStartIndex,
                                   std::min<int>(rowsPerPage, static_cast<int>(entryOffsets.size()) - pageStartIndex));
  renderer.fillRect(contentRect.x, CATALOG_LIST_TOP + (selectorIndex % rowsPerPage) * CATALOG_ROW_HEIGHT - 2,
                    contentRect.width - 1, CATALOG_ROW_HEIGHT);

  for (size_t i = pageStartIndex; i < entryOffsets.size() && i < static_cast<size_t>(pageStartIndex + rowsPerPage);
       i++) {
    const auto entry = getEntry(i);

    // Format display text with type indicator
    std::string displayText;
    if (entry.type == OpdsEntryType::NAVIGATION) {
      displayText = "> " + entry.title;  // Folder/navigation indicator
    } else {
      // Book: "Title - Author" or just "Title"
      displayText = entry.title;
      if (!entry.author.empty()) {
        displayText += " - " + entry.author;
      }
    }

    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), contentRect.width - 40);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, CATALOG_LIST_TOP + (i % rowsPerPage) * CATALOG_ROW_HEIGHT,
                      item.c_str(), i != static_cast<size_t>(selectorIndex));
  }
```

- [ ] **Step 9: Touch hooks**

Replace `selectListRow` and the `pageList` from PR 1 at the end of the `.cpp` with:

```cpp
ListRowTap::Result OpdsBookBrowserActivity::selectListRow(const int index) {
  // Two lists, two states. `state` is read on the loop task and may have moved on since the render
  // that recorded the band, so it decides which list a tap means.
  if (state == BrowserState::FORMAT_SELECTION) return formatList.tapRow(index);
  if (state == BrowserState::BROWSING) return catalogList.tapRow(index);
  return ListRowTap::Result::Rejected;
}

bool OpdsBookBrowserActivity::pageList(const ListPageDirection direction) {
  const int step = direction == ListPageDirection::Forward ? 1 : -1;
  if (state == BrowserState::BROWSING) {
    catalogList.page(step);
  } else if (state == BrowserState::FORMAT_SELECTION) {
    formatList.page(step);
  }
  return true;
}
```

In the header, update the `pageList` comment from PR 1 to:

```cpp
  // A swipe pages whichever list is on screen; it never reaches Left/Right (Search and Info).
  bool pageList(ListPageDirection direction) override;
```

- [ ] **Step 10: Format and build**

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/activities/browser/OpdsBookBrowserActivity.h src/activities/browser/OpdsBookBrowserActivity.cpp
grep -rn "ButtonNavigator" src/activities/browser/
```

Expected: no `ButtonNavigator` match. Build `default`, `x4pro` and `lilygo_t5s3` in PowerShell. Expected: SUCCESS. The `default` Flash delta against the PR 1 baseline must be at most +2 KB; record it.

- [ ] **Step 11: User guide — "Moving through lists" and OPDS browsing**

In `USER_GUIDE.md`:

1. In the table of contents, after the line `    - [Button Layout](#button-layout)`, add:

```markdown
    - [Moving through lists](#moving-through-lists)
```

2. After the §1 paragraph that ends `...and all touch behaviour can be switched off.` and before `### Taking a Screenshot`, insert:

```markdown
### Moving through lists

The firmware's lists are moving to one set of buttons, a screen at a time. So far the **OPDS
catalog** works this way; other lists still behave as their own sections describe.

| Button | Press | Hold |
| --- | --- | --- |
| **Up / Down** | Move one row | Jump to the first / last row |
| **Left / Right** | Move one row, or the action the screen shows | Page back / forward; keep holding to keep paging |
| **Confirm** | Open the selected row | The same as a press |
| **Back** | Go back | Return to the Home screen |

The hints show which is which. **«** on the Left box and **»** on the Right box mean *hold to
page*. The word after the arrow is what a short press does: **« Up** / **» Down** where Left and
Right move the selection, or the screen's own action, such as **« Search** / **» Info**. A box
showing only **«** or **»** has no action for the selected row right now. A short press does
nothing there, and holding it still pages.

On the **X4 Pro** and the **LilyGo T5 S3**, tap a hint box to press that button and hold the box
to hold it. Lists always show the side boxes **Up** and **Down**, so the T5 S3, which has no Up
key, can always move up.
```

3. At the end of §3.7.5, directly before `#### 3.7.6 Web Settings (WiFi + OPDS)`, insert:

```markdown
**Browsing a catalog.** The catalog follows **[Moving through lists](#moving-through-lists)**.
A short **Left** opens **Search** when the catalog offers one, and a short **Right** opens **Info**
for the selected book. Where either does not apply, that button does nothing on a short press,
and its hint shows only the arrow. Hold **Left** / **Right**, or swipe on a touch screen, to page
through a long catalog; **Up** / **Down** move one row. **Confirm** opens a folder or downloads a
book. When a book comes in several formats, you choose one from a short list that works the same
way.
```

4. In §5.2's gesture table, change the row edited in PR 1 to:

```markdown
| Swipe up / down over a list | Page the list (on Bookmarks, Starred pages and Wi-Fi networks, a swipe does nothing for now) |
```

- [ ] **Step 12: Release note**

Under `## Unreleased` in `RELEASE_NOTES.md`, add a `### OPDS` heading if absent, then:

```markdown
- **OPDS catalogs can be paged, and their buttons do what the hints say** (#374). Hold **Left** / **Right**, or swipe, to page through a catalog; **Up** / **Down** move one row. A short **Left** is **Search** and a short **Right** is **Info** for a book. Where those do not apply, the hint shows only « or », instead of "Up" / "Down" labels on buttons that did nothing. A long catalog in landscape no longer runs off the bottom of the screen. The side Up/Down hints also appear when choosing a download format, so the T5 S3 can move up there.
```

- [ ] **Step 13: Run the host suite**

```bash
cmake --build test/build -j 8 -- -k 0 && ctest --test-dir test/build --output-on-failure -j 8 | tail -3
```

Expected: the baseline count + 26 (ListGrammar) + 3 (DoubleActionWait, if PR 2 is in the base), all passing.

- [ ] **Step 14: Commit**

```bash
git add src/activities/browser/OpdsBookBrowserActivity.h src/activities/browser/OpdsBookBrowserActivity.cpp USER_GUIDE.md RELEASE_NOTES.md
git commit -m "feat(opds): the catalog and format picker follow the list button scheme (#374)

The catalog declares Search / Info on short Left/Right; where either does not
apply its side does nothing and its hint shows the page glyph alone, so no
button is labelled for a move it does not make. Long Left/Right and swipes
page by the rows actually drawn, which also keeps a long catalog on screen in
landscape. The format picker keeps the default pair and gains side hints.

Every OPDS state now reads button events, and the queue is drained after each
blocking download, so a press is handled once whichever state it lands in.
User guide: new Moving through lists section, OPDS browsing paragraph."
```

- [ ] **Step 15: Device checklist, then open the PR**

On an **X4 or X3** (physical keys):

1. Root catalog, folder selected, no search: front hints read `«` / `»`, side hints read `Up` / `Down`. A short Left/Right does nothing; Up/Down move one row.
2. Feed with a search link: short Left opens the search keyboard. Cancelling returns to the catalog with no extra step or back.
3. Book selected: short Right opens Info. Back, or a short Right in Info, returns to the same row.
4. A catalog longer than one screen: hold Right. It pages after about 1 s, keeps paging about every 0.5 s while held, and stops on the last page without wrapping. Hold Left back to the first page.
5. Hold Down: jumps to the last row. Hold Up: jumps to the first.
6. **Book Detail → Back:** returns to the catalog and stays in the same feed (no second Back).
7. **Error screen → Back** (e.g. turn the server off, open a folder): goes back exactly one level.
8. **Abort a download with Back:** returns to the catalog and stays in the same feed.
9. A feed with no entries: arrows do nothing, Back goes back, the hint shows `«` / `»` (or `« Search` if the feed has search).
10. Book with several formats: the picker shows side Up/Down hints. Up/Down/Left/Right step, Confirm downloads, Back returns to the catalog.
11. Hold Back in the catalog: returns to the Home screen.

On a **T5S3 or X4 Pro** (touch):

12. Tap the side `Up` / `Down` boxes: one row each. A long tap on the `«` / `»` box pages once.
13. Swipe up/down over the catalog: pages. A swipe never opens Search or Info.
14. Tap a row: it is selected. Tap it again: it opens.

```bash
git push -u origin feat/list-controller-opds
gh pr create --base fix/list-swipe-no-actions --title "feat(lists): ListGrammar + ListController, piloted on OPDS (closes #374)" --body "<summary, the two flash deltas (Task 4, Task 5), the checklist with results; note the PR is stacked on PR 1 and retargets to master after it merges>"
```

After PR 3 merges, the superseded branch `fix/374-opds-front-buttons` (commits `e8d974a7e`, `6e45c08fd`) can be deleted. Ask the user before deleting it.

---

## After PR 3

Write the plan for PRs 4–8 (spec §6 steps 4–8) against the `ListController` API as it landed. It needs to add the `freeink::ui::ListNav` selection adapter (spec §2) for the `UiListActivity`/`MenuListActivity`/`TabbedUiListActivity` families. Every screen in the spec's §3 mapping table needs a task, and every task updates the user-guide sections listed in spec §5. The "Moving through lists" qualifier ("So far the OPDS catalog works this way…") is reworded each PR and removed in PR 8.
