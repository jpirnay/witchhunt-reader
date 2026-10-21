# Book cache

Status: open items as of 2026-10-21, from the cache-clear work (PR #538). `lib/FsHelpers/BookCache`
decides what a clear keeps (`BookCache::Keep::ReadingState` keeps `progress.bin`, `bookmarks.bin`,
`linkstack.bin`, `koreader_syncmethod.txt`); the paths below do not use it yet.

## Replacing a book still wipes its reading state

Overwriting a book at the same path deletes its whole cache folder, progress and bookmarks included,
without asking:

- web `/upload` and the WebSocket/Calibre upload, WebDAV PUT and MOVE, the OPDS download over an
  existing file;
- `Epub::clearCacheIfContentChanged()` when the book's content changed at the same path.

Next step: decide per path whether the position still makes sense for the new content (an
optimised copy of the same book: yes; a different book under the same name: no), then use
`Keep::ReadingState` where it does. `/api/book/replace?keep=1` (web Optimize) already keeps it.

## TXT and Markdown

- There is no per-book Delete Book Cache for TXT/MD (the Library offers it for EPUB and XTC only);
  Settings → Clear Reading Cache covers them.
- Their `txt_`/`md_` folders are left behind when the file browser's Remove or a serial-transfer
  `rm` deletes the book.

## Inconsistencies

- Web `/delete` and WebDAV DELETE clear the cache even when deleting the book fails.
- WebDAV COPY over a book leaves the old cache in place.
- `BookRelocation` and the web/WebDAV TXT clears call `removeDir` directly instead of `BookCache`.
- `clearBookCachesInDirectory()` in `CrossPointWebServer.cpp` is dead code.
- Clear Reading Cache's "N items removed" also counts folders whose reading state was kept.
