# Book open keeps the content.opf SAX state on the heap

Part of [Memory and parsing](../memory-and-parsing.md). Checked against master on 2026-11-20 (8d1541bf4).

**Open.** `Epub::load` parses content.opf with a `ContentOpfParser` on the stack (`Epub.cpp:186`), whose `setup()`
(`ContentOpfParser.cpp:376`) has `SaxParser::init` take the parser's Full-profile state from the heap
(`SaxParserYxml.cpp:279`): 9,848 B on the 64-bit host. The spine table beside it already comes from the region lent to
`load()` (`loadScratch_`, `Epub.cpp:195-199`), and so do the zip lookup, the CSS cache compile, the TOC pass and the
`book.bin` build.

**Why it matters.** In the host harness most fixtures' arena-mode heap peak is in book open, not in the build (9 of 13
before #849, which passes the lent region to `load()`). This state stays on the heap after #849.

**Next step.** Offer the state from `loadScratch_` when the region has room for it beside the spine table and the
stream read's block (`streamScratchBytes()`), as the note previews do (`FootnotePreviews.cpp:1073`, `:1176`, through
`SaxParser::setExternalState`), and keep the heap otherwise. Mind the region's LIFO order: the state outlives the
read's block, so it is taken before the spine table. Then re-read the gate's arena rows (`HeapPeakRegression`).

From the attribution of master's heap-peak rise (2026-11-19, cb8a9a6ab). Its per-fixture peak figures are not
reliable to about 4 KB (the host heap tracker's snapshots lag), so measure each fixture before and after: #848
gave about a third of what that attribution predicted.
