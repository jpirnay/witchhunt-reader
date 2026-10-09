# Library: the book index and the screen

The Library is one screen with four tabs: **Books** (folders and books), **Recent** (books lately
opened), **New** (the ten books added most recently) and **Authors** (authors, each opening on their
books). Books and Recent list what the file system and `RecentBooksStore` hold. New and Authors read a
book index that the screen builds in the background, `lib/LibraryIndex`.

This record says why the code is shaped as it is; the code is the authority on what it does. The file
layout is in [../file-formats.md](../file-formats.md), under `library.bin`. Open items are in
[../future_work/library.md](../future_work/library.md).

## Principle

The index answers *which books, in what order*. It stores **paths**, never what a row shows:
`BookRowResolver` already turns a path into a title, subtitle, progress and cover for path rows, so
every view (Files, Details, Covers) and the Options menu work on New and Authors unchanged.

Pure logic (the format, keys, join, publish, ordering) lives in `lib/LibraryIndex/` and is host-tested
over a real directory tree through the stdio storage shim. The screen hosts the build and supplies the
metadata lookup.

Why not crosspoint-reader's Library (upstream `develop` @ 50823ffa): about 3,800 lines and ~53 KB of
flash, a 128-byte-record index, a separate list activity without covers that would duplicate
`FileBrowserActivity` and `BookRowResolver`, and a Unicode fold table this firmware does not ship. Its
ideas are kept: identity reconciliation, write-then-rename, and "first seen" as the added order.

## Identity, first seen and the New order

- **Identity** is FNV-1a of the ASCII-lowercased filename, mixed with the file size
  (`LibraryKeys::bookIdentity`). A book moved to another folder, `/COMPLETED` included, keeps its
  identity and does not come back as new; so does a case-only rename. Two copies of one book share an
  identity, which is harmless: both carry the same data.
- **`firstSeen`** is the `buildGen` of the build that first saw the identity. A build that finds at
  least one new identity bumps `buildGen`; the very first build gives every book the same value.
- **New** is `firstSeen` descending, then the file date (the later of modified and created)
  descending. It does not rely on file dates alone because boards without a clock (the X4) write
  1980 dates.
- **`sidecarSig`** is a hash of the metadata sidecar's size and FAT date, from the folder listing the
  walk reads anyway. A changed signature sends the book's author back through `details.bin`, whose
  content stamp is exact. A folder with more sidecars than the walk's table holds marks its books
  `SIDECAR_UNKNOWN`: always looked up again.

## Authors

- **Where an author comes from:** the same metadata the rest of the UI uses (`BookDetailsLookup`):
  the book's primary author -- the first `dc:creator` with role `aut` or no role -- and its
  `opf:file-as`, with a `.opf` sidecar overriding both for every book format. A TXT or Markdown book
  without a sidecar has no author.
- **One author across books:** `LibraryKeys::authorHash`, FNV-1a of the folded name, so case, accents
  and spacing do not split an author. Hash 0 is "Unknown author", `0xFFFFFFFF` "Not yet indexed";
  both file after every named author.
- **Filing name, shown and sorted:** `LibraryKeys::authorFilingName` -- the file-as as written when
  any of the author's books gives one; a name already holding a comma as written; otherwise the last
  word first ("Terry Pratchett" -> "Pratchett, Terry"). The Authors list shows it, so the list reads
  in the order it is sorted; an opened author's header shows the name as the books spell it. The sort
  key is the filing name folded (`LibraryKeys::filingKey`): ASCII lowercase, Latin-1 and Latin
  Extended-A letters to their base letter, combining marks dropped (so NFD names from macOS sort with
  NFC ones), commas and full stops dropped.
- **An author's books** are listed by series, series index, then title (`LibraryOrder`), read from
  `details.bin` when the author is opened and sorted on fixed-length natural-sort-key prefixes in the
  lent framebuffer. Without the framebuffer, or past 200 books, the index's order stands.

## Building

`LibraryBuilder` is a step machine: each `step()` does one bounded piece -- a few dozen directory
entries, the join, a publish, or one book's author -- so the screen stays responsive. Every phase but
the walk needs the lent secondary framebuffer, one phase at a time. Working files live in
`/.crosspoint/library/` beside the index.

1. **Walk.** Depth first, at most 8 levels, by the Books tab's rules (books only; hidden entries only
   when shown; never `/.crosspoint`). Each folder is listed twice: first its `.opf` sidecars into a
   table of 128 per folder, then its books, staged with identity, date, sidecar signature and path.
   At most 2,000 books (`library::MAX_BOOKS`); past that the index is marked partial.
2. **Join** (`LibraryJoin`). The staged books are sorted by identity in the framebuffer (16 B each) and
   merged in one pass with the previous index, which is in identity order: same identity and
   signature carry the author and `firstSeen`; same identity, new signature keep `firstSeen` and wait
   for their author; anything else is new. The previous index's named authors are carried into the
   names file, so publish finds their names without resolving their books again.
3. **Publish** (`LibraryPublish`). Builds the author table, sorted on an 8-byte key prefix with runs
   of equal prefixes put in full-key order, places every book in its author's slot, picks New, and
   writes the file under a temporary name then renames it -- in the framebuffer, at most ~22 B per
   book (44 KB at 2,000 books, of the X4's 48 KB). New is usable from the first publish.
4. **Resolve.** Books still waiting for their author are looked up one per step (`details.bin`, else
   a metadata-only parse in the framebuffer), patched into the working records and appended to the
   names file. Publish runs again every 100 books and at the end, so Authors fills in as it goes. An
   interrupted build loses no lookup: every answer is in `details.bin`, one small read each when the
   next build runs (which needs a reason from "When it rebuilds").

## When it rebuilds

The index lives on the card it describes, so it is kept across boots and wakes -- a different card
brings its own -- and built again only for a reason (`LibraryStaleness::rebuildNeeded`, checked by
`LibraryFreshness::stale` on entering New or Authors, never on Books or Recent):

- **No valid index**: missing, another format version, or sections that do not fit the file.
- **The other *Show Hidden Files* setting**: it lists other books (`acceptRules` in the header).
- **The card changed since the last finished build**, in this boot or an earlier one. Every change the
  firmware makes outside its cache folder goes through `HalStorage::noteContentChange`: web and WebDAV
  uploads, deletes and renames, Calibre wireless, OPDS downloads, the metadata editor's sidecars,
  Move, Remove and New folder on the device, finished books moved to `/COMPLETED`, and a USB Drive
  session (recorded at its start, while the card is still the firmware's to write: the host may change
  anything and the power may go first). The first such change after the mark leaves a marker file,
  `/.crosspoint/content-changed`; a build that finishes with no change since it started removes it
  (`HalStorage::markContentSeen`). The servers end with a restart, which the marker survives.
- **A change the firmware did not see**: a card edited in a computer's card reader. When the Books tab
  lists a book or folder dated later than the header's `newestDate` -- the newest book or folder the
  walk saw -- it is not in the index, and is counted as a change (`LibraryFreshness::checkListedEntry`,
  `HalStorage::noteFoundChange`). Folders count too, so a folder made on the device after its books is
  no false alarm. Not checked below the walk's depth, nor against a partial index. A copy that keeps an
  old date, or a book removed where the firmware did not see it, is not caught this way: New drops a
  missing book on load, and **Refresh library** (Options on New and Authors,
  `FileBrowserActivity::refreshLibrary`) builds the index again whatever the marker says, resolving
  every author anew (`LibraryBuilder::Config::resolveAll`). Each book keeps its `firstSeen`, so New
  keeps its order; a build already running starts over.

While a build runs the previous index stays on screen and the header says *Indexing*, then *Indexing
n/m* while authors are read. A build steps only while New or Authors is showing; on Books and Recent it
waits, and it goes on when one of them is shown again.

## Memory

| What | Where | Size |
|---|---|---|
| The index while browsing | One open file and its header; rows read as drawn | < 200 B |
| An opened author | Record indices in display order | 2 B a book |
| Walk | The folders on the path (≤ 8), each with its sidecars: 8 B each, ≤ 128 | ≤ 1 KB heap a level, usually far less |
| Join, publish, ordering an author | Lent framebuffer, one at a time | ≤ 44 KB at 2,000 books |
| A book's metadata parse | Lent framebuffer (as for titles and covers) | ≤ 32 KB |
| Working files | SD | 40 B a book (staged and joined records), plus paths and author names |

The screen gives the lent framebuffer to one job at a time: row titles first, then a build step, then
covers. A build that cannot borrow it is abandoned, not marked built, and tried on a later visit
while a reason to rebuild holds.

## Failure handling

| Case | Behaviour |
|---|---|
| Index missing, wrong version or truncated | Treated as missing; a build starts |
| A write fails | The index last published stays; the build fails, and runs again on a later visit while a reason to rebuild holds |
| Out of memory | Nothrow allocations; the step fails and the old index stays |
| Power lost mid-build | The old or the new index, never a broken one (rename); working files are rewritten next time |
| A press during a build | The build yields to input between steps |
| A book gone since the index was built | New drops it on load (one existence check per row) |

## The screen

- **Tabs inside the file browser.** `FileBrowserActivity` hosts all four tabs in its library modes
  (`FileBrowserModel::Mode` Books, Recents, Added, Authors) rather than moving onto
  `TabbedUiListActivity`: the browser reads its own keys (`navigateButtons`, `handleCustomInput`) and
  never runs `ListController`, where that class keeps its tab-bar focus. So there is no bar focus: a
  long Up/Down or a tap on a tab switches tab, and the side hints say *Up / Tab* and *Down / Tab*.
  Back on Recent, New and the Authors list goes Home; in a Books folder it goes up (ending a search
  first), and in an opened author back to the Authors list, on that author. A long Back goes Home from
  any tab. The bar itself is `ListTabBar`, shared with the tabbed screens.
- **Each tab is left where it was**: its row, the Books tab its folder, the Authors tab the author open
  in it. Home opens the tab last left (`APP_STATE.libraryTab`); each tab has its own view.
- **Returns.** `ReturnTo::Library` carries the tab and an open author's hash, so closing a book comes
  back to the tab, folder or author, and row it was opened from (`ActivityManager::goToLibrary`).
- **Options** (`FileContextMenuActivity::ListSource`): New and Authors have no sort, hidden-files
  toggle, search, Move to folder or New folder; they offer Go to folder and Refresh library, and Remove
  there deletes the book (with its sidecars).
- **Books leaves out folders with no book below them** (`FolderSearch`), so folders emptied by a sync,
  or holding only covers, do not clutter it; All Files and the Move-to-folder picker list them. The
  check stops at the first book; folders found empty go into the folder counts as 0 until the card
  changes. The SD folder index applies the same rule in its staleness scan, so a folder that gains a
  book comes back. *New folder* in Books opens the new folder.
- **Covers under the tab bar** (`CoverGridLayout::place`): a 4 px gap under the bar instead of the
  theme's spacing, and no reserve at the bottom, keep the X3 and X4 at full-size cells -- the
  thumbnails already made, shared with the finished-book screen -- at every theme and UI font size.
  Height the cells leave over goes to the rows equally, and where each row's share holds another line
  the label gets a third: title, author, series.
