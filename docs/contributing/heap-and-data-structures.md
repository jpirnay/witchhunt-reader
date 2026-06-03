# Heap and Data Structure Design

This document records the rationale behind the memory management constraints and data structure choices in the codebase. It is intended for contributors making changes to the ZIP, CSS, EPUB, or rendering layers — and for AI assistants reviewing or extending those areas.

## The core constraint

The ESP32-C3 has ~380 KB of SRAM and no PSRAM. One 48 KB primary framebuffer and one 48 KB secondary framebuffer are permanently allocated (the secondary can be released temporarily). This leaves roughly 280 KB for all runtime state, stacks, and heap.

**Fragmentation kills this device before total usage does.** The heap allocator is `dlmalloc`. After a WiFi session or heavy parse cycle, the heap can report 150 KB free while the largest contiguous block is only 40 KB — enough to fail a PNG decoder allocation. Every design decision below follows from this.

## Allocation rules

Apply in order; stop at the first yes:

1. **Stack** — local, bounded, under ~256 bytes total: plain array or struct.
2. **Flash/`static constexpr`** — compile-time constant: zero DRAM cost.
3. **Allocated once per activity lifetime** — allocate in `onEnter`, release in `onExit`, held as a member.
4. **Dynamic and fallible** — `new (std::nothrow)` with null-check and `LOG_ERR`. Never bare `new` (calls `abort()` on OOM under `-fno-exceptions`).
5. **SDK API takes ownership** — only then raw `malloc`, with a comment naming the owner.

## Vector discipline

`std::vector` grows by doubling: each growth is an alloc-copy-free triple that leaves a hole. Rules:

- Always `reserve(n)` before a `push_back` loop when `n` is known.
- When `n` is not known upfront, do a first pass to count, then `reserve`, then fill.
- Never grow a vector inside a render or per-element callback.

## The ZIP central directory — why no `unordered_map`

Early versions of `ZipFile` had `unordered_map<string, FileStatSlim> fileStatSlimCache` populated by `loadAllFileStatSlims()`. For a 3000-entry EPUB this consumed ~200 KB:

- 3000 heap nodes × ~40 bytes node overhead
- 3000 `std::string` keys × ~20 bytes average
- ~12 KB bucket array

This caused OOM crashes on large EPUBs. The function has been removed. The two correct access patterns are:

**`loadFileStatSlim(filename, &stat)`** — sequential central-directory scan with a wrap-around cursor. O(n) worst case, O(1) amortized when calls proceed in file order. No heap beyond two `std::string` temporaries for normalization.

**`streamCentralDirectoryNames(callback)`** — forward-only pass calling the callback with a `string_view` into a 256-byte stack buffer per entry. Zero heap allocation regardless of entry count. Use this whenever you only need filenames (CSS discovery, cover detection, image manifest building).

The cover fallback in `Epub::parseContentOpf` previously iterated ~72 candidate strings with one `loadFileStatSlim` per candidate. It now uses `streamCentralDirectoryNames` to do a single forward pass, matching stems in-place against a static array — one SD scan, zero heap.

## CSS rule index — flat sorted array over `unordered_map`

`CssParser::cacheRuleOffsets_` was `unordered_map<string, uint32_t>` (selector → file offset). At 1500 rules:

| | Before | After |
|---|---|---|
| Structure | `unordered_map<string, uint32_t>` | `vector<SelectorEntry>` |
| Per-entry cost | ~60 bytes (node + string + value) | 12 bytes (`uint64_t hash + uint32_t offset`) |
| 1500 rules | ~90 KB | ~18 KB |
| Lookup | hash map (pointer chase) | `std::lower_bound` (11 integer comparisons) |
| Load-time allocs | 1500 heap nodes + 1500 string allocs | 1 vector alloc + 256-byte stack buffer per entry |

`SelectorEntry` stores a **FNV-1a 64-bit hash** of the selector string. The hash is computed in-place from a stack buffer during index load — no `std::string` allocation per entry. The vector is sorted by hash after load. `lookupRule` binary-searches by hash; a match is treated as definitive (collision probability ~10⁻¹⁰ for 1500 entries over 64-bit FNV-1a).

The on-disk cache format is unchanged. No cache version bump was needed.

## CSS bounds and other bounded caches

The following in-RAM structures are all bounded at compile time:

| Structure | Bound | Enforcement |
|---|---|---|
| `cacheRuleOffsets_` | `MAX_RULES = 1500` entries | Parse-time cap; file header guard on load |
| `hotRuleCache_` | `HOT_RULE_CACHE_SIZE = 128` entries | LRU eviction on insert |
| `negativeRuleCache_` | `NEGATIVE_CACHE_SIZE = 256` entries | Cleared wholesale when full |
| `rulesBySelector_` | `MAX_RULES = 1500` | Parse-time cap (freed to disk after `saveToCache`) |
| `compileSelectorOffsets_` | `MAX_RULES = 1500` | Parse-time cap (freed after `endCacheCompile`) |

`negativeRuleCache_` clears wholesale rather than evicting LRU. This is a deliberate simplicity choice: under adversarial CSS it thrashes, but typical EPUB CSS is well-behaved and the cache stays cold.

## Image decode heap requirements

The PNGdec decoder allocates `sizeof(PNG) ≈ 59 KB` as a single contiguous block. This fails after the secondary buffer has been reallocated and heap is fragmented.

The warm-pass sequence that avoids this:

1. `createSectionFile` completes (secondary buffer still released — ~52 KB free).
2. `Section::warmAllImageCaches` is called immediately, before `reallocSecondaryBuffer`.
3. Largest free block is ~109 KB — PNG decoder fits.
4. `.pxc` pixel cache files are written to SD.
5. `reallocSecondaryBuffer` is called.
6. Subsequent `renderContents` calls find the `.pxc` already cached and skip the decoder entirely.

For extreme fragmentation cases where step 3 still fails, `maybeRestartForFragmentedHeap` releases both framebuffers via `releaseFrameBuffersWithScratch`, installs a ~20 KB scratch buffer in their place, retries the warm pass with maximum headroom (~96 KB freed), then reboots. The scratch buffer is intentionally leaked — the reboot is unconditional.

## `loadFileStatSlim` cursor optimization

`loadFileStatSlim` maintains `lastCentralDirPos` / `lastCentralDirPosValid` as a sequential scan cursor. When calls arrive in approximately file order (the common case during section build), the cursor advances without wrapping, giving O(1) amortized cost per lookup. Wrap-around handles out-of-order calls. This makes per-image stat lookups in `EpubImageManifest::build` efficient without building a full in-memory cache.

## `readFileToMemory` — unguarded whole-file allocation

`ZipFile::readFileToMemory` allocates `uncompressedSize` bytes from the ZIP header. Only one call site exists: `Epub::readItemContentsToBytes`, used to read a cover-page HTML for image-src extraction. The HTML is freed immediately after the `src` attribute is found. A malformed `uncompressedSize = 0xFFFFFFFF` would produce a null return from `malloc` and be handled gracefully. No other callers should be added without a size guard.
