#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "FsHelpers/NaturalSort.h"

namespace {

TEST(NaturalSort, Basic) {
  EXPECT_EQ(FsHelpers::naturalCompare("a", "a"), 0);
  EXPECT_LT(FsHelpers::naturalCompare("a", "b"), 0);
  EXPECT_GT(FsHelpers::naturalCompare("b", "a"), 0);
}

TEST(NaturalSort, CaseInsensitive) {
  EXPECT_EQ(FsHelpers::naturalCompare("ABC", "abc"), 0);
  EXPECT_EQ(FsHelpers::naturalCompare("Test", "test"), 0);
}

TEST(NaturalSort, Digits) {
  EXPECT_LT(FsHelpers::naturalCompare("file1.txt", "file2.txt"), 0);
  EXPECT_LT(FsHelpers::naturalCompare("file2.txt", "file10.txt"), 0);
  EXPECT_LT(FsHelpers::naturalCompare("v2", "v10"), 0);
}

TEST(NaturalSort, LeadingZeros) {
  EXPECT_EQ(FsHelpers::naturalCompare("file007.txt", "file7.txt"), 0);
  EXPECT_LT(FsHelpers::naturalCompare("file007.txt", "file8.txt"), 0);
}

TEST(NaturalSort, Prefix) {
  EXPECT_LT(FsHelpers::naturalCompare("file", "file.txt"), 0);
  EXPECT_LT(FsHelpers::naturalCompare("a", "ab"), 0);
}

TEST(NaturalSort, Mixed) {
  EXPECT_LT(FsHelpers::naturalCompare("track01_intro.mp3", "track02_verse.mp3"), 0);
  EXPECT_LT(FsHelpers::naturalCompare("img_01.jpg", "img_2.jpg"), 0);
}

TEST(NaturalSort, VectorSort) {
  std::vector<std::string> files = {"file10.txt", "file2.txt", "file1.txt", "File3.txt"};
  std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
    return FsHelpers::naturalCompare(a.c_str(), b.c_str()) < 0;
  });

  EXPECT_EQ(files[0], "file1.txt");
  EXPECT_EQ(files[1], "File3.txt");
  EXPECT_EQ(files[2], "file2.txt");
  EXPECT_EQ(files[3], "file10.txt");
}

TEST(NaturalSortKey, Basic) {
  uint8_t key1[28] = {}, key2[28] = {};
  size_t len1 = FsHelpers::naturalSortKey("file1.txt", key1, sizeof(key1));
  size_t len2 = FsHelpers::naturalSortKey("file2.txt", key2, sizeof(key2));

  EXPECT_LT(memcmp(key1, key2, std::max(len1, len2)), 0);
}

TEST(NaturalSortKey, CaseInsensitive) {
  uint8_t key1[28] = {}, key2[28] = {};
  FsHelpers::naturalSortKey("ABC", key1, sizeof(key1));
  FsHelpers::naturalSortKey("abc", key2, sizeof(key2));

  EXPECT_EQ(memcmp(key1, key2, sizeof(key1)), 0);
}

TEST(NaturalSortKey, NoNullTerminator) {
  uint8_t key[28] = {};
  size_t len = FsHelpers::naturalSortKey("test", key, sizeof(key));

  for (size_t i = 0; i < len; i++) {
    EXPECT_NE(key[i], 0x00);
  }
}

}  // namespace
