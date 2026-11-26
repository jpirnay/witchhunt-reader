# KOReader Sync setup from the reader menu on the C3s

Part of [Memory and parsing](../memory-and-parsing.md).

**Open, needs an X4 measurement.** On the PSRAM boards the reader menu's Sync tab is there without a KOReader account:
one row, KOReader Sync, opens `KOReaderSettingsActivity` over the book and its menu, and the tab is built again when
it closes (`EpubReaderMenuActivity::rebuildSyncItems`). The C3s still show the tab only with an account, as before,
behind `HalCapabilities::offersSyncSetupInReader()` (`lib/hal/HalCapabilities.h`).

What it puts on the heap: the KOReader settings list, and for the server, the user name or the password the keyboard
(`KeyboardEntryActivity`), all with the reader and the reader menu below them. The account check from there
(Authenticate, Register) is not the question: on the C3s it replaces every screen, the book included
(`KOReaderSettingsActivity`), and runs with the heap of a check from Settings. Nobody has measured the setup itself
over the reader on a C3; the T5S3 ran the whole flow (2026-11-26).

**Next step:** on the X4 (and the X3 when it is back), with the predicate forced to true, open a large book, then the
reader menu's Sync tab, KOReader Sync, the user name, the keyboard, and back, reading `CMD:STATE`'s `heap` and
`maxblock` at each screen. If the reader's next page turn and image decode still have their heap, make
`offersSyncSetupInReader()` true everywhere and delete this file.
