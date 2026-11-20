# A line the allocators refuse is dropped while the build goes on

Part of [Memory and parsing](../memory-and-parsing.md). Checked on 2026-11-20 against master 8d1541bf4 with 9b (#851).

**Open.** Two refusals drop a laid-out line and let the build go on, so the section is written without the line's text
and is not marked truncated:

- the line's `TextBlock`: `TextBlock::make` returns null when the heap refuses the object, and an invalid block when
  neither the heap nor the arena gives its bytes (`TextBlock.cpp:192`); `ParsedText.cpp:4405-4410` logs "Dropping line"
  and returns `Accepted`;
- the line's `PageLine`: `ChapterHtmlSlimParser.cpp:5957-5961`, the same.

The parser's other refusals stop the build instead (`stopForHeap`, `ChapterHtmlSlimParser.cpp:2112`), which keeps the
pages so far as a truncated section. Found in the review of 9b (#851); master had it too.

**Next step.** Stop the build on both. The `PageLine` site can call `stopForHeap`. `ParsedText` has no handle on the
parser: a third `LineProcessResult` (`ParsedText.h:33-36`) would carry the refusal back. A fault-injected host test for
each, as `PageHeapTest.cpp` (`PAGE_FAULT_INJECTION`) does for pages.
