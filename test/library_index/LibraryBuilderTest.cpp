// The builder walks a card, joins, publishes and resolves a step at a time (spec §3.5). These tests
// run it over a real directory tree through the stdio storage shim.

#include <BuildArena.h>
#include <LibraryBuilder.h>
#include <LibraryFormat.h>
#include <LibraryIndexReader.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool isBook(const char* name) {
  const std::string n(name);
  const auto ends = [&n](const char* ext) {
    const size_t len = std::strlen(ext);
    return n.size() > len && n.compare(n.size() - len, len, ext) == 0;
  };
  return ends(".epub") || ends(".txt") || ends(".md") || ends(".xtc");
}

// The authors the card's books carry, by filename; and which ones cannot be read right now.
struct FakeCatalog {
  std::map<std::string, LibraryBuilder::Author> authors;
  std::set<std::string> unreadable;
  int calls = 0;
};

bool fakeResolve(void* user, const std::string& path, uint32_t, LibraryBuilder::Author& out, BuildArena* scratch) {
  auto& catalog = *static_cast<FakeCatalog*>(user);
  ++catalog.calls;
  EXPECT_NE(scratch, nullptr);
  const std::string file = path.substr(path.rfind('/') + 1);
  if (catalog.unreadable.count(file) != 0) return false;
  const auto found = catalog.authors.find(file);
  out = found != catalog.authors.end() ? found->second : LibraryBuilder::Author{};
  return true;
}

class LibraryBuilderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    card_ = (fs::temp_directory_path() / ("library_builder_" + std::string(info->name()))).generic_string();
    fs::remove_all(card_);
    fs::create_directories(card_);
  }
  void TearDown() override { fs::remove_all(card_); }

  void file(const std::string& rel, const size_t bytes = 100) const {
    const fs::path path = fs::path(card_) / rel;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << std::string(bytes, 'x');
  }

  std::string workDir() const { return card_ + "/.crosspoint/library"; }
  std::string indexPath() const { return workDir() + "/library.bin"; }

  LibraryBuilder::Config config() {
    LibraryBuilder::Config c;
    c.root = card_;
    c.workDir = workDir();
    c.indexPath = indexPath();
    c.isBook = &isBook;
    c.resolve = &fakeResolve;
    c.resolveUser = &catalog_;
    return c;
  }

  // Runs a build to its end; returns the phase it ended in and counts the steps and publishes.
  LibraryBuilder::Phase build(LibraryBuilder::Config c, int* walkSteps = nullptr, int* publishes = nullptr) {
    LibraryBuilder builder(std::move(c));
    BuildArena framebuffer(48000);
    for (int i = 0; i < 100000 && !builder.finished(); ++i) {
      if (walkSteps != nullptr && builder.phase() == LibraryBuilder::Phase::Walk) ++*walkSteps;
      builder.step(&framebuffer);
      if (builder.takePublished() && publishes != nullptr) ++*publishes;
    }
    return builder.phase();
  }

  std::vector<std::string> authorNames() const {
    LibraryIndexReader index;
    EXPECT_TRUE(index.open(indexPath()));
    std::vector<std::string> names;
    for (uint16_t i = 0; i < index.header().authorCount; ++i) {
      library::AuthorRecord author{};
      std::string name;
      EXPECT_TRUE(index.author(i, author));
      EXPECT_TRUE(index.authorName(author, name));
      if (author.hash == library::AUTHOR_UNKNOWN) name = "<unknown>";
      if (author.hash == library::AUTHOR_PENDING) name = "<pending>";
      names.push_back(name + " x" + std::to_string(author.count));
    }
    return names;
  }

  std::string card_;
  FakeCatalog catalog_;
};

TEST_F(LibraryBuilderTest, AFirstBuildIndexesTheBooksAndTheirAuthors) {
  file("Books/Mort.epub");
  file("Books/Eric.epub");
  file("Books/Notes.txt");
  file("Pictures/cover.jpg");
  catalog_.authors["Mort.epub"] = {"Terry Pratchett", ""};
  catalog_.authors["Eric.epub"] = {"Terry Pratchett", "Pratchett, Terry"};
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(indexPath()));
  EXPECT_EQ(index.header().bookCount, 3);
  EXPECT_EQ(authorNames(), (std::vector<std::string>{"Terry Pratchett x2", "<unknown> x1"}));
  EXPECT_EQ(catalog_.calls, 3);
}

TEST_F(LibraryBuilderTest, ASecondBuildResolvesNothingAgain) {
  file("Books/Mort.epub");
  catalog_.authors["Mort.epub"] = {"Terry Pratchett", ""};
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  catalog_.calls = 0;
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(catalog_.calls, 0);
  EXPECT_EQ(authorNames(), (std::vector<std::string>{"Terry Pratchett x1"}));
}

TEST_F(LibraryBuilderTest, AnEditedSidecarResolvesItsBookAgain) {
  file("Books/Mort.epub");
  file("Books/Mort.opf", 50);
  file("Books/Eric.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  catalog_.calls = 0;
  file("Books/Mort.opf", 60);  // edited: another size
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(catalog_.calls, 1);
}

TEST_F(LibraryBuilderTest, HiddenFoldersAreListedOnlyWhenShownAndTheCacheNever) {
  file("Books/Mort.epub");
  file(".hidden/Secret.epub");
  file(".crosspoint/Cached.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(indexPath()));
  EXPECT_EQ(index.header().bookCount, 1);
  EXPECT_EQ(index.header().acceptRules, 0);
  index.close();
  auto shown = config();
  shown.showHidden = true;
  ASSERT_EQ(build(shown), LibraryBuilder::Phase::Done);
  ASSERT_TRUE(index.open(indexPath()));
  EXPECT_EQ(index.header().bookCount, 2);
  EXPECT_EQ(index.header().acceptRules, 1);
}

// Review focus 3.
TEST_F(LibraryBuilderTest, ABookThatCannotBeReadNowStaysPendingAndIsAskedAgain) {
  file("Books/Mort.epub");
  catalog_.unreadable.insert("Mort.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(authorNames(), (std::vector<std::string>{"<pending> x1"}));
  catalog_.unreadable.clear();
  catalog_.authors["Mort.epub"] = {"Terry Pratchett", ""};
  catalog_.calls = 0;
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(catalog_.calls, 1);
  EXPECT_EQ(authorNames(), (std::vector<std::string>{"Terry Pratchett x1"}));
}

// Review focus 4.
TEST_F(LibraryBuilderTest, TheCapStopsTheWalkAndMarksTheIndexPartial) {
  file("Books/a.epub");
  file("Books/b.epub");
  file("Books/c.epub");
  auto capped = config();
  capped.maxBooks = 2;
  ASSERT_EQ(build(capped), LibraryBuilder::Phase::Done);
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(indexPath()));
  EXPECT_EQ(index.header().bookCount, 2);
  EXPECT_TRUE(index.partial());
}

TEST_F(LibraryBuilderTest, AuthorsArePublishedAsTheyAreResolved) {
  file("Books/a.epub");
  file("Books/b.epub");
  file("Books/c.epub");
  auto often = config();
  often.republishEvery = 1;
  int publishes = 0;
  ASSERT_EQ(build(often, nullptr, &publishes), LibraryBuilder::Phase::Done);
  EXPECT_GE(publishes, 4) << "the first publish, then one per resolved book";
}

TEST_F(LibraryBuilderTest, TheWalkYieldsBetweenSteps) {
  for (int i = 0; i < 100; ++i) file("Books/b" + std::to_string(i) + ".txt");
  int walkSteps = 0;
  ASSERT_EQ(build(config(), &walkSteps), LibraryBuilder::Phase::Done);
  EXPECT_GT(walkSteps, 3);
}

TEST_F(LibraryBuilderTest, TheWorkingFilesAreRemovedWhenDone) {
  file("Books/Mort.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_TRUE(fs::exists(indexPath()));
  for (const char* name : {"stage.bin", "paths.bin", "records.bin", "names.bin"}) {
    EXPECT_FALSE(fs::exists(workDir() + "/" + name)) << name;
  }
}

TEST_F(LibraryBuilderTest, NewListsTheBooksAddedSinceTheLastBuild) {
  file("Books/Old.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  file("Books/Fresh.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(indexPath()));
  uint16_t newest = 0;
  library::BookRecord record{};
  std::string path;
  ASSERT_TRUE(index.newBook(0, newest));
  ASSERT_TRUE(index.book(newest, record));
  ASSERT_TRUE(index.blobString(record.pathOff, path));
  EXPECT_EQ(path.substr(path.rfind('/') + 1), "Fresh.epub");
}

// Review focus 1.
TEST_F(LibraryBuilderTest, ABookGoneBeforeItsResolveStaysPending) {
  file("Books/Mort.epub");
  LibraryBuilder builder(config());
  BuildArena framebuffer(48000);
  while (!builder.finished() && builder.phase() != LibraryBuilder::Phase::Resolve) builder.step(&framebuffer);
  ASSERT_EQ(builder.phase(), LibraryBuilder::Phase::Resolve);
  fs::remove(fs::path(card_) / "Books/Mort.epub");
  catalog_.unreadable.insert("Mort.epub");  // as the real resolver finds a missing book
  while (!builder.finished()) builder.step(&framebuffer);
  EXPECT_EQ(builder.phase(), LibraryBuilder::Phase::Done);
}

// Review focus 2.
TEST_F(LibraryBuilderTest, AFolderWithTooManySidecarsMarksTheUnpairedBooksUnknown) {
  for (int i = 0; i < 130; ++i) file("Books/s" + std::to_string(i) + ".opf", 10);
  file("Books/zz.epub");  // its sidecar would sort after the 128 the table keeps
  file("Books/zz.opf", 10);
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  catalog_.calls = 0;
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(catalog_.calls, 1) << "an unpaired book goes through the exact details.bin check every build";
}

TEST_F(LibraryBuilderTest, PhasesThatNeedTheFramebufferWaitForIt) {
  file("Books/Mort.epub");
  LibraryBuilder builder(config());
  for (int i = 0; i < 1000 && builder.phase() == LibraryBuilder::Phase::Walk; ++i) builder.step(nullptr);
  ASSERT_EQ(builder.phase(), LibraryBuilder::Phase::Join);
  EXPECT_TRUE(builder.needsArena());
  EXPECT_EQ(builder.step(nullptr), LibraryBuilder::Phase::Join) << "no framebuffer, no progress";
}

}  // namespace
