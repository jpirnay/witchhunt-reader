# Inline ids and footnote links drift with hyphenation

Part of [Memory and parsing](../memory-and-parsing.md). Checked against master on 2026-11-20 (8d1541bf4).

**Open.** An inline id waits with the index of the next word of its block, counted as the parser adds tokens
(`inlineAnchorWordIndex`, `ChapterHtmlSlimParser.cpp:5406`), and is recorded by the line whose running count of tokens
(`wordsExtractedInBlock`, `:5884`) reaches it. A word that hyphenation splits is one token to the parser and two in the
lines: the remainder becomes a token of its own (`ParsedText::hyphenateWordAtIndex`, `ParsedText.cpp:3157`, `:3190`),
which `extractLine` counts (`:3867-3870`). The lines' count runs a token ahead per split, so an id is recorded up to
that many tokens early, never late; within one layout window only, since `lineTokensSoFar` counts the splits already
made when the next id is queued. The bound is in tokens: under a page at 480x800, possibly more on a short screen
(Murphy, 240 px). Measured in `AnchorMapTest` (`test_inline_anchors.epub`, chapter 4: an id before every fourth marker
word of a paragraph of long words, 40 ids, 8 to a 96-token window, under the queue's 16): hyphenation off, 0 of 40 ids
off their word's page (`InlineIdsNameTheirWordsPageWithHyphenationOff`); on, 84 words split and 2 of 40 ids name the
page before their word's, none a later one or further (`InlineIdsHoldWithHyphenationOn`). (A first measure, 56 of 159,
put an id before every third token: past the 16 the queue holds per window, ids are recorded at the page reached so
far, and that cap, not hyphenation, made most of it.) A footnote link counts its word the same way (`:5166`) and
drifts alike (pre-existing).

**Next step**, a follow-up PR: `TokenBoundary::Boundary` gains `bool splitRest : 1` in the bitfield byte
`noSpaceBefore`/`keptSpaces` leave spare (`TokenBoundary.h:54-57`; no size change, lines do not serialize it), set on
the remainder's boundary in `hyphenateWordAtIndex` (`ParsedText.cpp:3190`), subtracted in `sourceTokenCount` (`:1172`)
and in `extractLine`'s `lineTokens` loop (`:3869`). A remainder never continues a group, so `mergeGluedGroups` keeps its
flag; a remainder split again keeps it and the new remainder gets one; the retry that joins the parts erases it. It
moves footnote links in hyphenated layouts too (the `footnotes=` lines of the `_hyph` goldens may change). Then
tighten `InlineIdsHoldWithHyphenationOn` to the word's page; until then it allows one page, the link Back stack's
tolerance (`EpubLinkBackStack.h:73-76`).

Also approximate, recorded only: an id between the images of an image-only paragraph whose images cross pages
(`<p><img/><a id/><img/><img/></p>`) names the last image's page, not the next image's: `placeImageOnlyParagraph`
records the waiting ids once all its images are placed (`:6856`). The one case now recorded late (master put it
later still, at the next block's lines); recording per placed image would be exact, worth it only if a book shows it.
