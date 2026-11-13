// The recent-books store on recent-books.bin: the order books were opened in, the hundred-book cap, removal and moves,
// the Library's pages, books gone from the card, the move from recent.json, and what a power cut can leave. Against
// real files in a folder of this process's own; the books' metadata and recent.json's parser are stood in for below.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "BookReaderOverrides.h"
#include "JsonSettingsIO.h"
#include "RecentBookMetadata.h"
#include "RecentBooksFile.h"
#include "RecentBooksStore.h"

namespace {

std::string g_root;
std::function<bool(RecentBook&)> g_refresh;
std::vector<LegacyRecentBook> g_legacy;
RecentBooksLoad g_load = RecentBooksLoad::Loaded;

}  // namespace

namespace RecentBookMetadata {
uint32_t openStamp(const std::string&) { return 0; }
bool refreshFromSidecar(RecentBook& book) { return g_refresh && g_refresh(book); }
std::string overridesCacheDir(const std::string& path) {
  if (path.size() < 5 || path.compare(path.size() - 5, 5, ".epub") != 0) return "";
  return g_root + "/cache/" + std::to_string(std::hash<std::string>{}(path));
}
}  // namespace RecentBookMetadata

namespace JsonSettingsIO {
RecentBooksLoad loadRecentBooks(std::vector<LegacyRecentBook>& out, const char*) {
  out = g_legacy;
  return g_load;
}
}  // namespace JsonSettingsIO

namespace {

class RecentBooksStoreTest : public testing::Test {
 protected:
  std::filesystem::path dir;
  std::string binPath;
  std::string jsonPath;

  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("recent_books_store_" + std::to_string(static_cast<long>(::getpid())) + "_" +
           testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "books");
    g_root = dir.string();
    g_refresh = nullptr;
    g_legacy.clear();
    g_load = RecentBooksLoad::Loaded;
    binPath = (dir / "recent-books.bin").string();
    jsonPath = (dir / "recent.json").string();
    RECENT_BOOKS.useFilesForTest(binPath, jsonPath);
  }
  void TearDown() override {
    RECENT_BOOKS.useFilesForTest("", "");
    std::filesystem::remove_all(dir);
  }

  // A book on the "card": its file exists.
  std::string bookAt(const std::string& name) {
    const std::string path = (dir / "books" / (name + ".epub")).string();
    std::ofstream(path) << "book";
    return path;
  }
  void open(const std::string& name) { RECENT_BOOKS.addBook(bookAt(name), "T " + name, "A " + name, "", ""); }

  std::vector<std::string> homeNames(size_t max = 100) {
    std::vector<std::string> names;
    for (const RecentBook& b : RECENT_BOOKS.newestBooks(max)) names.push_back(b.title.substr(2));
    return names;
  }
  std::vector<std::string> pageNames(uint32_t before, size_t max, uint32_t* last = nullptr) {
    std::vector<RecentBooksStore::Row> rows;
    RECENT_BOOKS.rowsBefore(before, max, rows);
    std::vector<std::string> names;
    for (const auto& row : rows) names.push_back(std::filesystem::path(row.path).stem().string());
    if (last && !rows.empty()) *last = rows.back().seq;
    return names;
  }
};

}  // namespace

TEST_F(RecentBooksStoreTest, WithNoFileTheListStartsEmpty) {
  EXPECT_EQ(0u, RECENT_BOOKS.count());
  EXPECT_TRUE(RECENT_BOOKS.newestBooks(10).empty());
  EXPECT_TRUE(std::filesystem::exists(binPath));
}

TEST_F(RecentBooksStoreTest, BooksComeNewestFirstAndAReopenedOneMovesToTheFront) {
  open("a");
  open("b");
  open("c");
  EXPECT_EQ((std::vector<std::string>{"c", "b", "a"}), homeNames());
  open("a");
  EXPECT_EQ((std::vector<std::string>{"a", "c", "b"}), homeNames());
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), homeNames(2));
  EXPECT_EQ(3u, RECENT_BOOKS.count());
  EXPECT_EQ(RecentBooksFile::kSlotsOffset + 3 * RecentBooksFile::kSlotSize, std::filesystem::file_size(binPath))
      << "a reopen takes no new slot";
}

TEST_F(RecentBooksStoreTest, TheHundredAndFirstBookTakesTheSlotOfTheOneOpenedLongestAgo) {
  for (int i = 0; i < 101; ++i) open("n" + std::to_string(i));
  EXPECT_EQ(100u, RECENT_BOOKS.count());
  const std::vector<std::string> names = homeNames(100);
  ASSERT_EQ(100u, names.size());
  EXPECT_EQ("n100", names.front());
  EXPECT_EQ("n1", names.back());
  EXPECT_EQ(RecentBooksFile::kSlotsOffset + 100 * RecentBooksFile::kSlotSize, std::filesystem::file_size(binPath));
}

TEST_F(RecentBooksStoreTest, ARemovedBookLeavesAndItsSlotIsTakenAgain) {
  open("a");
  open("b");
  RECENT_BOOKS.removeBook(bookAt("a"));
  EXPECT_EQ((std::vector<std::string>{"b"}), homeNames());
  open("c");
  EXPECT_EQ((std::vector<std::string>{"c", "b"}), homeNames());
  EXPECT_EQ(RecentBooksFile::kSlotsOffset + 2 * RecentBooksFile::kSlotSize, std::filesystem::file_size(binPath));
}

TEST_F(RecentBooksStoreTest, AMovedBookKeepsItsPlaceAndAnOldRowForItsNewPathGoes) {
  open("a");
  open("b");
  open("c");
  const std::string from = bookAt("b");
  const std::string to = (dir / "books" / "a.epub").string();  // a's old row would name the moved book twice
  std::filesystem::rename(from, to);
  RECENT_BOOKS.relocate(from, to, "", "");
  std::vector<RecentBooksStore::Row> rows;
  RECENT_BOOKS.rowsBefore(RecentBooksStore::kNewest, 10, rows);
  ASSERT_EQ(2u, rows.size());
  EXPECT_EQ(bookAt("c"), rows[0].path);
  EXPECT_EQ(to, rows[1].path);
  EXPECT_EQ("T b", RECENT_BOOKS.getBookByPath(to).title);
  EXPECT_TRUE(RECENT_BOOKS.getBookByPath(from).path.empty());
}

TEST_F(RecentBooksStoreTest, HomePassesOverAGoneBookAndTheLibraryDropsIt) {
  open("a");
  open("b");
  open("c");
  std::filesystem::remove(dir / "books" / "b.epub");
  EXPECT_EQ((std::vector<std::string>{"c", "a"}), homeNames());
  EXPECT_EQ(3u, RECENT_BOOKS.count()) << "Home keeps the row";
  EXPECT_EQ((std::vector<std::string>{"c", "a"}), pageNames(RecentBooksStore::kNewest, 10));
  EXPECT_EQ(2u, RECENT_BOOKS.count()) << "the Library's page drops it";
}

TEST_F(RecentBooksStoreTest, TheLibraryPagesThroughTheListByTheSeqAPageEndedOn) {
  for (int i = 0; i < 25; ++i) open("p" + std::to_string(i));
  uint32_t last = 0;
  const std::vector<std::string> first = pageNames(RecentBooksStore::kNewest, 10, &last);
  ASSERT_EQ(10u, first.size());
  EXPECT_EQ("p24", first.front());
  EXPECT_EQ("p15", first.back());
  const std::vector<std::string> second = pageNames(last, 10, &last);
  ASSERT_EQ(10u, second.size());
  EXPECT_EQ("p14", second.front());
  const std::vector<std::string> third = pageNames(last, 10, &last);
  ASSERT_EQ(5u, third.size());
  EXPECT_EQ("p0", third.back());
  EXPECT_TRUE(pageNames(last, 10).empty());
}

TEST_F(RecentBooksStoreTest, APageReadsOnPastGoneBooks) {
  for (int i = 0; i < 12; ++i) open("g" + std::to_string(i));
  for (int i = 11; i >= 6; --i) std::filesystem::remove(dir / "books" / ("g" + std::to_string(i) + ".epub"));
  EXPECT_EQ((std::vector<std::string>{"g5", "g4", "g3"}), pageNames(RecentBooksStore::kNewest, 3));
}

TEST_F(RecentBooksStoreTest, RecentJsonMovesOverOnceItsSettingsToTheBookAndThenItIsGone) {
  std::ofstream(jsonPath) << R"({"books":[]})";
  LegacyRecentBook newest;
  newest.book.path = bookAt("new");
  newest.book.title = "T new";
  newest.overrides.fontSizeOverride = 14;
  LegacyRecentBook older;
  older.book.path = bookAt("old");
  older.book.title = "T old";
  g_legacy = {newest, older};

  EXPECT_EQ((std::vector<std::string>{"new", "old"}), homeNames());
  EXPECT_FALSE(std::filesystem::exists(jsonPath));
  EXPECT_EQ(14, RECENT_BOOKS.readerOverrides(newest.book.path).fontSizeOverride);
  EXPECT_FALSE(RECENT_BOOKS.readerOverrides(older.book.path).any());
  open("third");
  EXPECT_EQ((std::vector<std::string>{"third", "new", "old"}), homeNames());
}

TEST_F(RecentBooksStoreTest, RecentJsonWaitsWhenThereIsNoMemoryToReadIt) {
  std::ofstream(jsonPath) << R"({"books":[]})";
  LegacyRecentBook row;
  row.book.path = bookAt("kept");
  row.book.title = "T kept";
  g_legacy = {row};
  g_load = RecentBooksLoad::ShortOfMemory;
  EXPECT_TRUE(homeNames().empty());
  EXPECT_TRUE(std::filesystem::exists(jsonPath));
  EXPECT_FALSE(std::filesystem::exists(binPath)) << "no empty list over the one still to move";
  g_load = RecentBooksLoad::Loaded;
  EXPECT_EQ((std::vector<std::string>{"kept"}), homeNames());
}

TEST_F(RecentBooksStoreTest, ASlotWrittenForANewBookWhoseEntryWasNotIsThatBook) {
  open("a");
  open("b");
  {
    // A power cut between the two writes of a new book: its slot, over a's, and not its entry.
    RecentBooksFile::File file;
    ASSERT_EQ(RecentBooksFile::File::Open::Ok, file.open(binPath.c_str()));
    RecentBook x;
    x.path = bookAt("x");
    x.title = "T x";
    ASSERT_TRUE(file.writeSlot(0, x));
  }
  EXPECT_EQ((std::vector<std::string>{"b", "x"}), homeNames());
  EXPECT_EQ("T x", RECENT_BOOKS.getBookByPath(bookAt("x")).title) << "its entry was put right";
}

TEST_F(RecentBooksStoreTest, ADamagedSlotIsDropped) {
  open("a");
  open("b");
  {
    std::fstream raw(binPath, std::ios::in | std::ios::out | std::ios::binary);
    raw.seekp(RecentBooksFile::kSlotsOffset + 20);
    raw.put('#');
  }
  EXPECT_EQ((std::vector<std::string>{"b"}), homeNames());
  EXPECT_EQ(1u, RECENT_BOOKS.count());
}

TEST_F(RecentBooksStoreTest, ASidecarChangeIsWrittenBackOnce) {
  open("a");
  int refreshed = 0;
  g_refresh = [&](RecentBook& book) {
    if (book.metadataStamp == 7) return false;
    book.title = "T edited";
    book.metadataStamp = 7;
    ++refreshed;
    return true;
  };
  EXPECT_EQ((std::vector<std::string>{"edited"}), homeNames());
  EXPECT_EQ((std::vector<std::string>{"edited"}), homeNames());
  EXPECT_EQ(1, refreshed);
}

TEST_F(RecentBooksStoreTest, ALookupFindsListedBooks) {
  open("a");
  open("b");
  RecentBooksStore::Lookup lookup;
  RecentBook found;
  ASSERT_TRUE(lookup.find(bookAt("a"), found));
  EXPECT_EQ("T a", found.title);
  EXPECT_FALSE(lookup.find(bookAt("z"), found));
}
