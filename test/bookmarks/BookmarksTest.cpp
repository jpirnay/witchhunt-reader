// The bookmark files: a damaged file or a short heap costs as little as it can, and never what is on the card.
//
// GlobalBookmarkIndex::load() ran at every boot, and a count or a length read from the file went straight to reserve()
// or resize(). On the device a failed allocation aborts (-fno-exceptions, HeapFit.h), so one bad file could be a boot
// loop. A failed load left an empty list that the next save wrote over the file, and every save truncated the file it
// replaced. Ideas from CrossInk 53aa4f39 (Julia Nguyen).
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <algorithm>
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
std::string storeFile(const uint16_t count, const std::string& body, const char version = '\x02') {
  std::string out(1, version);
  put16(out, count);
  return out + body;
}
void put32(std::string& out, const uint32_t v) {
  put16(out, static_cast<uint16_t>(v & 0xFFFF));
  put16(out, static_cast<uint16_t>(v >> 16));
}
// What follows a bookmark from version 3 on.
void putAnchor(std::string& out, const uint32_t layout, const uint16_t pageCount, const std::string& xpath) {
  put32(out, layout);
  put16(out, pageCount);
  putString(out, xpath);
}
StarAnchor anchorOf(const uint32_t layout, const uint16_t pageCount, std::string xpath) {
  StarAnchor anchor;
  anchor.layout = layout;
  anchor.pageCount = pageCount;
  anchor.xpath = std::move(xpath);
  return anchor;
}
constexpr char kXPath[] = "/body/DocFragment[6]/body/p[31]/text()[1].851";

std::vector<BookmarkStore::Bookmark> oneBookmark() { return {{3, 4, "mine"}}; }

class Bookmarks : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    GLOBAL_BOOKMARKS.forgetFileState();  // the singleton outlives each test's card
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
  Storage.put(kIndexPath, indexFile(1, indexEntry("/a.epub", {"x"})));
  EXPECT_TRUE(GLOBAL_BOOKMARKS.hasAny());
  EXPECT_TRUE(GLOBAL_BOOKMARKS.getEntries().empty());
}

TEST_F(Bookmarks, HasAnyFindsNoFileEmpty) { EXPECT_FALSE(GLOBAL_BOOKMARKS.hasAny()); }

// Home asks on every entry: the header is read once, and the answer stands until a save changes it.
TEST_F(Bookmarks, HasAnyReadsTheFileOnce) {
  Storage.put(kIndexPath, indexFile(1, indexEntry("/a.epub", {"x"})));
  EXPECT_TRUE(GLOBAL_BOOKMARKS.hasAny());
  Storage.reset();  // behind the index's back: not read again
  EXPECT_TRUE(GLOBAL_BOOKMARKS.hasAny());
  GLOBAL_BOOKMARKS.forgetFileState();
  EXPECT_FALSE(GLOBAL_BOOKMARKS.hasAny());
}

// A save that fails may leave the old file or a half-done replace: the next answer comes from the card.
TEST_F(Bookmarks, HasAnyRereadsAfterAFailedSave) {
  EXPECT_FALSE(GLOBAL_BOOKMARKS.hasAny());
  Storage.writeBudget = 4;
  GLOBAL_BOOKMARKS.upsertFromStore("/a.epub", kCache, "A", false, oneBookmark());
  EXPECT_FALSE(GLOBAL_BOOKMARKS.hasAny());
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

// ---- What finds a starred page's text again (version 3)

TEST_F(Bookmarks, AStoreKeepsEachStarsAnchor) {
  BookmarkStore store;
  store.load(kCache);
  ASSERT_TRUE(store.toggle(5, 6, anchorOf(0xC0FFEEu, 41, kXPath)));
  store.toggle(7, 8);  // a TXT-style star: nothing to find it by but its page
  store.save();
  BookmarkStore again;
  again.load(kCache);
  ASSERT_EQ(again.getAll().size(), 2u);
  ASSERT_EQ(again.getAnchors().size(), 2u);
  EXPECT_EQ(again.getAnchors()[0].layout, 0xC0FFEEu);
  EXPECT_EQ(again.getAnchors()[0].pageCount, 41);
  EXPECT_EQ(again.getAnchors()[0].xpath, kXPath);
  EXPECT_EQ(again.getAnchors()[1].layout, 0u);
  EXPECT_TRUE(again.getAnchors()[1].xpath.empty());
}

TEST_F(Bookmarks, AStoreFromTheSecondFormatReadsWithoutAnchorsAndKeepsItsNames) {
  std::string body;
  putBookmark(body, 1, 2, "first");
  putBookmark(body, 3, 4, "second");
  Storage.put(kStorePath, storeFile(2, body));
  BookmarkStore store;
  store.load(kCache);
  ASSERT_EQ(store.getAll().size(), 2u);
  ASSERT_EQ(store.getAnchors().size(), 2u);
  EXPECT_EQ(store.getAll()[1].name, "second");
  EXPECT_EQ(store.getAnchors()[1].layout, 0u);
  EXPECT_TRUE(store.getAnchors()[1].xpath.empty());

  // Written back in the new format, and still the same two.
  store.markDirty();
  store.save();
  EXPECT_EQ(Storage.content(kStorePath)[0], '\x03');
  BookmarkStore again;
  again.load(kCache);
  ASSERT_EQ(again.getAll().size(), 2u);
  EXPECT_EQ(again.getAll()[0].name, "first");
}

TEST_F(Bookmarks, AStoreOfTheThirdFormatReads) {
  std::string body;
  putBookmark(body, 1, 2, "x");
  putAnchor(body, 7, 30, kXPath);
  Storage.put(kStorePath, storeFile(1, body, '\x03'));
  BookmarkStore store;
  store.load(kCache);
  ASSERT_EQ(store.getAnchors().size(), 1u);
  EXPECT_EQ(store.getAnchors()[0].layout, 7u);
  EXPECT_EQ(store.getAnchors()[0].xpath, kXPath);
}

TEST_F(Bookmarks, AnAnchorCutOffKeepsTheBookmarksBeforeIt) {
  std::string body;
  putBookmark(body, 1, 2, "kept");
  putAnchor(body, 7, 30, kXPath);
  putBookmark(body, 3, 4, "lost");
  putAnchor(body, 7, 30, kXPath);
  body.resize(body.size() - 5);  // into the second XPath
  Storage.put(kStorePath, storeFile(2, body, '\x03'));
  BookmarkStore store;
  store.load(kCache);
  ASSERT_EQ(store.getAll().size(), 1u);
  EXPECT_EQ(store.getAll()[0].name, "kept");
  EXPECT_EQ(store.getAnchors()[0].xpath, kXPath);
}

TEST_F(Bookmarks, AnXPathLengthBeyondTheFileIsDamageNotAnAllocation) {
  std::string body;
  putBookmark(body, 1, 2, "x");
  put32(body, 7);
  put16(body, 30);
  put16(body, 60000);  // an XPath of 60000 bytes, in a file of a few
  body += "/body";
  Storage.put(kStorePath, storeFile(1, body, '\x03'));
  BookmarkStore store;
  store.load(kCache);
  EXPECT_LT(g_largestRequest, 4096u);
  EXPECT_TRUE(store.getAll().empty());
}

TEST_F(Bookmarks, AnXPathTooLongToKeepIsWrittenAsNoneNeverCut) {
  BookmarkStore store;
  store.load(kCache);
  ASSERT_TRUE(store.toggle(1, 2, anchorOf(7, 30, std::string(bookmark_file::kMaxXPathBytes + 1, 'p'))));
  store.save();
  BookmarkStore again;
  again.load(kCache);
  ASSERT_EQ(again.getAnchors().size(), 1u);
  EXPECT_EQ(again.getAnchors()[0].layout, 7u);
  EXPECT_TRUE(again.getAnchors()[0].xpath.empty());
}

TEST_F(Bookmarks, AStoreWhoseXPathTheHeapCannotTakeIsNotSavedOver) {
  const std::string xpath = std::string(kXPath) + std::string(60, '0');
  std::string body;
  putBookmark(body, 1, 2, "mine");
  putAnchor(body, 7, 30, xpath);
  const std::string file = storeFile(1, body, '\x03');
  Storage.put(kStorePath, file);
  BookmarkStore store;
  {
    // A block for the lists of one, not for the XPath (heapHasBlockFor keeps 64 bytes of slack).
    const HostLargestFreeBlock tight(64 + std::max(sizeof(Bookmark), sizeof(StarAnchor)));
    store.load(kCache);
  }
  EXPECT_TRUE(store.getAll().empty());
  store.toggle(9, 9);
  store.save();
  EXPECT_EQ(Storage.content(kStorePath), file);
}

// The anchors stay in the book's own file: the index, held in RAM for every book at once, keeps its format.
TEST_F(Bookmarks, TheIndexKeepsNoAnchors) {
  BookmarkStore store;
  store.load(kCache);
  ASSERT_TRUE(store.toggle(1, 0, anchorOf(7, 30, kXPath)));
  store.rename(0, "x");
  GLOBAL_BOOKMARKS.syncFromStore(store, "/a.epub", kCache, "Title of /a.epub", false);
  EXPECT_EQ(Storage.content(kIndexPath), indexFile(1, indexEntry("/a.epub", {"x"})));
}

// What the bookmark screens do to a book's stars (StarredPagesActivity, GlobalBookmarksActivity::mutateBook): each
// star keeps its own anchor through a delete and a rename, and a load and save in between.
TEST_F(Bookmarks, EachStarKeepsItsAnchorThroughADeleteAndARename) {
  BookmarkStore store;
  store.load(kCache);
  ASSERT_TRUE(store.toggle(1, 1, anchorOf(11, 10, "/body/DocFragment[2]/body/p[1]/text()[1].0")));
  ASSERT_TRUE(store.toggle(2, 2, anchorOf(22, 20, "/body/DocFragment[3]/body/p[2]/text()[1].0")));
  ASSERT_TRUE(store.toggle(3, 3, anchorOf(33, 30, "/body/DocFragment[4]/body/p[3]/text()[1].0")));
  store.save();

  BookmarkStore screen;
  screen.load(kCache);
  screen.removeAt(1);
  screen.rename(1, "third");
  screen.save();

  BookmarkStore again;
  again.load(kCache);
  ASSERT_EQ(again.getAll().size(), 2u);
  EXPECT_EQ(again.getAll()[1].spineIndex, 3);
  EXPECT_EQ(again.getAll()[1].name, "third");
  EXPECT_EQ(again.getAnchors()[0].layout, 11u);
  EXPECT_EQ(again.getAnchors()[1].layout, 33u);
  EXPECT_EQ(again.getAnchors()[1].xpath, "/body/DocFragment[4]/body/p[3]/text()[1].0");
}
