// FontCatalog: the font list in one exactly sized block -- built in two passes by its Builder, read by
// index, stashed to the card and read back whole.

#include <FontCatalog.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "FailingArrayNew.h"

namespace {

struct FileSpec {
  std::string name;
  uint32_t size = 0;
  uint32_t crc32 = 0;
  bool hasCrc32 = false;
};

struct FamilySpec {
  std::string name;
  std::string description;
  bool hasDescription = true;
  std::vector<FileSpec> files;
  bool keep = true;
};

void feed(FontCatalog::Builder& builder, const std::vector<FamilySpec>& families) {
  for (const auto& family : families) {
    builder.beginFamily();
    if (family.hasDescription) builder.description(family.description.data(), family.description.size());
    for (const auto& file : family.files) {
      builder.addFile(file.name.data(), file.name.size(), file.size, file.crc32, file.hasCrc32);
    }
    if (family.keep) {
      builder.keepFamily(family.name.data(), family.name.size());
    } else {
      builder.dropFamily();
    }
  }
}

// The two passes the manifest reader makes: count, allocate exactly that, fill.
FontCatalog::Size build(FontCatalog& catalog, const std::vector<FamilySpec>& families) {
  FontCatalog::Builder counter;
  feed(counter, families);
  EXPECT_FALSE(counter.overflowed());
  EXPECT_TRUE(catalog.allocate(counter.size()));
  FontCatalog::Builder filler(catalog);
  feed(filler, families);
  EXPECT_FALSE(filler.overflowed());
  EXPECT_EQ(filler.size(), counter.size());
  return counter.size();
}

// Every string a kept family puts in the block, with its terminator; a family without a description
// still has one, empty.
size_t stringBytesOf(const std::vector<FamilySpec>& families) {
  size_t bytes = 0;
  for (const auto& family : families) {
    if (!family.keep) continue;
    bytes += family.name.size() + 1 + family.description.size() + 1;
    for (const auto& file : family.files) bytes += file.name.size() + 1;
  }
  return bytes;
}

const std::vector<FamilySpec> kSample = {
    {"Alpha",
     "First",
     true,
     {{"Alpha/Alpha_10.cpfont", 1000, 11, true}, {"Alpha/Alpha_12.cpfont", 2000, 22, true}},
     true},
    {"Beta", "", false, {{"Beta_10.cpfont", 3000, 0, false}}, true},
    {"Gamma", "Third", true, {}, true},
};

// Everything a reader can see, for comparing two catalogs.
std::string dump(const FontCatalog& catalog) {
  std::string out;
  for (size_t i = 0; i < catalog.count(); ++i) {
    out += std::string(catalog.name(i)) + "|" + catalog.description(i) + "|" + std::to_string(catalog.totalSize(i)) +
           "|" + std::to_string(catalog.installed(i)) + std::to_string(catalog.hasUpdate(i)) +
           std::to_string(catalog.hasResumableDownload(i)) + "\n";
    for (size_t j = 0; j < catalog.fileCount(i); ++j) {
      out += std::string("  ") + catalog.fileName(i, j) + "|" + catalog.fileLocalName(i, j) + "|" +
             std::to_string(catalog.fileSize(i, j)) + "|" + std::to_string(catalog.fileCrc32(i, j)) + "|" +
             std::to_string(catalog.fileHasCrc32(i, j)) + "\n";
    }
  }
  return out;
}

std::string stashBytes(const FontCatalog& catalog) {
  HalFile file = HalFile::forReadWrite();
  EXPECT_TRUE(catalog.writeTo(file));
  std::string bytes(file.size(), '\0');
  file.seekSet(0);
  EXPECT_EQ(file.read(bytes.data(), bytes.size()), static_cast<int>(bytes.size()));
  return bytes;
}

void putU16(std::string& bytes, const size_t at, const uint16_t value) { std::memcpy(&bytes[at], &value, 2); }
void putU32(std::string& bytes, const size_t at, const uint32_t value) { std::memcpy(&bytes[at], &value, 4); }

// Where the documented layout puts things (FontCatalog.h).
constexpr size_t HEADER = 16;
constexpr size_t RECORD = 12;
size_t familyAt(const size_t i) { return HEADER + RECORD * i; }
size_t fileAt(const size_t families, const size_t j) { return HEADER + RECORD * families + RECORD * j; }

}  // namespace

TEST(FontCatalogTest, ReadsBackEveryFieldOfEveryFamily) {
  FontCatalog catalog;
  build(catalog, kSample);

  ASSERT_EQ(catalog.count(), 3u);
  EXPECT_FALSE(catalog.empty());
  EXPECT_EQ(catalog.totalFiles(), 3u);

  EXPECT_STREQ(catalog.name(0), "Alpha");
  EXPECT_STREQ(catalog.description(0), "First");
  ASSERT_EQ(catalog.fileCount(0), 2u);
  EXPECT_EQ(catalog.totalSize(0), 3000u);
  EXPECT_STREQ(catalog.fileName(0, 0), "Alpha/Alpha_10.cpfont");
  EXPECT_STREQ(catalog.fileLocalName(0, 0), "Alpha_10.cpfont");
  EXPECT_EQ(catalog.fileSize(0, 0), 1000u);
  EXPECT_EQ(catalog.fileCrc32(0, 0), 11u);
  EXPECT_TRUE(catalog.fileHasCrc32(0, 0));
  EXPECT_STREQ(catalog.fileName(0, 1), "Alpha/Alpha_12.cpfont");
  EXPECT_EQ(catalog.fileSize(0, 1), 2000u);
  EXPECT_EQ(catalog.fileCrc32(0, 1), 22u);

  EXPECT_STREQ(catalog.name(1), "Beta");
  EXPECT_STREQ(catalog.description(1), "");
  ASSERT_EQ(catalog.fileCount(1), 1u);
  EXPECT_EQ(catalog.totalSize(1), 3000u);
  EXPECT_STREQ(catalog.fileName(1, 0), "Beta_10.cpfont");
  EXPECT_STREQ(catalog.fileLocalName(1, 0), "Beta_10.cpfont");
  EXPECT_FALSE(catalog.fileHasCrc32(1, 0));
  EXPECT_EQ(catalog.fileCrc32(1, 0), 0u);

  EXPECT_STREQ(catalog.name(2), "Gamma");
  EXPECT_STREQ(catalog.description(2), "Third");
  EXPECT_EQ(catalog.fileCount(2), 0u);
  EXPECT_EQ(catalog.totalSize(2), 0u);

  for (size_t i = 0; i < catalog.count(); ++i) {
    EXPECT_FALSE(catalog.installed(i));
    EXPECT_FALSE(catalog.hasUpdate(i));
    EXPECT_FALSE(catalog.hasResumableDownload(i));
  }
}

// The counting pass and the filling pass agree to the byte: the block holds the records and the
// strings, and nothing else.
TEST(FontCatalogTest, TheCountingPassSizesTheBlockExactly) {
  FontCatalog catalog;
  const FontCatalog::Size size = build(catalog, kSample);

  EXPECT_EQ(size.families, 3u);
  EXPECT_EQ(size.files, 3u);
  EXPECT_EQ(size.stringBytes, stringBytesOf(kSample));
  EXPECT_EQ(size.stringBytes, 89u);
  EXPECT_EQ(FontCatalog::blockBytesFor(size), HEADER + RECORD * 3 + RECORD * 3 + 89);
  EXPECT_EQ(catalog.blockBytes(), FontCatalog::blockBytesFor(size));

  const std::string bytes = stashBytes(catalog);
  EXPECT_EQ(bytes.size(), catalog.blockBytes());
  EXPECT_EQ(bytes.back(), '\0');  // the last string ends at the block's last byte
}

// A writing pass given more than it counted -- the events changed between the passes -- never writes
// past the block: what does not fit is dropped and reported, and what was kept stays intact.
TEST(FontCatalogTest, AFillingPassNeverWritesPastTheBlock) {
  FontCatalog::Builder counter;
  feed(counter, kSample);
  FontCatalog catalog;
  ASSERT_TRUE(catalog.allocate(counter.size()));

  std::vector<FamilySpec> more = kSample;
  more[2].description = "Third, now much longer than it was counted";
  more.push_back({"Delta", "Extra", true, {{"Delta/Delta_10.cpfont", 5, 0, false}}, true});
  FontCatalog::Builder filler(catalog);
  feed(filler, more);

  EXPECT_TRUE(filler.overflowed());
  EXPECT_EQ(catalog.blockBytes(), FontCatalog::blockBytesFor(counter.size()));
  EXPECT_STREQ(catalog.name(0), "Alpha");
  EXPECT_STREQ(catalog.fileName(0, 1), "Alpha/Alpha_12.cpfont");
  EXPECT_STREQ(catalog.name(1), "Beta");
  EXPECT_EQ(stashBytes(catalog).back(), '\0');
}

// A family dropped after its description and files came in takes no room, in either pass -- not even
// when it would not fit the block at all.
TEST(FontCatalogTest, ADroppedFamilyTakesNoRoom) {
  std::vector<FamilySpec> families = {
      {"Rejected",
       std::string(400, 'd'),
       true,
       {{"Rejected/a.cpfont", 7, 1, true}, {"Rejected/b.cpfont", 8, 2, true}},
       false},
      kSample[0],
      {"AlsoRejected", "x", true, {{"x.cpfont", 1, 0, false}}, false},
      kSample[2],
  };
  FontCatalog catalog;
  const FontCatalog::Size size = build(catalog, families);

  EXPECT_EQ(size.families, 2u);
  EXPECT_EQ(size.files, 2u);
  EXPECT_EQ(size.stringBytes, stringBytesOf(families));
  ASSERT_EQ(catalog.count(), 2u);
  EXPECT_STREQ(catalog.name(0), "Alpha");
  EXPECT_EQ(catalog.fileCount(0), 2u);
  EXPECT_STREQ(catalog.fileName(0, 0), "Alpha/Alpha_10.cpfont");
  EXPECT_STREQ(catalog.name(1), "Gamma");
  EXPECT_EQ(catalog.fileCount(1), 0u);
}

// Strings are taken by length, not up to a terminator the caller may not have, and stop at an
// embedded NUL -- in both passes alike.
TEST(FontCatalogTest, StringsStopAtTheirLengthOrAnEmbeddedNul) {
  const std::string description = "First|not this";
  const std::string name("Ab\0cd", 5);
  FontCatalog::Builder counter;
  FontCatalog catalog;
  for (int pass = 0; pass < 2; ++pass) {
    FontCatalog::Builder filler(catalog);
    FontCatalog::Builder& builder = pass == 0 ? counter : filler;
    builder.beginFamily();
    builder.description(description.data(), 5);
    builder.keepFamily(name.data(), name.size());
    if (pass == 0) {
      EXPECT_EQ(counter.size().stringBytes, 6u + 3u);
      ASSERT_TRUE(catalog.allocate(counter.size()));
    } else {
      EXPECT_FALSE(filler.overflowed());
      EXPECT_EQ(filler.size(), counter.size());
    }
  }
  EXPECT_STREQ(catalog.description(0), "First");
  EXPECT_STREQ(catalog.name(0), "Ab");
}

TEST(FontCatalogTest, TheLastDescriptionGivenWins) {
  FontCatalog::Builder counter;
  FontCatalog catalog;
  for (int pass = 0; pass < 2; ++pass) {
    FontCatalog::Builder filler(catalog);
    FontCatalog::Builder& builder = pass == 0 ? counter : filler;
    builder.beginFamily();
    builder.description("one", 3);
    builder.description("two", 3);
    builder.keepFamily("F", 1);
    if (pass == 0) {
      ASSERT_TRUE(catalog.allocate(counter.size()));
    }
  }
  EXPECT_STREQ(catalog.description(0), "two");
}

// Only the family's own folder comes off: a name in another folder, or one that merely starts with
// the family's name, is the local name as it stands.
TEST(FontCatalogTest, TheLocalNameDropsOnlyTheFamilysOwnFolder) {
  FontCatalog catalog;
  build(catalog, {{"Alpha",
                   "",
                   true,
                   {{"Alpha/Alpha_10.cpfont", 1, 0, false},
                    {"Other/Alpha_12.cpfont", 1, 0, false},
                    {"Alphabet/x.cpfont", 1, 0, false},
                    {"Alpha_14.cpfont", 1, 0, false},
                    {"Alpha", 1, 0, false}},
                   true}});
  EXPECT_STREQ(catalog.fileLocalName(0, 0), "Alpha_10.cpfont");
  EXPECT_STREQ(catalog.fileLocalName(0, 1), "Other/Alpha_12.cpfont");
  EXPECT_STREQ(catalog.fileLocalName(0, 2), "Alphabet/x.cpfont");
  EXPECT_STREQ(catalog.fileLocalName(0, 3), "Alpha_14.cpfont");
  EXPECT_STREQ(catalog.fileLocalName(0, 4), "Alpha");
}

TEST(FontCatalogTest, FlagsAreKeptPerFamily) {
  FontCatalog catalog;
  build(catalog, kSample);

  catalog.setInstalled(0, true);
  catalog.setHasUpdate(0, true);
  catalog.setHasResumableDownload(2, true);
  EXPECT_TRUE(catalog.installed(0));
  EXPECT_TRUE(catalog.hasUpdate(0));
  EXPECT_FALSE(catalog.hasResumableDownload(0));
  EXPECT_FALSE(catalog.installed(1));
  EXPECT_FALSE(catalog.hasUpdate(1));
  EXPECT_FALSE(catalog.installed(2));
  EXPECT_TRUE(catalog.hasResumableDownload(2));

  catalog.setHasUpdate(0, false);
  EXPECT_TRUE(catalog.installed(0));
  EXPECT_FALSE(catalog.hasUpdate(0));
  // Setting a flag does not disturb what else the record holds.
  EXPECT_STREQ(catalog.name(0), "Alpha");
  EXPECT_EQ(catalog.fileCount(0), 2u);
  EXPECT_EQ(catalog.totalSize(0), 3000u);
}

TEST(FontCatalogTest, AnIndexOutOfRangeIsHarmless) {
  FontCatalog catalog;
  build(catalog, kSample);

  EXPECT_STREQ(catalog.name(3), "");
  EXPECT_STREQ(catalog.description(3), "");
  EXPECT_EQ(catalog.fileCount(3), 0u);
  EXPECT_EQ(catalog.totalSize(3), 0u);
  EXPECT_STREQ(catalog.fileName(3, 0), "");
  EXPECT_STREQ(catalog.fileName(0, 2), "");  // Alpha has two files; this is not Beta's first
  EXPECT_STREQ(catalog.fileLocalName(0, 2), "");
  EXPECT_EQ(catalog.fileSize(0, 2), 0u);
  EXPECT_EQ(catalog.fileCrc32(0, 2), 0u);
  EXPECT_FALSE(catalog.fileHasCrc32(0, 2));
  EXPECT_FALSE(catalog.installed(3));
  catalog.setInstalled(3, true);
  catalog.setHasUpdate(3, true);
  catalog.setHasResumableDownload(3, true);
  for (size_t i = 0; i < catalog.count(); ++i) {  // no flag set anywhere
    EXPECT_FALSE(catalog.installed(i));
    EXPECT_FALSE(catalog.hasUpdate(i));
    EXPECT_FALSE(catalog.hasResumableDownload(i));
  }
}

TEST(FontCatalogTest, AnEmptyCatalogHoldsNoBlock) {
  FontCatalog catalog;
  EXPECT_EQ(catalog.count(), 0u);
  EXPECT_TRUE(catalog.empty());
  EXPECT_EQ(catalog.totalFiles(), 0u);
  EXPECT_EQ(catalog.blockBytes(), 0u);
  EXPECT_STREQ(catalog.name(0), "");
  HalFile file = HalFile::forReadWrite();
  EXPECT_FALSE(catalog.writeTo(file));
  EXPECT_EQ(file.size(), 0u);

  // A manifest whose every family fails its checks: nothing to allocate, and that is no failure.
  build(catalog, {{"Rejected", "", true, {}, false}});
  EXPECT_TRUE(catalog.empty());
  EXPECT_EQ(catalog.blockBytes(), 0u);

  build(catalog, kSample);
  ASSERT_EQ(catalog.count(), 3u);
  catalog.clear();
  EXPECT_TRUE(catalog.empty());
  EXPECT_EQ(catalog.blockBytes(), 0u);
}

TEST(FontCatalogTest, RefusesASizeTheFormatCannotHold) {
  EXPECT_TRUE(FontCatalog::fits({65535, 65535, 65535}));
  EXPECT_FALSE(FontCatalog::fits({65536, 0, 0}));
  EXPECT_FALSE(FontCatalog::fits({1, 65536, 2}));
  EXPECT_FALSE(FontCatalog::fits({1, 0, 65536}));

  FontCatalog catalog;
  build(catalog, kSample);
  EXPECT_FALSE(catalog.allocate({1, 0, 65536}));
  EXPECT_TRUE(catalog.empty());  // and what it held is gone
}

TEST(FontCatalogTest, MovingHandsTheBlockOver) {
  FontCatalog catalog;
  build(catalog, kSample);
  const std::string before = dump(catalog);
  const char* name = catalog.name(0);

  FontCatalog other(std::move(catalog));
  EXPECT_EQ(dump(other), before);
  EXPECT_EQ(other.name(0), name);  // the same block, not a copy

  FontCatalog third;
  third.swap(other);
  EXPECT_TRUE(other.empty());
  EXPECT_EQ(dump(third), before);
}

// The stash round trip: the whole block out in one write, back in one read, identical -- flags too.
TEST(FontCatalogTest, AStashReadsBackIdentical) {
  FontCatalog catalog;
  build(catalog, kSample);
  catalog.setInstalled(0, true);
  catalog.setHasUpdate(0, true);
  catalog.setHasResumableDownload(1, true);
  const std::string bytes = stashBytes(catalog);
  EXPECT_EQ(bytes.size(), catalog.blockBytes());

  HalFile file = HalFile::fromString(bytes);
  FontCatalog restored;
  ASSERT_TRUE(restored.readFrom(file));
  EXPECT_EQ(dump(restored), dump(catalog));
  EXPECT_TRUE(restored.installed(0));
  EXPECT_TRUE(restored.hasUpdate(0));
  EXPECT_TRUE(restored.hasResumableDownload(1));
  EXPECT_FALSE(restored.installed(1));
  EXPECT_EQ(restored.blockBytes(), catalog.blockBytes());
  EXPECT_EQ(stashBytes(restored), bytes);

  // Read from the start, wherever the file stood.
  HalFile written = HalFile::forReadWrite();
  ASSERT_TRUE(catalog.writeTo(written));
  FontCatalog again;
  ASSERT_TRUE(again.readFrom(written));
  EXPECT_EQ(dump(again), dump(catalog));
}

TEST(FontCatalogTest, ACorruptOrShortStashIsRejected) {
  FontCatalog catalog;
  build(catalog, kSample);
  const std::string good = stashBytes(catalog);
  const size_t families = 3;
  const size_t strings = fileAt(families, 3);
  const uint16_t stringBytes = static_cast<uint16_t>(good.size() - strings);

  struct Case {
    const char* what;
    std::string bytes;
  };
  std::vector<Case> cases;
  cases.push_back({"empty", ""});
  cases.push_back({"header only", good.substr(0, HEADER)});
  cases.push_back({"one byte short", good.substr(0, good.size() - 1)});
  cases.push_back({"one byte over", good + '\0'});
  cases.push_back({"not a catalog", good});
  cases.back().bytes[0] ^= 0x20;
  cases.push_back({"another version", good});
  putU16(cases.back().bytes, 4, 1);
  cases.push_back({"counts that do not match the size", good});
  putU16(cases.back().bytes, 6, 4);
  cases.push_back({"no families", good});
  putU16(cases.back().bytes, 6, 0);
  cases.push_back({"a family name past the strings", good});
  putU16(cases.back().bytes, familyAt(1) + 4, stringBytes);
  cases.push_back({"a description past the strings", good});
  putU16(cases.back().bytes, familyAt(2) + 6, 0xFFFF);
  cases.push_back({"a file name past the strings", good});
  putU16(cases.back().bytes, fileAt(families, 2) + 8, stringBytes);
  cases.push_back({"strings without a final terminator", good});
  cases.back().bytes.back() = 'x';
  cases.push_back({"a first family that does not start at the first file", good});
  putU16(cases.back().bytes, familyAt(0) + 8, 1);
  cases.push_back({"files out of order", good});
  putU16(cases.back().bytes, familyAt(2) + 8, 1);
  cases.push_back({"files past the last", good});
  putU16(cases.back().bytes, familyAt(2) + 8, 4);
  cases.push_back({"a total that does not add up", good});
  putU32(cases.back().bytes, familyAt(0), 2999);

  for (const Case& c : cases) {
    SCOPED_TRACE(c.what);
    FontCatalog restored;
    build(restored, kSample);  // whatever it held is dropped
    HalFile file = HalFile::fromString(c.bytes);
    EXPECT_FALSE(restored.readFrom(file));
    EXPECT_TRUE(restored.empty());
  }

  FontCatalog restored;
  HalFile unreadable;  // no backing data: size 0, reads fail
  EXPECT_FALSE(restored.readFrom(unreadable));
  EXPECT_TRUE(restored.empty());

  // The untouched stash still reads, so it is the corruption each case is rejected for.
  HalFile file = HalFile::fromString(good);
  EXPECT_TRUE(restored.readFrom(file));
}

TEST(FontCatalogTest, ACopyHoldsTheSelectedFamiliesAndTheirFlags) {
  FontCatalog catalog;
  build(catalog, kSample);
  catalog.setInstalled(0, true);
  catalog.setHasUpdate(0, true);
  catalog.setInstalled(1, true);
  catalog.setHasResumableDownload(2, true);

  // As the web install selects them: not installed, or installed with an update.
  FontCatalog targets;
  ASSERT_TRUE(targets.copyFrom(
      catalog, [](const void*, const FontCatalog& from, size_t i) { return !from.installed(i) || from.hasUpdate(i); },
      nullptr));
  ASSERT_EQ(targets.count(), 2u);
  EXPECT_STREQ(targets.name(0), "Alpha");
  EXPECT_TRUE(targets.installed(0));
  EXPECT_TRUE(targets.hasUpdate(0));
  ASSERT_EQ(targets.fileCount(0), 2u);
  EXPECT_STREQ(targets.fileName(0, 1), "Alpha/Alpha_12.cpfont");
  EXPECT_EQ(targets.fileCrc32(0, 1), 22u);
  EXPECT_EQ(targets.totalSize(0), 3000u);
  EXPECT_STREQ(targets.name(1), "Gamma");
  EXPECT_STREQ(targets.description(1), "Third");
  EXPECT_TRUE(targets.hasResumableDownload(1));
  EXPECT_EQ(targets.blockBytes(),
            FontCatalog::blockBytesFor({2, 2, stringBytesOf({kSample[0], kSample[2]})}));  // exactly sized

  FontCatalog one;
  ASSERT_TRUE(one.copyFamilyFrom(catalog, 1));
  ASSERT_EQ(one.count(), 1u);
  EXPECT_STREQ(one.name(0), "Beta");
  EXPECT_STREQ(one.description(0), "");
  EXPECT_TRUE(one.installed(0));
  EXPECT_STREQ(one.fileName(0, 0), "Beta_10.cpfont");
  EXPECT_FALSE(one.fileHasCrc32(0, 0));
  EXPECT_EQ(one.blockBytes(), FontCatalog::blockBytesFor({1, 1, stringBytesOf({kSample[1]})}));

  FontCatalog all;
  ASSERT_TRUE(all.copyFrom(catalog, nullptr, nullptr));
  EXPECT_EQ(dump(all), dump(catalog));

  FontCatalog none;
  build(none, kSample);
  EXPECT_TRUE(none.copyFrom(catalog, [](const void*, const FontCatalog&, size_t) { return false; }, nullptr));
  EXPECT_TRUE(none.empty());

  EXPECT_FALSE(one.copyFamilyFrom(catalog, 3));
  EXPECT_TRUE(one.empty());
  EXPECT_FALSE(all.copyFrom(all, nullptr, nullptr));
  EXPECT_TRUE(all.empty());
}

// The heap cannot give the block: every path that allocates one fails cleanly and holds nothing.
TEST(FontCatalogTest, ABlockTheHeapCannotGiveLeavesTheCatalogEmpty) {
  FontCatalog catalog;
  build(catalog, kSample);
  const std::string stash = stashBytes(catalog);

  FontCatalog target;
  build(target, kSample);
  {
    const FailingArrayNew noHeap(1);
    EXPECT_FALSE(target.allocate({1, 0, 2}));
    EXPECT_TRUE(target.empty());  // and what it held is gone

    HalFile file = HalFile::fromString(stash);
    EXPECT_FALSE(target.readFrom(file));
    EXPECT_TRUE(target.empty());

    EXPECT_FALSE(target.copyFrom(catalog, nullptr, nullptr));
    EXPECT_TRUE(target.empty());
    EXPECT_FALSE(target.copyFamilyFrom(catalog, 0));
    EXPECT_TRUE(target.empty());
  }

  HalFile file = HalFile::fromString(stash);
  EXPECT_TRUE(target.readFrom(file));
  EXPECT_EQ(dump(target), dump(catalog));
}

// The flags the manifest sets, not the ones the Font Manager sets later, ride along a copy and a stash.
TEST(FontCatalogTest, TheOutlineAndDownloadAllFlagsSurviveACopyAndAStash) {
  FontCatalog::Builder counter;
  FontCatalog catalog;
  const auto feedFlags = [](FontCatalog::Builder& builder) {
    builder.beginFamily();
    builder.keepFamily("Plain", 5);
    builder.beginFamily();
    builder.outline(true);
    builder.inDownloadAll(false);
    builder.keepFamily("Cjk", 3);
    builder.beginFamily();  // a family begins with neither flag, whatever the one before it said
    builder.keepFamily("After", 5);
  };
  feedFlags(counter);
  ASSERT_TRUE(catalog.allocate(counter.size()));
  FontCatalog::Builder filler(catalog);
  feedFlags(filler);
  EXPECT_FALSE(catalog.outline(0));
  EXPECT_TRUE(catalog.inDownloadAll(0));
  EXPECT_TRUE(catalog.outline(1));
  EXPECT_FALSE(catalog.inDownloadAll(1));
  EXPECT_FALSE(catalog.outline(2));
  EXPECT_TRUE(catalog.inDownloadAll(2));
  EXPECT_FALSE(catalog.outline(3));  // out of range
  EXPECT_FALSE(catalog.inDownloadAll(3));

  FontCatalog one;
  ASSERT_TRUE(one.copyFamilyFrom(catalog, 1));
  EXPECT_TRUE(one.outline(0));
  EXPECT_FALSE(one.inDownloadAll(0));

  HalFile file = HalFile::fromString(stashBytes(one));
  FontCatalog restored;
  ASSERT_TRUE(restored.readFrom(file));
  EXPECT_TRUE(restored.outline(0));
  EXPECT_FALSE(restored.inDownloadAll(0));

  restored.setInstalled(0, true);  // the later flags do not disturb them
  EXPECT_TRUE(restored.outline(0));
  EXPECT_FALSE(restored.inDownloadAll(0));
}
