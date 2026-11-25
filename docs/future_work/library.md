# Library

Status: open items from the Library work (#426; design in `docs/design/library-index.md`). The screen and
the index ship without these. Each change must end flash-negative or name what it buys. Re-measure the
`default` build (X3, X4) before relying on a budget: #868's body gives 6,178,901 B of the 6,553,600 B app
slot on 2026-11-21.

## Measure a large card

A 25-book card was measured first (X4: first build 2.5 s, free heap at least 22.4 KB during the build).
The design was budgeted for 2,000 books. On a 3,692-book card an X4 Pro's index walk reached that limit
in 32 s, in the background (#606). The rest of that first build is not measured: its total time on a
card of a few hundred never-opened EPUBs (~300 ms of OPF parse each), the author lookups, publish at
2,000 and the heap.

- Where: the `[LIB]` log lines from `FileBrowserActivity::stepLibraryBuild` (start, walk, each publish,
  done with heap); `scripts/script_profile_mem.sh` for the heap.
- Measure: the first build; a rebuild after a change that adds no book (walk, join and publish, nothing
  to parse); Refresh library, which looks up every author again (from `details.bin` where it is
  cached); and publish time at 2,000 authors.
- Also check: the screen has no `skipLoopDelay()`, so once input has been idle a second the main loop
  light-sleeps about 50 ms between build steps. If the first build is slow, hold the loop awake while
  New or Authors builds.

## "Library too large"

The index's partial flag (`library::FLAG_PARTIAL`, more than 2,000 books) is set and tested, but nothing
on screen uses it: past 2,000 books, the books the walk did not reach never appear in New, and nothing
says so. `LibraryIndexReader::partial()` exists and has no caller in `src/`; only
`LibraryFreshness::checkListedEntry` reads the flag.

- Next step: a notice on Authors when the header is partial ("Library too large").

## Changes made in a computer's card reader

The index is kept across boots and rebuilt for a reason (design record, "When it rebuilds"). A card
edited in a computer is noticed only when the Books tab lists a book or folder dated after the index's
`newestDate`. A copy that keeps its old date (macOS Finder keeps both dates on FAT), a removal, or a
change in a folder nobody opens goes unnoticed until Refresh library.

- Where: `LibraryFreshness::checkListedEntry`, `FileBrowserModel::load`.
- Ideas: compare each listed book's identity with the index (records are in identity order, so a
  binary search) for folders under a size limit; or let the folder's own SD index signature
  (`FileIndex`) vote.

## Build failures are silent

A build that fails (a write error, out of memory, no framebuffer to lend) logs, without telling the
reader; it runs again on a later New or Authors visit while a reason to build holds (next item).

- Where: `FileBrowserActivity::stepLibraryBuild`.
- Next step: show a message once per boot when a build fails.

## An interrupted build resumes only after a card change

A build that stops early -- the screen left (opening a book is enough), a failure, no framebuffer to
lend -- runs again on the next New or Authors visit only if `LibraryFreshness::stale` still finds a
reason. A build started for a missing index, the other hidden-files setting or Refresh library leaves
none once it has published: the index is valid, matches the setting, and there is no change marker. The
authors it had not looked up yet stay under *Not yet indexed* until the card changes. On a first build
of a large card, leaving before the last author is read leaves Authors that way.

- Where: `FileBrowserActivity::onExit` (its comment says the next visit walks again),
  `stepLibraryBuild`, `LibraryFreshness::stale`, `LibraryStaleness::Facts`.
- Next step: make unresolved authors a reason to build. Pending authors file last, so the index's
  last author record having `library::AUTHOR_PENDING` says so in one read; add a case for it to
  `test/library_index/LibraryStalenessTest.cpp`.

## New's covers wait for every author

The screen gives the lent framebuffer to one job at a time: titles, then the build, then covers. So on a
first build of a few hundred EPUBs, New shows title cards until the last author is read, though New does
not depend on authors.

- Where: `FileBrowserActivity::loop`, `stepLibraryBuild`, `generateCovers`.
- Next step: let the page on screen take its covers between Resolve steps (after the first publish),
  or pause the build while New's page lacks covers.

## Author lists

- **An author with more than 200 books** -- and on a large first build the *Unknown author* and *Not
  yet indexed* groups -- keeps the index's order, which is identity-hash order and looks random
  (`FileBrowserModel::orderAuthorBooks`, `MAX_ORDERED`). Cheap fallback: order by `pathOff`, which
  groups by folder.
- **Remove inside an opened author** deletes the book, but its row stays until the rebuild the removal
  starts finishes (`FileBrowserModel::openAuthor` does not check each book exists). Mark as read's
  move to `/COMPLETED` marks the card changed, so the next visit to New or Authors rebuilds, but the
  Stay branch of `FileBrowserActivity::doMarkAsRead` starts no build: the opened author keeps the
  book's old path until then.
- **Move to folder and New folder** are not offered on New and Authors
  (`FileContextMenuActivity`: a fixed-order list returns before them): `moveToFolder` appends the
  row's entry, which is a path on those lists. Offer them once it appends the file's name.
- **A corrupt author record**: `FileBrowserModel::openAuthor` reserves `author.count` slots unchecked;
  clamp it to `bookCount - firstBook` first.
- **A frame drawn mid-publish** can show Authors empty: the index is released before the publish and
  reopened by the reload after it (read in `stepLibraryBuild`, not seen on a device).

## Sidecar matching is case-sensitive in the walk

The walk pairs a book with its `.opf` by an exact stem hash and accepts `.opf` / `.OPF` only, while
`SidecarFiles` finds a sidecar through the card's case-insensitive lookup. A `Book.opf` beside
`book.epub` gets no signature, so editing it does not send the book's author back through `details.bin`
until something else changes. `docs/sidecar-files.md` ("When changes take effect") describes this for
readers.

- Where: `LibraryBuilder` (`stemHash`, `isMetadataSidecar`, `sidecarFor`).
- Next step: fold the stem before hashing, as `SidecarFiles` matches.

## Cover thumbnails' midtones (matcha-reader #487): decide

1-bit cover thumbnails are dithered (Atkinson) with gamma off and contrast 1.15 (`GAMMA_CORRECTION` and
`CONTRAST_FACTOR` in `lib/GfxRenderer/BitmapHelpers.cpp`). matcha lifts the midtones (gamma 0.75) before
dithering, which makes dark covers lighter in the 1-bit thumbs. A taste choice: here it would need a
thumb-only flag, since the same `Atkinson1BitDitherer` also dithers page images
(`JpegToFramebufferConverter.cpp`); old thumbs stay until they are made again; a 256-byte flash table.
matcha-reader #487, Eszter Schuffert (eszter007).

## Tests

- The Home-entry migration in `JsonSettingsIO::loadSettings` (old `showBrowseFilesOnHome` /
  `showRecentBooksOnHome` keys) has no host test; only `libraryOnHomeFromLegacy` has. No host target
  links `JsonSettingsIO.cpp`.
- `test/file_browser_model` covers `FileBrowserModel`'s folder listing, search and the streamed Recents
  only. Its Authors side has no host test and no host harness links `FileBrowserActivity`: tab
  switching, the Authors drill-in, return hints and the selection kept across a republish were checked
  on a device only.
