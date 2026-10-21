# OPDS and KOSync

Status: open items as of 2026-10-21, from the device checks of PRs #534 and #536.

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
