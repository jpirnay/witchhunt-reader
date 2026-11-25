# The body-size census is larger than its decision

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A build that measures the body size takes a `BodySize::Census` from the heap (`st.bodySizeCensus` in
`Section::runBuildParse`): `Census::kBuckets` (256) 32-bit counts and a total, 1,028 B (`BodySize.h`), held to the end
of the build. Only builds with the book's style and Normalize Font Size on take it, until one build has counted
`BodySize::kDecideChars` (8,000) characters: in practice the cold builds at a book's first open.

**Why it matters.** It is live at every peak of those builds, heap and lent alike.

**Next step.** A smaller census with the same decision: the attribution proposed a 16-entry table of sizes and counts
(about 100 B) or 16-bit saturating counts. Either has to keep what `BodySize::bodySizeOf` (`BodySize.cpp`) reads: it
takes the largest of all 256 buckets first and answers 100 when that one lies outside 30-250 %
(`ParsedText::MIN/MAX_WORD_SIZE_PCT`), so a table of the in-range sizes alone decides differently when most text is
out of range, and saturated counts can tie. `BodySizeTest` and the body-size goldens cover the decision.

From the attribution of master's heap-peak rise (2026-11-19, cb8a9a6ab). Its per-fixture peak figures are not
reliable to about 4 KB (the host heap tracker's snapshots lag), so measure each fixture before and after: #848
gave about a third of what that attribution predicted.
