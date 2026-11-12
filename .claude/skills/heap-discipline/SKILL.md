---
name: heap-discipline
description: Memory allocation discipline for this firmware (ESP32-C3 X3/X4 with ~380 KB SRAM and no PSRAM; ESP32-S3 boards with 8 MB PSRAM). Use whenever writing or reviewing code that allocates: new / malloc / std::vector / std::string, buffers, caches, tables that grow with a book, or anything held across a loop, a build or an activity lifecycle. Covers the -fno-exceptions abort trap, makeUniqueNoThrow / HeapFit / ChunkedVector / BuildArena / PassTable, largest-free-block thinking, lending the secondary framebuffer, data on SD instead of RAM, and PSRAM on the S3 boards.
---

# Heap Discipline

The main guide (`.skills/SKILL.md`, "Memory") states the facts. This skill is the procedure you run
while writing the code and the gate you run before handing it back. Background:
`docs/memory-allocation-strategy.md`, `docs/contributing/heap-and-data-structures.md`.

**The largest free block kills this device, not total usage.** The X3 is the tightest board. Free heap can
read 15 KB while no block over 4 KB exists. Optimise for not leaving holes and for asking for blocks that
exist, not only for using fewer bytes.

## The abort trap

The build is `-fno-exceptions`. Every throwing allocation calls `abort()` on failure and the device
reboots: plain `new`, `make_unique`, `std::vector` growth (`push_back`, `resize`, `reserve`) and
`std::string` growth. A reader that dies in the middle of a chapter build reboots, rebuilds from page 0 and
dies at the same page again.

## Allocation decision procedure

Ask in order; stop at the first yes.

1. **Does it belong on the card?** Data per spine item, per page or per book that is read later goes in
   one compact file under the book's cache folder, read in ranges. Never size a RAM table by spine count
   (books with 1,700+ spine items exist).
2. **Stack?** Bounded and small (a few hundred bytes). The loop task has 16 KB and the render task
   10 KB, shared with every frame above you, and the deepest chains (page build, CSS, image decode)
   run there.
3. **Compile-time constant?** `static constexpr`, or `const char* const` for string tables (without
   the second `const` the pointer array sits in DRAM).
4. **Allocated once, reused?** Hold it in a member from `onEnter()` to `onExit()`, or for the length of
   the build. Never allocate per frame, per line or per glyph.
5. **Part of a section build?** Take it from the `BuildArena` (`lib/Memory/BuildArena.h`): one budgeted
   block, bump allocation, no fragmentation.
6. **Grows with the document?** Use `ChunkedVector` (`lib/Memory/ChunkedVector.h`). It never
   reallocates, never copies, and returns false instead of aborting.
7. **A lookup table that may not fit?** Use `PassTable` (`lib/Memory/PassTable.h`). It sizes to what
   is there and takes more passes over the file instead of failing.
8. **Dynamic and fallible?** Use `makeUniqueNoThrow<T>(...)` or `makeUniqueNoThrow<T[]>(n)`
   (`lib/Memory/Memory.h`). Null-check it, `LOG_ERR` the size, and take a defined fallback.
9. **A std container must grow by a data-driven amount?** Ask `HeapFit` (`lib/Memory/HeapFit.h`) for
   a block first, or grow through its helper, and have a path for "no".
10. **A C/SDK API takes ownership?** Only then use raw `malloc` or `new (std::nothrow)`, with a comment
    naming who frees it.

## Fragmentation rules

- Reserve before a `push_back` loop. A growth step is three heap operations (allocate, copy, free) and
  leaves a hole.
- Do not allocate inside a loop or render path when the allocation can be hoisted out.
- `vector::shrink_to_fit()` is a no-op under `-fno-exceptions`. Free with `std::vector<T>().swap(v)`.
  (`std::string::shrink_to_fit()` does shrink.)
- Clear a cache before you reallocate it. Otherwise the old and the new block coexist at the peak.
- A long-lived allocation made right after a large block is freed lands in that hole and pins it. Allocate
  long-lived state first, or keep it out of the hole.
- Large transient work borrows the secondary framebuffer (`borrowSecondaryBuffer()` /
  `returnSecondaryBuffer()`); it does not `malloc` a second big block. A lent block never enters the heap,
  so returning it cannot fail. Read the sync rule in the main guide before you borrow.
- Grayscale passes use `storeBwBuffer()` / `restoreBwBuffer()` (chunked). Do not add another full-screen
  buffer.
- Clean up on every exit path: early returns, aborted builds, an activity buried by a child.

## The S3 boards

The X4 Pro, T5S3 and Murphy have 8 MB of PSRAM. Logic must still fit the X3, but do not hold the S3s to
the X3's budget when PSRAM makes something faster or more robust. Gate the choice on a capability in
`HalCapabilities` (`keepsSecondaryBuffer()` and `decodesThroughRefresh()` are the existing examples),
never on a board name. S3 boards never release the secondary framebuffer.

## Justify every allocation

When you add a heap allocation, state in one line its worst-case size and why stack, static, card or
reuse were rejected. If you cannot name the size, you cannot budget it. Claims of memory saved come from
a measurement: the `RAM:` line of `pio run` against master in the same worktree, a build summary's arena
lanes, or a device `STATE` heap/maxblock reading.

## Self-review before handoff

- [ ] No throwing allocation (`new`, `make_unique`, unguarded container growth) whose size comes from data.
- [ ] Every fallible allocation is null-checked, logged with its size, and has a fallback.
- [ ] Nothing per spine item or per page is held in RAM; it lives on the card.
- [ ] No allocation in a loop or render path that could be hoisted; every `push_back` loop reserves.
- [ ] No `vector::shrink_to_fit()` relied on to free memory.
- [ ] String and pointer tables are `const char* const`.
- [ ] Anything allocated in `onEnter()` or a build is released on every exit path.
- [ ] No second full-screen buffer; big transient work borrows the secondary buffer (synced first where
      an overlay follows).
- [ ] Board differences go through a `HalCapabilities` check, and the S3 path uses PSRAM where it helps.
- [ ] Each new allocation carries a one-line size and why-not-stack/static/card note.
