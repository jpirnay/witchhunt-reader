// sleep_pixel_cache (src/activities/boot_sleep/SleepPixelCacheStamp.h): the source stamp after a sleep PNG's pixel
// cache. A cache replays only while the file at its image's path is the one it was decoded from; a PNG replaced by
// another of the same size at the same path used to replay the old picture (crosspoint-reader PR #4542).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "SleepPixelCacheStamp.h"

namespace {

using sleep_pixel_cache::SourceStamp;

constexpr size_t kHeaderBytes = 6;                    // PixelCache::PXC_HEADER_BYTES: magic, width, height
constexpr size_t kPixelBytes = kHeaderBytes + 3 * 2;  // a 12 x 2 picture: 3 bytes a row

// A cache as the decode leaves it: header and rows, no stamp.
HalFile decodedCache() {
  HalFile cache = HalFile::forReadWrite();
  const std::string bytes(kPixelBytes, '\x5A');
  cache.write(bytes.data(), bytes.size());
  return cache;
}

SourceStamp stamp(const uint32_t size, const uint16_t date, const uint16_t time) {
  SourceStamp s;
  s.size = size;
  s.date = date;
  s.time = time;
  return s;
}

TEST(SleepPixelCacheStamp, AStampedCacheMatchesItsSourceAndReadsOnFromThePixels) {
  HalFile cache = decodedCache();
  const SourceStamp source = stamp(48213, 0x5B37, 0x6C2A);
  ASSERT_TRUE(sleep_pixel_cache::append(cache, kPixelBytes, source));
  EXPECT_EQ(cache.size(), kPixelBytes + sleep_pixel_cache::kBytes);

  EXPECT_TRUE(sleep_pixel_cache::matches(cache, kPixelBytes, source, kHeaderBytes));
  EXPECT_EQ(cache.position(), kHeaderBytes) << "the replay reads the rows next";
}

TEST(SleepPixelCacheStamp, AnotherFileAtThePathDoesNotMatch) {
  HalFile cache = decodedCache();
  const SourceStamp source = stamp(48213, 0x5B37, 0x6C2A);
  ASSERT_TRUE(sleep_pixel_cache::append(cache, kPixelBytes, source));

  EXPECT_FALSE(sleep_pixel_cache::matches(cache, kPixelBytes, stamp(48214, 0x5B37, 0x6C2A), kHeaderBytes))
      << "a different size";
  EXPECT_FALSE(sleep_pixel_cache::matches(cache, kPixelBytes, stamp(48213, 0x5B38, 0x6C2A), kHeaderBytes))
      << "the same size, modified another day";
  EXPECT_FALSE(sleep_pixel_cache::matches(cache, kPixelBytes, stamp(48213, 0x5B37, 0x6C2B), kHeaderBytes))
      << "the same size and day, another time";
}

// Every cache written before the stamp, and one cut off between the decode and the stamp, ends at its rows.
TEST(SleepPixelCacheStamp, ACacheWithoutAStampNeverMatches) {
  HalFile cache = decodedCache();
  EXPECT_FALSE(sleep_pixel_cache::matches(cache, kPixelBytes, SourceStamp{}, kHeaderBytes));
  EXPECT_FALSE(sleep_pixel_cache::matches(cache, kPixelBytes, stamp(0, 0, 0), kHeaderBytes));
}

// A cache of other geometry than the caller expects is longer or shorter than header, rows and stamp.
TEST(SleepPixelCacheStamp, ACacheOfAnotherLengthNeverMatches) {
  HalFile cache = decodedCache();
  const SourceStamp source = stamp(100, 1, 2);
  ASSERT_TRUE(sleep_pixel_cache::append(cache, kPixelBytes, source));
  EXPECT_FALSE(sleep_pixel_cache::matches(cache, kPixelBytes + 3, source, kHeaderBytes));
  EXPECT_FALSE(sleep_pixel_cache::matches(cache, kPixelBytes - 3, source, kHeaderBytes));
}

TEST(SleepPixelCacheStamp, TrailingBytesOfAnotherKindAreNoStamp) {
  HalFile cache = decodedCache();
  const std::string junk(sleep_pixel_cache::kBytes, '\0');
  cache.write(junk.data(), junk.size());
  EXPECT_FALSE(sleep_pixel_cache::matches(cache, kPixelBytes, SourceStamp{}, kHeaderBytes))
      << "zero bytes are not a stamp of an empty file dated 0";
}

// A cache stamped by an earlier version holds pixels this one would not draw: "SPS1" caches from the X4 Pro and the
// Picco took the 16-level Equalize curve into four levels. It is not replayed, whatever file it was decoded from.
TEST(SleepPixelCacheStamp, AStampOfAnEarlierVersionNeverMatches) {
  HalFile cache = decodedCache();
  const SourceStamp source = stamp(48213, 0x5B37, 0x6C2A);
  uint8_t version1[sleep_pixel_cache::kBytes] = {'S', 'P', 'S', '1'};
  memcpy(version1 + 4, &source.size, sizeof(source.size));
  memcpy(version1 + 8, &source.date, sizeof(source.date));
  memcpy(version1 + 10, &source.time, sizeof(source.time));
  cache.write(version1, sizeof(version1));
  EXPECT_FALSE(sleep_pixel_cache::matches(cache, kPixelBytes, source, kHeaderBytes));

  HalFile current = decodedCache();
  ASSERT_TRUE(sleep_pixel_cache::append(current, kPixelBytes, source));
  EXPECT_TRUE(sleep_pixel_cache::matches(current, kPixelBytes, source, kHeaderBytes)) << "control: the same stamp now";
}

// The stamp goes on a cache the decode has just finished, exactly once.
TEST(SleepPixelCacheStamp, OnlyAFinishedUnstampedCacheTakesAStamp) {
  HalFile stamped = decodedCache();
  ASSERT_TRUE(sleep_pixel_cache::append(stamped, kPixelBytes, stamp(1, 2, 3)));
  EXPECT_FALSE(sleep_pixel_cache::append(stamped, kPixelBytes, stamp(1, 2, 3))) << "already stamped";
  EXPECT_EQ(stamped.size(), kPixelBytes + sleep_pixel_cache::kBytes);

  HalFile shortCache = decodedCache();
  EXPECT_FALSE(sleep_pixel_cache::append(shortCache, kPixelBytes + 1, stamp(1, 2, 3))) << "rows missing";
  EXPECT_EQ(shortCache.size(), kPixelBytes) << "and nothing written";
}

// The source's size comes from the file; a date the card cannot give stays 0, and the size still tells files apart.
TEST(SleepPixelCacheStamp, TheStampOfASourceIsItsSizeAndDate) {
  HalFile source = HalFile::fromString(std::string(321, 'p'));
  const SourceStamp s = sleep_pixel_cache::stampOf(source);
  EXPECT_EQ(s.size, 321u);
  EXPECT_EQ(s.date, 0u);  // the host HalFile has no dates
  EXPECT_EQ(s.time, 0u);
}

}  // namespace
