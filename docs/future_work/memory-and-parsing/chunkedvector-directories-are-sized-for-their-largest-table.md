# ChunkedVector directories are sized for their largest table

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A `ChunkedVector` holds its chunk directory inline (`chunks_[MAX_CHUNKS]`, `ChunkedVector.h`), sized at
compile time for its `Capacity`. The section's page table (`Section::PageLut`, 10,000 entries) has 43 chunk pointers
and the parser's paragraph table (`ChapterHtmlSlimParser::ParagraphLut`, `Page::MAX_PAGES_PER_SECTION` entries) 81
(from `MAX_CHUNKS`' formula), though almost every chapter fits its first chunk. One `PageLut` is in each build's
`BuildState` (`BuildState::lut`, `Section.cpp`) and one in every resident `Section` (`Section::lut`).

**Why it matters.** The parser and the `BuildState` are each one heap block that lives from setup to the end of the
build, so the directories' bytes are held at every peak of every build, heap and lent alike: about 500 B a build on the
C3 and about 170 B in each resident `Section` (estimated from the pointer counts, not measured).

**Next step.** Keep chunk 0's pointer inline and allocate the rest of the directory, nothrow, only when a table outgrows
its first chunk. A refusal is a `push_back` that returns false, which the callers already turn into the full-table stop.
`CHUNKED_VECTOR_FAULT_INJECTION` (`test/chunked_vector/`, `LongChapterPageTablesTest.cpp`) drives the refusals.

From the attribution of master's heap-peak rise (2026-11-19, cb8a9a6ab). Its per-fixture peak figures are not
reliable to about 4 KB (the host heap tracker's snapshots lag), so measure each fixture before and after: #848
gave about a third of what that attribution predicted.
