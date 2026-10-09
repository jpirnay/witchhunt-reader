# Library Plan 3: the screen and its entry — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Browse Files and Recent Books become one Library screen with Books · Recent · New · Authors tabs, reached from one Home entry that reopens the last tab.

**Architecture:** `FileBrowserActivity` keeps its own input model (its `navigateButtons()`/`handleCustomInput()` bypass `ListController`) and gains a tab bar drawn by a composer shared with `TabbedUiListActivity`. A tab switch swaps the model's mode in place (`FileBrowserModel::setMode`) and restores the row (and Books' folder, Authors' open author) the tab was left on. One `ReturnTo::Library` route and `ActivityManager::goToLibrary()` replace the two browser routes.

**Tech Stack:** C++20 on ESP32-C3/S3 (PlatformIO, `-fno-exceptions`), FreeInkUI, gtest host tests under `test/` (CMake).

**Spec:** `docs/superpowers/specs/2026-10-09-library-view-design.md` (§4, §5, §6). Plans 1, 2a and 2b built the metadata, the index and the builder.

## Global Constraints

- No resident RAM growth beyond a few bytes per tab; large temporary buffers only in the lent secondary framebuffer.
- Strings in `lib/I18n/translations/english.yaml` only; regenerate i18n through the build, never by running `gen_i18n.py` by hand.
- Tab labels get their own keys (`STR_TAB_*`), not `STR_NEW`.
- `HomeMenuAction::Library`, Folder icon; setting `showLibraryOnHome`; migration: shown if either old setting was on.
- `APP_STATE.libraryTab` (default Books), written on leaving the Library, only if it changed. `APP_STATE.addedBooksView`, `APP_STATE.authorsView`.
- Sort and search on Books only. "Remove from recents" on Recent only.
- All files and the pickers have no tab bar.
- clang-format 21 style (CI); build with `PLATFORMIO_CORE_DIR='C:\pio'`.

## Rulings carried in from design (deviations from the spec, to ledger)

- **No rebase onto `TabbedUiListActivity`, no bar focus.** The spec moves `FileBrowserActivity` onto `TabbedUiListActivity` with a `showsTabBar()` hook. The browser overrides `navigateButtons()` and `handleCustomInput()` and never runs `ListController`, whose position grammar the bar focus lives in, so the rebase is a rewrite of the browser's input. Instead: long Up/Down and a tab tap switch tabs; Back at a tab's top level goes Home (as Browse Files and Recent Books do today); the bar is drawn unfocused (dithered pill). Cost if wrong: a follow-up that adds bar focus for button-only discoverability.
- **New/Authors rows get Open, Fetch, Mark read, Info, Delete cache, Remove, Go to folder — not Move to folder / New folder.** `moveToFolder()` appends the row's entry, which is a path on these lists. Cost if wrong: one more menu entry after a basename fix.
- **Remove in an open author drops the row after the rebuild it triggers**, not on the spot: `openAuthor()` does not stat each book. Cost: the row lingers seconds.
- **Refresh library and "Library too large" deferred.** The builder supports both (`resolveAll`, partial flag); the menu entry and the notice are not in this plan.

## Review Focus

1. **A long Up/Down in a list view** must switch tab without moving the old tab's row and without the navigator's 1.5 s jump-to-end firing on the new tab while the key is still held.
2. **Opening an author row** must never be taken for a folder: no path surgery, no folder-delete from Options, no folder count walk on "/Name".
3. **A book opened from New or an open author** must come back to the same tab, author and row (`ReturnTo::Library`).
4. **The index rebuild** must only step and reload while New or Authors is showing; Books/Recent must not lose their rows to a reload.
5. **Settings files written before this change** keep the Library on Home when either old entry was there.

---

### Task 1: Library tab type, Home entry, settings migration, state fields

**Files:**
- Create: `src/activities/home/LibraryTab.h`
- Modify: `src/activities/home/HomeMenu.h`, `src/activities/home/HomeMenu.cpp`, `src/CrossPointSettings.h:728-732`, `src/SettingsList.h:230-236`, `src/JsonSettingsIO.cpp` (loadSettings after the `forEachSetting` loop; saveState/loadState), `src/CrossPointState.h:141-144`, `src/activities/ActivityManager.cpp:626-631`, `lib/I18n/translations/english.yaml`
- Test: `test/home_menu/HomeMenuTest.cpp`

**Interfaces:**
- Produces: `enum class LibraryTab : uint8_t { Books, Recent, New, Authors }`, `LIBRARY_TAB_COUNT`, `libraryTabFrom(uint8_t)`, `libraryTabStep(LibraryTab, int)`; `HomeMenuAction::Library`; `libraryOnHomeFromLegacy(uint8_t, uint8_t)`; `SETTINGS.showLibraryOnHome`; `APP_STATE.libraryTab/addedBooksView/authorsView`; strings `STR_LIBRARY, STR_TAB_BOOKS, STR_TAB_RECENT, STR_TAB_NEW, STR_TAB_AUTHORS, STR_NO_NEW_BOOKS`.

- [ ] **Step 1: Write the failing tests** — in `HomeMenuTest.cpp`: `SETTINGS.showLibraryOnHome` replaces both old fields in `SetUp` and every test; `A::Library` replaces `A::FileBrowser`; `A::Recents` disappears from every expected list. Add:

```cpp
#include "activities/home/LibraryTab.h"

TEST_F(HomeMenuTest, OneLibraryEntryLabelledLibrary) {
  std::vector<HomeMenuEntry> entries;
  collectHomeMenuEntries(HomeMenuPlacement::Home, everything, entries);
  ASSERT_FALSE(entries.empty());
  EXPECT_EQ(entries.front().action, A::Library);
  EXPECT_EQ(entries.front().label, StrId::STR_LIBRARY);
  EXPECT_EQ(entries.front().icon, Folder);
}

// Browse Files and Recent Books had a home entry each; the Library is shown where either was.
TEST(HomeMenuMigration, LibraryShownWhereEitherOldEntryWas) {
  EXPECT_EQ(libraryOnHomeFromLegacy(1, 1), 1);
  EXPECT_EQ(libraryOnHomeFromLegacy(1, 0), 1);
  EXPECT_EQ(libraryOnHomeFromLegacy(0, 1), 1);
  EXPECT_EQ(libraryOnHomeFromLegacy(0, 0), 0);
}

TEST(LibraryTabs, StoredValueOutOfRangeOpensBooks) {
  EXPECT_EQ(libraryTabFrom(2), LibraryTab::New);
  EXPECT_EQ(libraryTabFrom(LIBRARY_TAB_COUNT), LibraryTab::Books);
  EXPECT_EQ(libraryTabFrom(255), LibraryTab::Books);
}

TEST(LibraryTabs, StepWrapsAtBothEnds) {
  EXPECT_EQ(libraryTabStep(LibraryTab::Books, 1), LibraryTab::Recent);
  EXPECT_EQ(libraryTabStep(LibraryTab::Authors, 1), LibraryTab::Books);
  EXPECT_EQ(libraryTabStep(LibraryTab::Books, -1), LibraryTab::Authors);
}
```

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build test/build --target HomeMenuTest` 
Expected: compile errors (`LibraryTab.h` missing, `A::Library`, `showLibraryOnHome`, `libraryOnHomeFromLegacy`, `STR_LIBRARY` undefined).

- [ ] **Step 3: Implement**

`src/activities/home/LibraryTab.h`:

```cpp
#pragma once

#include <cstdint>

// The Library screen's tabs, in bar order: Books (the folders), Recent, New (the book index's newest)
// and Authors. Stored as APP_STATE.libraryTab, so the order is part of the state file.
enum class LibraryTab : uint8_t { Books, Recent, New, Authors };
constexpr uint8_t LIBRARY_TAB_COUNT = 4;

// A stored tab, or Books when the value is out of range (a state file from another build, or damaged).
constexpr LibraryTab libraryTabFrom(const uint8_t stored) {
  return stored < LIBRARY_TAB_COUNT ? static_cast<LibraryTab>(stored) : LibraryTab::Books;
}

// The tab `direction` steps from `tab`, wrapping at both ends.
constexpr LibraryTab libraryTabStep(const LibraryTab tab, const int direction) {
  const int count = LIBRARY_TAB_COUNT;
  return static_cast<LibraryTab>(((static_cast<int>(tab) + direction) % count + count) % count);
}
```

`HomeMenu.h`: `FileBrowser, Recents,` → `Library,`; add after `collectHomeMenuEntries`:

```cpp
// The Library entry's setting for a settings file written before it existed, when Browse Files and
// Recent Books had an entry each: shown on the home screen if either was.
[[nodiscard]] uint8_t libraryOnHomeFromLegacy(uint8_t browseFiles, uint8_t recentBooks);
```

`HomeMenu.cpp`: the first two rows become

```cpp
    {{HomeMenuAction::Library, StrId::STR_LIBRARY, Folder}, Requires::Nothing, &CrossPointSettings::showLibraryOnHome},
```

and at the end

```cpp
uint8_t libraryOnHomeFromLegacy(const uint8_t browseFiles, const uint8_t recentBooks) {
  return browseFiles != 0 || recentBooks != 0 ? 1 : 0;
}
```

`CrossPointSettings.h`: `showBrowseFilesOnHome`/`showRecentBooksOnHome` → `uint8_t showLibraryOnHome = 1;`.
`SettingsList.h`: the two toggles → one:

```cpp
  emit(SettingInfo::Toggle(StrId::STR_LIBRARY, &CrossPointSettings::showLibraryOnHome, "showLibraryOnHome",
                           StrId::STR_CAT_DISPLAY)
           .withSubmenu(StrId::STR_MENU_DISP_HOME));
```

`JsonSettingsIO.cpp` loadSettings, right after the `forEachSetting` loop closes (`#include "activities/home/HomeMenu.h"`):

```cpp
  // Browse Files and Recent Books had a home entry each before they became the Library's tabs. The
  // Library shows where either did; the old keys go with the next save.
  if (doc["showLibraryOnHome"].isNull()) {
    s.showLibraryOnHome =
        libraryOnHomeFromLegacy(doc["showBrowseFilesOnHome"] | uint8_t{1}, doc["showRecentBooksOnHome"] | uint8_t{1});
  }
```

`CrossPointState.h`, after `recentBooksView`:

```cpp
  // The Library tab last left (a LibraryTab), reopened from Home.
  uint8_t libraryTab = 0;
  // How New and Authors show their lists, each chosen there from Options > View: New as covers,
  // like Recent Books; Authors in Details, its rows being names.
  uint8_t addedBooksView = 2;
  uint8_t authorsView = 1;
```

saveState: `doc["libraryTab"] = s.libraryTab; doc["addedBooksView"] = s.addedBooksView; doc["authorsView"] = s.authorsView;`
loadState (validated):

```cpp
  s.libraryTab = static_cast<uint8_t>(libraryTabFrom(doc["libraryTab"] | uint8_t{0}));
  s.addedBooksView = doc["addedBooksView"] | uint8_t{CrossPointSettings::BROWSER_VIEW_COVERS};
  if (s.addedBooksView >= CrossPointSettings::FILE_BROWSER_VIEW_COUNT) s.addedBooksView = CrossPointSettings::BROWSER_VIEW_COVERS;
  s.authorsView = doc["authorsView"] | uint8_t{CrossPointSettings::BROWSER_VIEW_DETAILS};
  if (s.authorsView >= CrossPointSettings::FILE_BROWSER_VIEW_COUNT) s.authorsView = CrossPointSettings::BROWSER_VIEW_DETAILS;
```

`ActivityManager.cpp` goToHomeMenuAction: the `FileBrowser` and `Recents` cases become `case HomeMenuAction::Library: goToFileBrowser(); break;` (Task 5 points it at `goToLibrary`).

`english.yaml`, beside `STR_UNKNOWN_AUTHOR`:

```yaml
STR_LIBRARY: "Library"
STR_TAB_BOOKS: "Books"
STR_TAB_RECENT: "Recent"
STR_TAB_NEW: "New"
STR_TAB_AUTHORS: "Authors"
STR_NO_NEW_BOOKS: "No new books"
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build test/build --target HomeMenuTest && test/build/home_menu/HomeMenuTest`
Expected: all HomeMenu, HomeMenuMigration and LibraryTabs tests pass.

- [ ] **Step 5: Full host suite, commit**

Run: `cmake --build test/build && ctest --test-dir test/build` → all pass (settings_grouping / setting_info_options may count Home rows: update counts if they name the removed toggle).

```bash
git add -A src lib/I18n test/home_menu && git commit -m "feat(library): one Library entry on Home; tab and view state"
```

---

### Task 2: One tab-bar composer

**Files:**
- Create: `src/activities/ListTabBar.h`, `src/activities/ListTabBar.cpp`
- Modify: `src/activities/TabbedUiListActivity.cpp` (`buildTabBar`), `src/activities/TabbedUiListActivity.h` (`tabBarHeight`)

**Interfaces:**
- Produces: `namespace ListTabBar { constexpr int16_t HEIGHT = 54; freeink::ui::TabBarProps props(UiAppHost::UiScreen&, const freeink::ui::TabItem*, uint8_t count, freeink::ui::ActionId, bool barFocused); }`

- [ ] **Step 1: Extract** — `ListTabBar.h`:

```cpp
#pragma once

#include "components/UiAppHost.h"

// The text tab bar the tabbed screens share: Settings, the reader menu and the Library.
namespace ListTabBar {

constexpr int16_t HEIGHT = 54;

// Props for `count` tabs (the caller's array, alive until the bar is drawn); a tap on one fires
// `action` with that tab's value. The selected tab is filled while the bar holds focus and a dithered
// pill while the focus is in the list, so the bar says whether the next Confirm changes tabs or acts
// on a row.
freeink::ui::TabBarProps props(UiAppHost::UiScreen& screen, const freeink::ui::TabItem* tabs, uint8_t count,
                               freeink::ui::ActionId action, bool barFocused);

}  // namespace ListTabBar
```

`ListTabBar.cpp` holds the body moved out of `TabbedUiListActivity::buildTabBar()` (the CrossInk attribution comment moves with it):

```cpp
#include "ListTabBar.h"

namespace fui = freeink::ui;

namespace ListTabBar {

fui::TabBarProps props(UiAppHost::UiScreen& screen, const fui::TabItem* tabs, const uint8_t count,
                       const fui::ActionId action, const bool barFocused) {
  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = count;
  props.action = action;
  props.inputMask = fui::InputTouch;
  props.text = screen.theme().bodyText;
  props.tabInset = fui::Insets{};
  props.contentInset = fui::Insets{};

  // Adapted from CrossInk (MIT): https://github.com/uxjulia/crossink -- the FreeInkUI tab
  // composition and touch tab routing at commit cd4b122e, which the settings screen and the
  // reader menu had each taken a copy of. The reader menu's icon-tab treatment (commit 60cc4da5)
  // stays with it, in its customizeTabBar().
  fui::StyleSet tabStyles;
  tabStyles.explicitlySet = true;
  tabStyles.normal.background = fui::Paint::solid(fui::Color::White);
  tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  tabStyles.normal.border = fui::Paint::solid(fui::Color::Black);
  tabStyles.normal.borderWidth = 1;
  tabStyles.selected.background =
      barFocused ? fui::Paint::solid(fui::Color::Black) : fui::Paint::dither(fui::Color::LightGray);
  tabStyles.selected.foreground = fui::Paint::solid(barFocused ? fui::Color::White : fui::Color::Black);
  tabStyles.selected.border = fui::Paint::solid(fui::Color::Black);
  tabStyles.selected.borderWidth = 1;
  tabStyles.focused = tabStyles.selected;
  tabStyles.active = tabStyles.selected;
  props.tabStyles = tabStyles;
  return props;
}

}  // namespace ListTabBar
```

`TabbedUiListActivity::buildTabBar()` keeps the tab array and becomes:

```cpp
  auto props = ListTabBar::props(screen, tabs, static_cast<uint8_t>(count), ACTION_USER, tabsFocused());
  customizeTabBar(screen, props);
  fui::tabBar(screen.frame(), screen.takeTop(tabBarHeight()), props);
```

`tabBarHeight()` default returns `ListTabBar::HEIGHT`.

- [ ] **Step 2: Verify** — firmware compile at the end of Task 5 covers it; host suite unaffected: `ctest --test-dir test/build` → all pass.

- [ ] **Step 3: Commit** — `git commit -m "refactor(ui): one tab-bar composer for the tabbed screens"`

---

### Task 3: Model — switch mode, open an author by hash, name it

**Files:** Modify `src/activities/home/FileBrowserModel.h`, `src/activities/home/FileBrowserModel.cpp`

**Interfaces:**
- Produces: `void setMode(Mode)`, `bool openAuthorByHash(uint32_t)`, `std::string openAuthorName()`, `uint32_t openAuthorKey() const`.

- [ ] **Step 1: Implement** (no host harness links `FileBrowserModel`; firmware compile in Task 5)

Header, after `getMode()`:

```cpp
  // Switches what is listed -- the Library's tabs -- letting go of everything the old mode held. The
  // folder path is kept; load() reads the new rows.
  void setMode(const Mode newMode) {
    clear();
    mode = newMode;
  }
```

After `closeAuthor()`:

```cpp
  // Opens the author whose hash is `hash`, after load(): how a return from a book, or a tab switched
  // back to, finds the author it left. False when the index has no such author.
  bool openAuthorByHash(uint32_t hash);
  // The open author's name as its row shows it; "" at the author list.
  std::string openAuthorName();
  // The open author's hash, for openAuthorByHash(); meaningful only while !atAuthorList().
  [[nodiscard]] uint32_t openAuthorKey() const { return openAuthorHash; }
```

Cpp — `loadAuthors()` reuses it:

```cpp
void FileBrowserModel::loadAuthors() {
  clearDeepSearch();
  deepRoot = "/";
  if (!bookIndex.isOpen()) bookIndex.open(library::INDEX_PATH);
  if (openAuthorRow < 0) return;
  closeAuthor();
  openAuthorByHash(openAuthorHash);
}

bool FileBrowserModel::openAuthorByHash(const uint32_t hash) {
  if (mode != Mode::Authors) return false;
  if (openAuthorRow >= 0) closeAuthor();
  library::AuthorRecord author{};
  for (uint16_t row = 0; row < bookIndex.header().authorCount; ++row) {
    if (bookIndex.author(row, author) && author.hash == hash) return openAuthor(row);
  }
  return false;
}

std::string FileBrowserModel::openAuthorName() {
  if (mode != Mode::Authors || openAuthorRow < 0) return "";
  std::string name = authorRowName(static_cast<size_t>(openAuthorRow));
  if (!name.empty()) name.pop_back();  // the folder mark
  return name;
}
```

- [ ] **Step 2: Commit** — `git commit -m "feat(library): the model switches mode and reopens an author by hash"`

---

### Task 4: The browser menu knows which list it serves

**Files:** Modify `src/activities/home/FileContextMenuActivity.h`, `src/activities/home/FileContextMenuActivity.cpp`

**Interfaces:**
- Produces: `enum class FileContextMenuActivity::ListSource : uint8_t { Folder, Recents, Index }`; the constructor's last parameter `bool recentsList` becomes `ListSource source = ListSource::Folder, uint8_t view = CrossPointSettings::BROWSER_VIEW_FILES`.

- [ ] **Step 1: Implement**

Header: add the enum before the constructor:

```cpp
  // The list the menu was opened on. Recents and Index (the Library's New and Authors) have a fixed
  // order and list books from all over the card: no sort, no hidden-files toggle, no search. Only
  // Recents' Remove takes a book off its list rather than off the card.
  enum class ListSource : uint8_t { Folder, Recents, Index };
```

Member `bool recentsList;` → `ListSource source;` (its comment generalised). Constructor init: `source(source)`, `browserView(view)`.

`buildMenuItems()`: `const bool fixedOrder = source != ListSource::Folder;` replaces `recentsList` in the sort and hidden-files guards and in `if (isBrowserMode) { if (fixedOrder) return; ...`. The tail:

```cpp
  if (fixedOrder) {
    // Off the list, not off the card: Recent Books swaps the file's own Remove for that.
    if (source == ListSource::Recents) {
      for (auto& item : menuItems) {
        if (item.nameId == StrId::STR_REMOVE) item.nameId = StrId::STR_REMOVE_FROM_RECENTS;
      }
    }
    if (offerGoToFolder) menuItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_FOLDER, SettingAction::None));
    return;
  }
```

`render()` header: `isBrowserMode ? std::string(source == ListSource::Recents ? tr(STR_MENU_RECENT_BOOKS) : source == ListSource::Index ? tr(STR_LIBRARY) : tr(STR_SORT_BY))`.

- [ ] **Step 2: Commit** with Task 5 (the two call sites change there): one commit.

---

### Task 5: The Library screen

**Files:**
- Modify: `src/activities/home/FileBrowserActivity.h`, `src/activities/home/FileBrowserActivity.cpp`, `src/activities/UiListActivity.h/.cpp` (`layoutListArea` spacer flag), `src/activities/ActivityManager.h/.cpp`, `src/activities/reader/ReaderActivity.cpp:783-787`

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: `ActivityManager::goToLibrary(LibraryTab, std::string path = {}, std::string focusName = {}, std::optional<uint32_t> author = {})`; `ReturnTo::Library` (selectIndex = tab, selectionContext = open author hash as decimal); `FileBrowserActivity::modeFor(LibraryTab)`, `FileBrowserActivity::openAuthorOnEnter(uint32_t)`. Removed: `goToFileBrowser`, `goToRecentBooks`, `ReturnTo::FileBrowser`, `ReturnTo::RecentBooks`.

- [ ] **Step 1: Routes.** `ActivityManager.h`: `enum class ReturnTo : uint8_t { Home, Library, AllFiles, GlobalBookmarks };` (comment: Library carries the tab in `selectIndex` and an open author in `selectionContext`); replace the two browser routes with

```cpp
  // The Library on `tab`: Books in `path` (the root when empty); `focusName` selected; on Authors,
  // `author` (a hash) open.
  void goToLibrary(LibraryTab tab, std::string path = {}, std::string focusName = {},
                   std::optional<uint32_t> author = std::nullopt);
```

(`#include "home/LibraryTab.h"`, `<optional>`). `ActivityManager.cpp`:

```cpp
void ActivityManager::goToLibrary(const LibraryTab tab, std::string path, std::string focusName,
                                  const std::optional<uint32_t> author) {
  auto library = std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path), std::move(focusName),
                                                       FileBrowserActivity::modeFor(tab));
  if (author) library->openAuthorOnEnter(*author);
  replaceActivity(std::move(library));
}
```

`returnFromChild()`:

```cpp
    case ReturnTo::Library: {
      std::optional<uint32_t> author;
      if (!hint.selectionContext.empty()) {
        author = static_cast<uint32_t>(std::strtoul(hint.selectionContext.c_str(), nullptr, 10));
      }
      goToLibrary(libraryTabFrom(static_cast<uint8_t>(hint.selectIndex)), std::move(hint.path),
                  std::move(hint.selectName), author);
      break;
    }
```

`goToHomeMenuAction`: `case HomeMenuAction::Library: goToLibrary(libraryTabFrom(APP_STATE.libraryTab)); break;`
`ReaderActivity::goToLibrary()`: `activityManager.goToLibrary(LibraryTab::Books, std::move(initialPath));`

- [ ] **Step 2: Layout.** `UiListActivity::layoutListArea(UiScreen&, int16_t extraTop = 0, int16_t extraBottom = 0, bool spacer = true)` — the closing `screen.spacer(...)` only `if (spacer)`; comment: "`spacer` false leaves room for a bar the caller puts directly under the header".

- [ ] **Step 3: Activity header.** `#include "LibraryTab.h"`. Public:

```cpp
  // The tab a mode is, and the mode a tab lists.
  static Mode modeFor(LibraryTab tab);
  // Authors: the author to open on entering (a return from one of its books).
  void openAuthorOnEnter(const uint32_t hash) {
    authorToOpen = true;
    authorToOpenHash = hash;
  }
```

Private:

```cpp
  // The Library: Books, Recent, New and Authors as tabs of one screen. Each tab is left where it
  // was: its row, Books its folder, Authors the author open in it.
  [[nodiscard]] bool libraryTabs() const;
  [[nodiscard]] LibraryTab currentTab() const;
  void showLibraryTab(LibraryTab tab);
  void composeLibraryTabBar(UiScreen& screen);
  static void tabActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  void loadRows();
  [[nodiscard]] ReturnHint returnHint(std::string selectName) const;
  std::array<int, LIBRARY_TAB_COUNT> tabRows{};
  std::string booksPath = "/";
  bool authorToOpen = false;
  uint32_t authorToOpenHash = 0;
  // A long Up/Down switches tab. In a list, the press stepped a row on its way down (stepOrigin is the
  // row it left, put back first); and the key still held must not reach the navigator, whose 1.5 s
  // hold jumps to the list's end (tabHold, until it is let go).
  int stepOrigin = 0;
  bool tabHold = false;
```

- [ ] **Step 4: Activity — tabs, rows, entry and exit.**

```cpp
FileBrowserActivity::Mode FileBrowserActivity::modeFor(const LibraryTab tab) {
  switch (tab) {
    case LibraryTab::Recent: return Mode::Recents;
    case LibraryTab::New: return Mode::Added;
    case LibraryTab::Authors: return Mode::Authors;
    default: return Mode::Books;
  }
}

bool FileBrowserActivity::libraryTabs() const {
  const Mode mode = model.getMode();
  return mode == Mode::Books || mode == Mode::Recents || mode == Mode::Added || mode == Mode::Authors;
}

LibraryTab FileBrowserActivity::currentTab() const {
  switch (model.getMode()) {
    case Mode::Recents: return LibraryTab::Recent;
    case Mode::Added: return LibraryTab::New;
    case Mode::Authors: return LibraryTab::Authors;
    default: return LibraryTab::Books;
  }
}
```

Constructor: `if (mode == Mode::Books) booksPath = model.path();` after `setPath`.

`loadRows()` takes the Recents refresh out of `onEnter()` (same comment), and `onEnter()` becomes `loadRows(); if (authorToOpen) { model.openAuthorByHash(authorToOpenHash); authorToOpen = false; } startLibraryBuildIfStale(); ...` and, after `UiListActivity::onEnter()`, `if (libraryTabs()) app.on(ACTION_USER, &FileBrowserActivity::tabActionTrampoline, this);`.

`onExit()`, before `model.clear()`:

```cpp
  // Home reopens the tab left last; written only when it changed.
  if (libraryTabs() && static_cast<uint8_t>(currentTab()) != APP_STATE.libraryTab) {
    APP_STATE.libraryTab = static_cast<uint8_t>(currentTab());
    APP_STATE.saveToFile();
  }
```

```cpp
void FileBrowserActivity::tabActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FileBrowserActivity*>(user);
  self->app.clearTapFlash();
  self->showLibraryTab(libraryTabFrom(static_cast<uint8_t>(event.value)));
}

// Leaves the tab showing where it is and opens `tab` where it was left.
void FileBrowserActivity::showLibraryTab(const LibraryTab tab) {
  const LibraryTab current = currentTab();
  if (tab == current) return;
  tabRows[static_cast<size_t>(current)] = nav.selected;
  if (current == LibraryTab::Books) booksPath = model.path();
  if (current == LibraryTab::Authors) {
    authorToOpen = !model.atAuthorList();
    authorToOpenHash = model.openAuthorKey();
  }
  mappedInput.flushTouchEvents();  // contacts still queued were aimed at the tab being left
  {
    RenderLock lock(*this);
    model.setMode(modeFor(tab));
    if (tab == LibraryTab::Books) model.setPath(booksPath);
    loadRows();
    if (tab == LibraryTab::Authors && authorToOpen) model.openAuthorByHash(authorToOpenHash);
    if (tab == LibraryTab::Authors) authorToOpen = false;
    resetNavigation(tabRows[static_cast<size_t>(tab)]);
  }
  startLibraryBuildIfStale();
  requestUpdate();
}

void FileBrowserActivity::composeLibraryTabBar(UiScreen& screen) {
  static constexpr StrId kLabels[LIBRARY_TAB_COUNT] = {StrId::STR_TAB_BOOKS, StrId::STR_TAB_RECENT,
                                                       StrId::STR_TAB_NEW, StrId::STR_TAB_AUTHORS};
  fui::TabItem tabs[LIBRARY_TAB_COUNT]{};
  for (uint8_t slot = 0; slot < LIBRARY_TAB_COUNT; ++slot) {
    tabs[slot].label = I18n::getInstance().get(kLabels[slot]);
    tabs[slot].value = static_cast<int16_t>(slot);
    tabs[slot].selected = slot == static_cast<uint8_t>(currentTab());
  }
  // Never focused: the browser keeps its own keys, and a long Up/Down or a tap is how a tab changes.
  const auto props = ListTabBar::props(screen, tabs, LIBRARY_TAB_COUNT, ACTION_USER, /*barFocused=*/false);
  fui::tabBar(screen.frame(), screen.takeTop(ListTabBar::HEIGHT), props);
}
```

- [ ] **Step 5: Activity — input.** In `handleCustomInput()`'s event loop, first:

```cpp
    using Direction = MappedInputManager::Direction;
    const bool vertical = MappedInputManager::isDirection(ev.button, Direction::Up) ||
                          MappedInputManager::isDirection(ev.button, Direction::Down);
    // A long Up/Down switches the Library's tab, as it does in Settings.
    if (libraryTabs() && vertical && ev.type == ButtonEventManager::PressType::Long) {
      if (!coversView()) nav.selected = stepOrigin;
      tabHold = true;
      showLibraryTab(libraryTabStep(currentTab(), MappedInputManager::isDirection(ev.button, Direction::Down) ? 1 : -1));
      return true;
    }
```

Back: `if (model.getMode() == Mode::Recents)` → Home becomes

```cpp
      // Recent, New and the author list are one level with nowhere to go up to: Back is Home.
      const Mode mode = model.getMode();
      if (mode == Mode::Recents || mode == Mode::Added || model.atAuthorList()) {
        onGoHome();
        return true;
      }
      // An author's books: Back returns to the authors, on the one that was open; a long Back is Home.
      if (mode == Mode::Authors) {
        if (ev.type == ButtonEventManager::PressType::Long) {
          onGoHome();
          return true;
        }
        mappedInput.flushTouchEvents();
        {
          RenderLock lock(*this);
          resetNavigation(static_cast<int>(model.closeAuthor()));
        }
        requestUpdate();
        return true;
      }
```

`navigateButtons()`, first lines:

```cpp
  if (tabHold) {
    const bool held = mappedInput.isPressed(MappedInputManager::buttonFor(MappedInputManager::Direction::Up)) ||
                      mappedInput.isPressed(MappedInputManager::buttonFor(MappedInputManager::Direction::Down));
    if (held) return;
    tabHold = false;
  }
```

and `if (changed) stepOrigin = previous;` inside the lock after the two `onNextList/onPreviousList` calls.

`activateSelected()`: in the directory branch, before the path code:

```cpp
    if (model.atAuthorList()) {  // an author, not a folder: its books
      mappedInput.flushTouchEvents();
      {
        RenderLock lock(*this);
        model.openAuthor(static_cast<size_t>(nav.selected));
        resetNavigation();
      }
      requestUpdate();
      return;
    }
```

- [ ] **Step 6: Activity — drawing.**
  - `chosenView()`: `case Mode::Added: return APP_STATE.addedBooksView; case Mode::Authors: return APP_STATE.authorsView;`
  - `rowName()`: book paths (Recents, Added, an author's books) show their last part; `const Mode m = model.getMode(); if (m != Mode::Recents && m != Mode::Added && !(m == Mode::Authors && !model.atAuthorList())) return getFileName(entry);`
  - `coverGrid()`: `g.top = ... + (libraryTabs() ? ListTabBar::HEIGHT : 0);`
  - `drawCoverCell()` folder branch: `int books = -1; if (model.atAuthorList()) { uint16_t count = 0; uint32_t hash = 0; if (model.authorAt(index, count, hash)) books = count; } else { books = bookRows.folder(path).bookCount; }`
  - `materializeListWindow()`: after `windowLabels[offset] = rowName(entry);`

```cpp
    // An author: its name, unbracketed -- it is not a folder -- and its book count.
    if (model.atAuthorList() && !entry.empty()) {
      windowLabels[offset] = utf8NfcNorm(entry.substr(0, entry.size() - 1));
      uint16_t books = 0;
      uint32_t hash = 0;
      if (model.authorAt(index, books, hash)) windowValues[offset] = std::to_string(books);
    }
```

  (and `windowValues[offset].clear()` moves above it).
  - `buildScreen()`:

```cpp
  if (libraryTabs()) {
    layoutListArea(screen, 0, 0, /*spacer=*/false);
    composeLibraryTabBar(screen);
    screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
  } else {
    layoutListArea(screen);
  }
```

  empty text: `const bool indexing = libraryBuilder != nullptr;` then PickFirmware → NO_BIN; Recents → NO_RECENT_BOOKS; Added → indexing ? STR_INDEXING : STR_NO_NEW_BOOKS; Authors && indexing → STR_INDEXING; else NO_FILES_FOUND.
  - `drawChrome()`: title — PickFirmware/PickFolder unchanged; Books: folder name or (root) `STR_LIBRARY`; Recents, Added, author list: `STR_LIBRARY`; open author: `model.openAuthorName()`; AllFiles: path or `STR_ALL_FILES`. Subtitle while `libraryBuilder` runs: `STR_INDEXING`, plus `" resolved/pending"` in the Resolve phase.
  - `drawFooter()` back label: `Home` when Back leaves the Library — Books at the root, Recents, Added, the author list — else `Back`.

- [ ] **Step 7: Activity — menus, view choice, returns, build.**
  - `openContextMenu()`: a directory row on the author list opens the plain browser menu (`showBrowserOptionsMenu()` with no entry: no Open/Remove of a "folder" called after an author). Both menu constructions pass `listSource(), chosenView()` instead of `recentsList`, where

```cpp
FileContextMenuActivity::ListSource FileBrowserActivity::listSource() const {
  switch (model.getMode()) {
    case Mode::Recents: return FileContextMenuActivity::ListSource::Recents;
    case Mode::Added:
    case Mode::Authors: return FileContextMenuActivity::ListSource::Index;
    default: return FileContextMenuActivity::ListSource::Folder;
  }
}
```

  - DisplayOptionsChanged: the Recents branch covers Recents, Added and Authors, storing the view in that list's own `APP_STATE` field.
  - `offersViewChoice()`: `return libraryTabs();`
  - `returnTarget()`: `AllFiles` → `ReturnTo::AllFiles`, otherwise `ReturnTo::Library`.
  - `returnHint(selectName)` builds every `ReturnHint` in the file (5 sites):

```cpp
ReturnHint FileBrowserActivity::returnHint(std::string selectName) const {
  ReturnHint hint;
  hint.target = returnTarget();
  hint.path = model.path();
  hint.selectName = std::move(selectName);
  if (libraryTabs()) {
    hint.selectIndex = static_cast<int>(currentTab());
    if (model.getMode() == Mode::Authors && !model.atAuthorList()) {
      hint.selectionContext = std::to_string(model.openAuthorKey());
    }
  }
  return hint;
}
```

  - `goToResultFolder()`: Recents, New or an author's book → the Books tab in place, at that folder, the book selected: `booksPath = folder; tabRows[Books] = 0; focusName = name; showLibraryTab(LibraryTab::Books);` then select `focusName` (findEntry) under the lock.
  - `doRemove()`: sidecars go with a book removed from Books, New or Authors; after a removal, `startLibraryBuildIfStale()`.
  - `stepLibraryBuild()`: `if (model.getMode() != Mode::Added && model.getMode() != Mode::Authors) return false;` first; `requestUpdate()` when the builder finishes too (the header's Indexing goes).

- [ ] **Step 8: Build and commit**

Run (background): `PLATFORMIO_CORE_DIR='C:\pio' pio run -e default` 
Expected: SUCCESS. Host suite: `ctest --test-dir test/build` → all pass.

```bash
git add -A src && git commit -m "feat(library): Books, Recent, New and Authors as tabs of one Library screen"
```

---

### Task 6: Docs, other boards, sizes

**Files:** `docs/USER_GUIDE.md` (Home entries, the Library tabs, long Up/Down), any doc naming Browse Files / Recent Books as Home entries.

- [ ] **Step 1:** Update the docs.
- [ ] **Step 2:** Build `x4pro` and `lilygo_t5s3`, one at a time. Expected: SUCCESS.
- [ ] **Step 3:** Record flash/RAM of `default` against b6874d990 (Plan 2b) and master; commit docs.
