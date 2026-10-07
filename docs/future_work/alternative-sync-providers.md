# Alternative sync providers (OPDS progression, BookOrbit)

Status: **shelved 2026-10-07**, not started. Written so the work can be picked up without redoing the analysis.
Related: `docs/superpowers/specs/2026-10-06-kosync-content-offset-design.md` (the position layer
this builds on), PR #398 (merged via #399).

## What is already in place (master, 2026-10-07)

Protocol-free, usable by any sync provider:

- Per-page content offsets in the section cache (`Section::getVisibleTextOffsetForPage`,
  `getVisibleTextOffsetAfterPage`, `getPageForVisibleTextOffset`), layout-independent, exact.
- `ProgressMapper::percentageFor` (byte-based fraction of the book for a page) and
  `countTotalTextBytes` (one streamed pass over a chapter, cached per spine).
- `ProgressComparison::compareProgress` with the offset tier.
- The sync activity's memory pattern: map the local position before WiFi; take the radio down
  before any chapter parse that follows a GET; reconnect only when an upload follows;
  `SaxParser::Profile::Lean` for mapper parses; the per-book last-push cache (`LastPushCache.h`).

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
  flash partition; we are at 94.9 % (about 336 KB free).

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

## Plan A: OPDS progression sync in witchhunt-reader (recommended first)

Port witchling's feature with attribution (source comment + commit trailer, per the porting
convention), then wire it to the position layer. Bounded-plus scope: a short design in chat, then
TDD; no spec needed unless the reader hooks turn out to need a new activity flow.

1. **Parser:** `OpdsEntry::progressionHref` from the rel or the media type (port).
2. **Client:** `OpdsProgressionSync` GET/PUT through `SecureHttpClient`, device id/name, `modified`
   timestamp from `HalClock`; per-book config file beside `progress.bin` (port, trimmed).
3. **Position mapping (new, ours):**
   - push: book fraction = `ProgressMapper::percentageFor(epub, spine, page, totalPages)` (bytes),
     `title` = TOC title, `reference` = spine href. Optional: append `#vto=<offset>` to the
     reference so two Witch Hunt devices land exactly through Kavita; other clients ignore it.
   - pull: fraction → spine by inverting `Epub::calculateProgress` (cumulative chapter bytes), intra
     fraction → offset = intra × `countTotalTextBytes(spine)` → `getPageForVisibleTextOffset`; if the
     reference carries `#vto=`, use that offset directly.
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
7. **Flash:** expect +20 to 30 KB; check against the 94.9 % budget before merging.

Effort: two to three days including reviews. Preconditions: a Kavita or Audiobookshelf server to
test against.

## Plan B: BookOrbit

- **B1 (one hour):** test today's KOReader sync against a BookOrbit account; document "works with
  BookOrbit" with the server URL form; release note.
- **B2 (half a day, only if B1 mis-lands):** `DomFlavour { Crengine, W3C }` on `StackState` gating
  R1-R3 and the whitespace collapse, plus a "server type" choice in the sync settings; mirror the
  rule tests for W3C.
- **B3 (a week plus flash work, separate decision):** stats, highlights/annotations, bookmarks,
  catalog from the fork. Needs a flash diet or a build variant (sync providers as variants, like the
  language packs) first.

## Open questions for pickup

- Which test servers exist: Kavita, Audiobookshelf, BookOrbit account?
- Flash budget: accept +30 KB for Plan A; decide on variants before B3.
- One provider switch (KOSync / OPDS / BookOrbit) per book, or several at once?
