#pragma once

#include <WString.h>

// What the reader keeps for a book at a path (position, bookmarks, laid-out chapters, covers and
// thumbnails) is about the bytes that were there. Every transfer that writes, replaces or removes
// a file calls this once the change is made: the web file manager (upload, fetch, delete) and
// WebDAV (PUT, COPY, MOVE onto a file, DELETE). For a book it clears the book's cache folder. For a
// cover sidecar ("book.jpg" beside "book.epub") it drops that book's cached covers and
// thumbnails, which were drawn from the old cover. Anything else is left alone.
//
// One helper for both servers: WebDAV kept a copy of its own that left the cover sidecar out, so a
// cover replaced over WebDAV went on showing the old one.
void clearBookCacheIfNeeded(const String& path);
