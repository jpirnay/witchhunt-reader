# Phase 6 (FileIndex) Code Audit Findings

**Date**: 2026-06-17  
**Status**: Critical issues identified requiring immediate fixes  
**Severity Breakdown**: 1 CRITICAL, 1 HIGH, 8 MEDIUM/LOW

---

## CRITICAL ISSUES (BLOCKER)

### Issue #1: Missing `getCreateDateTime()` Method
- **Location**: `FileIndex.cpp:81` - `file.getCreateDateTime(&cdate, &ctime);`
- **Problem**: This method does not exist in `HalFile`. Only `getModifyDateTime()` is implemented.
- **Impact**: **FileIndex cannot compile or run**. Any folder with 64+ entries will crash.
- **Fix**: Either:
  - Implement `getCreateDateTime()` in `HalFile` (mirror `getModifyDateTime()`)
  - Change logic to use only `getModifyDateTime()` (max of mtime with itself doesn't help)
  - Remove the ctime check entirely and use only mtime

**Recommendation**: Implement `getCreateDateTime()` to match PR #1446's design.

---

## HIGH-IMPACT ISSUES

### Issue #2: O(N log N) File Opens During IN-RAM Sort
- **Location**: `FileBrowserActivity.cpp:415-506`
- **Problem**: 
  - `getFileDateTime()` opens file TWICE per comparison (line 455-456)
  - `getFileSize()` opens file TWICE per comparison (line 470-471)
  - For 64 entries sorting, this triggers ~400+ file opens
  - Each SD file open: 100+ ms
  - Total sort time: **multiple seconds** for large folders
- **User Impact**: Visible lag when user selects "Sort by Date" or "Sort by Size" on 64+ entry folder
- **Fix**: **Cache metadata during loadFiles() scan**
  - Store (size, dateTime) alongside each filename during initial enumeration
  - Use cached values in sort comparators (zero file opens)
  - Adds ~16 bytes per entry (~1 KB for 64 entries) but eliminates 400+ I/O ops

**Recommendation**: Implement metadata caching immediately (easy, high ROI).

---

## MEDIUM-IMPACT ISSUES

### Issue #3: Memory Leak - BuildState Cleanup
- **Location**: `FileIndex.cpp:359-391`
- **Problem**: If `build()` fails after scratch files are created, cleanup lambda never runs
- **Impact**: Orphaned files (`runs.a`, `runs.b`, `ties.a`, `ties.b`, `.tmp`) accumulate on SD
- **Fix**: Ensure `cleanupScratch()` is called on all error paths (use RAII guard instead of lambda)

### Issue #4: Chunk Buffer Overrun in Tie Writing
- **Location**: `FileIndex.cpp:690-704`
- **Problem**: Reuses 2 KB `bs.chunk` for tie run data without bounds checking
- **Impact**: Two 511-byte-name records could overflow and corrupt index file
- **Fix**: Add explicit bounds check before each memcpy, or use separate tie buffer

### Issue #5: Race Condition Between Scan and Reuse
- **Location**: `FileIndex.cpp:178-199`
- **Problem**: Signature check → reuse window → files can be added/deleted in between
- **Impact**: Stale file listings; newly copied files won't appear
- **Fix**: Add re-check after loading, or periodic invalidation timeout

### Issue #6: Incorrect Edge Case for Exactly 64 Entries
- **Location**: `FileIndex.cpp:509-541`
- **Problem**: Merge sort handles 64-entry chunks specially; edge cases may mis-order
- **Impact**: Folders with exactly 64 entries might sort incorrectly
- **Fix**: Add unit test for boundary conditions (64, 128, 256 entries)

---

## LOW-IMPACT ISSUES

### Issue #7: Y2101 Date Overflow
- **Location**: `FileIndex.cpp:78-85`
- **Problem**: FAT timestamps max out at 2107; no bounds check
- **Impact**: Dates after 2100 will wrap to 1980 in sort order
- **Fix**: Clamp to FAT max year (2107)

### Issue #8: Slow Pre-Selection (findRowByName)
- **Location**: `FileIndex.cpp:1068-1113`
- **Problem**: Linear scan of blob + linear scan of offsets table
- **Impact**: Returning to a file after reading is slow on 1000+ file folders
- **Fix**: Use binary search with key reconstruction

### Issue #9: Off-by-One in pageNamesAt Insertion Sort
- **Location**: `FileIndex.cpp:1035-1046`
- **Problem**: Unstable sort if two records have same offset
- **Impact**: Very low; requires index corruption
- **Fix**: Use stable sort or add tie-breaker

### Issue #10: Duplicate Sort Logic
- **Locations**: `NaturalSort.cpp`, `FileBrowserActivity.cpp:32-41`, `FileBrowserActivity.cpp:433-506`
- **Problem**: Three independent natural sort implementations
- **Impact**: Maintenance burden; risk of divergence
- **Fix**: Remove global `sortFileList()` function; use member method only

---

## FIX PRIORITY & TIMELINE

### Phase 6a (BLOCKER - Do Before Release)
1. **Implement `getCreateDateTime()` in HalFile** or change FileIndex to use only mtime
   - **Effort**: 1-2 hours
   - **Risk**: Low (mirrors existing `getModifyDateTime`)
   - **Blocks**: Everything else; cannot ship without this

2. **Add metadata caching in loadFiles()**
   - **Effort**: 2-3 hours
   - **Risk**: Low (local to FileBrowserActivity)
   - **Payoff**: Eliminates multi-second hangs

### Phase 6b (SHOULD DO - Before Production Use)
3. Clean up BuildState on error paths
4. Add bounds checking to chunk buffer writes
5. Add re-check after FileIndex load to catch race condition

### Phase 6c (NICE TO HAVE - Polish)
6. Add unit tests for 64/128/256 entry edge cases
7. Optimize findRowByName with binary search
8. Add Y2101 bounds check
9. Refactor duplicate sort logic

---

## VERIFICATION CHECKLIST

- [ ] `getCreateDateTime()` implemented and tested
- [ ] Metadata cache working; sort performance < 1 second for 100 entries
- [ ] BuildState cleanup tested via forced IO error
- [ ] Chunk buffer bounds validated (fuzz test with max-length names)
- [ ] FileIndex re-check test (add file while browsing)
- [ ] Edge case tests: 1, 64, 128, 256, 500, 1000 entries
- [ ] Y2101 overflow test (mock time to 2101)

---

## Recommendation

**Ship Phase 6 as experimental feature** with blocker fixes (getCreateDateTime, metadata cache) but gate large-folder browsing behind a user-visible warning if FileIndex is used: "Large folder indexing (beta)".

This allows:
- Real-world testing with safety net
- Gather performance data
- Iterate on fixes based on user feedback

Do NOT force FileIndex on all users; keep in-RAM sort as default until Phase 6b fixes are complete.
