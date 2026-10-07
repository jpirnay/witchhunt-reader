// Our own last push of a book (lib/KOReaderSync/LastPushCache.h). A server record that is exactly
// that push (same document id, same XPath) is a position we already know to the byte, so the sync
// screen fills the remote position from the cache instead of inflating and parsing the chapter.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "KOReaderSync/LastPushCache.h"

namespace {

const char* const kHash = "0123456789abcdef0123456789abcdef";
const char* const kXPath = "/body/DocFragment[30]/body/div[1]/p[5]/text()[1].467";

LastPush pushed() {
  const auto push = LastPush::make(kHash, kXPath, 29, 3, 848, true);
  EXPECT_TRUE(push.has_value());
  return push.value_or(LastPush{});
}

TEST(LastPushCache, TheSamePushMatches) {
  const LastPush last = pushed();
  EXPECT_TRUE(remoteIsOurLastPush(last, kHash, kXPath));
  const CrossPointPosition pos = last.position();
  EXPECT_EQ(pos.spineIndex, 29);
  EXPECT_EQ(pos.pageNumber, 3);
  EXPECT_EQ(pos.visibleTextOffset, 848u);
  EXPECT_TRUE(pos.hasVisibleTextOffset);
  EXPECT_TRUE(pos.hasResolvedSpineIndex);
}

TEST(LastPushCache, AnotherDocumentIdDoesNotMatch) {
  EXPECT_FALSE(remoteIsOurLastPush(pushed(), "fedcba9876543210fedcba9876543210", kXPath));
  EXPECT_FALSE(remoteIsOurLastPush(pushed(), "", kXPath));
  EXPECT_FALSE(remoteIsOurLastPush(pushed(), nullptr, kXPath));
}

TEST(LastPushCache, AnotherXPathDoesNotMatch) {
  // A different character, a prefix, and a longer path all name other positions.
  EXPECT_FALSE(remoteIsOurLastPush(pushed(), kHash, "/body/DocFragment[30]/body/div[1]/p[5]/text()[1].468"));
  EXPECT_FALSE(remoteIsOurLastPush(pushed(), kHash, "/body/DocFragment[30]/body/div[1]/p[5]/text()[1].46"));
  EXPECT_FALSE(remoteIsOurLastPush(pushed(), kHash, "/body/DocFragment[30]/body/div[1]/p[5]/text()[1].4670"));
  EXPECT_FALSE(remoteIsOurLastPush(pushed(), kHash, ""));
  EXPECT_FALSE(remoteIsOurLastPush(pushed(), kHash, nullptr));
}

TEST(LastPushCache, NothingCachedMatchesNothing) {
  const LastPush none{};
  EXPECT_FALSE(remoteIsOurLastPush(none, kHash, kXPath));
  EXPECT_FALSE(remoteIsOurLastPush(none, "", ""));
}

TEST(LastPushCache, AnOverLongXPathIsNeverCached) {
  // KOReader XPaths run under 160 bytes; one that does not fit the 192-byte slot with its NUL is
  // not cached (and so is mapped like any other record) rather than cut.
  const std::string fits(LastPush::kMaxXPath - 1, 'x');
  const std::string tooLong(LastPush::kMaxXPath, 'x');
  EXPECT_TRUE(LastPush::make(kHash, fits.c_str(), 1, 1, 1, true).has_value());
  EXPECT_FALSE(LastPush::make(kHash, tooLong.c_str(), 1, 1, 1, true).has_value());
  EXPECT_FALSE(LastPush::make(kHash, "", 1, 1, 1, true).has_value());
  // A document id that is not 32 characters is not one this cache can match.
  EXPECT_FALSE(LastPush::make("abc", kXPath, 1, 1, 1, true).has_value());
}

TEST(LastPushCache, ARecordRoundTripsThroughThePersistedFormat) {
  const LastPush last = pushed();
  uint8_t data[LastPush::kMaxEncoded] = {};
  const size_t size = last.encode(data);
  ASSERT_GT(size, 0u);
  ASSERT_LE(size, LastPush::kMaxEncoded);
  const auto back = LastPush::decode(data, size);
  ASSERT_TRUE(back.has_value());
  EXPECT_STREQ(back->documentHash, kHash);
  EXPECT_STREQ(back->xpath, kXPath);
  EXPECT_EQ(back->spineIndex, 29);
  EXPECT_EQ(back->page, 3);
  EXPECT_EQ(back->visibleOffset, 848u);
  EXPECT_TRUE(back->hasVisibleOffset);
  EXPECT_TRUE(remoteIsOurLastPush(*back, kHash, kXPath));

  // The longest cacheable XPath survives too.
  const std::string longest(LastPush::kMaxXPath - 1, 'q');
  const auto big = LastPush::make(kHash, longest.c_str(), 7, 8, 9, false);
  ASSERT_TRUE(big.has_value());
  const size_t bigSize = big->encode(data);
  const auto bigBack = LastPush::decode(data, bigSize);
  ASSERT_TRUE(bigBack.has_value());
  EXPECT_EQ(std::string(bigBack->xpath), longest);
  EXPECT_FALSE(bigBack->hasVisibleOffset);
}

TEST(LastPushCache, AnOtherVersionOrADamagedFileReadsAsNone) {
  const LastPush last = pushed();
  uint8_t data[LastPush::kMaxEncoded] = {};
  const size_t size = last.encode(data);

  uint8_t older[LastPush::kMaxEncoded];
  std::memcpy(older, data, size);
  older[0] = static_cast<uint8_t>(LastPush::kVersion - 1);
  EXPECT_FALSE(LastPush::decode(older, size).has_value()) << "an older version";
  older[0] = static_cast<uint8_t>(LastPush::kVersion + 1);
  EXPECT_FALSE(LastPush::decode(older, size).has_value()) << "a newer version";

  EXPECT_FALSE(LastPush::decode(data, 0).has_value()) << "an empty file";
  EXPECT_FALSE(LastPush::decode(nullptr, size).has_value());
  for (size_t cut = 1; cut < size; ++cut) {
    EXPECT_FALSE(LastPush::decode(data, cut).has_value()) << "truncated to " << cut;
  }
  EXPECT_FALSE(LastPush::decode(data, size + 1).has_value()) << "trailing bytes";
}

}  // namespace
