# ChunkedVector directories are sized for their largest table

Part of [Memory and parsing](../memory-and-parsing.md). Checked against master on 2026-11-20 (8d1541bf4).

**Open.** A `ChunkedVector` holds its chunk directory inline, sized at compile time for its `Capacity`
(`ChunkedVector.h:185`). The section's page table (`Section::PageLut`, `Section.h:59`, 10,000 entries) has about 43
chunk pointers and the parser's paragraph table (`ParagraphLut`, `ChapterHtmlSlimParser.h:998`, 10,000 pages) about
81, though almost every chapter fits its first chunk. One `PageLut` is in each build's `BuildState`
(`Section.cpp:1118`) and one in every resident `Section` (`Section.h:66`).

**Why it matters.** The parser and the `BuildState` are each one heap block that lives from setup to the end of the
build, so the directories' bytes are held at every peak of every build, heap and lent alike: about 500 B a build on the
C3 and about 170 B in each resident `Section` (estimated from the pointer counts, not measured).

**Next step.** Keep chunk 0's pointer inline and allocate the rest of the directory, nothrow, only when a table outgrows
its first chunk. A refusal is a `push_back` that returns false, which the callers already turn into the full-table stop.
`CHUNKED_VECTOR_FAULT_INJECTION` (`test/chunked_vector/`, `LongChapterPageTablesTest.cpp`) drives the refusals.

From the attribution of master's heap-peak rise (2026-11-19, cb8a9a6ab). Its per-fixture peak figures are not
reliable to about 4 KB (the host heap tracker's snapshots lag), so measure each fixture before and after: #848
gave about a third of what that attribution predicted.
