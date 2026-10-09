// Join merges a new walk with the previous index: books it knows keep their author and when they
// were first seen; books it does not are new (spec §3.5, phase 2).

#include <LibraryJoin.h>

#include <fstream>

#include "LibraryIndexFixture.h"

namespace {

class LibraryJoinTest : public LibraryIndexFixture {
 protected:
  library::StagedBook staged(const std::string& path, const uint32_t size = 100, const uint32_t date = 0,
                             const uint32_t sidecar = library::SIDECAR_NONE) const {
    const std::string file = path.substr(path.rfind('/') + 1);
    return {LibraryKeys::bookIdentity(file.c_str(), size), date, sidecar, appendString(at("paths.bin"), path)};
  }

  LibraryJoin::Result join(const std::vector<library::StagedBook>& books, const bool resolveAll = false) {
    writeAll(at("stage.bin"), books);
    LibraryJoin::Input in;
    in.stagePath = at("stage.bin");
    in.stageCount = static_cast<uint16_t>(books.size());
    in.previousPath = at("library.bin");
    in.resolveAll = resolveAll;
    in.recordsPath = at("records.bin");
    in.namesPath = at("names.bin");
    BuildArena framebuffer(48000);
    LibraryJoin::Result result;
    EXPECT_TRUE(LibraryJoin::join(in, framebuffer, result));
    return result;
  }

  std::vector<library::BookRecord> joined() const {
    std::ifstream in(at("records.bin"), std::ios::binary);
    std::vector<library::BookRecord> records;
    library::BookRecord record{};
    while (in.read(reinterpret_cast<char*>(&record), sizeof(record))) records.push_back(record);
    return records;
  }

  // Publishes what the join left, as the builder does before resolving anything.
  bool publishJoined(const LibraryJoin::Result& result) const {
    LibraryPublish::Input in;
    in.recordsPath = at("records.bin");
    in.pathsPath = at("paths.bin");
    in.namesPath = at("names.bin");
    in.bookCount = static_cast<uint16_t>(joined().size());
    in.buildGen = result.buildGen;
    BuildArena framebuffer(48000);
    return LibraryPublish::publish(in, framebuffer, at("library.bin"));
  }
};

TEST_F(LibraryJoinTest, AFirstBuildLeavesEveryBookPendingAndNew) {
  const auto result = join({staged("/c.epub"), staged("/a.epub"), staged("/b.epub")});
  EXPECT_EQ(result.pending, 3);
  EXPECT_EQ(result.buildGen, 1u);
  const auto records = joined();
  ASSERT_EQ(records.size(), 3u);
  for (size_t i = 0; i < records.size(); ++i) {
    EXPECT_EQ(records[i].authorHash, library::AUTHOR_PENDING);
    EXPECT_EQ(records[i].firstSeen, 1u);
    if (i > 0) {
      EXPECT_LT(records[i - 1].identity, records[i].identity) << "records must come out in identity order";
    }
  }
}

TEST_F(LibraryJoinTest, AuthorsAndFirstSeenAreCarriedByIdentity) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Disc/Mort.epub", "Terry Pratchett", 0, 4)}, 4));
  const auto result = join({staged("/Disc/Mort.epub")});
  EXPECT_EQ(result.pending, 0);
  EXPECT_EQ(result.buildGen, 4u) << "nothing new: the generation stays";
  const auto records = joined();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].authorHash, LibraryKeys::authorHash("Terry Pratchett"));
  EXPECT_EQ(records[0].firstSeen, 4u);
}

TEST_F(LibraryJoinTest, AMovedBookKeepsItsAuthorAndFirstSeen) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Inbox/Mort.epub", "Terry Pratchett", 0, 2)}, 2));
  const auto result = join({staged("/Discworld/Mort.epub")});
  EXPECT_EQ(result.pending, 0);
  EXPECT_EQ(joined()[0].firstSeen, 2u);
}

TEST_F(LibraryJoinTest, ANewBookTakesTheNextGenerationAndIsNewest) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Mort.epub", "Terry Pratchett", 5000, 1)}, 1));
  const auto result = join({staged("/Mort.epub", 100, 5000), staged("/Eric.epub", 100, 10)});
  EXPECT_EQ(result.buildGen, 2u);
  EXPECT_EQ(result.pending, 1);
  ASSERT_TRUE(publishJoined(result));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  uint16_t newest = 0;
  ASSERT_TRUE(index.newBook(0, newest));
  EXPECT_EQ(pathOf(index, newest), "/Eric.epub") << "newly seen beats a newer file date";
}

TEST_F(LibraryJoinTest, AChangedSidecarResolvesAgainButKeepsFirstSeen) {
  addName("Terry Pratchett");
  auto mort = book("/Mort.epub", "Terry Pratchett", 0, 3);
  mort.sidecarSig = 5;
  ASSERT_TRUE(publish({mort}, 3));
  const auto result = join({staged("/Mort.epub", 100, 0, 6)});
  EXPECT_EQ(result.pending, 1);
  EXPECT_EQ(joined()[0].firstSeen, 3u);
}

TEST_F(LibraryJoinTest, AnUnpairedSidecarAlwaysResolvesAgain) {
  addName("Terry Pratchett");
  auto mort = book("/Mort.epub", "Terry Pratchett");
  mort.sidecarSig = library::SIDECAR_UNKNOWN;
  ASSERT_TRUE(publish({mort}));
  EXPECT_EQ(join({staged("/Mort.epub", 100, 0, library::SIDECAR_UNKNOWN)}).pending, 1);
}

// Refresh library: every author is resolved again, but New must not reshuffle.
TEST_F(LibraryJoinTest, ResolveAllCarriesNoAuthorButKeepsFirstSeen) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Mort.epub", "Terry Pratchett", 0, 2)}, 2));
  const auto result = join({staged("/Mort.epub")}, true);
  EXPECT_EQ(result.pending, 1);
  EXPECT_EQ(joined()[0].firstSeen, 2u);
}

TEST_F(LibraryJoinTest, ThePreviousAuthorsNamesAreCarried) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Mort.epub", "Terry Pratchett")}));
  std::filesystem::remove(at("names.bin"));  // the join must write it afresh from the index
  const auto result = join({staged("/Mort.epub")});
  ASSERT_TRUE(publishJoined(result));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(authorNames(index), (std::vector<std::string>{"Terry Pratchett"}));
}

TEST_F(LibraryJoinTest, TwoCopiesOfABookBothKeepItsAuthor) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Mort.epub", "Terry Pratchett")}));
  const auto result = join({staged("/A/Mort.epub"), staged("/B/Mort.epub")});
  EXPECT_EQ(result.pending, 0);
  EXPECT_EQ(joined().size(), 2u);
}

// Review focus 3.
TEST_F(LibraryJoinTest, AnEmptyCardJoinsToNothing) {
  const auto result = join({});
  EXPECT_EQ(result.pending, 0);
  EXPECT_TRUE(joined().empty());
  EXPECT_TRUE(publishJoined(result));
}

// Plan 2a review I1: a nameless author must not be carried. Its blank entry would be taken for a
// file-as one and beat the real name for good, through every build and every Refresh.
TEST_F(LibraryJoinTest, ANamelessAuthorIsNotCarriedSoItsNameCanArriveLater) {
  ASSERT_TRUE(publish({book("/Ghost.epub", "Ghost Writer")}));  // resolved, its name never recorded
  const auto result = join({staged("/Ghost.epub")});
  addName("Ghost Writer");  // recorded by this build
  ASSERT_TRUE(publishJoined(result));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(authorNames(index), (std::vector<std::string>{"Ghost Writer"}));
}

// Plan 2a review I2: Refresh library carries no author, names included, so a corrected file-as
// takes effect.
TEST_F(LibraryJoinTest, ResolveAllCarriesNoNamesSoACorrectedFileAsTakesEffect) {
  addName("Ursula K. Le Guin", "Le Guin, Ursula K.");
  ASSERT_TRUE(publish({book("/Tehanu.epub", "Ursula K. Le Guin")}));
  const auto result = join({staged("/Tehanu.epub")}, true);
  // What the builder's resolve does next: record the author anew and patch the book's record.
  addName("Ursula K. Le Guin", "LeGuin, Ursula");
  auto records = joined();
  ASSERT_EQ(records.size(), 1u);
  records[0].authorHash = LibraryKeys::authorHash("Ursula K. Le Guin");
  writeAll(at("records.bin"), records);
  ASSERT_TRUE(publishJoined(result));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  library::AuthorRecord author{};
  std::string name;
  std::string filing;
  ASSERT_TRUE(index.author(0, author));
  ASSERT_TRUE(index.authorName(author, name, &filing));
  EXPECT_EQ(filing, "LeGuin, Ursula");
}

// Plan 2a review I3: a read error in the previous index fails the join. Taking it for the end of the
// index would make every later book new: New flooded, and their firstSeen lost at the next publish.
TEST_F(LibraryJoinTest, AReadErrorInThePreviousIndexFailsTheJoin) {
  ASSERT_TRUE(publish({book("/a.epub", ""), book("/b.epub", "")}));
  writeAll(at("stage.bin"), std::vector<library::StagedBook>{staged("/a.epub"), staged("/b.epub")});
  LibraryJoin::Input in;
  in.stagePath = at("stage.bin");
  in.stageCount = 2;
  in.previousPath = at("library.bin");
  in.recordsPath = at("records.bin");
  in.namesPath = at("names.bin");
  BuildArena framebuffer(48000);
  LibraryJoin::Result result;
  HalFile::failOneReadFrom = static_cast<long>(sizeof(library::Header) + sizeof(library::BookRecord));
  const bool joinedOk = LibraryJoin::join(in, framebuffer, result);
  HalFile::failOneReadFrom = -1;
  EXPECT_FALSE(joinedOk);
}

// The cap (user decision 2026-10-09): a full card's join fits the X4's 48,000-byte framebuffer.
TEST_F(LibraryJoinTest, AJoinAtTheCapFitsTheFramebuffer) {
  std::vector<library::StagedBook> books;
  for (int i = 0; i < library::MAX_BOOKS; ++i) books.push_back(staged("/b" + std::to_string(i) + ".epub"));
  writeAll(at("stage.bin"), books);
  LibraryJoin::Input in;
  in.stagePath = at("stage.bin");
  in.stageCount = library::MAX_BOOKS;
  in.previousPath = at("library.bin");
  in.recordsPath = at("records.bin");
  in.namesPath = at("names.bin");
  BuildArena framebuffer(48000);
  LibraryJoin::Result result;
  ASSERT_TRUE(LibraryJoin::join(in, framebuffer, result));
  EXPECT_EQ(result.pending, library::MAX_BOOKS);
  EXPECT_LE(framebuffer.highWater(), 32000u);
}

}  // namespace
