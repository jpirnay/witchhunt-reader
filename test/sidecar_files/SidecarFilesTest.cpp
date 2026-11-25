// SidecarFiles move/remove: the book browser hides a book's sidecars, so moving or deleting the
// book there has to take them along, or they stay behind where nothing lists them.

#include <SidecarFiles.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

class SidecarFilesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    root_ = fs::temp_directory_path() / ("sidecar_test_" + std::string(info->name()));
    fs::remove_all(root_);
    fs::create_directories(root_ / "from");
    fs::create_directories(root_ / "to");
  }
  void TearDown() override { fs::remove_all(root_); }

  std::string path(const std::string& rel) const { return (root_ / rel).string(); }
  void touch(const std::string& rel, const std::string& content = "x") const { std::ofstream(root_ / rel) << content; }
  bool exists(const std::string& rel) const { return fs::exists(root_ / rel); }
  std::string read(const std::string& rel) const {
    std::ifstream in(root_ / rel);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  fs::path root_;
};

TEST_F(SidecarFilesTest, MoveAllCarriesCoverAndMetadata) {
  touch("from/Book.epub");
  touch("from/Book.jpg");
  touch("from/Book.opf");

  EXPECT_FALSE(SidecarFiles::anyTargetTaken(path("from/Book.epub"), path("to/Book.epub")));
  EXPECT_TRUE(SidecarFiles::moveAll(path("from/Book.epub"), path("to/Book.epub")));

  EXPECT_TRUE(exists("to/Book.jpg"));
  EXPECT_TRUE(exists("to/Book.opf"));
  EXPECT_FALSE(exists("from/Book.jpg"));
  EXPECT_FALSE(exists("from/Book.opf"));
  // The book itself is the caller's to move.
  EXPECT_TRUE(exists("from/Book.epub"));
}

// A protected book's key and the licence its plugin wrote go where the book goes, and with it.
TEST_F(SidecarFilesTest, ProtectedBookKeyAndLicenceTravelWithIt) {
  touch("from/Loan.epub");
  touch("from/Loan.epub.key");
  touch("from/Loan.epub.rights");

  EXPECT_TRUE(SidecarFiles::moveAll(path("from/Loan.epub"), path("to/Loan.epub")));
  EXPECT_TRUE(exists("to/Loan.epub.key"));
  EXPECT_TRUE(exists("to/Loan.epub.rights"));
  EXPECT_FALSE(exists("from/Loan.epub.key"));
  EXPECT_FALSE(exists("from/Loan.epub.rights"));

  EXPECT_TRUE(SidecarFiles::removeAll(path("to/Loan.epub")));
  EXPECT_FALSE(exists("to/Loan.epub.key"));
  EXPECT_FALSE(exists("to/Loan.epub.rights"));
}

TEST_F(SidecarFilesTest, MoveAllWithNoSidecarsSucceeds) {
  touch("from/Book.epub");
  EXPECT_TRUE(SidecarFiles::moveAll(path("from/Book.epub"), path("to/Book.epub")));
}

TEST_F(SidecarFilesTest, AnyTargetTakenReportsAClash) {
  touch("from/Book.epub");
  touch("from/Book.png");
  touch("to/Book.png", "someone else's cover");

  EXPECT_TRUE(SidecarFiles::anyTargetTaken(path("from/Book.epub"), path("to/Book.epub")));
}

TEST_F(SidecarFilesTest, AnyTargetTakenIgnoresFilesTheBookDoesNotHave) {
  // A cover already in the destination only clashes when the book brings one of its own.
  touch("from/Book.epub");
  touch("to/Book.jpg");
  EXPECT_FALSE(SidecarFiles::anyTargetTaken(path("from/Book.epub"), path("to/Book.epub")));
}

TEST_F(SidecarFilesTest, RemoveAllDeletesSidecarsButNotTheBookOrItsNeighbours) {
  touch("from/Book.epub");
  touch("from/Book.jpg");
  touch("from/Book.opf");
  touch("from/Book 2.jpg");
  touch("from/Other.jpg");

  EXPECT_TRUE(SidecarFiles::removeAll(path("from/Book.epub")));

  EXPECT_FALSE(exists("from/Book.jpg"));
  EXPECT_FALSE(exists("from/Book.opf"));
  EXPECT_TRUE(exists("from/Book.epub"));
  EXPECT_TRUE(exists("from/Book 2.jpg"));
  EXPECT_TRUE(exists("from/Other.jpg"));
}

TEST_F(SidecarFilesTest, RemoveAllOnAnExtensionlessPathTouchesNothing) {
  touch("from/Book");
  touch("from/Book.jpg");
  EXPECT_TRUE(SidecarFiles::removeAll(path("from/Book")));
  EXPECT_TRUE(exists("from/Book.jpg"));
}

// A book moved where nothing took its sidecars along (the web File Manager, WebDAV, the serial transfer:
// BookRelocation::bookMoved()) keeps its key and licence: without the key it reported "no key". Its cover and metadata
// stay where they were, as before.
TEST_F(SidecarFilesTest, MoveProtectionCarriesTheKeyAndLicenceOnly) {
  touch("to/Renamed.epub");  // the book itself has moved already
  touch("from/Loan.epub.key", "key");
  touch("from/Loan.epub.rights", "rights");
  touch("from/Loan.jpg");
  touch("from/Loan.opf");

  EXPECT_TRUE(SidecarFiles::moveProtection(path("from/Loan.epub"), path("to/Renamed.epub")));

  EXPECT_EQ(read("to/Renamed.epub.key"), "key");
  EXPECT_EQ(read("to/Renamed.epub.rights"), "rights");
  EXPECT_FALSE(exists("from/Loan.epub.key"));
  EXPECT_FALSE(exists("from/Loan.epub.rights"));
  EXPECT_TRUE(exists("from/Loan.jpg"));
  EXPECT_TRUE(exists("from/Loan.opf"));
}

// A key already at the target belonged to a book no longer at that path: the moving book's own replaces it.
TEST_F(SidecarFilesTest, MoveProtectionReplacesAStaleKeyAtTheTarget) {
  touch("from/Loan.epub.key", "this book's key");
  touch("to/Loan.epub.key", "a deleted book's key");

  EXPECT_TRUE(SidecarFiles::moveProtection(path("from/Loan.epub"), path("to/Loan.epub")));

  EXPECT_EQ(read("to/Loan.epub.key"), "this book's key");
  EXPECT_FALSE(exists("from/Loan.epub.key"));
  EXPECT_FALSE(exists("to/Loan.epub.key.old"));
}

// The device's own moves take the sidecars along first, and a book in a moved folder has them with it already: nothing
// left at the old path is no failure, and what is at the target stays.
TEST_F(SidecarFilesTest, MoveProtectionWithNothingLeftBehindSucceeds) {
  touch("to/Loan.epub.key", "key");
  EXPECT_TRUE(SidecarFiles::moveProtection(path("from/Loan.epub"), path("to/Loan.epub")));
  EXPECT_EQ(read("to/Loan.epub.key"), "key");
}

// A change of case alone: FAT finds the key under either spelling, so nothing is renamed (a replace would set the file
// itself aside as its own old copy).
TEST_F(SidecarFilesTest, MoveProtectionLeavesACaseOnlyRenameAlone) {
  touch("from/Loan.epub.key", "key");
  EXPECT_TRUE(SidecarFiles::moveProtection(path("from/Loan.epub"), path("from/LOAN.EPUB")));
  EXPECT_EQ(read("from/Loan.epub.key"), "key");
}

}  // namespace
