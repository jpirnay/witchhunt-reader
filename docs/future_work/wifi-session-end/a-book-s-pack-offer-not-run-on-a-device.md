# A book's pack offer, not run on a device

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, a device run.** After PR 2 the Font Manager opened by a book's offer (a reading font or hyphenation pack the
book's language needs, `BookPackOffer`, `EpubReaderActivity`) goes back into the book in the same boot on the PSRAM
boards: `FontDownloadActivity::leave()` opens it with `openBook()`, which arms the book-open HALF that the restart's
reader resume armed. Every other route of the Font Manager and Check for updates ran on the X4 Pro and the Murphy
([network-stack-findings.md](../../network-stack-findings.md), "Downloads"). This one did not: the reader makes no
offer for a book under `/test` (`SerialTestApi::isTestBook()`), and the device runs open only those.

**Next step:** a copy of a book whose language lacks its hyphenation pack, outside `/test` (back up `recent-books.bin`,
the reading stats and `state.json` first, and restore them), on a PSRAM board. Accept the offer, let it download,
Back. Expect `Session end [FONT] ... -> carry on`, `STATE` on the same book and page, and the first frame
`mode=HALF`. Then delete the pack again and restore the card files.
