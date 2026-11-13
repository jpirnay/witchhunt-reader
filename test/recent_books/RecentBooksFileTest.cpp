// recent.bin's layout: the table read a chunk at a time, slots round-tripped and checked, the file growing a slot at a
// time. Against real files in a folder of this process's own.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "RecentBooksFile.h"
#include "RecentBooksStore.h"

namespace {

using namespace RecentBooksFile;

class RecentBooksFileTest : public testing::Test {
 protected:
  std::filesystem::path dir;
  std::string path;

  void SetUp() override {
    dir =
        std::filesystem::temp_directory_path() / ("recent_books_file_" + std::to_string(static_cast<long>(::getpid())) +
                                                  "_" + testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    path = (dir / "recent.bin").string();
  }
  void TearDown() override { std::filesystem::remove_all(dir); }
};

RecentBook book(const std::string& name) {
  RecentBook b;
  b.path = "/Books/" + name + ".epub";
  b.title = "Title of " + name;
  b.author = "Author of " + name;
  b.series = "Series #2";
  b.coverBmpPath = "/.crosspoint/epub_1/thumb_[HEIGHT].bmp";
  b.metadataStamp = 0x12345678u;
  return b;
}

}  // namespace

TEST(RecentBooksPathHash, IsNeverZeroAndTellsPathsApart) {
  EXPECT_NE(0u, pathHash(""));
  EXPECT_NE(pathHash("/Books/a.epub"), pathHash("/Books/b.epub"));
  EXPECT_EQ(pathHash("/Books/a.epub"), pathHash("/Books/a.epub"));
}

TEST_F(RecentBooksFileTest, ANewFileHasEveryEntryFreeAndNoSlots) {
  File file;
  ASSERT_TRUE(file.create(path.c_str()));
  EXPECT_EQ(0, file.slotsOnCard());
  int visited = 0;
  bool anyUsed = false;
  ASSERT_TRUE(file.forEachEntry([&](uint8_t, const Entry& e) {
    ++visited;
    anyUsed = anyUsed || e.used();
  }));
  EXPECT_EQ(kSlotCount, visited);
  EXPECT_FALSE(anyUsed);
  EXPECT_EQ(kSlotsOffset, std::filesystem::file_size(path));
}

TEST_F(RecentBooksFileTest, EntriesRoundTripInSlotOrder) {
  File file;
  ASSERT_TRUE(file.create(path.c_str()));
  // Both ends and either side of a chunk boundary.
  for (const uint8_t slot : {0, 7, 8, 63, 64, 99}) {
    ASSERT_TRUE(file.writeEntry(slot, Entry{1000u + slot, 7u * slot + 1}));
  }
  file.close();
  ASSERT_EQ(File::Open::Ok, file.open(path.c_str()));
  std::vector<uint8_t> used;
  ASSERT_TRUE(file.forEachEntry([&](const uint8_t slot, const Entry& e) {
    if (!e.used()) return;
    used.push_back(slot);
    EXPECT_EQ(1000u + slot, e.hash);
    EXPECT_EQ(7u * slot + 1, e.seq);
  }));
  EXPECT_EQ((std::vector<uint8_t>{0, 7, 8, 63, 64, 99}), used);
  Entry one;
  ASSERT_TRUE(file.readEntry(63, one));
  EXPECT_EQ(1063u, one.hash);
}

TEST_F(RecentBooksFileTest, SlotsRoundTripAndTheFileGrowsOneAtATime) {
  File file;
  ASSERT_TRUE(file.create(path.c_str()));
  EXPECT_FALSE(file.writeSlot(1, book("a"))) << "a slot past the next one would leave a hole";
  ASSERT_TRUE(file.writeSlot(0, book("a")));
  ASSERT_TRUE(file.writeSlot(1, book("b")));
  EXPECT_EQ(2, file.slotsOnCard());
  EXPECT_EQ(kSlotsOffset + 2 * kSlotSize, std::filesystem::file_size(path));
  ASSERT_TRUE(file.writeSlot(0, book("c"))) << "rewritten in place";
  EXPECT_EQ(2, file.slotsOnCard());

  file.close();
  ASSERT_EQ(File::Open::Ok, file.open(path.c_str()));
  RecentBook out;
  ASSERT_TRUE(file.readSlot(0, out));
  const RecentBook c = book("c");
  EXPECT_EQ(c.path, out.path);
  EXPECT_EQ(c.title, out.title);
  EXPECT_EQ(c.author, out.author);
  EXPECT_EQ(c.series, out.series);
  EXPECT_EQ(c.coverBmpPath, out.coverBmpPath);
  EXPECT_EQ(c.metadataStamp, out.metadataStamp);
  ASSERT_TRUE(file.readSlot(1, out));
  EXPECT_EQ(book("b").path, out.path);
  EXPECT_FALSE(file.readSlot(2, out)) << "not on the card";
}

TEST_F(RecentBooksFileTest, ALongTitleIsCutAtACharacterAndATooLongPathIsRefused) {
  File file;
  ASSERT_TRUE(file.create(path.c_str()));
  RecentBook b = book("long");
  // ä is two bytes: one more byte would split it at the limit.
  b.title = std::string(kTitleMax - 1, 'x') + "\xC3\xA4" + "tail";
  ASSERT_TRUE(file.writeSlot(0, b));
  RecentBook out;
  ASSERT_TRUE(file.readSlot(0, out));
  EXPECT_EQ(std::string(kTitleMax - 1, 'x'), out.title);

  b.path = "/" + std::string(kPathMax, 'p');
  EXPECT_FALSE(file.writeSlot(1, b));
  EXPECT_EQ(1, file.slotsOnCard());
}

TEST_F(RecentBooksFileTest, ADamagedSlotIsNotRead) {
  File file;
  ASSERT_TRUE(file.create(path.c_str()));
  ASSERT_TRUE(file.writeSlot(0, book("a")));
  file.close();
  {
    std::fstream raw(path, std::ios::in | std::ios::out | std::ios::binary);
    raw.seekp(kSlotsOffset + 20);
    raw.put('#');
  }
  ASSERT_EQ(File::Open::Ok, file.open(path.c_str()));
  RecentBook out;
  EXPECT_FALSE(file.readSlot(0, out));
}

TEST_F(RecentBooksFileTest, AFileOfAnotherLayoutIsNotOpened) {
  {
    std::ofstream raw(path, std::ios::binary);
    raw << std::string(kSlotsOffset, 'z');
  }
  File file;
  EXPECT_EQ(File::Open::Foreign, file.open(path.c_str()));
  EXPECT_EQ(File::Open::Missing, file.open((dir / "missing.bin").string().c_str()));
  {
    std::ofstream raw(path, std::ios::binary | std::ios::trunc);
    raw << "RCNT";
  }
  EXPECT_EQ(File::Open::Foreign, file.open(path.c_str())) << "cut short";
}
