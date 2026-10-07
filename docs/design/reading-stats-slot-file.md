# Reading stats in a fixed-slot file — design

> **Status.** Implemented (PR #353). `src/ReadingStatsSlotFile.h` is the layout layer and
> `src/ReadingStats.h` (`ReadingStatsStore`) the store on top of it. This record also carries the
> design of the streamed store it replaced the file layer of (no resident history, bounded reads,
> books off the card), which is still how the store behaves.

## Problem

**Memory first.** Every on-device consumer of the history used to load all of it: the whole
`reading-stats.json` parsed into an ArduinoJson document, then copied into per-book vectors.
Measured on the X3: ~1.5 KB per book transient at load (18 books cost a ~27 KB transient) and
~420 B per book resident while a screen held it. The tightest caller is the session end, which
loads, merges one book and saves on reader exit, after the reader's teardown: 43 556 B free and
23 540 B contiguous. The load stopped fitting at roughly 25 to 30 books, the store stayed unloaded,
the save refused to overwrite, and the session that had just ended was silently lost.

Nothing on the device needs the whole history at once. A book screen needs one book, Home its
recent books (at most 10, `MAX_RECENT_BOOKS` in RecentBooksStore) and the global pace, the summary a few numbers per book, the list one page.
So the store stopped holding the history (section 8) and became stateless over the file.

**Then time.** The first streamed form fixed memory (no call holds more than a few KB, whatever the history
holds) but not time, because a single JSON file can only be updated by
rewriting all of it. Measured on the X3 with the worst-case history (100 books × 60 day buckets,
108 KB):

- SD writes cost **~17.7 ms per KB** (~56 KB/s), however the writes are split (1 KB and 8 KB
  chunks measured the same); reads cost ~2.1 ms per KB per pass.
- A session end takes **2.7 s**: scan 228–241 ms, copy ~2.2 s (of which ~1.9 s in the card),
  read-back 230 ms, swap 22 ms. The real 16-book history takes 167 ms.
- It is paid at reader exit, and on a sleep from the reader *before the sleep screen paints*:
  `goToSleep()` replaces the reader, whose `onExit()` ends the session.

A deferred commit was designed and dropped: record the session in a small pending file, apply it in
the background later. It needed a background task sliced across loop iterations, rules for when to
abort it, a pre-sleep hook, a cache that folds in pending sessions, and exactly-once replay. Too
many parts that can each go wrong.

The time goes into rewriting 99 books that did not change. A file with a fixed place for each
book removes that.

## Goals

- **An update writes only what changed**: one book record and one fixed-size block of global
  figures, whatever the history holds. Target: under 0.3 s worst case on the X3 (measured before
  the gate was a timing build on both boards before the rest was built).
- **Every update is atomic.** Power lost at any point loses at most the update in progress; it
  never corrupts the history.
- **Memory stays bounded and independent of the history**: a fixed transient per call, nothing
  resident beyond Home's few snapshots.
- **Every screen and the web show what they show today**, from the same arithmetic.
- **JSON stays the interchange format**: the web export is today's file format, which older
  firmware reads.

## Non-goals

- **No padded, fixed-width JSON.** It would keep the format, but a torn in-place write breaks the
  JSON of the whole file, and removing a book shifts bytes.
- **No journal or write-ahead log.** Copy-on-write (section 3) makes every update atomic without
  replay or recovery code.
- **No restore from a JSON file** after the one-time migration. A JSON placed on the card later is
  ignored while the binary file exists.
- **No change to the caps** (100 books, 60 days per book, 400 global days) or to any arithmetic.
- **No change of book key.** The history stays keyed by the MD5 of the basename, which the
  existing file holds and a move between folders keeps. Two other hashes exist and were weighed:
  - the cache-folder hash (`std::hash` of the full path, in `Epub.h`, `Xtc.h`, `Txt.cpp`) changes
    when a book moves, e.g. to `/COMPLETED`, and `std::hash` is not guaranteed stable across
    toolchain versions;
  - the KOReader content hash (`KOReaderDocumentId::calculate()`) needs the book opened, which
    Home cannot afford for its recent list;
  - neither can be derived from the existing history, which stores no path.
- **No `path` field** (a follow-up, see section 9; a slot has room for it).

## Design

### 1. File layout

One file, `/.crosspoint/reading-stats.bin`, created at its full size and never resized:

| Region | Offset | Size | Contents |
|---|---|---|---|
| Meta copy A | 0 | 6 144 B | header, directory, global days, CRC |
| Meta copy B | 6 144 | 6 144 B | the same, one generation apart |
| Book slots | 12 288 | 101 × 1 024 B | one book each; one more slot than the cap |

**115 712 bytes** in all. Every region starts on a 512-byte sector boundary.

**Meta copy** (6 036 bytes used):

- *Header* (32 B): magic `RSTB`, format version, slot count (101), **seq** (the copy's
  generation), totalSeconds, totalSessions, totalPagesTurned, longestStreak, number of books,
  number of global day buckets.
- *Directory* (100 × 36 B), one entry per book, holding what everything but the book screens
  needs:
  - docId (16 bytes);
  - slot number (`0xFF` = entry free);
  - dated-day count;
  - progress;
  - finishedCount;
  - totalSeconds;
  - lastReadEpoch.
- *Global days* (400 × 6 B): day index and seconds, ascending.
- *CRC32* over everything before it (`uzlib_crc32`, already in `lib/uzlib`).

**Book slot** (904 of 1 024 bytes used):

- docId;
- totalSeconds, pagesTurned, sessions;
- firstReadEpoch, lastReadEpoch, lastFinishedEpoch;
- finishedCount, progress;
- day count and up to 60 day buckets;
- title (up to 320 bytes) and author (up to 160 bytes), each stored with its length;
- CRC32 at the end of the slot.

**Encoding:**

- Little-endian fields, written and read field by field; no struct `memcpy`, so no padding or
  compiler layout reaches the card.
- Epochs are 64-bit.
- The docId is the key the history has always used,
  `KOReaderDocumentId::calculateFromFilename()`: the MD5 of the file's basename. It is stored as
  the 16 bytes its 32 lowercase hex characters encode.
- Strings are raw UTF-8, cut at a character boundary when over their limit.
- There is no JSON escaping anywhere in the file.

### 2. Reading

**Loading the meta.** Read the two 32-byte headers and try the copy with the higher seq first: read
it whole and check magic, version and CRC. If it fails, try the other copy. If neither is valid,
the file is corrupt (section 4). Nothing stays resident between calls.

| Consumer | Reads | Estimated time |
|---|---|---|
| Home (history line, badge, ETA) | meta | ~15 ms |
| Stats summary (totals, streaks, chart, counts, pace, top 3) | meta + 3 slots | ~20 ms |
| All-books list | meta for the order; one slot per visible row | ~15 ms + ~8 slots a page |
| Book Info, book stats screen | meta + 1 slot | ~20 ms |
| Web dashboard, export | meta + every used slot | ~0.3 s at 100 books |

The meta alone answers every aggregate: book count, finished count (entries with finishedCount
> 0), the global pace sums (`countsTowardPace` needs only progress and seconds), the time order,
and the eviction victim (`evictsBefore` needs only lastReadEpoch and seconds).

- **Home.** `prefetchRecent()` reads the meta on every Home entry and fills its ≤ 10 snapshots (one per recent book) and
  the global pace. The write-through cache logic goes: a 15 ms read on entry cannot be stale.
- **The list.** It sorts the directory by time and keeps each book's docId, plus the meta's seq it
  was read at. A page of rows loads the meta once. A different seq means the file changed under
  the list, and the list rebuilds its order; otherwise each row is read through the directory.
  Checking a row's docId would not be enough: after an update, the book's old slot still holds a
  valid copy of the same book.
- **A slot is trusted only if** its CRC holds and its docId matches the directory entry. Otherwise
  the book reads as its directory figures with no title (shown as its docId) and no days, and the
  failure is logged. The book's next update writes a whole new slot, which repairs it.

### 3. Writing: copy-on-write

**A slot in use is never overwritten, and neither is the valid newest meta copy.**

Record session and mark finished:

1. Load the meta; read the book's slot if it has one.
2. Apply the existing arithmetic (`applySession` / `applyFinish`, the trims) to the book and to
   the totals and global days decoded from the meta.
3. Choose the **free slot** with the lowest number, i.e. one no directory entry of the loaded
   meta refers to. Write the book into it, then flush. With 100 books there is exactly one.
4. Update the meta in memory:
   - the entry points at the new slot, with the new figures;
   - a new book takes a free entry. At the cap, the eviction victim's entry is freed first, and
     with it the victim's slot;
   - totals and global days change;
   - seq goes up by one.
5. Write the meta into the **other** copy (the older one), then flush. The book's old slot is now
   free.

**Remove:** load the meta, read the book's slot (its days leave the global days through
`takeOut`), free the entry, update the totals and write the meta into the other copy.

**Where power can fail:**

| Failure point | On the next load | Outcome |
|---|---|---|
| During or after the slot write (step 3) | The loaded meta is still valid and newest; it never referred to that slot | The update is lost; the history is unchanged |
| During the meta write (step 5) | That copy fails its CRC; the copy the update started from is used | The update is lost; the history is unchanged |
| After the meta write | The new copy is valid and newest | The update is applied |

Falling back one generation is always safe. The slot an update writes was free in the meta that
update started from, and that copy is not touched until the next update.

**Write volume per update:** one 1 KB slot plus one 6 KB meta copy, i.e. ~7 KB instead of the
whole file. At the streamed rate that is ~0.12 s.

### 4. First use, migration, corruption

On the first store call after the update:

**`reading-stats.bin` exists.** It is used. Any `reading-stats.json` is ignored.

**Only `reading-stats.json` exists.** Import once:

1. Stream the JSON with the existing scanner (`scan` with the time index, then `readBookAt` per
   book; bounded memory).
2. Write every book into slots 0…n−1 of **`reading-stats.bin.tmp`**, written at full size, then
   meta copy A (seq 1). Copy B stays invalid.
3. Flush, and read the meta back.
4. Rename the tmp file to `reading-stats.bin`, then rename the JSON to
   **`reading-stats.json.imported`**.

Details:

- A crash before step 4 leaves only a tmp file, which the next call discards before importing again.
- Titles and authors over the limits are cut.
- A hand-made file with more than 100 books keeps the 100 the cap would keep.
- A docId that is not 32 hex characters is skipped and logged. This firmware never writes one.
- A malformed JSON is set aside as `reading-stats.corrupt.json`, as today, and the history starts
  empty.
- One-time cost at the worst case: ~0.5 s reading plus ~2 s writing 113 KB. Paid by whichever call
  comes first, usually Home's prefetch on the first boot after the update.

**Neither file exists.** The first update creates the file: full size, zeroed slots, meta A.

**Both meta copies are invalid.** The file is set aside as `reading-stats.corrupt.bin` and the
history starts empty. This mirrors the malformed-JSON rule.

**Downgrade.** Older firmware finds no `reading-stats.json` and starts an empty history. There are
two ways back:

- the web export, saved to the card as `reading-stats.json`;
- `reading-stats.json.imported` renamed back, which holds the history as of the import.

Re-upgrading with the `.bin` still present ignores whatever JSON the older firmware wrote.

### 5. Web

The endpoints, their requests and their responses stay as they are, and the stats page's script
does not change. Only where the handlers get their data changes.

- **`/api/stats`** — the same payload as today, generated: the figures, streaks and global days
  from the meta, then each book as `writeBook` renders it plus its `etaSeconds`, streamed to the
  response one slot at a time.
- **Export** — today's file format, generated the same way (header, global days, books via
  `writeBook`) and served as `reading-stats.json`. Older firmware can read it, which makes it the
  backup and the downgrade path. Two things change:
  - it is sent chunked like `/api/stats`, with no Content-Length, because it is generated rather
    than copied from the card;
  - titles and authors over the new limits come out cut.

  Otherwise it is identical to what the device wrote before. A card with no history still answers
  404.
- **Remove** — `READING_STATS.removeBook()`, as today.
- **Threads.** The handlers run on the loop task (`handleClient()` is called from the activity's
  `loop()`), like every other store call.
- **Memory** while Wi-Fi is up: the meta and one slot. That is fixed and never grows with the
  number of books, which keeps the rule that web handlers stream rather than load.

### 6. Code structure

- **`ReadingStatsSlotFile`** (new) — the layout, and nothing else:
  - encoding and decoding of the header, directory entries, day arrays and slots;
  - CRC checks;
  - choosing the valid newest meta copy and the free slot;
  - reads and writes at fixed offsets through `HalFile` (`Storage.openFileForUpdate`);
  - creating the zeroed file.

  Pure functions over byte buffers where possible, so the host tests need no card.
- **`ReadingStatsStore`** — the same public API:
  - updates with `WriteResult`;
  - `querySummary`, `queryBook`, and the book-by-position query, which becomes a query by slot
    checked against a docId;
  - `prefetchRecent` / `recent` / `recentPooledPace`;
  - every arithmetic static.

  Its internals are rewritten on the slot file, and the import lives here.
- **`ReadingStatsFile` becomes `ReadingStatsJson`**:
  - it keeps what the import and the web need: the streamed scanner, `readBookAt`, `writeBook`, and
    writers for the dashboard and export heads;
  - it loses the rewrite (`Rewrite`, `writeRewrite`, `writeWithoutTarget`, the copy passes);
  - in the store, the swap, interrupted-swap recovery and write timing go too;
  - the query result types (`Summary`, the index entry, `RecentSnapshot`, the result enum) move to
    `ReadingStatsTypes.h`, and the index entry holds a slot and a docId instead of a byte offset.
- **Consumers** change only where the index entry changed (the list, the summary's top 3) and in
  the web handlers.

### 7. Costs

- **Memory per call:** ~10 KB peak, with no block over ~3.6 KB when the meta image is held as two
  blocks (directory, and header plus days):
  - the meta image, 6 KB;
  - the global days decoded for the arithmetic, ~3.2 KB;
  - one book, ≤ ~1.5 KB.

  Resident: Home's snapshots, ~0.7 KB. The reader exit, the tightest place, measured 43 KB free
  with 23 KB contiguous.
- **Time:** an update reads ~7 KB and writes ~7 KB. Estimated at 0.1 to 0.2 s. Reads are
  in section 2.
- **SD wear:** ~7 KB per update instead of the whole file. The two meta copies alternate, and the
  card's own wear levelling spreads the rest.
- **Flash:** the binary layer and the import come; the rewrite and copy passes go. Expected
  roughly neutral; measured against the base branch.

### 8. No resident history

`ReadingStatsStore` holds no history: no per-book vector, no global day vector, no load/release
state. Every call reads what it needs from the file and returns it; a screen keeps its own small copy.
The one resident piece is Home's set of snapshots (at most 10 books, ~0.6 KB): per book the total
seconds, dated-day count, last-read epoch, progress and the book's own pace, plus the global pace
for books too new to have their own. Home calls `prefetchRecent()` on entry; the themes read the
snapshots through `BookProgressPresentation`, and no theme `render()` touches the card. The only
way the file changes behind the device's back is USB drive mode, which always leaves by rebooting.

A load that cannot get its memory drops that one update and logs it; the file stays as it was. A
scan or read reports Ok, NoMemory or Malformed, so the store can tell the transient case from the
permanent one.

### 9. Books no longer on the card

**Deleting a book from the card never touches its reading history.** Only *Remove from stats* does.
A book that was read and then deleted was still read: its entry, figures and share of the totals and
streaks stay. Neither delete path (the device file browser, the web file manager) touches the stats
file.

Entries are keyed by `KOReaderDocumentId::calculateFromFilename()`, the MD5 of the basename; no
path is stored and nothing checks an entry against the card. So:

- a deleted book's entry stays and is listed like any other; *Remove from stats* works on the docId
  alone and never opens the book;
- moving a book between folders (for example to `/COMPLETED`) keeps its history, renaming it starts
  a new entry and leaves the old one as history;
- such entries hold a slot under the cap and leave only by least-recently-read eviction or removal;
- nothing tells the user which books are gone; without a path, finding out means walking the card
  and hashing every filename.

A possible follow-up is an additive `path` field (the last path the book was read from) so the list
could mark "not on card". It is display only and would not change eviction.

### 10. Testing

`test/reading_stats` covers the layer (round trips at the limits, UTF-8 cuts, CRC failures, choice of
the newest valid copy, free-slot choice) and the store on the file-backed `HalStorage` shim: a write
stopped at each failure point in section 3 leaves the old history, 300 updates on a full file leak
no slot, eviction reuses the victim's slot, and importing a JSON this firmware wrote and exporting it
again gives the same bytes. `scripts/gen_reading_stats_history.py` makes the worst-case history for
device measurements.

## Open questions

None blocking.

- **The `path` field** (section 9) — a slot has 120 spare bytes. Adding it later means a format version bump,
  handled inside this format (older slots read it as absent), not another migration.
