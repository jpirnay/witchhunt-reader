// The bookmark files: a damaged file or a short heap costs as little as it can, and never what is on the card.
//
// GlobalBookmarkIndex::load() ran at every boot, and a count or a length read from the file went straight to reserve()
// or resize(). On the device a failed allocation aborts (-fno-exceptions, HeapFit.h), so one bad file could be a boot
// loop. A failed load left an empty list that the next save wrote over the file, and every save truncated the file it
// replaced. Ideas from CrossInk 53aa4f39 (Julia Nguyen).
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "BookmarkStore.h"
#include "GlobalBookmarkIndex.h"
#include "HalStorage.h"

// The largest single request the code under test makes: what a device's heap would be asked for.
namespace {
size_t g_largestRequest = 0;
}

void* operator new(const size_t bytes) {
  if (bytes > g_largestRequest) g_largestRequest = bytes;
  if (void* p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

constexpr char kIndexPath[] = "/.crosspoint/global_bookmarks.bin";
constexpr char kCache[] = "/.crosspoint/epub_1";
constexpr char kStorePath[] = "/.crosspoint/epub_1/bookmarks.bin";

void put16(std::string& out, const uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>(v >> 8));
}
void putString(std::string& out, const std::string& s) {
  put16(out, static_cast<uint16_t>(s.size()));
  out += s;
}
void putBookmark(std::string& out, const uint16_t spine, const uint16_t page, const std::string& name) {
  put16(out, spine);
  put16(out, page);
  putString(out, name);
}
// One index entry as version 1 of the file has it.
std::string indexEntry(const std::string& path, const std::vector<std::string>& names) {
  std::string out;
  putString(out, path);
  putString(out, kCache);
  putString(out, "Title of " + path);
  out.push_back('\0');
  put16(out, static_cast<uint16_t>(names.size()));
  for (size_t i = 0; i < names.size(); ++i) putBookmark(out, 1, static_cast<uint16_t>(i), names[i]);
  return out;
}
std::string indexFile(const uint16_t count, const std::string& body) {
  std::string out(1, '\x01');
  put16(out, count);
  return out + body;
}
std::string storeFile(const uint16_t count, const std::string& body) {
  std::string out(1, '\x02');
  put16(out, count);
  return out + body;
}

std::vector<BookmarkStore::Bookmark> oneBookmark() { return {{3, 4, "mine"}}; }

class Bookmarks : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    g_largestRequest = 0;
  }
  void TearDown() override { Storage.reset(); }
};

using Hold = GlobalBookmarkIndex::Hold;

}  // namespace

// ---- The index

TEST_F(Bookmarks, AnIndexReadsBackWhatWasSaved) {
  GLOBAL_BOOKMARKS.upsertFromStore("/a.epub", kCache, "A", false, oneBookmark());
  const Hold hold;
  ASSERT_EQ(GLOBAL_BOOKMARKS.getEntries().size(), 1u);
  const auto& entry = GLOBAL_BOOKMARKS.getEntries()[0];
  EXPECT_EQ(entry.sourcePath, "/a.epub");
  ASSERT_EQ(entry.bookmarks.size(), 1u);
  EXPECT_EQ(entry.bookmarks[0].name, "mine");
}

TEST_F(Bookmarks, AFileFromTheFirmwareBeforeThisReads) {
  Storage.put(kIndexPath, indexFile(2, indexEntry("/a.epub", {"x"}) + indexEntry("/b.epub", {"y", "z"})));
  const Hold hold;
  ASSERT_EQ(GLOBAL_BOOKMARKS.getEntries().size(), 2u);
  EXPECT_EQ(GLOBAL_BOOKMARKS.getEntries()[1].bookmarks[1].name, "z");
}

TEST_F(Bookmarks, ACountBeyondTheFileAsksForNoMoreThanTheFileHolds) {
  Storage.put(kIndexPath, indexFile(1000, indexEntry("/a.epub", {"x"})));
  const Hold hold;
  EXPECT_LT(g_largestRequest, 4096u);
}

TEST_F(Bookmarks, ABookmarkCountBeyondTheFileAsksForNoMoreThanTheFileHolds) {
  std::string entry;
  putString(entry, "/a.epub");
  putString(entry, kCache);
  putString(entry, "A");
  entry.push_back('\0');
  put16(entry, 1000);
  Storage.put(kIndexPath, indexFile(1, entry));
  const Hold hold;
  EXPECT_LT(g_largestRequest, 4096u);
}

TEST_F(Bookmarks, AStringLengthBeyondTheFileIsDamageNotAnAllocation) {
  std::string entry;
  put16(entry, 60000);  // a path of 60000 bytes, in a file of a few
  entry += "/a";
  Storage.put(kIndexPath, indexFile(1, entry));
  const Hold hold;
  EXPECT_LT(g_largestRequest, 4096u);
  EXPECT_TRUE(GLOBAL_BOOKMARKS.getEntries().empty());
}

TEST_F(Bookmarks, ATruncatedIndexKeepsTheEntriesBeforeTheCut) {
  std::string body = indexEntry("/a.epub", {"x"}) + indexEntry("/b.epub", {"y"});
  body.resize(body.size() - 3);
  Storage.put(kIndexPath, indexFile(2, body));
  const Hold hold;
  ASSERT_EQ(GLOBAL_BOOKMARKS.getEntries().size(), 1u);
  EXPECT_EQ(GLOBAL_BOOKMARKS.getEntries()[0].sourcePath, "/a.epub");
}

TEST_F(Bookmarks, AnIndexTheHeapCannotTakeIsLeftAlone) {
  const std::string file = indexFile(2, indexEntry("/a.epub", {"x"}) + indexEntry("/b.epub", {"y"}));
  Storage.put(kIndexPath, file);
  {
    const HostLargestFreeBlock tight(64);
    GLOBAL_BOOKMARKS.upsertFromStore("/c.epub", kCache, "C", false, oneBookmark());
    GLOBAL_BOOKMARKS.removeBySourcePath("/a.epub");
    GLOBAL_BOOKMARKS.reconcile();
  }
  EXPECT_EQ(Storage.content(kIndexPath), file);
}

TEST_F(Bookmarks, AnIndexNotLoadedIsNotSavedOverTheFile) {
  const std::string file = indexFile(1, indexEntry("/a.epub", {"x"}));
  Storage.put(kIndexPath, file);
  GLOBAL_BOOKMARKS.save();
  EXPECT_EQ(Storage.content(kIndexPath), file);
}

TEST_F(Bookmarks, AFailedIndexSaveKeepsTheFileItWouldHaveReplaced) {
  const std::string file = indexFile(1, indexEntry("/a.epub", {"x"}));
  Storage.put(kIndexPath, file);
  Storage.writeBudget = 4;
  GLOBAL_BOOKMARKS.upsertFromStore("/b.epub", kCache, "B", false, oneBookmark());
  EXPECT_EQ(Storage.content(kIndexPath), file);
  EXPECT_FALSE(Storage.exists("/.crosspoint/global_bookmarks.bin.part"));
}

TEST_F(Bookmarks, NothingStaysInMemoryAfterAnUpdate) {
  GLOBAL_BOOKMARKS.upsertFromStore("/a.epub", kCache, "A", false, oneBookmark());
  EXPECT_TRUE(GLOBAL_BOOKMARKS.getEntries().empty());
}

TEST_F(Bookmarks, AHoldKeepsTheIndexAndTheLastOneLetsItGo) {
  GLOBAL_BOOKMARKS.upsertFromStore("/a.epub", kCache, "A", false, oneBookmark());
  {
    const Hold outer;
    {
      const Hold inner;
      EXPECT_EQ(GLOBAL_BOOKMARKS.getEntries().size(), 1u);
    }
    EXPECT_EQ(GLOBAL_BOOKMARKS.getEntries().size(), 1u);
  }
  EXPECT_TRUE(GLOBAL_BOOKMARKS.getEntries().empty());
}

TEST_F(Bookmarks, HasAnyAnswersFromTheFileWithoutLoadingIt) {
  EXPECT_FALSE(GLOBAL_BOOKMARKS.hasAny());
  Storage.put(kIndexPath, indexFile(1, indexEntry("/a.epub", {"x"})));
  EXPECT_TRUE(GLOBAL_BOOKMARKS.hasAny());
  EXPECT_TRUE(GLOBAL_BOOKMARKS.getEntries().empty());
}

TEST_F(Bookmarks, HasAnyFollowsAnUpdate) {
  EXPECT_FALSE(GLOBAL_BOOKMARKS.hasAny());
  GLOBAL_BOOKMARKS.upsertFromStore("/a.epub", kCache, "A", false, oneBookmark());
  EXPECT_TRUE(GLOBAL_BOOKMARKS.hasAny());
  GLOBAL_BOOKMARKS.removeBySourcePath("/a.epub");
  EXPECT_FALSE(GLOBAL_BOOKMARKS.hasAny());
}

// A save cut off between the two renames leaves the only copy aside: it comes back, and saving still works.
TEST_F(Bookmarks, AReplaceCutOffHalfWayIsPickedUp) {
  const std::string file = indexFile(1, indexEntry("/a.epub", {"x"}));
  Storage.put(std::string(kIndexPath) + ".old", file);
  {
    const Hold hold;
    EXPECT_EQ(GLOBAL_BOOKMARKS.getEntries().size(), 1u);
  }
  GLOBAL_BOOKMARKS.upsertFromStore("/b.epub", kCache, "B", false, oneBookmark());
  const Hold hold;
  EXPECT_EQ(GLOBAL_BOOKMARKS.getEntries().size(), 2u);
}

// ---- One book's store

TEST_F(Bookmarks, AStoreReadsBackWhatWasSaved) {
  BookmarkStore store;
  store.load(kCache);
  store.toggle(5, 6);
  store.rename(0, "named");
  store.save();
  BookmarkStore again;
  again.load(kCache);
  ASSERT_EQ(again.getAll().size(), 1u);
  EXPECT_EQ(again.getAll()[0].pageNumber, 6);
  EXPECT_EQ(again.getAll()[0].name, "named");
}

TEST_F(Bookmarks, AStoreFromTheFirstFormatReads) {
  std::string file(1, '\x01');
  put16(file, 1);
  put16(file, 7);
  put16(file, 8);
  Storage.put(kStorePath, file);
  BookmarkStore store;
  store.load(kCache);
  ASSERT_EQ(store.getAll().size(), 1u);
  EXPECT_EQ(store.getAll()[0].spineIndex, 7);
}

TEST_F(Bookmarks, AStoreCountBeyondTheFileAsksForNoMoreThanTheFileHolds) {
  std::string body;
  putBookmark(body, 1, 2, "x");
  Storage.put(kStorePath, storeFile(1000, body));
  BookmarkStore store;
  store.load(kCache);
  EXPECT_LT(g_largestRequest, 4096u);
}

TEST_F(Bookmarks, ATruncatedStoreKeepsTheBookmarksBeforeTheCut) {
  std::string body;
  putBookmark(body, 1, 2, "kept");
  putBookmark(body, 3, 4, "lost");
  body.resize(body.size() - 2);
  Storage.put(kStorePath, storeFile(2, body));
  BookmarkStore store;
  store.load(kCache);
  ASSERT_EQ(store.getAll().size(), 1u);
  EXPECT_EQ(store.getAll()[0].name, "kept");
}

TEST_F(Bookmarks, AStoreTheHeapCannotTakeIsNotSavedOver) {
  std::string body;
  putBookmark(body, 1, 2, "mine");
  const std::string file = storeFile(1, body);
  Storage.put(kStorePath, file);
  BookmarkStore store;
  {
    const HostLargestFreeBlock tight(8);
    store.load(kCache);
  }
  store.toggle(9, 9);
  store.save();
  EXPECT_EQ(Storage.content(kStorePath), file);
}

TEST_F(Bookmarks, AFailedStoreSaveKeepsTheFileItWouldHaveReplaced) {
  std::string body;
  putBookmark(body, 1, 2, "mine");
  const std::string file = storeFile(1, body);
  Storage.put(kStorePath, file);
  BookmarkStore store;
  store.load(kCache);
  store.toggle(9, 9);
  Storage.writeBudget = 3;
  store.save();
  EXPECT_EQ(Storage.content(kStorePath), file);
  EXPECT_FALSE(Storage.exists("/.crosspoint/epub_1/bookmarks.bin.part"));
}
