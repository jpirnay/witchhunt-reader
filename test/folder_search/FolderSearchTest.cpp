// Whether a folder holds a book anywhere below it: the Library's Books tab leaves out the folders
// that do not. Runs over a real directory tree through the stdio storage shim.
#include <FolderSearch.h>
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace {

bool isBook(const char* name) {
  const std::string n(name);
  return n.size() > 5 && n.compare(n.size() - 5, 5, ".epub") == 0;
}

bool notHidden(const char* name) { return name[0] != '.'; }

// What a test tells the search it already knows, and what the search reports back.
struct Memo {
  std::map<std::string, int> known;
  std::set<std::string> foundNone;
  bool stop = false;
};

class FolderSearchTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    root_ = (fs::temp_directory_path() / ("folder_search_" + std::string(info->name()))).generic_string();
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  void TearDown() override { fs::remove_all(root_); }

  void file(const std::string& rel) const {
    const fs::path path = fs::path(root_) / rel;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "x";
  }
  void folder(const std::string& rel) const { fs::create_directories(fs::path(root_) / rel); }

  FolderSearch::Rules rules(Memo* memo = nullptr, const int maxDepth = 8) {
    FolderSearch::Rules r;
    r.listable = &notHidden;
    r.wanted = &isBook;
    r.maxDepth = maxDepth;
    if (memo != nullptr) {
      r.user = memo;
      r.known = [](void* user, const std::string& path) {
        const auto& known = static_cast<Memo*>(user)->known;
        const auto it = known.find(path);
        return it == known.end() ? -1 : it->second;
      };
      r.foundNone = [](void* user, const std::string& path) { static_cast<Memo*>(user)->foundNone.insert(path); };
      r.stop = [](void* user) { return static_cast<Memo*>(user)->stop; };
    }
    return r;
  }

  std::string at(const std::string& rel) const { return root_ + "/" + rel; }

  std::string root_;
};

}  // namespace

TEST_F(FolderSearchTest, ABookInTheFolderIsFound) {
  file("Pratchett/Mort.epub");
  EXPECT_TRUE(FolderSearch::anyBelow(at("Pratchett"), rules()));
}

TEST_F(FolderSearchTest, ABookSomeFoldersDownIsFound) {
  file("Pratchett/Discworld/Rincewind/Eric.epub");
  EXPECT_TRUE(FolderSearch::anyBelow(at("Pratchett"), rules()));
}

TEST_F(FolderSearchTest, AnEmptyFolderHasNone) {
  folder("Empty");
  EXPECT_FALSE(FolderSearch::anyBelow(at("Empty"), rules()));
}

// What a folder emptied of its books by a PC sync keeps: covers, sidecars, empty subfolders.
TEST_F(FolderSearchTest, CoversSidecarsAndEmptySubfoldersAreNotBooks) {
  file("Leftover/cover.jpg");
  file("Leftover/metadata.opf");
  folder("Leftover/Series/Empty");
  EXPECT_FALSE(FolderSearch::anyBelow(at("Leftover"), rules()));
}

TEST_F(FolderSearchTest, HiddenEntriesArePassedOver) {
  file("Shelf/.trash/Old.epub");
  file("Shelf/.Hidden.epub");
  EXPECT_FALSE(FolderSearch::anyBelow(at("Shelf"), rules()));
}

TEST_F(FolderSearchTest, BooksBelowTheDepthLimitAreNotLookedFor) {
  file("A/B/C/Deep.epub");  // two folder levels below A
  EXPECT_FALSE(FolderSearch::anyBelow(at("A"), rules(nullptr, 1)));
  EXPECT_TRUE(FolderSearch::anyBelow(at("A"), rules(nullptr, 2)));
}

// The folders searched to the end without a book are reported, so they are not walked again.
TEST_F(FolderSearchTest, FoldersSearchedWithoutAFindAreReported) {
  folder("Leftover/Series/Empty");
  file("Leftover/cover.jpg");
  Memo memo;
  EXPECT_FALSE(FolderSearch::anyBelow(at("Leftover"), rules(&memo)));
  EXPECT_EQ(memo.foundNone,
            (std::set<std::string>{at("Leftover"), at("Leftover/Series"), at("Leftover/Series/Empty")}));
}

// A folder cut off by the depth limit was not searched to its end: neither it nor its parents count
// as known to be empty.
TEST_F(FolderSearchTest, AFolderCutOffByTheDepthLimitIsNotReportedEmpty) {
  file("A/B/C/Deep.epub");  // two folder levels below A
  Memo memo;
  EXPECT_FALSE(FolderSearch::anyBelow(at("A"), rules(&memo, 1)));
  EXPECT_EQ(memo.foundNone.count(at("A")), 0u);
}

TEST_F(FolderSearchTest, WhatIsKnownIsAnsweredWithoutWalking) {
  file("Known/Mort.epub");
  folder("Counted");
  Memo memo;
  memo.known[at("Known")] = 0;    // trusted, though a book is there
  memo.known[at("Counted")] = 3;  // trusted, though it is empty
  EXPECT_FALSE(FolderSearch::anyBelow(at("Known"), rules(&memo)));
  EXPECT_TRUE(FolderSearch::anyBelow(at("Counted"), rules(&memo)));
}

TEST_F(FolderSearchTest, AKnownSubfolderIsNotEntered) {
  file("Shelf/Sub/Mort.epub");
  Memo memo;
  memo.known[at("Shelf/Sub")] = 0;
  EXPECT_FALSE(FolderSearch::anyBelow(at("Shelf"), rules(&memo)));
}

// Giving up answers "yes": a folder that may hold books stays listed.
TEST_F(FolderSearchTest, GivingUpKeepsTheFolder) {
  folder("Empty");
  Memo memo;
  memo.stop = true;
  EXPECT_TRUE(FolderSearch::anyBelow(at("Empty"), rules(&memo)));
  EXPECT_TRUE(memo.foundNone.empty());
}

TEST_F(FolderSearchTest, AFolderThatIsNotThereHasNone) { EXPECT_FALSE(FolderSearch::anyBelow(at("Missing"), rules())); }
