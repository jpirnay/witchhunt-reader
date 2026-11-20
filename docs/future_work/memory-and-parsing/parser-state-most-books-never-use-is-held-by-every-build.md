# Parser state most books never use is held by every build

Part of [Memory and parsing](../memory-and-parsing.md). Checked against master on 2026-11-20 (8d1541bf4).

**Open.** The parser and the `BuildState` are each one heap block held from setup to the end of every build. Across
the PRs the attribution covered the parser grew from 2,064 to 4,360 B and `BuildState` from 640 to 1,488 B (host
`sizeof`). Two members serve only some books:

- `cssAncestorMasks_`, 32 `uint64_t` (256 B, `ChapterHtmlSlimParser.h:1115-1116`), read only for a sheet with compound
  rules;
- `BuildState::Carry` (`Section.cpp:1093-1105`, 296 B host), used only when the build carries a heading file
  (`carry.spine >= 0`).

**Why it matters.** Both are live at every peak, heap and lent alike.

**Next step.** Box each: allocate it, nothrow, only when the book needs it. A refusal stops the build, unless matching
as if the sheet had no compound rules is shown to be safe. Then add host `static_assert` ceilings on
`sizeof(ChapterHtmlSlimParser)` and `sizeof(BuildState)`, so a PR that adds a member states its size, and box new state
that most books never use.

From the attribution of master's heap-peak rise (2026-11-19, cb8a9a6ab). Its per-fixture peak figures are not
reliable to about 4 KB (the host heap tracker's snapshots lag), so measure each fixture before and after: #848
gave about a third of what that attribution predicted.
