# Library

Status: open items as of 2026-10-09, from the Library work (PR #426; design in
`docs/design/library-index.md`). The screen and the index ship without these. Each change must end
flash-negative (the C3 partition is about 95 % full) or name what it buys.

## Measure a large card

Only a 25-book card has been measured (X4: first build 2.5 s, free heap at least 22.4 KB during the
build). The design was budgeted for 2,000 books, and the time a first build takes on a card of a few
hundred never-opened EPUBs (~300 ms of OPF parse each) is unknown.

- Where: the `[LIB]` log lines from `FileBrowserActivity::stepLibraryBuild` (start, walk, each publish,
  done with heap); `scripts/script_profile_mem.sh` for the heap.
- Measure: the first build, the walk after a reboot with nothing to parse, a rebuild with no changes,
  and publish time at 2,000 authors.
- Also check: the screen has no `skipLoopDelay()`, so once input has been idle a second the main loop
  light-sleeps about 50 ms between build steps. If the first build is slow, hold the loop awake while
  New or Authors builds.

## "Library too large"

The index's partial flag (`library::FLAG_PARTIAL`, more than 2,000 books) is set and tested, but nothing
on screen uses it: past 2,000 books, the books the walk did not reach never appear in New, and nothing
says so.

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

A build that fails (a write error, out of memory, no framebuffer to lend) logs and is tried again on
the next New or Authors visit, without telling the reader.

- Where: `FileBrowserActivity::stepLibraryBuild`.
- Next step: show a message once per boot when a build fails.

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
  (`FileBrowserModel::orderAuthorBooks`). Cheap fallback: order by `pathOff`, which groups by folder.
- **Remove inside an opened author** deletes the book, but its row stays until the rebuild the removal
  starts finishes (`FileBrowserModel::openAuthor` does not check each book exists). Mark as read's
  move to `/COMPLETED` does not start that rebuild at all (`FileBrowserActivity::doMarkAsRead`, the
  Stay branch).
- **Move to folder and New folder** are not offered on New and Authors: `moveToFolder` appends the
  row's entry, which is a path on those lists. Offer them once it appends the file's name.
- **A corrupt author record**: `FileBrowserModel::openAuthor` reserves `author.count` slots unchecked;
  clamp it to `bookCount - firstBook` first.
- **A frame drawn mid-publish** can show Authors empty: the index is released before the publish and
  reopened by the reload after it.

## Sidecar matching is case-sensitive in the walk

The walk pairs a book with its `.opf` by an exact stem hash and accepts `.opf` / `.OPF` only, while
`SidecarFiles` finds a sidecar case-insensitively. A `Book.opf` beside `book.epub` gets no signature,
so editing it does not send the book's author back through `details.bin` until something else changes.

- Where: `LibraryBuilder` (sidecar table, `sidecarFor`).
- Next step: fold the stem before hashing, as `SidecarFiles` matches.

## Tabs on button-only boards

There is no tab-bar focus (see the design record, "The screen"): a long Up/Down switches tab, and the
side hints say so. Settings and the reader menu also offer Confirm on a focused bar. If readers on
button-only boards still miss the tabs, give the browser a bar focus position, which means moving its
input onto `ListController` (see `list-input-harmonization.md`, the file browser item).

## Tests

- The Home-entry migration in `JsonSettingsIO::loadSettings` (old `showBrowseFilesOnHome` /
  `showRecentBooksOnHome` keys) has no host test; only `libraryOnHomeFromLegacy` has.
- No host harness links `FileBrowserActivity` or `FileBrowserModel`: tab switching, the Authors
  drill-in, return hints and the selection kept across a republish were checked on a device only.
