# The shaped store is on the heap, and the paragraph gate ignores its size

Part of [Memory and parsing](../memory-and-parsing.md). Checked against master on 2026-11-23 (1a9f93b08).

**Open, two parts.** A shaped paragraph's records live in its `ShapedStore`, on the heap. #660 shrank the store's
index and took four more changes (Tier A), and #672 added a free-heap floor (`heap_growth::kShapedFreeFloor`, 12 KB,
checked by `heap_growth::shapedHeapAllows` in `HeapGrowth.h`). What failed on the X3 and what each change bought is in
[The shaped store's heap](../../text-shaping.md#the-shaped-stores-heap). Two things are left:
- **Tier B and C.** Tier B puts the store's chunks (about 4 KB of the stress chapter's store) above the page block and
  rehomes them below it at each page end. Tier C makes the index arena arrays too, which takes the whole store (about
  9.5 KB) off the heap.
- **A size-aware gate.** `ChapterHtmlSlimParser::ensureHeapForTextLayout` compares free heap and the largest block with
  fixed floors; it does not know what the paragraph will take. Before Tier A the stress paragraph took the heap 8.7 KB
  below the last gate reading, 3 KB under the hard floor, with nothing refused. A gate that adds the block's token
  count times a per-token cost would degrade predictably instead of by luck.

**Why it matters.** Only if more room is wanted. The stress case is chapter 2 of `test/epubs/test_shaped_arabic.epub`
in landscape: a span on every word of a 400-token paragraph at a quarter of the body size. With Tier A the X3 builds it
with no gate reached and about 19.8 KB at its lowest. The realistic case, a tajweed book at body size, holds blocks of
96-144 tokens and a store of about 4-6 KB, like Zaydan's chapter, and needed neither.

**Cost.** Tier B is some 250 lines, and on the X4 a span-heavy paragraph at a full page would empty the word cache
for that page.

**Where.** `ShapedStore` (`lib/Epub/Epub/ShapedStore.h`: its chunks and index); `ChapterHtmlSlimParser::releasePageBlock`
(the page block's rewind); `ParsedText`'s `beforeLine_` hook (a line's piece views point into the chunks);
`ensureHeapForTextLayout`.

**Ruled out.** A slab of the lent arena reserved for the store at the parse lane's start, and the store in the free
tail under the word cache:
[Memory Allocation Strategy §8.5](../../memory-allocation-strategy.md#85-eliminated-approaches), rows 17 and 18.

**Next step** (design, not built). Tier B needs the chunks' arena offsets, a rehome inside `releasePageBlock()` before
any other allocation, the line's piece views made after `beforeLine_`, and every live `ParsedText` told (a table's
repeated header keeps its cells' records across pages). Since this was worked out, a line's `TextBlock` and `PageLine`
moved into the line arena and the page block (#851, #856), so check the rehome against them first. For the gate,
measure a per-token cost on the X3 with the stress chapter.
