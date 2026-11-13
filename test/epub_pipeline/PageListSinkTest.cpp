// A TOC parse that fails partway must not leave its printed-page list behind, and must drop it
// through the sink that holds it open: deleting pagelist.bin from outside left the sink to patch
// its count into, and close, a file that no longer existed.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub/parsers/PageListSink.h"
#include "HalStorage.h"
#include "Serialization.h"

namespace fs = std::filesystem;

namespace {

class PageListSinkTest : public testing::Test {
 protected:
  fs::path dir;

  void SetUp() override {
    dir = fs::temp_directory_path() /
          (std::string("page_list_sink_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  void TearDown() override { fs::remove_all(dir); }

  bool listExists() const { return fs::exists(dir / "pagelist.bin"); }
};

TEST_F(PageListSinkTest, DiscardDropsAListWithEntries) {
  PageListSink sink(dir.string());
  ASSERT_TRUE(sink.isOpen());
  sink.addEntry("c1.xhtml", "p1", "1");
  sink.addEntry("c1.xhtml", "p2", "2");
  sink.discard();
  EXPECT_FALSE(listExists());
}

TEST_F(PageListSinkTest, DiscardIsFinalAndTheDestructorWritesNothingBack) {
  {
    PageListSink sink(dir.string());
    sink.addEntry("c1.xhtml", "p1", "1");
    sink.discard();
    sink.addEntry("c1.xhtml", "p2", "2");
    sink.finalize();
  }
  EXPECT_FALSE(listExists());
}

TEST_F(PageListSinkTest, FinalizeKeepsAListWithEntries) {
  {
    PageListSink sink(dir.string());
    sink.addEntry("c1.xhtml", "p1", "1");
    sink.finalize();
  }
  EXPECT_TRUE(listExists());
}

// The sink gathers its writes in a buffer, and the readers read through a window: what goes in comes out, in order,
// across many buffers' worth, with the count patched at the front.
TEST_F(PageListSinkTest, ManyEntriesReadBackInOrder) {
  constexpr int kEntries = 300;  // ~9 KB, many times the sink's buffer and the readers' window
  {
    PageListSink sink(dir.string());
    ASSERT_TRUE(sink.isOpen());
    for (int i = 0; i < kEntries; i++) {
      sink.addEntry("OEBPS/chapter" + std::to_string(i / 25) + ".xhtml", "page" + std::to_string(i),
                    std::to_string(i + 1));
    }
    EXPECT_EQ(sink.entryCount(), kEntries);
    sink.finalize();
  }
  // Read back in the on-card format (u16 count, then three strings an entry), as the readers do.
  FsFile in;
  ASSERT_TRUE(Storage.openFileForRead("TST", (dir / "pagelist.bin").string(), in));
  uint16_t count = 0;
  serialization::readPod(in, count);
  EXPECT_EQ(count, kEntries);
  std::vector<std::string> labels;
  bool inOrder = true;
  std::string href, anchor, label;
  for (int index = 0; index < count; index++) {
    ASSERT_TRUE(serialization::readString(in, href) && serialization::readString(in, anchor) &&
                serialization::readString(in, label))
        << index;
    inOrder = inOrder && href == "OEBPS/chapter" + std::to_string(index / 25) + ".xhtml" &&
              anchor == "page" + std::to_string(index);
    labels.push_back(label);
  }
  in.close();
  ASSERT_EQ(labels.size(), static_cast<size_t>(kEntries));
  EXPECT_TRUE(inOrder);
  EXPECT_EQ(labels.front(), "1");
  EXPECT_EQ(labels.back(), std::to_string(kEntries));
}

TEST_F(PageListSinkTest, AListWithNoEntriesLeavesNoFile) {
  {
    PageListSink sink(dir.string());
    sink.finalize();
  }
  EXPECT_FALSE(listExists());
}

}  // namespace
