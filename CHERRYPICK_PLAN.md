# Cherrypick Strategy: PR #1446 (File Sorting & RTC Persistence)

**Source:** crosspoint-reader/crosspoint-reader PR #1446  
**Date:** 2026-06-17  
**Scope:** RTC full-date persistence + sorting UI, skip on-SD FileIndex

---

## Decision Summary

- ✅ **Adopt:** RTC full-date storage (X3 only) + FAT timestamp callback
- ✅ **Adopt:** Natural sort shared utility (NaturalSort.h)
- ✅ **Adopt:** File sorting (name, date, size, type) via per-session enum
- ✅ **Adopt:** Hotspot client upload timestamps (validation + RTC persist)
- ❌ **Skip:** FileIndex on-SD streaming index (1100 LOC, marginal RAM savings for small folders)
- ❌ **Skip:** Settings menu entries (sorting will be session-only, in browser menu)
- ✅ **Attribution:** Cherry-pick Patryk's original commit; co-author all refactored/new work

---

## Phase 1: Extract FsHelpers & NaturalSort

**Goal:** Establish shared sorting infrastructure  
**Files affected:** lib/FsHelpers/*, lib/I18n/translations  
**Commits:** 2 (refactor + additions)

### Changes

1. **Extract NaturalSort.h / NaturalSort.cpp**
   - Move `FsHelpers::naturalCompare` → `NaturalSort.cpp:naturalCompare()`
   - Add `NaturalSort.cpp:naturalSortKey()` (28-byte order-preserving encoding)
   - Add `unsigned char` casts for UTF-8 safety (matching PR #1446)
   - Header: `#include <NaturalSort.h>` publicly in FsHelpers.h

2. **Update FsHelpers.cpp**
   - Replace inline naturalCompare in `sortFileList()` with call to NaturalSort
   - Keep the `sortFileList()` wrapper for backward compat (it now calls NaturalSort)

3. **Test coverage**
   - Run existing `test/natural_sort/NaturalSortTest.cpp` (cherry-pick from PR)

**Commit message:**
```
refactor: extract natural sort to shared NaturalSort utility

- Extract naturalCompare() and add naturalSortKey() to lib/NaturalSort
  for use by file browser, BMP viewer, and future sorting modes
- Add unsigned char casts so isdigit/tolower stay defined on UTF-8 bytes
- Update FsHelpers to use the shared utility

Co-Authored-By: zgredex <112968378+zgredex@users.noreply.github.com>
```

---

## Phase 2: RTC Full-Date Persistence & FAT Timestamp Callback

**Goal:** Ensure FAT timestamps survive power cycles  
**Files affected:** lib/hal/HalClock.*, lib/hal/HalStorage.*, src/I18n/translations  
**Commits:** 2 (RTC + callback plumbing)

### Changes

1. **HalClock.cpp / HalClock.h**
   - Add `FsDateTime::setCallback()` function typedef (already in SDK, just expose wrapper)
   - In `HalClock::begin()`: seed system clock from X3 RTC (if date >= 2020, plausibility check)
   - In `HalClock::syncNtp()`: persist synced time to X3 RTC registers (all 7 bytes: year, mon, mday, hour, min, sec, dow)
   - Add `HalClock::applyClientTime(time_t)`: 
     - Validates timestamp in [2020-01-01, 2100-01-01]
     - Only applies when SNTP is not active (clock not synced from network)
     - Persists to X3 RTC if applicable
     - Exported for webserver use

2. **HalStorage.cpp / HalStorage.h**
   - In `HalStorage::begin()`: register `FsDateTime::setCallback()` callback after RTC init
   - Callback ensures all device-written files (exports, web uploads, OTA) get real FAT timestamps
   - Falls back to FAT epoch (1980-01-01) if clock unset

3. **I18n additions** (if needed)
   - No new UI strings for Phase 2

**Commit messages:**

```
feat: use X3 RTC for persistent FAT file timestamps

- Store full UTC date+time in DS3231 RTC (7 registers) on clock writes
- Seed system clock from RTC at boot with plausibility check (year >= 2020)
- Persist NTP-synced and hotspot client times to RTC for one-boot persistence
- Epoch conversion is TZ-independent (mktime honours TZ env var)

Co-Authored-By: zgredex <112968378+zgredex@users.noreply.github.com>
```

```
feat: register FAT timestamp callback in HalStorage

- All files written by the device (exports, web uploads, OTA) receive real
  FAT timestamps via FsDateTime callback registered in HalStorage::begin()
- Falls back to FAT epoch while clock is unset
- Add HalClock::applyClientTime() for hotspot timestamp sync

Co-Authored-By: zgredex <112968378+zgredex@users.noreply.github.com>
```

---

## Phase 3: File Sorting Enum & Infrastructure

**Goal:** Thread sort mode through the browser  
**Files affected:** src/CrossPointSettings.h, src/activities/home/FileBrowserActivity.*  
**Commits:** 1

### Changes

1. **Add SortMode enum** (src/CrossPointSettings.h or new lib/FsHelpers header)
   ```cpp
   enum class SortMode : uint8_t {
     Name = 0,
     Date = 1,
     Size = 2,
     Type = 3,
   };
   ```

2. **FileBrowserActivity.h**
   - Add member: `SortMode sortMode = SortMode::Name;`
   - Add member: `bool sortAscending = true;`
   - Add member: `bool hideExtensions = false;` (persist per-session)
   - Existing: `bool showHiddenFiles` (already in SETTINGS, no change)

3. **FileBrowserActivity.cpp**
   - Update `loadFiles()` to accept sort parameters
   - Replace inline `sortFileList()` with new `sortFileListBy(files, sortMode, sortAscending, hideExtensions)`
   - Implement sort comparators for each mode (Name, Date, Size, Type)
     - Name: use `NaturalSort::naturalCompare()`
     - Date: `max(mtime, ctime)` via `getModifyDateTime()` + `getCreateDateTime()`
     - Size: file size descending
     - Type: extension group (case-insensitive) → name order within
   - Directories always sort first
   - Extension hiding happens at display time (render), not during sort

4. **Optional: hideExtensions flag**
   - Store per-session; set in browser options menu
   - Strip extension at render time for display (not in files vector)

**Commit message:**
```
feat: add file browser sorting by name, date, size, and type

- Add SortMode enum and per-session sort state to FileBrowserActivity
- Implement sort comparators: name (natural), date (max mtime/ctime),
  size, and type (extension group + name)
- Directories always sort first; ties use natural name order
- Date sort uses FAT timestamps (requires Phase 2 clock setup)

Co-Authored-By: zgredex <112968378+zgredex@users.noreply.github.com>
```

---

## Phase 4: Browser Options Menu UI

**Goal:** Expose sort options in FileBrowserActivity via right-button press  
**Files affected:** src/activities/home/FileBrowserActivity.*, FileContextMenuActivity.*, I18n  
**Commits:** 2 (menu refactor + browser integration)

### Changes

1. **Refactor FileContextMenuActivity → BrowserOptionsMenuActivity**
   - Rename class (or create new BrowserOptionsMenuActivity)
   - Extend to support two sections:
     - **File Display** (always shown):
       - Sort by: Name / Date / Size / Type
       - Sort direction: Ascending / Descending
       - Show/Hide Hidden Files
       - Show/Hide File Extensions
     - **File Type Actions** (shown only when a specific file type is selected):
       - Existing actions (Open, Info, Delete, etc.) based on file type
   
   - If target is a directory or unsupported type: only show File Display options
   - If target is a supported file type (EPUB, TXT, etc.): show both sections

2. **FileBrowserActivity loop() / input handling**
   - When right-button pressed:
     - Launch BrowserOptionsMenuActivity (always, not just for specific files)
     - Pass current file path (empty string if no file selected) + sort state
   - On return: update sortMode, sortAscending, hideExtensions
   - Reload files and refresh display

3. **I18n additions** (new strings)
   ```yaml
   STR_SORT_BY: "Sort By"
   STR_SORT_NAME: "Name"
   STR_SORT_DATE: "Date"
   STR_SORT_SIZE: "Size"
   STR_SORT_TYPE: "Type"
   STR_SORT_DIRECTION: "Direction"
   STR_SORT_ASCENDING: "Ascending"
   STR_SORT_DESCENDING: "Descending"
   STR_SHOW_HIDDEN: "Show Hidden Files"
   STR_HIDE_EXTENSIONS: "Hide Extensions"
   ```

4. **MenuListActivity enhancement** (if needed)
   - Support sections / dividers in menu
   - Or use two separate menu lists (File Display → return → File Type Actions → return)

**Commit messages:**

```
refactor: generalize menu activity for browser options

- Rename FileContextMenuActivity → BrowserMenuActivity (or keep both)
- Support mode: file-specific actions vs. browser-wide options
- When no file selected or unsupported type: show only file display options
- Prepare for sort/filter UI

Co-Authored-By: zgredex <112968378+zgredex@users.noreply.github.com>
```

```
feat: add browser options menu for sort and display settings

- Right-button press in file browser opens browser options (always)
- Options: Sort By (Name/Date/Size/Type), Direction, Hidden Files, Extensions
- Per-file-type actions shown below (Open, Info, Delete, etc.)
- Selection returns to browser, updates sort state and reloads view
- Changes persist for the session
```

---

## Phase 5: Hotspot Client Upload Timestamps (Optional)

**Goal:** Enable date stamping on web uploads when no RTC/NTP available  
**Files affected:** src/network/CrossPointWebServer.cpp, src/network/html/FilesPage.html  
**Commits:** 1

### Changes (conditional on Phase 2)

1. **FilesPage.html**
   - Add hidden timestamp field to upload form
   - On each file upload, set `&t=<unix-timestamp>` query param
   - Space consecutive timestamps ≥ 2 seconds apart (FAT resolution)

2. **CrossPointWebServer.cpp**
   - Extract `&t=` parameter from upload request
   - Call `HalClock::applyClientTime(t)` if provided
   - Validation + bounds check done in HalClock

**Commit message:**
```
fix: send client upload timestamps from browser to set device clock

- FilesPage.html adds Unix timestamp to each upload
- Hotspot mode uses timestamp if device clock is unset
- Consecutive uploads spaced >= 2s apart to match FAT mtime resolution

Co-Authored-By: zgredex <112968378+zgredex@users.noreply.github.com>
```

---

## Implementation Order

1. **Phase 1** (NaturalSort extraction) — prerequisite for all others
2. **Phase 2** (RTC + callback) — independent, enables Phase 3 (Date sort)
3. **Phase 3** (Sort enum + comparators) — foundation for Phase 4
4. **Phase 4** (Menu UI) — user-visible, brings everything together
5. **Phase 5** (Hotspot timestamps) — polish, optional if web uploads not a priority

---

## Testing Strategy

- Unit: NaturalSortTest.cpp (cherry-pick from PR)
- Integration: Manual browser navigation + sort mode cycles
- Manual: Date sort after clock sync; hotspot uploads with timestamps
- Device: Verify FAT timestamps on SD card files (PC explorer)

---

## Attribution

All commits will follow this pattern:

**Primary commit (Patryk's original from PR #1446):**
```
Co-Authored-By: Patryk Radtke <patryk@Patryks-MacBook-Pro.local>
```

**Refactored/extracted work (Phase 1-5):**
```
Co-Authored-By: zgredex <112968378+zgredex@users.noreply.github.com>
```

The justification: Patryk wrote the bulk of the logic; we're adapting it for witchhunt's simpler browser model (no settings menu, session-only sort state, no on-SD index).

---

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| Date sort requires valid FAT timestamps | Phase 2 ensures clock plumbing; graceful fallback to 0 |
| Menu UX adds complexity | Start simple: two-menu model (display options, then file actions) |
| Breaking existing browser behavior | Keep sortFileList() wrapper; default sort is Name (current behavior) |
| X4 (no RTC) doesn't persist timestamps | Acceptable; NTP sync still works per boot; doc it |

---

## Files Checklist

### To Add
- [ ] lib/FsHelpers/NaturalSort.h
- [ ] lib/FsHelpers/NaturalSort.cpp
- [ ] test/natural_sort/NaturalSortTest.cpp
- [ ] (Updated) src/I18n/translations/english.yaml

### To Modify
- [ ] lib/FsHelpers/FsHelpers.h
- [ ] lib/FsHelpers/FsHelpers.cpp
- [ ] lib/hal/HalClock.h
- [ ] lib/hal/HalClock.cpp
- [ ] lib/hal/HalStorage.h
- [ ] lib/hal/HalStorage.cpp
- [ ] src/CrossPointSettings.h (add SortMode enum)
- [ ] src/activities/home/FileBrowserActivity.h
- [ ] src/activities/home/FileBrowserActivity.cpp
- [ ] src/activities/home/FileContextMenuActivity.h (rename or extend)
- [ ] src/activities/home/FileContextMenuActivity.cpp
- [ ] src/network/CrossPointWebServer.cpp (Phase 5)
- [ ] src/network/html/FilesPage.html (Phase 5)

### To Review / No Changes
- [ ] test/CMakeLists.txt (add NaturalSortTest)
- [ ] lib/hal/HalStorage constructor/begin() (add callback registration)

---

## Success Criteria

✅ Files sorted by Name, Date, Size, Type in browser  
✅ Sort direction (Asc/Desc) toggleable  
✅ Sorting options accessible via right-button menu in browser  
✅ Hidden files toggle works across all sort modes  
✅ Extensions toggle hides ".epub" etc. at render time  
✅ FAT timestamps on SD files are real dates (not epoch 1980)  
✅ Timestamps persist across reboots (X3 with RTC)  
✅ Hotspot uploads timestamp the files correctly  
✅ No regression in existing file browser behavior  
✅ Natural sort matches PR #1446 order  

