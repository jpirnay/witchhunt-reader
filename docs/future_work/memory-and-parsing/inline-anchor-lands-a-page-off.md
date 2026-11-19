# An inline anchor lands a page off (CrossPoint #4328)

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A block's anchor waits for its first line, but an `id` on an inline element is placed approximately: an
`<a id>` inside a paragraph is recorded with the next block (`pendingAnchorId`, `ChapterHtmlSlimParser.cpp:2992`), a
`<span id>` with its block's first line (`recordSpanAnchor`, `:5352`). A link to it (a backlink to a noteref) can land a
page off; the link Back stack already allows for that (`EpubLinkBackStack.h:73-75`). Take the inline-anchor part only:
record such an id at the page of its own line. Needs a `SECTION_FILE_VERSION` bump. crosspoint-reader #4328, Mikalai
Kryvusha (@MikalaiKryvusha).
