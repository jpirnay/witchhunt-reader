#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// A book's cache folder (/.crosspoint/epub_<hash>, xtc_<hash> or txt_<hash>) holds two kinds of
// file. Most of it is derived from the book and made again on the next open: book.bin, the spine
// caches, the compiled CSS, the decoded images, the covers. A few files hold what the reader made
// there, which nothing can make again. Every clear of a cache folder goes through here, so that
// the two are told apart in one place.
//
// A NEW FILE THE READER KEEPS IN A BOOK'S CACHE FOLDER, AND THAT A CLEAR MUST NOT LOSE, IS ADDED
// TO isReadingState() AND NOWHERE ELSE.
namespace BookCache {

// What a clear leaves in the folder.
enum class Keep : uint8_t {
  Nothing,       // the folder goes, and the book's reading state with it
  ReadingState,  // the isReadingState() files stay; the next open rebuilds everything else
};

// The reader's own files: where the book was left (progress.bin) and the link Back stack that
// leads back from there (linkstack.bin), its bookmarks (bookmarks.bin), and which document id the
// KOReader sync server keeps it under (koreader_syncmethod.txt), learned from the server. None of
// them needs what a clear removes. A position and a bookmark name a page of the book's layout, and
// the next open lays the book out again the same way from the same book and settings; a position
// also carries its page's paragraph, which finds it in a layout that did change.
bool isReadingState(std::string_view name);

// Covers and thumbnails. They are derived from the book, but slow to make again (an inflate and an
// image conversion), so a clear can leave them for a book that Home is about to show.
bool isCoverImage(std::string_view name);

// Clears `cacheDir` as `keep` says, and also leaves the covers when `keepCovers` is set. The folder
// itself goes when nothing is left in it. True when it is clear: there was no folder, or every
// removal succeeded.
bool clear(const std::string& cacheDir, Keep keep, bool keepCovers = false);

}  // namespace BookCache
