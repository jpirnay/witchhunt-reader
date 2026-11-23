# Alternative sync providers (OPDS progression, BookOrbit)

Status: **shelved 2026-10-07**, not started. Written so the work can be picked up without redoing the analysis.
Related: `docs/contributing/koreader-synchronization.md` (the position layer this builds on), #398 (landed
through #399).

## What is already in place

Protocol-free, usable by any sync provider (details in `docs/contributing/koreader-synchronization.md`):

- Per-page content offsets in the section cache (`Section::getVisibleTextOffsetForPage`,
  `getVisibleTextOffsetAfterPage`, `getPageForVisibleTextOffset`), layout-independent, exact.
- `ProgressMapper::percentageFor`: the book fraction of a page in billionths (`ProgressPpb`,
  `lib/Epub/Epub/ProgressPpb.h`, #863), weighted by XHTML bytes as `Epub::calculateProgress` weighs a
  chapter. `ChapterXPathIndexerInternal::countTotalTextBytes`: a chapter's visible text bytes in one
  streamed pass, cached per spine by the forward mapper.
- `ProgressComparison::compareProgress` with the offset tier.
- The sync activity's memory pattern: map the local position before WiFi; take the radio down
  before any chapter parse that follows a GET; reconnect only when an upload follows;
  `SaxParser::Profile::Lean` for mapper parses; the per-book last-push cache (`LastPushCache.h`).
- A JSON fraction enters as an integer: `progressFromJsonFloat` (`lib/KOReaderSync/KOReaderJsonNumber.h`)
  turns ArduinoJson's float into a `ProgressPpb` without floating point.

KOSync-specific: the XPath serialisation and crengine's text-node rules in `StackState`
(`ChapterXPathIndexerState.h`, rules R1-R7 in the header of `ChapterXPathForwardMapper.cpp`).

## Findings about the two providers

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
  flash partition. Our `default` build (X3/X4): #868's body gives 6,178,901 B of the 6,553,600 B app
  slot on 2026-11-21; re-measure before relying on it.

### OPDS Progression 1.0 (Kavita, Audiobookshelf; fork syfq91/witchling-reader)

- Our OPDS parser does not recognise the progression link (`rel="http://opds-spec.org/progression"`
  or `type="application/opds-progression+json"`). witchling's implementation:
  `lib/OpdsParser` (link detection), `src/network/OpdsProgressionSync.{h,cpp}` (333 lines; the
  JSON is `{position: {progression, title, reference}, device: {id, name}, modified}`),
  `src/activities/reader/OpdsProgressionSyncActivity.cpp` (297 lines; compare and prompt),
  reader hooks (`currentSyncPosition`, push on close, pull on open, conflict prompt), per-book sync
  config saved from the catalog entry.
- witchling lands a pulled position with `jumpToPercent(progression * 100)`: one percent of the
  whole book as granularity. The position is a book fraction plus the spine href; no node path.

## Plan A: OPDS progression sync in the firmware

Port witchling's feature with attribution (source comment + commit trailer, per the porting
convention), then wire it to the position layer. Bounded-plus scope: a short design, then TDD; no spec
needed unless the reader hooks turn out to need a new activity flow.

1. **Parser:** `OpdsEntry::progressionHref` from the rel or the media type (port).
2. **Client:** `OpdsProgressionSync` GET/PUT through `SecureHttpClient`, device id/name, `modified`
   timestamp from `HalClock`; per-book config file beside `progress.bin` (port, trimmed). Add its name to
   `BookCache::isReadingState`: a clear that keeps the reading state deletes every other file
   (`BookCache::clear`).
3. **Position mapping (new, ours), in `ProgressPpb`:**
   - push: book fraction = `ProgressMapper::percentageFor(epub, spine, page, totalPages)`, written as
     the JSON number with its digits, as `KOReaderJsonNumber.h` writes KOReader's; `title` = TOC title,
     `reference` = spine href. Optional: append `#vto=<offset>` to the reference so two of our devices
     land exactly through Kavita; other clients ignore it.
   - pull: the fraction enters through `progressFromJsonFloat`; fraction → spine by inverting
     `Epub::calculateProgress` (cumulative chapter bytes), intra fraction → offset = intra ×
     `countTotalTextBytes(spine)` → `getPageForVisibleTextOffset`; if the reference carries `#vto=`, use
     that offset directly.
   - compare: `compareProgress` with the offset tier (local handoff offsets vs the mapped remote).
4. **Activity:** reuse `KOReaderSyncActivity`'s flow shape (map local before WiFi; GET; radio down
   before `countTotalTextBytes`; reconnect only for a PUT) rather than witchling's separate screen,
   or port witchling's screen if the KOSync one is too entangled. Push on close and pull on open
   follow the KOSync settings (auto-push, min pages) under a per-provider switch.
5. **Settings, i18n, docs, release note.** Settings: enable, device name; credentials come from the
   OPDS server entry already stored.
6. **Tests:** host: JSON codec, fraction↔spine inversion, fraction→offset→page on the corpus
   (round trip within one page), `#vto=` round trip exact. Device: one Kavita and one Audiobookshelf
   server, push/pull/compare, `Sync mem` troughs.
7. **Flash:** an estimate of +20 to 30 KB; measure against the budget before merging.

Effort: two to three days including reviews. Preconditions: a Kavita or Audiobookshelf server to
test against.

## Plugin route

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

Which route to take -- Plan A in the firmware, a plugin for the push with the pull in the firmware, or the
plugin alone without a pull -- is the owner's decision.

## Plan B: BookOrbit

- **B1 (one hour):** test today's KOReader sync against a BookOrbit account; document "works with
  BookOrbit" with the server URL form; release note.
- **B2 (half a day, only if B1 mis-lands):** `DomFlavour { Crengine, W3C }` on `StackState` gating
  R1-R3 and the whitespace collapse, plus a "server type" choice in the sync settings; mirror the
  rule tests for W3C.
- **B3 (a week plus flash work, separate decision):** stats, highlights/annotations, bookmarks,
  catalog from the fork. Needs a flash diet or a build variant (sync providers as variants, like the
  language packs) first, or the plugin route above for what it can carry.

## Open questions for pickup

- Which test servers exist: Kavita, Audiobookshelf, BookOrbit account?
- Firmware or plugin route (above): the owner's decision.
- Flash budget: accept +30 KB for Plan A; decide on variants before B3.
- One provider switch (KOSync / OPDS / BookOrbit) per book, or several at once?
