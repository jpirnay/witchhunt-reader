# OPDS and KOSync

Status: open items from the device checks of #534 and #536, and the auto-push's mapping.

## Gutenberg: the OpenSearch description on every feed

Every Gutenberg feed load (root, search, book feed, and their re-fetches) is followed by a fetch of
`osd-books.xml` with its own TLS handshake, about 1.2 s each on the X3. Seen on the device.

- Where: `OpdsBookBrowserActivity::fetchFeed()` resets `searchTemplate` from each parsed feed and,
  when the feed names only a description, calls `fetchOsdTemplate()`. Nothing keeps the template.
- Next step: keep the picked template per server for the session (or in the discovery cache) and
  fetch the description once.

## Gutenberg: "Feed parse truncated some fields (flags=0x4)"

Logged on Gutenberg's root and search feeds (`OpdsBookBrowserActivity::fetchFeed()`). Every list still
renders. `0x4` is `SaxParser::kTruncAttrValue`: an attribute value longer than the yxml backend's 384
bytes (`kAttrValueLen` in `lib/SaxParser/SaxParserYxml.cpp`), most likely a link's `href`. Titles and
summaries are element text, so the flag does not point at them. Find which attribute is cut and whether
a cut acquisition or navigation link breaks a download or a page.

## KOSync

- The TLS heap gate on an http URL that redirects to https (`fix(kosync)` in #536) is checked only by
  reading the code: the local test server does not redirect.
- Misleading log labels: plain-http requests are labelled "(session reused)" (`logResponseBody` in
  `KOReaderSyncClient.cpp`), and the http progress PUT logs `tls_mode=handshake` (`logTlsAttemptPlan`).
  Neither knows whether the URL is http.
- **The in-reader auto-push maps the XPath on the heap.** Its pushes (`silentUploadCurrentPosition`,
  `maybeAutoPushInterval`, `maybeAutoPushOnSleep`) take the position from
  `EpubReaderActivity::currentKoPosition` (`src/activities/reader/EpubReaderAutoSync.cpp`), which calls
  `ProgressMapper::toKOReader`, whose `ChapterXPathIndexer::findXPathForVisibleOffset` call passes no
  lent region, so the chapter parse takes its parser state, read buffer and 32 KB inflate ring from the
  heap. On the X3 a chapter over 32 KB may not get its ring (read in the code, not seen on a device).
  The bookmark mapping already parses in the lent secondary framebuffer (`EpubReaderActivity::stepStarredPages()`,
  `LentSecondary`, `StarAnchoring`); the mappers take that region as their `scratch` argument
  (`ChapterXPathIndexer.h`). Next step: lend it for the push's mapping too. The sync screen's own push
  (`KOReaderSyncActivity::computeLocalProgressAndChapter`) maps on the heap as well.
