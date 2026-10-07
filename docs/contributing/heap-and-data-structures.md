# Heap and Data Structure Design

This document records the rationale behind the memory management constraints and data structure choices in the codebase. It is intended for contributors making changes to the ZIP, CSS, EPUB, or rendering layers — and for AI assistants reviewing or extending those areas.

The memory budget, the allocation classes and the borrow-versus-release rules live in [Memory Allocation Strategy](../memory-allocation-strategy.md). That document carries the measured numbers; this one does not repeat them.

## The core constraint

The ESP32-C3 boards (X3, X4) have about 380 KB of SRAM and no PSRAM. Two framebuffers, one panel-sized buffer each, are allocated at boot and are the largest heap consumers; the secondary one can be lent to a build or released temporarily. Fonts, display init, stacks and the rest of the runtime share what is left. See [Memory Allocation Strategy §1](../memory-allocation-strategy.md#1-the-budget) for the figures per board.

**Fragmentation kills this device before total usage does.** The ESP-IDF heap (TLSF) has no compaction. After a heavy parse the heap can report plenty of free memory while the largest contiguous block is too small for one framebuffer. Every design decision below follows from this.

## Allocation rules

Apply in order; stop at the first yes:

1. **Stack** — local, bounded, under ~256 bytes total: plain array or struct.
2. **Flash/`static constexpr`** — compile-time constant: zero DRAM cost.
3. **Allocated once per activity lifetime** — allocate in `onEnter`, release in `onExit`, held as a member.
4. **Dynamic and fallible** — `new (std::nothrow)` or `makeUniqueNoThrow` with a null check and `LOG_ERR`. Never bare `new` (calls `abort()` on OOM under `-fno-exceptions`).
5. **SDK API takes ownership** — only then raw `malloc`, with a comment naming the owner.

Short-lived allocations repeated inside a long pass (per image, per line, per paragraph) belong in an arena or in storage reused across items, not on the heap. See [Memory Allocation Strategy §2](../memory-allocation-strategy.md#2-classify-every-allocation-by-lifetime).

## Vector discipline

`std::vector` grows by doubling: each growth is an alloc-copy-free triple that leaves a hole. Rules:

- Always `reserve(n)` before a `push_back` loop when `n` is known.
- When `n` is not known upfront, do a first pass to count, then `reserve`, then fill, or reserve from an estimate that errs high.
- Never grow a vector inside a render or per-element callback.
- A growth that cannot allocate calls `abort()`. Where a vector can grow on a tight heap, check for the block first (see `ParsedText::addWord`).

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
| Per-entry cost | ~60 bytes (node + string + value) | 8 bytes (`uint32_t hash + uint32_t offset`) |
| 1500 rules | ~90 KB | ~12 KB |
| Lookup | hash map (pointer chase) | `std::lower_bound` (11 integer comparisons) |
| Load-time allocs | 1500 heap nodes + 1500 string allocs | 1 vector alloc + 256-byte stack buffer per entry |

`SelectorEntry` stores a **32-bit FNV-1a hash** of the normalized selector and the byte offset of its rule record in the cache file. The hash is computed in place from a stack buffer during index load — no `std::string` allocation per entry. The vector is sorted by hash after load, and `lookupRule` binary-searches it. The hash is only a candidate filter: a lookup re-reads the length-prefixed selector at the offset and compares it with the query, so a collision costs one extra SD read instead of resolving the wrong style.

`CssParser::CSS_INDEX_BYTES_PER_RULE` (8) is pinned to `sizeof(SelectorEntry)` by a `static_assert`, because `Section::heapAllowsEmbeddedStyle` sizes its contiguous-block floor from it.

When a section build runs in the borrowed framebuffer, the parser keeps the ruleset in the build arena instead (`CssParser::setIndexArena`): either resident, as a sorted index plus a pool of distinct compressed styles, or as an arena-backed copy of the same 8-byte index. Every build also runs with `setLeanResolve(true)`, which skips the hot-rule cache.

## CSS bounds and other bounded caches

The following in-RAM structures are all bounded at compile time:

| Structure | Bound | Enforcement |
|---|---|---|
| `cacheRuleOffsets_` | `MAX_RULES = 1500` entries | Parse-time cap; the cache is stamped truncated (`rulesTruncated()`) so the book is not re-parsed on every open |
| `hotRuleCache_` | `HOT_RULE_CACHE_SIZE = 128` entries | LRU eviction on insert; unused during builds (lean resolve) |
| `negativeRuleCache_` | `NEGATIVE_CACHE_SIZE = 256` entries | Cleared wholesale when full |
| `rulesBySelector_` | `MAX_RULES = 1500` | Parse-time cap (freed to disk after `saveToCache`) |
| `compileSelectorOffsets_` | `MAX_RULES = 1500` | Parse-time cap (freed after `endCacheCompile`) |

`negativeRuleCache_` clears wholesale rather than evicting LRU. This is a deliberate simplicity choice: under adversarial CSS it thrashes, but typical EPUB CSS is well-behaved and the cache stays cold.

## Image decode heap requirements

PNG decoding uses `PngStreamDecoder` (uzlib): a decoder object of about 3 KB, an inflate ring of at most 32 KB and two scanline buffers. JPEG decoding uses TJpgDec with a 12 KB work pool, or a larger workspace for progressive images. The large blocks come from the pass-wide decode arena (`image_scratch`) when one is installed, and from the heap otherwise.

There is no decode pass after a section build. A section needs only image dimensions, which come from the image headers. An image is decoded when its page is drawn, or earlier by the reader's image lane, and the result is written to a `.pxc` pixel cache on SD so later renders skip the decoder. The per-page decode borrows the secondary framebuffer as its arena rather than releasing it; see [Memory Allocation Strategy §9.3](../memory-allocation-strategy.md#93-image_scratch-and-the-borrowed-region).

For the fragmented-heap case, `maybeRestartForFragmentedHeap` frees both framebuffers, installs a framebuffer-sized scratch buffer in their place, decodes the section's uncached images once more with the freed headroom, and reboots. See [Temporary Memory Increase Logic](./temporary-memory-increase.md).

## `loadFileStatSlim` cursor optimization

`loadFileStatSlim` maintains `lastCentralDirPos` / `lastCentralDirPosValid` as a sequential scan cursor. When calls arrive in approximately file order (the common case during section build), the cursor advances without wrapping, giving O(1) amortized cost per lookup. Wrap-around handles out-of-order calls. This makes the per-image stat lookups in `EpubImageManifest::resolve`, which keeps its `ZipFile` open for the whole build, efficient without building a full in-memory cache.

## `readFileToMemory` — unguarded whole-file allocation

`ZipFile::readFileToMemory` allocates `uncompressedSize` bytes from the ZIP header. Only one call site exists: `Epub::readItemContentsToBytes`, used to read a cover-page HTML for image-src extraction. The HTML is freed immediately after the `src` attribute is found. A malformed `uncompressedSize = 0xFFFFFFFF` would produce a null return from `malloc` and be handled gracefully. No other callers should be added without a size guard.
