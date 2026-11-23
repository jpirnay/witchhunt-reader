# Memory and parsing

The open items are one file each in [memory-and-parsing/](memory-and-parsing/). A PR that closes an item deletes its file,
and a follow-up it finds goes into a new file. Neither touches this page or this topic's row in
[README.md](README.md), so PRs that work on different items never conflict over these docs.

Items last checked against master on 2026-11-23 (1a9f93b08), unless their file says otherwise. Collected from the
heap-work handover (2026-08), the memory audit (2026-09), the earlier memory-allocation-strategy.md (trimmed in the
2026-10-07 docs cleanup; in git history), the triage of 2026-11
([spec](../superpowers/specs/2026-11-18-memory-and-parsing-triage.md), [plan](../superpowers/plans/2026-11-18-memory-and-parsing.md))
and what its PRs found, and the attribution of master's heap-peak rise (2026-11-19, at cb8a9a6ab).

The rules and measurements these items refer to are in [Memory Allocation Strategy](../memory-allocation-strategy.md).
What the triage shipped is recorded there (§9.4-§9.6) and in [Stylesheet scope](../design/stylesheet-scope.md); the
spec's status list says where each of its sections went.
