// clearBookCacheIfNeeded() (src/BookCacheReset.cpp), the one clear every caller that writes, replaces or removes a
// file goes through: the device's file browser, the web file manager and WebDAV. On the in-memory card of
// stubs/HalStorage.h, with the real BookCache.

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "BookCacheReset.h"
#include "BookRelocation.h"
#include "HalStorage.h"

namespace book_protection_stub {
extern std::vector<std::pair<std::string, bool>> closed;
}  // namespace book_protection_stub

namespace {

class BookCacheReset : public ::testing::Test {
 protected:
  void SetUp() override {
    book_protection_stub::closed.clear();
    Storage.reset();
    Storage.putFolder("/.crosspoint");
    Storage.putFolder("/Books");
  }

  // A book with what a reader keeps for it: position and bookmarks, a layout, a cover and a thumbnail.
  static std::string book(const std::string& path) {
    Storage.put(path, "book bytes");
    const std::string dir = BookRelocation::cacheDirFor(path);
    Storage.putFolder(dir);
    for (const char* name : {"progress.bin", "bookmarks.bin", "index.bin", "cover.bmp", "thumb_240.bmp"}) {
      Storage.put(dir + "/" + name, name);
    }
    return dir;
  }
  static bool has(const std::string& dir, const char* name) { return Storage.exists((dir + "/" + name).c_str()); }
};

// The file browser's own copy knew EPUB and XTC only: a TXT or Markdown book deleted on the device left its cache,
// and a new file at its path opened at the old position with the old bookmarks.
TEST_F(BookCacheReset, ADeletedTextBookLosesItsCache) {
  for (const char* path : {"/Books/notes.txt", "/Books/README.md", "/Books/novel.epub", "/Books/comic.xtc"}) {
    const std::string dir = book(path);
    Storage.remove(path);
    clearBookCacheIfNeeded(std::string(path));
    EXPECT_FALSE(Storage.exists(dir.c_str())) << path;
  }
}

// The browser's Delete cache that keeps the reading state, for every kind of book.
TEST_F(BookCacheReset, KeepingTheReadingStateKeepsPositionAndBookmarks) {
  for (const char* path : {"/Books/notes.txt", "/Books/novel.epub"}) {
    const std::string dir = book(path);
    clearBookCacheIfNeeded(std::string(path), BookCache::Keep::ReadingState);
    EXPECT_TRUE(has(dir, "progress.bin")) << path;
    EXPECT_TRUE(has(dir, "bookmarks.bin")) << path;
    EXPECT_FALSE(has(dir, "index.bin")) << path;
    EXPECT_FALSE(has(dir, "thumb_240.bmp")) << path;
  }
}

// A cover sidecar written or removed: only the book's covers and thumbnails go, never its reading state or layout.
TEST_F(BookCacheReset, ACoverSidecarDropsOnlyTheThumbnails) {
  const std::string dir = book("/Books/novel.epub");
  Storage.put("/Books/novel.JPG", "cover");
  clearBookCacheIfNeeded(std::string("/Books/novel.JPG"));
  EXPECT_FALSE(has(dir, "cover.bmp"));
  EXPECT_FALSE(has(dir, "thumb_240.bmp"));
  EXPECT_TRUE(has(dir, "progress.bin"));
  EXPECT_TRUE(has(dir, "bookmarks.bin"));
  EXPECT_TRUE(has(dir, "index.bin"));
}

// The protection a protected book registers holds the manifest in its cache folder. Epub::clearCache() closes it
// first; this clear, which the web file manager, WebDAV and the OPDS download go through, did not.
TEST_F(BookCacheReset, ABookIsClosedBeforeItsCacheGoes) {
  const std::string dir = book("/Books/loan.epub");
  clearBookCacheIfNeeded(std::string("/Books/loan.epub"));
  ASSERT_EQ(book_protection_stub::closed.size(), 1u);
  EXPECT_EQ(book_protection_stub::closed[0].first, "/Books/loan.epub");
  EXPECT_TRUE(book_protection_stub::closed[0].second) << "closed after its cache folder was removed";
  EXPECT_FALSE(Storage.exists(dir.c_str()));
}

TEST_F(BookCacheReset, OtherFilesTouchNothing) {
  const std::string dir = book("/Books/novel.epub");
  Storage.put("/Books/novel.pdf", "not a book here");
  clearBookCacheIfNeeded(String("/Books/novel.pdf"));
  EXPECT_TRUE(book_protection_stub::closed.empty());
  for (const char* name : {"progress.bin", "bookmarks.bin", "index.bin", "cover.bmp", "thumb_240.bmp"}) {
    EXPECT_TRUE(has(dir, name)) << name;
  }
}

}  // namespace
