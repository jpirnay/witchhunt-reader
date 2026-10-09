# Library view: one screen, four sources

Status: design approved in brainstorming on 2026-10-09; awaiting spec review.
Working material: once the work merges, move the lasting parts to `docs/design/library-index.md` and
`docs/file-formats.md`, then delete this file (see the docs layout convention).

Items marked ⚑ were settled while writing this spec rather than discussed. Check them in review.

## 1. Goal

Browse Files and Recent Books become one **Library** screen with four tabs:

| Tab | Rows | Source |
|---|---|---|
| **Books** | Folders and books, as Browse Files shows them today | Directory listing (`FileBrowserModel::Mode::Books`) |
| **Recent** | Books recently opened | `RecentBooksStore` (`Mode::Recents`) |
| **New** | The 10 books most recently added to the card | Book index (new) |
| **Authors** | Authors; opening one lists their books | Book index (new) |

Constraints:
- No resident RAM growth.
- Large temporary buffers only in the lent secondary framebuffer.
- A flash budget of 8–15 KB, measured.

`SCOPE.md` lists library management as in scope.

**Out of scope:**
- An A–Z title view, and metadata search
- A Series tab
- Separate entries for co-authors
- Treating spelling variants as one author ("J.R.R." vs "J. R. R.")
- Guessing authors from filenames or folder names
- Bumping `book.bin`'s version

## 2. Background

- **Upstream comparison.** crosspoint-reader's Library (upstream `develop` @ 50823ffa) was evaluated.
  - It is about 3,800 lines: a 128 B-record index `CLX1` and a separate list activity without covers.
  - Its activity would duplicate `FileBrowserActivity` + `BookRowResolver`. Their index needs a Unicode fold table we don't ship.
  - We take ideas from it: identity reconciliation, staged write-then-rename, and `firstSeen` as the "added" order.
- **Role models.** Kobo, Kindle, Boox and KOReader all keep *which books* (tabs/sources), *what order* (sort) and *how drawn* (list/grid) separate.
  - We already have the second (Options → Sort, for folders) and the third (Files/Details/Covers per source).
  - This work adds the first.

## 3. The book index

### 3.1 Principle

The index answers *which books, in what order* and stores **paths**. It never stores what a row
shows. `BookRowResolver` already turns a path into title, subtitle, progress and cover for path
rows (Recents), so every view and the context menu work unchanged.

Pure logic lives in `lib/LibraryIndex/` and is host-tested: the format, author keys, the join and the
New ordering. The card walk and metadata lookups live in `src/`, behind the HAL.

### 3.2 File: `/.crosspoint/library.bin`

It is written to a temp file and renamed into place. All integers are little-endian.

| Section | Layout |
|---|---|
| Header | Magic `WLIB`; version; flags (bit0 = partial); `buildGen` `u32`; `acceptRules` `u8` ⚑ (the `showHiddenFiles` value the walk used); `bookCount` `u16`; `authorCount` `u16`; section offsets |
| Records | `bookCount` × 24 B, **sorted by identity**: `identity` `u32`, `authorHash` `u32`, `date` `u32`, `pathOff` `u32`, `sidecarSig` `u32`, `firstSeen` `u32` |
| New | 10 × `u16` record indices (fewer when the card holds fewer books) |
| Authors | `authorCount` × 12 B, **sorted by author key**: `hash` `u32`, `nameOff` `u32`, `firstBook` `u16`, `count` `u16` |
| Author books ⚑ | `bookCount` × `u16` record indices, grouped by author. An author's books are `[firstBook, firstBook + count)` |
| Blob | Paths, author display names, author sort keys |

Reserved `authorHash` values:
- `0`: no author. Grouped under "Unknown author", sorted last.
- `0xFFFFFFFF`: not yet resolved. Grouped under "Not yet indexed (n)", after Unknown.

Cap: **4,000 books**. Beyond that the walk stops, the partial flag is set, and the Authors tab
shows "Library too large" (New still works over the books indexed).

### 3.3 Identity, added order, sidecar signature

- **`identity`** is FNV-1a of the case-folded ⚑ filename, mixed with the file size.
  - A book moved to another folder (including `/COMPLETED`) keeps its identity, so it does not reappear as new.
  - A case-only rename does not make it new either.
  - Two copies of one book (same name and size) share an identity. That is harmless: both carry the same data ⚑.
- **`firstSeen`** is the `buildGen` of the build that first saw the identity.
  - `buildGen` goes up by one in every build that finds at least one new identity.
  - On the very first build every book has the same `firstSeen`.
- **`date`** is `max(mtime, ctime)` as a packed FAT date/time. It is the same signal `FileIndex` uses.
- **New order:** `firstSeen` descending, then `date` descending. It is computed at publish time with a 10-element insertion.
  - This stays correct on boards with no RTC (X4), where device-written files may carry 1980 dates.
- **`sidecarSig`** is a hash of the metadata sidecar's size and FAT date, taken from the folder listing the walk already reads.
  - `0` means no sidecar.
  - `SIG_UNKNOWN` ⚑ means the folder's sidecar table overflowed (§3.5). The book then always goes through `details.bin`, whose content-stamp check is exact.

### 3.4 Author resolution

The author comes from the same metadata path the rest of the UI uses, extended as follows.

- **`BookDetails` gains `primaryAuthor` and `authorSort`.** `details.bin` goes from version 1 to 2, so a version 1 file is parsed again once.
  - `ContentOpfParser` keeps the joined display `author` exactly as today ("A, B" for every `dc:creator`).
  - It also records the **first `dc:creator` whose role is `aut` or has no role**, as `primaryAuthor`, together with that creator's `opf:file-as`, as `authorSort`.
- **Sidecar wins.** `Epub::applyMetadataSidecar()` already overlays the sidecar's fields. It now overlays `primaryAuthor` and `authorSort` too.
  - **Already-opened books** ⚑ (changed while planning): `book.bin` stores neither `primaryAuthor` nor `authorSort`.
    - `BookDetailsLookup::parse()` therefore always does the metadata-only OPF parse and skips the `book.bin` fast path (`loadForMetadata(scratch, /*useBookBin=*/false)`).
    - Each book pays the ~300 ms once; `details.bin` keeps the answer after that.
    - The `book.bin` version stays as it is.
- **TXT / MD / XTC:**
  - The sidecar overlay is lifted out of `Epub` into a free function: one `ContentOpfParser` pass over a file of at most 16 KB, with no ZIP. `BookDetailsLookup` applies it to every book type.
  - XTC also reads its header (`Xtc::getTitle()`/`getAuthor()`).
  - A TXT or MD book without a sidecar has no author.
  - **Side effect, intended:** Browse Files' Details and Covers views show sidecar and XTC-header metadata for these formats instead of filenames.
- **An EPUB never opened, with no sidecar:** `BookDetailsLookup::parse()` does a metadata-only OPF parse (~300 ms; inflate ring of up to 32 KB in the lent framebuffer) and writes `details.bin`. A failed parse is not recorded and is retried by the next build.
- **Author identity:** `authorHash` is FNV-1a of the folded display name (`primaryAuthor`).
- **Author key**, the sort order:
  - If any of the author's books supplies `authorSort` (`file-as`), that is the key.
  - Otherwise, a name that contains a comma is used as it is.
  - Otherwise the last word moves to the front ("Terry Pratchett" → "Pratchett Terry").
  - The key is then folded: ASCII lowercase; Latin-1 and Latin Extended-A letters mapped to their base letter (a table of about 0.5 KB); combining marks U+0300–U+036F skipped, so NFD names from macOS sort like NFC ones.
  - Names are **displayed** as the book spells them.

### 3.5 Build pipeline

`LibraryIndexBuilder` is a step machine. `step()` does one bounded chunk: some directory entries, one
resolve, or one sort phase. It lives only while a build runs. The lent framebuffer goes to one phase at a time.

1. **Walk.**
   - Depth-first with an explicit stack, at most 8 levels deep (as `countBooksBelow` does).
   - It uses Browse Files' acceptance rules: books only, and dot entries only when `showHiddenFiles` is on. It always skips `/.crosspoint` ⚑.
   - Each folder is listed twice:
     - **Pass A** puts its `.opf`/`.OPF` sidecars into a fixed table of 128 × {stem hash, signature}, about 1 KB ⚑. Overflow marks that folder's books `SIG_UNKNOWN`.
     - **Pass B** stages each book as `{identity, date, sidecarSig, pathOff}` plus its path, in stage files on SD.
2. **Join.**
   - Load the staged identities (8 B each) into the lent framebuffer and sort them. At 4,000 books that is 32 KB.
   - Merge them in one sequential pass against the previous index's records, which are already in identity order:
     - **Identity and `sidecarSig` both match:** carry `authorHash` and `firstSeen`.
     - **Identity matches, signature differs:** keep `firstSeen`; set `authorHash` to "not yet resolved".
     - **No match:** `firstSeen = buildGen` (bumped); "not yet resolved".
   - Write the new records in identity order.
3. **Publish.**
   - Rebuild the author table. Names and sort keys come from the previous index's table by hash, and from the resolve phase for new authors.
   - Sort the authors by key, and the (author, record) pairs, in the lent framebuffer. At 4,000 books the pairs take 16 KB.
   - Compute New. Write the file to a temp name and rename it.
   - **New is usable from here on.**
4. **Resolve.**
   - Records still "not yet resolved" are resolved one at a time, through `BookDetailsLookup::cached()` and then `parse()`. A parse gets the lent framebuffer to itself.
   - Results go to a resolve stage file, never into the published index, which only ever changes by rename:
     - `{record index, authorHash}` per book;
     - `{hash, display name, sort key}` per author not seen before.
   - Phase 3 runs again after every 100 resolved books ⚑ and at the end, merging that stage file in, so Authors fills in progressively.
   - **Interrupted resolves lose nothing:** every result is in `details.bin`, so the next build picks them up with one small read each.

**Refresh library** (Options, on the New and Authors tabs) runs the same pipeline without carrying anything over. Every book is resolved again through `details.bin`'s content stamps. This covers the one known gap: a sidecar edit that keeps the same length, written while the clock was unset.

### 3.6 When it rebuilds

Entering the New or Authors tab starts a build when any of these holds:
- the file is missing or invalid;
- no build has completed during this boot ⚑ (a RAM flag; a wake from sleep is a boot);
- `HalStorage::contentGeneration()` has changed since the RAM stamp taken at the last build;
- the header's `acceptRules` differs from the current `showHiddenFiles`.

The Books and Recent tabs never trigger a build. While a build runs, the previous index stays on
screen and the footer shows "Indexing %d/%d".

### 3.7 Memory budget

| What | Where | Size |
|---|---|---|
| Index while browsing | One open file and the header; rows are read a window at a time | < 200 B resident |
| An opened author | Record indices in display order | 2 B × that author's books |
| Walk | Explicit stack of ≤ 8 open directories with their paths; sidecar table | ~1.5 KB heap |
| Join sort | Lent framebuffer | 8 B × books (32 KB at the cap) |
| Publish sorts | Lent framebuffer | 4 B × books + ~20 B × authors |
| OPF parse inflate ring | Lent framebuffer (as today) | ≤ 32 KB |
| Stage files | SD | ~16 B × books + paths |

Phases never hold the framebuffer at the same time. Outside it, a build may use about 2 KB of heap.

### 3.8 Failure handling

| Case | Behaviour |
|---|---|
| Index missing, wrong version, or truncated | Treated as missing; a rebuild starts |
| Card full or write error | Old index kept; logged; message shown once; stage files cleared at the next build |
| Out of memory | Nothrow allocations; the phase stops and the old index is kept |
| Power lost mid-build | Old or new index, never a corrupt one; leftover stage files removed |
| Input during a build | `CooperativeAbort` yields; the walk restarts later; `details.bin` results are kept |
| An indexed book has since gone missing | Its row is dropped at load (one existence check per row) |

## 4. Model and activity integration

- **`FileBrowserModel` gains two modes.**
  - **`Mode::Added`** lists the index's New paths. Its rows are paths, as in Recents (`listsPaths()` is true).
  - **`Mode::Authors`** is a one-level tree:
    - At the top, each row is an author, rendered like a folder: the display name with the book count as the row's value. In Covers view it's a folder card with the count.
    - Opening an author lists their books as path rows, ordered by series, series index, then title.
      - These come from `details.bin` when the author is opened: one small read per book.
      - The model holds only the author's record indices (2 B each) in that order. Paths are read from the index for the rows in the window, so a prolific author costs no more RAM than a short list.
    - Author rows are keyed by their position in the author table, not by name, so a "/" in a name is safe. "Go up" in this mode is a model call rather than path surgery in the activity.
- **Per-source view choice.**
  - Books: `SETTINGS.fileBrowserView`.
  - Recent: `APP_STATE.recentBooksView`.
  - New and Authors: two new one-byte fields, `APP_STATE.addedBooksView` and `APP_STATE.authorsView`.
- **Sort:** Options → Sort applies to Books only. Recent, New and Authors have a fixed order.
- **Search** (filter and card-wide) stays on the Books tab.
- **Context menu:** book rows in New and Authors get the same actions as in Books. "Remove from recents" stays on Recent only; `FileContextMenuActivity`'s `recentsList` flag becomes a source parameter.
- **Delete or move from New/Authors:** the row is dropped on the spot. That bumps `contentGeneration`, so the next visit rebuilds.
- **Hosting the builder:** `FileBrowserActivity::loop()` gives the lent framebuffer to one job at a time:
  1. row titles (`BookRowResolver::resolveOne`);
  2. build steps (only while on New or Authors);
  3. covers.

## 5. The screen

### 5.1 Tabs

- A text tab bar under the header (option A): **Books · Recent · New · Authors**. It's the same bar Settings and the reader menu ship (`TabbedUiListActivity`).
- **Header:** the open folder on Books, the open author on Authors, otherwise "Library".
- `FileBrowserActivity` moves from `UiListActivity` to `TabbedUiListActivity`, and its `nav.` uses become `activeNav()`.
- All Files and the pickers stay without a bar. That needs a new `TabbedUiListActivity::showsTabBar()` hook; when it returns false, there is no lead position and positions map straight onto rows.

### 5.2 Input

| Input | Effect |
|---|---|
| Long Up / Down | Switch tab |
| Confirm on the bar | Next tab; the Confirm hint names it |
| Back on a row | Up a folder, or out of an author. At a tab's top level, focus moves to the bar |
| Back on the bar | Home |
| Long Back, anywhere | Home |
| Covers view: Up from the first grid row | Focus the bar |
| Tap a tab (touch) | Switch tab. Each tab is a quarter of the width × the bar height |

### 5.3 Entry, the remembered tab, routes

- **Home:** one **Library** entry (`HomeMenuAction::Library`, Folder icon) replaces Browse Files and Recent Books.
  - One setting, `showLibraryOnHome`, replaces `showBrowseFilesOnHome` and `showRecentBooksOnHome`.
  - **Migration:** the Library is shown if either old setting was on. `JsonSettingsIO` reads the old keys as a fallback, and they disappear on the next save.
- **Remembered tab:** `APP_STATE.libraryTab` (one byte, default Books). It is written when you leave the Library, and only if it changed.
  - Opening from Home uses the remembered tab.
  - The Books tab opens at the root, as Browse Files does today.
- **Routes:** these name a tab, which overrides the remembered one.
  - `goToFileBrowser()` and `goToRecentBooks()` become `goToLibrary(tab, path, focus)`.
  - Closing a book: `ReturnTo::Library` with {tab, folder or author, row} replaces `ReturnTo::FileBrowser` and `ReturnTo::RecentBooks`.
  - `ReaderActivity::goToLibrary()` opens the Books tab at that book's folder.

### 5.4 Strings

- Tab labels get their own keys rather than reusing `STR_NEW`, because grammar differs between languages.
- "Library", "Unknown author", "Not yet indexed (%d)", "Indexing %d/%d", "Refresh library", "Library too large" and "No new books".
- Missing translations fall back to English through the language packs. Regenerate i18n through the build, never by running `gen_i18n.py` by hand.

## 6. Tests and verification

**Host (gtest):**
- **New suite `test/library_index`:**
  - the file round-trips;
  - the join carries author and `firstSeen` by identity;
  - a moved book keeps its `firstSeen`, and a new book gets a new one;
  - a changed `sidecarSig` forces a resolve but keeps `firstSeen`;
  - the New order (`firstSeen` descending, ties broken by date);
  - the cap and the partial flag; corrupt and truncated files are rejected;
  - author keys: `file-as` precedence per author, the comma rule, surname-first, the Latin fold, NFD marks, and Unknown and Not-yet-indexed sorting last.
- **`epub_opf`:**
  - the primary author is the first `aut` or no-role creator;
  - translators are skipped;
  - `file-as` is captured;
  - the joined display author is unchanged.
- **`book_details_cache`:** version 2 round-trip; a version 1 file is rejected.
- **`sidecar_files`:** a sidecar-only parse for TXT/MD gives title and author; the sidecar overrides embedded metadata.
- **`home_menu`:** one Library entry; the settings migration.

**Device (X3, X4; T5S3 for touch):**
- **Flash:** the delta per environment with `scripts/firmware_size_history.py`. The budget is 8–15 KB; past about 20 KB, stop and discuss.
- **RAM:**
  - steady-state MEM lines with the Library open should equal today's Browse Files, plus the per-tab `ListNav`;
  - heap low-water and largest free block during a build, with `scripts/script_profile_mem.sh`.
- **Time,** on a card of about 300 never-opened EPUBs:
  - the first build;
  - the walk after a reboot with nothing to parse;
  - a rebuild with no changes.
  
  Record the numbers; there are no targets up front.
- **Behaviour:**
  - the screen stays responsive during a build;
  - a reset mid-build leaves an intact index;
  - books added over USB Drive appear in New;
  - a sidecar edited through the metadata-editor regroups its author;
  - a moved book does not appear in New;
  - closing a book returns to the right tab and row;
  - the remembered tab survives a reboot;
  - tab taps work on the T5S3.

## 7. Delivery order

Small, single-concern PRs, each building and passing on its own:

1. **Metadata:** `BookDetails` `primaryAuthor`/`authorSort`, `details.bin` version 2, `ContentOpfParser` role and `file-as`, the sidecar overlay as a free function for every format, and the XTC header. Visible on its own: Details view shows sidecar and XTC metadata.
2. **`lib/LibraryIndex`:** format, author keys, join, New ordering, plus host tests. No caller yet.
3. **Builder and model modes:** `LibraryIndexBuilder`, `Mode::Added`, `Mode::Authors`, the staleness rules.
4. **Screen:** the `showsTabBar()` hook, `FileBrowserActivity` on `TabbedUiListActivity`, tabs, input, header.
5. **Entry:** the Home Library entry, settings migration, `libraryTab`, `goToLibrary`/`ReturnTo::Library`, strings.

PRs 4 and 5 may merge together if splitting them leaves a dead end on Home.

## 8. Docs on merge

- `docs/file-formats.md`: the `library.bin` section.
- `docs/sidecar-files.md`: sidecars now label TXT/MD/XTC books, and `file-as` sets the author order.
- `docs/design/library-index.md`: the parts of §3 that code comments cite.
- Delete this spec.
