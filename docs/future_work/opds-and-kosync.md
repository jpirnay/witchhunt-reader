# OPDS and KOSync

Status: open items as of 2026-10-21, from the device checks of PRs #534 and #536; the auto-push item added
2026-11-15.

## Gutenberg: the OpenSearch description on every feed

Every Gutenberg feed load (root, search, book feed, and their re-fetches) is followed by a fetch of
`osd-books.xml` with its own TLS handshake, about 1.2 s each on the X3; master does the same. Seen on
the device.

- Where: `OpdsBookBrowserActivity::fetchOsdTemplate()`, called after each feed parse.
- Next step: keep the picked template per server for the session (or in the discovery cache) and
  fetch the description once.

## Gutenberg: "Feed parse truncated some fields (flags=0x4)"

Logged on Gutenberg's root and search feeds, master too. Every list still renders. Find which field
is truncated and whether it matters (titles, summaries).

## KOSync

- The TLS heap gate on an http URL that redirects to https (`fix(kosync)` in #536) is checked only by
  reading the code: the local test server does not redirect.
- Misleading log labels: plain-http requests are labelled "(session reused)", and the http progress
  PUT logs `tls_mode=handshake`.
- **The in-reader auto-push maps the XPath on the heap.** Its pushes (`silentUploadCurrentPosition`,
  `maybeAutoPushInterval`, `maybeAutoPushOnSleep`) take the position from `currentKoPosition`
  (`src/activities/reader/EpubReaderAutoSync.cpp:69-85`), which calls `ProgressMapper::toKOReader`, whose
  `ChapterXPathIndexer::findXPathForVisibleOffset` call (`lib/KOReaderSync/ProgressMapper.cpp:146`) passes no lent
  region, so the chapter parse takes its parser state, read buffer and 32 KB inflate ring from the heap. On the X3 a
  chapter over 32 KB may not get its ring (read in the code, not seen on a device). The bookmark mapping already parses in the lent secondary framebuffer
  (`EpubReaderActivity::stepStarredPages()`, `LentSecondary`); the mappers take that region as their `scratch`
  argument (`ChapterXPathIndexer.h`). Next step: lend it for the push's mapping too. The sync screen's own push
  (`KOReaderSyncActivity.cpp:1270`) maps on the heap as well.
