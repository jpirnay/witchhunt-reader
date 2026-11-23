# Alternative sync providers (OPDS progression, BookOrbit)

Status: **decided 2026-11-23: no firmware work.** A provider other than KOReader sync comes as a plugin, and only
when it is asked for. The firmware plans written up on 2026-10-07 (an OPDS progression parser with pull on open,
compare and landing; BookOrbit's text-node flavour, stats, highlights and catalog in the firmware) are dropped. What
stays is what a plugin can carry and what a plugin author needs to know about the two providers. Related:
[plugins.md](plugins.md) (events step 2), [../sd-plugins.md](../sd-plugins.md), and
[../contributing/koreader-synchronization.md](../contributing/koreader-synchronization.md) for the position layer.

## What a plugin can carry

Since #861 a plugin's `device.json` can declare device events (`docs/sd-plugins.md`, "Events: the reader
sends a plugin's requests"): on `reader.open`, `reader.exit`, `reader.session` or `sleep.enter` the reader
fills in a declared HTTP request and sends it at the next Wi-Fi join or at sleep. It reads the answer to
8 KB and drops it. No plugin code runs on the device.

- **Push:** a handler could PUT the progression when a book closes, from `{event.end_progress_bp}`
  (basis points of 10,000). The progression URL is per book and comes from the catalog entry, so the
  handler needs `{meta.*}` from the book's `<book>.meta.json` with `require_meta`. Nothing writes those
  files yet: that is events step 2, parked ([plugins.md](plugins.md)). From the variables
  `docs/sd-plugins.md` lists (inference): an event carries Unix seconds, not an ISO 8601 `modified`, and
  no chapter title or href for `title` and `reference`. Whether Kavita and Audiobookshelf take a record
  without them is untested.
- **Pull, compare and landing cannot be a plugin:** events are queued and sent later, even `reader.open`,
  and nothing hands a server's answer back to the reader.
- KOSync push already has this form: `docs/sd-plugins.md` names a progress sync (`kosync-autopush`) as a
  usual use of events.
- BookOrbit (B3): its reading stats could be a `reader.session` handler if its endpoint takes such a
  request (inference, untested). The device has no highlights or annotations to send.

So a plugin pushes and never pulls: a position read on another device is not taken over here. That follows from the
decision above.

## The two providers

### BookOrbit (bookorbit.app, fork feyded1020/witchhunt-bookorbit)

- Its progress sync **is the KOSync protocol**: headers `x-auth-user` / `x-auth-key` (MD5 of the
  password), `POST /users/auth`, `GET /syncs/progress/<hash>`, `PUT /syncs/progress`, same document
  hash (`KOReaderDocumentId`). A BookOrbit server entered as the KOReader sync server URL should work
  today, with #398's exact landing. **Not yet tested.**
- Open question: BookOrbit's web reader is a browser DOM. If it numbers text nodes the W3C way
  (whitespace-only nodes kept, raw codepoints), our crengine-shaped `text()[N].M` points mis-land
  there. Test with one push and one pull on a BookOrbit account; the fork's own highlight resolver
  (`lib/EpubPosition/XPathBuildCommon.h`) counts raw, which suggests W3C.
- Beyond progress, the fork adds reading stats (`/plugin/page-stats`), highlights and annotations
  (`/plugin/annotations/exchange`), bookmarks (`/plugin/bookmarks/exchange`) and catalog browsing:
  `lib/BookOrbitSync` + `src/bookorbit`, about 3,000 lines. The fork removed KOReader sync to fit the
  flash partition, which is one reason these stay out of our firmware.
- If the W3C numbering is confirmed, landing a BookOrbit position exactly would need a second text-node
  flavour in the firmware's XPath mapper. That is firmware work the decision rules out: progress through
  BookOrbit then stays unsupported.

### OPDS Progression 1.0 (Kavita, Audiobookshelf; fork syfq91/witchling-reader)

- Our OPDS parser does not recognise the progression link (`rel="http://opds-spec.org/progression"`
  or `type="application/opds-progression+json"`). witchling's implementation:
  `lib/OpdsParser` (link detection), `src/network/OpdsProgressionSync.{h,cpp}` (333 lines; the
  JSON is `{position: {progression, title, reference}, device: {id, name}, modified}`),
  `src/activities/reader/OpdsProgressionSyncActivity.cpp` (297 lines; compare and prompt),
  reader hooks (`currentSyncPosition`, push on close, pull on open, conflict prompt), per-book sync
  config saved from the catalog entry.
- witchling's code is a reference for the record's format only; its firmware side is not taken over.
- witchling lands a pulled position with `jumpToPercent(progression * 100)`: one percent of the
  whole book as granularity. The position is a book fraction plus the spine href; no node path.

## Open

- **BookOrbit with today's KOReader sync.** Enter a BookOrbit server as the KOReader sync server and try one push and
  one pull. If it lands, document "works with BookOrbit" with the server URL form. Needs an account.
- **An OPDS progression push plugin**, once events step 2 brings `{meta.*}` ([plugins.md](plugins.md)). First check
  whether Kavita and Audiobookshelf take a record without an ISO 8601 `modified`, a `title` and a `reference`.
- Which test servers exist: Kavita, Audiobookshelf, a BookOrbit account.
