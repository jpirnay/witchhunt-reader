# Book cache

Status: open items from the cache-clear work (#538). `lib/FsHelpers/BookCache` decides what a clear keeps
(`BookCache::Keep::ReadingState` keeps `progress.bin`, `bookmarks.bin`, `linkstack.bin`,
`koreader_syncmethod.txt` and `overrides.bin`: `BookCache::isReadingState`); the paths below do not use
it yet. The file browser, the web file manager and WebDAV clear a book's cache through one helper,
`clearBookCacheIfNeeded()` (`src/BookCacheReset.h`, #781).

## Replacing a book still wipes its reading state

Overwriting a book at the same path deletes its whole cache folder, progress and bookmarks included,
without asking:

- web `/upload` and the WebSocket/Calibre upload, WebDAV PUT and MOVE, the OPDS download over an
  existing file;
- `Epub::clearCacheIfContentChanged()` when the book's content changed at the same path (it calls
  `clearCache()` with the default `Keep::Nothing`).

Next step: decide per path whether the position still makes sense for the new content (an
optimised copy of the same book: yes; a different book under the same name: no), then use
`Keep::ReadingState` where it does. `/api/book/replace?keep=1` (web Optimize) already keeps it.

## TXT and Markdown

- There is no per-book Delete Book Cache for TXT/MD (the Library's Options offer it for EPUB and XTC only,
  `FileContextMenuActivity`); Settings → Clear Reading Cache covers them.
- A serial-transfer `rm` leaves the book's cache folder behind, for every format
  (`SerialTransferDevice::removeFile` does not call `clearBookCacheIfNeeded()`). A new file at that path
  then opens at the old position.

## Inconsistencies

- `BookRelocation::bookMoved` calls `Storage.removeDir` directly instead of `BookCache`
  (`src/BookRelocation.cpp`).
- `clearBookCachesInDirectory()` (`src/network/CrossPointWebServer.cpp`) is dead code: only it calls
  itself.
- Clear Reading Cache's "N items removed" also counts folders whose reading state was kept
  (`ClearCacheActivity::clearCache`).
