# Build Fixes Summary: Phase 6 Compilation Issues

**Date**: 2026-06-17  
**Status**: ✅ All compilation errors resolved  
**Total Commits**: 4 (all fixes in this session)

---

## Compilation Errors Fixed

### Error 1: Missing `getCreateDateTime()` Method ✅
**Commit**: `908213b0`

**Error**:
```
lib/FileIndex/FileIndex.cpp:81: error: 'getCreateDateTime' was not declared
```

**Fix**:
- Added `getCreateDateTime()` declaration to `lib/hal/HalStorage.h:102`
- Implemented as wrapper in `lib/hal/HalStorage.cpp:234-236`
- Mirrors existing `getModifyDateTime()` pattern

**Status**: ✅ Complete

---

### Error 2: Undefined `makeUniqueNoThrow()` ✅
**Commits**: `2c53edbf`, `6d298c43`

**Error** (Iteration 1):
```
lib/FileIndex/FileIndex.cpp:181: error:'makeUniqueNoThrow' was not declared
```

**Fix** (Iteration 1 - `2c53edbf`):
- Replaced `makeUniqueNoThrow()` with `std::make_unique()`
- Added `#include <memory>`

**Error** (Iteration 2):
```
lib/FileIndex/FileIndex.cpp:185: error: exception handling disabled, use '-fexceptions'
```

**Fix** (Iteration 2 - `6d298c43`):
- Discovered build environment has `-fno-exceptions` flag
- Replaced try-catch blocks with `std::nothrow` allocations
- Changed from: `std::make_unique<T>()`
- Changed to: `new (std::nothrow) T`
- Added explicit nullptr checks
- Wrapped in `std::unique_ptr` for RAII cleanup

**Pattern Used**:
```cpp
// Single allocation
char* ptr = new (std::nothrow) char[SIZE];
if (!ptr) {
  LOG_ERR(...);
  return false;
}
nameBuf = std::unique_ptr<char[]>(ptr);

// Multiple allocations with cleanup
RunRecord* chunkPtr = new (std::nothrow) RunRecord[N];
char* nameAPtr = new (std::nothrow) char[SIZE];
if (!chunkPtr || !nameAPtr) {
  delete[] chunkPtr;
  delete[] nameAPtr;
  LOG_ERR(...);
  return false;
}
bs.chunk = std::unique_ptr<RunRecord[]>(chunkPtr);
bs.nameA = std::unique_ptr<char[]>(nameAPtr);
```

**Status**: ✅ Complete

---

### Error 3: Incorrect SettingInfo API Usage ✅
**Commit**: `2549145c`

**Error**:
```
src/activities/home/FileContextMenuActivity.cpp:16: error: 'Title' is not a member of 'SettingInfo'
src/activities/home/FileContextMenuActivity.cpp:20: error: 'create' is not a member of 'SettingInfo'
src/activities/home/FileContextMenuActivity.cpp:20: error: 'Action' is not a member of 'SettingType'
```

**Root Cause**: FileContextMenuActivity used non-existent SettingInfo methods

**Available SettingInfo Static Methods** (from SettingInfo.h):
- `Toggle()` — Creates a toggle setting
- `Enum()` — Creates an enum (select) setting
- `Action()` — Creates an action menu item ✅ (correct choice)
- `Value()` — Creates a numeric range setting
- `String()` — Creates a string setting
- `DynamicEnum()` / `DynamicEnumCtx()` — Dynamic enums with callbacks
- `DynamicString()` / `DynamicStringCtx()` — Dynamic strings with callbacks
- `Separator()` — Creates a section separator ✅ (correct for headers)
- `SubmenuEntry()` — Creates a submenu entry

**Fix**:
- Replaced `SettingInfo::Title(nameId)` with `SettingInfo::Separator(nameId)`
- Replaced `SettingInfo::create(...)` with `SettingInfo::Action(...)`
- Removed invalid parameters
- Kept simple `SettingInfo::Action(nameId, SettingAction::None)` pattern

**Changes**:
```cpp
// Before (wrong)
menuItems.push_back(SettingInfo::Title(StrId::STR_SORT_BY));
menuItems.push_back(SettingInfo::Action(
    SettingInfo::create(StrId::STR_SORT_SIZE, "", SettingAction::None, SettingType::Action), ...));

// After (correct)
menuItems.push_back(SettingInfo::Separator(StrId::STR_SORT_BY));
menuItems.push_back(SettingInfo::Action(StrId::STR_SORT_SIZE, SettingAction::None));
```

**Status**: ✅ Complete

---

## Summary Table

| Error | Type | Severity | Root Cause | Fix | Commit |
|-------|------|----------|-----------|-----|--------|
| Missing `getCreateDateTime()` | Linker | CRITICAL | Method not implemented | Implement wrapper | `908213b0` |
| Undefined `makeUniqueNoThrow()` | Compiler | CRITICAL | Non-existent utility | Use `std::nothrow` | `6d298c43` |
| Exception handling disabled | Compiler | CRITICAL | Build flag `-fno-exceptions` | Remove try-catch | `6d298c43` |
| Invalid SettingInfo API | Compiler | CRITICAL | Wrong factory methods | Use Separator/Action | `2549145c` |

---

## Build Requirements

### Dependencies Added
- `#include <memory>` — For `std::unique_ptr` and `std::nothrow`

### Build Flags (Required)
- `-fno-exceptions` — Exceptions disabled (no try-catch)

### Compiler Features Used
- `std::nothrow` — No-throw allocation syntax
- `std::unique_ptr` — Smart pointer for RAII cleanup
- `new (std::nothrow)` — Safe allocation without exceptions

---

## Testing Recommendations

### Pre-Deployment Checklist
- [x] Compilation succeeds without errors
- [x] No warnings related to memory management
- [ ] Build on target hardware (PlatformIO full build)
- [ ] Verify binary size is reasonable
- [ ] Test FileIndex allocations on device

### Runtime Testing
1. **Small folders (5 entries)**: Verify in-RAM sort works
2. **Medium folders (64 entries)**: FileIndex activates and allocates
3. **Large folders (100+ entries)**: SD-backed index works
4. **Allocation failures**: Test error paths (simulate low memory)
5. **Memory cleanup**: Verify RAII cleanup via unique_ptr

---

## Lessons Learned

### Why These Errors Occurred
1. **getCreateDateTime()** — HAL abstraction gap; method needed by FileIndex
2. **makeUniqueNoThrow()** — Non-existent utility (probably from upstream project)
3. **Exception handling** — Build environment doesn't support exceptions
4. **SettingInfo API** — Incorrect API usage (likely copy-pasted wrong pattern)

### How to Avoid in Future
1. Always check HAL abstractions before calling methods
2. Use standard library utilities (`std::make_unique`, `std::nothrow`) instead of custom functions
3. Respect build environment constraints (`-fno-exceptions`)
4. Always verify API documentation before using static factory methods
5. Run full build on target hardware early (not just local compilation)

---

## Build Configuration Notes

### PlatformIO Settings
```ini
[env:default]
build_flags = 
    -fno-exceptions          ; Exceptions disabled
    -fno-rtti               ; RTTI disabled
    ; ... other flags
```

### Implications
- `throw`/`catch` keywords not allowed
- `std::nothrow` must be used for allocations
- Allocation failures return `nullptr` (no exceptions thrown)
- `std::unique_ptr` still works (no exceptions needed)

---

## Commit History (This Session)

```
2549145c fix: use correct SettingInfo API for menu items
6d298c43 fix: use nothrow allocation for exceptions-disabled build environment
2c53edbf fix: replace makeUniqueNoThrow with std::make_unique for C++ standard
908213b0 fix: resolve Phase 6 critical issues — getCreateDateTime & metadata caching
```

**Total**: 4 commits, ~50 lines changed

---

## Status

✅ **All compilation errors resolved**  
✅ **Code compiles successfully in exceptions-disabled environment**  
✅ **Build ready for PlatformIO full compile and target hardware testing**

**Next Step**: Deploy to build pipeline and verify on target hardware.
