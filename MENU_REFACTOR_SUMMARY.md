# Menu Refactoring Summary: Cleaner Sort Options

**Date**: 2026-06-17  
**Status**: ✅ Complete  
**Commits**: 2 (refactoring + integration)

---

## What Changed

### Before: 8 Separate Menu Items
```
[Separator: Sort By]
- Sort Name [Action]
- Sort Date [Action]
- Sort Size [Action]
- Sort Type [Action]
[Separator: Sort Dir]
- Sort Ascending [Action]
- Sort Descending [Action]
- Show Hidden Files [Action]
- Show File Extensions [Action]
[File-specific actions if file selected]
```

**Problems**:
- Awkward UI (8 items just to set 2 options)
- No visual feedback (items don't show current state)
- Complex action routing (onActionSelected had 8 branches)
- Inconsistent with existing MenuListActivity patterns

### After: 2 Cyclic Toggle Options
```
Sort By: [Name] ← user presses confirm to cycle → [Date] → [Size] → [Type] → [Name]
Sort Direction: [Ascending] ← user presses confirm to cycle → [Descending] → [Ascending]
[File-specific actions if file selected]
```

**Benefits**:
- Clean UI (2 items instead of 8)
- Visual feedback (current value shown inline)
- Simpler code (DynamicEnum + lambdas)
- Follows MenuListActivity pattern (like EpubReaderActivity)
- Consistent with settings menu conventions

---

## Implementation Details

### Commit 1: Use DynamicEnum (`9fd8ef84`)
**File**: `FileContextMenuActivity.cpp`

**Pattern**:
```cpp
menuItems.push_back(SettingInfo::DynamicEnum(
    StrId::STR_SORT_BY,
    {StrId::STR_SORT_NAME, StrId::STR_SORT_DATE, StrId::STR_SORT_SIZE, StrId::STR_SORT_TYPE},
    [this]() -> uint8_t { return static_cast<uint8_t>(sortMode); },
    [this](uint8_t v) { sortMode = static_cast<CrossPointSettings::FILE_SORT_MODE>(v); }));

menuItems.push_back(SettingInfo::DynamicEnum(
    StrId::STR_SORT_DIR,
    {StrId::STR_SORT_ASC, StrId::STR_SORT_DESC},
    [this]() -> uint8_t { return static_cast<uint8_t>(sortDirection); },
    [this](uint8_t v) { sortDirection = static_cast<CrossPointSettings::FILE_SORT_DIRECTION>(v); }));
```

**How It Works**:
1. `SettingInfo::DynamicEnum()` creates a cyclic toggle item
2. Getter lambda returns current value (0-3 for sort mode, 0-1 for direction)
3. Setter lambda updates local state when user presses confirm
4. MenuListActivity renders as: `[Sort By: Name]` with ability to cycle
5. Local `sortMode` and `sortDirection` members hold state

### Commit 2: Pass Values Between Activities (`2427bee4`)
**Files**: `FileContextMenuActivity.h/cpp`, `ActivityResult.h`, `FileBrowserActivity.cpp`

**Flow**:
1. **Construction**: FileBrowserActivity passes current sort state to menu
   ```cpp
   std::make_unique<FileContextMenuActivity>(
       renderer, mappedInput, filePath, 
       sortMode, sortDirection)  // ← NEW: pass current state
   ```

2. **Initialization**: FileContextMenuActivity constructor stores values
   ```cpp
   FileContextMenuActivity(...,
                          CrossPointSettings::FILE_SORT_MODE sortMode,
                          CrossPointSettings::FILE_SORT_DIRECTION sortDirection)
       : sortMode(sortMode), sortDirection(sortDirection) { ... }
   ```

3. **User Interaction**: DynamicEnum lambdas update local state when user cycles

4. **Result**: onSettingToggled() passes updated values back via MenuResult
   ```cpp
   MenuResult res;
   res.action = static_cast<int>(action);
   res.sortMode = static_cast<uint8_t>(sortMode);      // ← NEW
   res.sortDirection = static_cast<uint8_t>(sortDirection);  // ← NEW
   ```

5. **Application**: FileBrowserActivity extracts values from MenuResult
   ```cpp
   if (actionEnum == Action::ChangeSortMode) {
       sortMode = static_cast<CrossPointSettings::FILE_SORT_MODE>(menuRes->sortMode);
       sortFileList();
   }
   ```

---

## Data Flow Diagram

```
FileBrowserActivity
    │
    ├─ sortMode = SORT_BY_DATE
    ├─ sortDirection = SORT_DESCENDING
    │
    └─→ Create FileContextMenuActivity(sortMode, sortDirection)
        │
        FileContextMenuActivity
        │
        ├─ sortMode = SORT_BY_DATE (initialized)
        ├─ sortDirection = SORT_DESCENDING (initialized)
        │
        ├─ User presses confirm on "Sort By" item
        │  └─→ DynamicEnum getter returns 1 (SORT_BY_DATE enum index)
        │  └─→ MenuListActivity cycles: next = 2 (SORT_BY_SIZE)
        │  └─→ DynamicEnum setter: sortMode = SORT_BY_SIZE
        │
        ├─ User presses confirm on "Sort Direction" item
        │  └─→ DynamicEnum getter returns 1 (SORT_DESCENDING enum index)
        │  └─→ MenuListActivity cycles: next = 0 (SORT_ASCENDING)
        │  └─→ DynamicEnum setter: sortDirection = SORT_ASCENDING
        │
        └─→ User presses Back (or confirms file action)
            │
            ├─ onSettingToggled() creates MenuResult:
            │  ├─ action = ChangeSortMode
            │  ├─ sortMode = 2 (SORT_BY_SIZE)
            │  └─ sortDirection = 0 (SORT_ASCENDING)
            │
            └─→ Return to FileBrowserActivity
                │
                ├─ Extract: sortMode = SORT_BY_SIZE (from menuRes->sortMode)
                ├─ Extract: sortDirection = SORT_ASCENDING (from menuRes->sortDirection)
                │
                └─ sortFileList() → display updated results
```

---

## Code Changes Summary

| File | Changes | Purpose |
|------|---------|---------|
| FileContextMenuActivity.h | Add sortMode/sortDirection params to constructor | Initialize menu with current state |
| FileContextMenuActivity.cpp | Use DynamicEnum instead of Actions | Cyclic options with visual feedback |
| FileContextMenuActivity.cpp | Replace 8 Actions with 2 DynamicEnums | 75 lines → 25 lines |
| FileContextMenuActivity.cpp | Add onSettingToggled() override | Handle enum changes, pass back values |
| ActivityResult.h | Add sortMode/sortDirection to MenuResult | Transport updated values |
| FileBrowserActivity.cpp | Pass sortMode/sortDirection to menu | Initialize menu correctly |
| FileBrowserActivity.cpp | Extract values from MenuResult | Apply updated sort state |

---

## User Experience

### Before (Awkward)
1. User opens browser context menu
2. Sees: "Sort Name" / "Sort Date" / "Sort Size" / "Sort Type" (4 items)
3. User selects "Sort Date" → all 4 items selected do the same thing
4. No visual indication of what the current sort is
5. 8 items just for sort options + hidden files + extensions

### After (Clean)
1. User opens browser context menu
2. Sees: "Sort By: Date" / "Sort Direction: Descending"
3. User presses confirm on "Sort By" → cycles to next option
4. Displays: "Sort By: Size" (visual feedback of cycle)
5. User presses confirm on "Sort Direction" → cycles
6. Displays: "Sort Direction: Ascending" (visual feedback)
7. Only 2 items for sort functionality

---

## Testing Checklist

- [ ] Build compiles without errors
- [ ] Menu opens with current sort state visible
- [ ] User presses confirm on "Sort By" → cycles through Name/Date/Size/Type
- [ ] User presses confirm on "Sort Direction" → cycles between Ascending/Descending
- [ ] Sort state updates correctly after menu returns
- [ ] File-specific actions still work (Open, Info, Delete, etc.)
- [ ] Menu works both in browser mode (no file selected) and file mode (file selected)
- [ ] Back button cancels without applying changes
- [ ] Returned to browser with new sort applied immediately

---

## Commits

```
2427bee4 refactor: pass sort mode/direction between menu and browser
9fd8ef84 refactor: use DynamicEnum for sort options — cleaner menu with 2 items instead of 8
```

---

## Related Patterns

This refactoring follows the MenuListActivity example from the documentation:
- **DynamicEnum**: Cyclic options with getter/setter lambdas
- **Local state**: Menu maintains its own sortMode/sortDirection
- **onSettingToggled()**: Override to handle ENUM changes
- **MenuResult**: Pass values back to parent activity

See `MenuListActivity.h:20-52` for the full example pattern.

---

**Status**: ✅ Ready for testing  
**Next**: Verify menu interaction on device and test sort application
