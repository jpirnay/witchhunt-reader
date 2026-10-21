// ZipFile::KnownEntries: the stats a walk the caller made anyway noted, which loadFileStatSlim
// answers from without walking the central directory again.
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "ZipFile.h"

namespace {

struct Seen {
  std::string name;
  ZipFile::FileStatSlim stat;
};

void collect(void* const ctx, const std::string_view name, const ZipFile::FileStatSlim& stat) {
  static_cast<std::vector<Seen>*>(ctx)->push_back({std::string(name), stat});
}

bool sameStat(const ZipFile::FileStatSlim& a, const ZipFile::FileStatSlim& b) {
  return a.method == b.method && a.compressedSize == b.compressedSize && a.uncompressedSize == b.uncompressedSize &&
         a.localHeaderOffset == b.localHeaderOffset;
}

TEST(ZipKnownEntries, FindsWhatWasAddedByExactName) {
  ZipFile::KnownEntries known;
  EXPECT_TRUE(known.add("OEBPS/content.opf", {8, 100, 300, 42}));
  EXPECT_TRUE(known.add("OEBPS/toc.ncx", {0, 50, 50, 900}));
  ASSERT_NE(known.find("OEBPS/toc.ncx"), nullptr);
  EXPECT_EQ(known.find("OEBPS/toc.ncx")->localHeaderOffset, 900u);
  EXPECT_EQ(known.find("OEBPS/content.opf")->compressedSize, 100u);
  EXPECT_EQ(known.find("oebps/toc.ncx"), nullptr) << "exact names only: the walk handles the rest";
  EXPECT_EQ(known.find("OEBPS/toc"), nullptr);
  EXPECT_EQ(known.size(), 2u);
  EXPECT_EQ(known.name(1), "OEBPS/toc.ncx");
  EXPECT_FALSE(known.overflowed());
}

TEST(ZipKnownEntries, OverflowsWhenEntriesOrNamesRunOut) {
  ZipFile::KnownEntries known;
  for (size_t i = 0; i < ZipFile::KnownEntries::CAPACITY; i++) {
    EXPECT_TRUE(known.add("s" + std::to_string(i) + ".css", {0, 1, 1, static_cast<uint32_t>(i + 1)}));
  }
  EXPECT_FALSE(known.add("one-too-many.css", {}));
  EXPECT_TRUE(known.overflowed());
  ASSERT_NE(known.find("s3.css"), nullptr) << "what it holds still answers";

  ZipFile::KnownEntries names;
  EXPECT_FALSE(names.add(std::string(ZipFile::KnownEntries::NAME_BYTES + 1, 'x'), {}));
  EXPECT_TRUE(names.overflowed());
}

// The fingerprint walk shows each entry with the stat a lookup by name finds for it, and a lookup
// seeded with them answers without a walk of its own.
TEST(ZipKnownEntries, TheFingerprintWalkShowsEveryEntryWithItsStat) {
  const std::string path = FIXTURE_EPUB;
  std::vector<Seen> seen;
  uint64_t fingerprint = 0;
  uint64_t plain = 0;
  {
    ZipFile zip(path);
    ASSERT_TRUE(zip.contentFingerprint(&fingerprint, collect, &seen));
  }
  {
    ZipFile zip(path);
    ASSERT_TRUE(zip.contentFingerprint(&plain));
  }
  EXPECT_EQ(fingerprint, plain) << "the visitor does not change the fingerprint";
  ASSERT_GT(seen.size(), 3u);
  ZipFile zip(path);
  for (const Seen& s : seen) {
    ZipFile::FileStatSlim stat = {};
    ASSERT_TRUE(zip.loadFileStatSlim(s.name.c_str(), &stat)) << s.name;
    EXPECT_TRUE(sameStat(stat, s.stat)) << s.name;
  }

  ZipFile::KnownEntries known;
  const ZipFile::FileStatSlim planted = {8, 7, 7, 12345};
  ASSERT_TRUE(known.add(seen.back().name, planted));
  ZipFile seeded(path);
  seeded.seedKnownEntries(&known);
  ZipFile::FileStatSlim stat = {};
  ASSERT_TRUE(seeded.loadFileStatSlim(seen.back().name.c_str(), &stat));
  EXPECT_TRUE(sameStat(stat, planted)) << "answered from the known entries, not the archive";
  ASSERT_TRUE(seeded.loadFileStatSlim(seen.front().name.c_str(), &stat));
  EXPECT_TRUE(sameStat(stat, seen.front().stat)) << "anything else is still looked up";
}

}  // namespace
