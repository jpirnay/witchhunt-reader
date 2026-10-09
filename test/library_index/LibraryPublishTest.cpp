// Publish assembles the book index from a build's working files; the reader serves it to the
// screens (spec §3.2, §3.5 phase 3).

#include <filesystem>
#include <fstream>

#include "LibraryIndexFixture.h"

namespace {

using LibraryPublishTest = LibraryIndexFixture;

TEST_F(LibraryPublishTest, APublishedIndexReadsBackWhatWasWritten) {
  addName("Terry Pratchett");
  const auto mort = book("/Books/Mort.epub", "Terry Pratchett", 7, 1);
  ASSERT_TRUE(publish({mort}, 3));

  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(index.header().bookCount, 1);
  EXPECT_EQ(index.header().buildGen, 3u);
  EXPECT_EQ(index.header().acceptRules, 1);
  EXPECT_FALSE(index.partial());
  library::BookRecord record{};
  ASSERT_TRUE(index.book(0, record));
  EXPECT_EQ(record.identity, mort.identity);
  EXPECT_EQ(record.authorHash, mort.authorHash);
  EXPECT_EQ(record.date, 7u);
  EXPECT_EQ(pathOf(index, 0), "/Books/Mort.epub");
}

TEST_F(LibraryPublishTest, AuthorsFileSurnameFirstWithUnknownAndPendingLast) {
  addName("Terry Pratchett");
  addName("Iain Banks");
  addName("Ursula K. Le Guin", "Le Guin, Ursula K.");
  auto pending = book("/c.epub", "Someone");
  pending.authorHash = library::AUTHOR_PENDING;
  ASSERT_TRUE(publish({book("/a.epub", "Terry Pratchett"), book("/b.epub", "Iain Banks"), pending,
                       book("/d.epub", "Ursula K. Le Guin"), book("/e.txt", "")}));

  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(authorNames(index),
            (std::vector<std::string>{"Iain Banks", "Ursula K. Le Guin", "Terry Pratchett", "<unknown>", "<pending>"}));
}

TEST_F(LibraryPublishTest, EachAuthorListsTheirOwnBooks) {
  addName("Terry Pratchett");
  addName("Iain Banks");
  ASSERT_TRUE(publish({book("/Disc/Mort.epub", "Terry Pratchett"), book("/Culture/Excession.epub", "Iain Banks"),
                       book("/Disc/Small Gods.epub", "Terry Pratchett")}));

  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  ASSERT_EQ(authorNames(index), (std::vector<std::string>{"Iain Banks", "Terry Pratchett"}));
  EXPECT_EQ(booksOf(index, 0), (std::vector<std::string>{"/Culture/Excession.epub"}));
  EXPECT_EQ(booksOf(index, 1), (std::vector<std::string>{"/Disc/Mort.epub", "/Disc/Small Gods.epub"}));
}

// New lists the books first seen most recently; within one build, the newest file date first.
TEST_F(LibraryPublishTest, NewIsByFirstSeenThenDate) {
  ASSERT_TRUE(publish({book("/old.epub", "", 900, 1), book("/later.epub", "", 100, 2), book("/latest.epub", "", 200, 2)}));

  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  ASSERT_EQ(index.header().newCount, 3);
  std::vector<std::string> order;
  for (uint16_t rank = 0; rank < 3; ++rank) {
    uint16_t record = 0;
    ASSERT_TRUE(index.newBook(rank, record));
    order.push_back(pathOf(index, record));
  }
  EXPECT_EQ(order, (std::vector<std::string>{"/latest.epub", "/later.epub", "/old.epub"}));
}

TEST_F(LibraryPublishTest, NewStopsAtTen) {
  std::vector<library::BookRecord> books;
  for (int i = 0; i < 15; ++i) books.push_back(book("/b" + std::to_string(i) + ".epub", "", static_cast<uint32_t>(i)));
  ASSERT_TRUE(publish(books));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(index.header().newCount, library::NEW_COUNT);
  uint16_t record = 0;
  ASSERT_TRUE(index.newBook(0, record));
  EXPECT_EQ(pathOf(index, record), "/b14.epub");
}

// The sort compares an 8-byte prefix first; authors sharing it are put in full-key order.
TEST_F(LibraryPublishTest, AuthorsSharingTheKeyPrefixAreInFullKeyOrder) {
  addName("Terry Pratchett");
  addName("Anne Pratchett");
  ASSERT_TRUE(publish({book("/t.epub", "Terry Pratchett"), book("/a.epub", "Anne Pratchett")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(authorNames(index), (std::vector<std::string>{"Anne Pratchett", "Terry Pratchett"}));
}

// Review focus 5: a tie run longer than MAX_TIE_RUN keeps hash order, but nothing is lost.
TEST_F(LibraryPublishTest, AVeryLongTieRunStillPublishesEveryAuthor) {
  std::vector<library::BookRecord> books;
  for (int i = 0; i < 40; ++i) {
    const std::string name = "Zed Aaaaaaaaxx" + std::to_string(i);
    addName(name);
    books.push_back(book("/z" + std::to_string(i) + ".epub", name));
  }
  ASSERT_TRUE(publish(books));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(index.header().authorCount, 40);
}

TEST_F(LibraryPublishTest, TheFirstNameEntryForAnAuthorWins) {
  addName("Terry Pratchett");
  addName("TERRY PRATCHETT");  // same hash, later entry
  ASSERT_TRUE(publish({book("/a.epub", "Terry Pratchett")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(authorNames(index), (std::vector<std::string>{"Terry Pratchett"}));
}

// Spec §3.4: if any of the author's books supplies a file-as, that is the key.
TEST_F(LibraryPublishTest, AFileAsEntryBeatsAnEarlierPlainOne) {
  addName("Ursula K. Le Guin");                        // a book without file-as: "guin ursula k le"
  addName("Ursula K. Le Guin", "Le Guin, Ursula K.");  // a later book with one
  ASSERT_TRUE(publish({book("/a.epub", "Ursula K. Le Guin"), book("/b.epub", "Ursula K. Le Guin")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  library::AuthorRecord author{};
  std::string name;
  std::string key;
  ASSERT_TRUE(index.author(0, author));
  ASSERT_TRUE(index.authorName(author, name, &key));
  EXPECT_EQ(key, "le guin ursula k");
}

// Review focus 1: power lost while the builder appended a name.
TEST_F(LibraryPublishTest, ANamesFileCutShortStillPublishes) {
  addName("Terry Pratchett");
  {
    std::ofstream out(at("names.bin"), std::ios::binary | std::ios::app);
    const uint32_t hash = LibraryKeys::authorHash("Iain Banks");
    const uint8_t flags = 0;
    const uint16_t len = 10;
    out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    out.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out.write("Iain", 4);
  }
  ASSERT_TRUE(publish({book("/a.epub", "Terry Pratchett"), book("/b.epub", "Iain Banks")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  const auto names = authorNames(index);
  EXPECT_NE(std::find(names.begin(), names.end(), "Terry Pratchett"), names.end());
}

// Review focus 2: a resolved author whose name never reached the names file.
TEST_F(LibraryPublishTest, AnAuthorWithNoNameEntryPublishesNameless) {
  ASSERT_TRUE(publish({book("/a.epub", "Ghost Writer")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  ASSERT_EQ(index.header().authorCount, 1);
  library::AuthorRecord author{};
  std::string name;
  ASSERT_TRUE(index.author(0, author));
  ASSERT_TRUE(index.authorName(author, name));
  EXPECT_EQ(name, "");
  EXPECT_EQ(booksOf(index, 0), (std::vector<std::string>{"/a.epub"}));
}

// Review focus 3.
TEST_F(LibraryPublishTest, AnEmptyCardPublishesAnEmptyIndex) {
  ASSERT_TRUE(publish({}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(index.header().bookCount, 0);
  EXPECT_EQ(index.header().authorCount, 0);
  EXPECT_EQ(index.header().newCount, 0);
}

// Review focus 4.
TEST_F(LibraryPublishTest, ALongNonAsciiPathRoundTrips) {
  std::string path = "/B\xC3\xBC" "cher/";
  while (path.size() < 300) path += "\xC3\xA4";
  path += ".epub";
  ASSERT_TRUE(publish({book(path, "")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(pathOf(index, 0), path);
}

TEST_F(LibraryPublishTest, ThePartialFlagIsRecorded) {
  ASSERT_TRUE(publish({book("/a.epub", "")}, 1, nullptr, true));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_TRUE(index.partial());
}

// A failed publish -- here the arena cannot hold the sort -- leaves the previous index as it was.
TEST_F(LibraryPublishTest, AFailedPublishLeavesThePreviousIndex) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/a.epub", "Terry Pratchett")}));
  const auto before = std::filesystem::file_size(at("library.bin"));
  BuildArena tiny(16);
  EXPECT_FALSE(publish({book("/a.epub", "Terry Pratchett"), book("/b.epub", "Terry Pratchett")}, 2, &tiny));
  EXPECT_EQ(std::filesystem::file_size(at("library.bin")), before);
  EXPECT_FALSE(std::filesystem::exists(at("library.bin.tmp")));
}

TEST_F(LibraryPublishTest, TheReaderRejectsATruncatedOrForeignFile) {
  ASSERT_TRUE(publish({book("/a.epub", "")}));
  const auto size = std::filesystem::file_size(at("library.bin"));
  std::filesystem::resize_file(at("library.bin"), size - 1);
  LibraryIndexReader index;
  EXPECT_FALSE(index.open(at("library.bin")));
  ASSERT_TRUE(publish({book("/a.epub", "")}));
  {
    std::fstream f(at("library.bin"), std::ios::in | std::ios::out | std::ios::binary);
    f.put('X');
  }
  EXPECT_FALSE(index.open(at("library.bin")));
  EXPECT_FALSE(index.open(at("missing.bin")));
}

}  // namespace
