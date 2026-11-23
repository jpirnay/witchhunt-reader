# A line the allocators refuse is dropped while the build goes on

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** Two refusals drop a laid-out line and let the build go on, so the section is written without the line's text
and is not marked truncated:

- the line's `TextBlock`: `TextBlock::make` (`TextBlock.cpp`) returns null when the heap refuses the object, and an
  invalid block when neither the heap nor the arena gives its bytes. `ParsedText::emitLine` logs "Dropping line" and
  returns `LineProcessResult::Accepted`;
- the line's `PageLine`: in `ChapterHtmlSlimParser::addLineToPage`, `PageLine::make` (`Page.cpp`) returns null when
  neither the page block nor the heap takes it, and the parser logs "Dropping line" and returns `Accepted` too.

The parser's other refusals stop the build instead (`ChapterHtmlSlimParser::stopForHeap`), which keeps the pages so far
as a truncated section. Found in the review of 9b (#851); master had it too.

**Next step.** Stop the build on both. The `PageLine` site can call `stopForHeap`. `ParsedText` has no handle on the
parser: a third value of `ParsedText::LineProcessResult` (today `Accepted` and `RetryWithoutHyphenation`) would carry
the refusal back. A fault-injected host test for each, as `PageHeapTest.cpp` (`PAGE_FAULT_INJECTION`) does for pages.
