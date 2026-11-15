#pragma once

#include <BookCache.h>
#include <WString.h>

#include <string>

// What the reader keeps for a book at a path (position, bookmarks, laid-out chapters, covers and
// thumbnails) is about the bytes that were there. Whatever writes, replaces or removes a file calls
// this once the change is made: the device's file browser (delete, Delete cache), the web file
// manager (upload, fetch, delete) and WebDAV (PUT, COPY, MOVE onto a file, DELETE).
//  - For a book (EPUB, XTC, TXT, Markdown) it clears the book's cache folder as `keep` says.
//  - For a cover sidecar ("book.jpg" beside "book.epub") it drops only that book's cached covers
//    and thumbnails, which were drawn from the old cover; the book's reading state and layout stay.
// Anything else is left alone.
//
// One helper for every caller: the file browser and WebDAV each kept a narrower copy. The browser's
// left TXT and Markdown caches behind, so a new file at the path of a deleted one opened at the old
// position with the old bookmarks; WebDAV's left a replaced cover's thumbnails.
void clearBookCacheIfNeeded(const std::string& path, BookCache::Keep keep = BookCache::Keep::Nothing);

inline void clearBookCacheIfNeeded(const String& path, const BookCache::Keep keep = BookCache::Keep::Nothing) {
  clearBookCacheIfNeeded(std::string(path.c_str(), path.length()), keep);
}
