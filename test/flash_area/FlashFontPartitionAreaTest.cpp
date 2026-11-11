// FlashFontPartition, the .cpfont cache, over a simulated flash area instead of the `spiffs` partition: what it reads,
// erases and maps goes through FlashArea::get(), so any backend with NOR semantics serves it. The area is laid out as
// on a board without PSRAM: the font area, the hyphenation region, the language slot.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "FlashAreaRegion.h"
#include "FlashFontPartition.h"
#include "SimFlashArea.h"
#include "SimNorFlash.h"

namespace {

namespace ffp = FlashFontPartition;

constexpr uint32_t kAreaBytes = 1024 * 1024;  // 512 KB of font area, the 384 KB region, the 128 KB slot
constexpr uint32_t kFontArea = kAreaBytes - ffp::LANG_RESERVED_BYTES - ffp::HYPH_RESERVED_BYTES;

uint8_t dataByte(const uint32_t i) { return static_cast<uint8_t>(i * 29u + 3u); }

class FlashFontPartitionAreaTest : public ::testing::Test {
 protected:
  void SetUp() override { sim_area::use(&area); }
  void TearDown() override {
    ffp::unmap();
    EXPECT_EQ(0u, area.mappings()) << "every mapping a test made is released";
    EXPECT_EQ(0u, area.violations()) << "nothing programmed a 0 back to 1";
    sim_area::use(nullptr);
  }

  // The index finaliseWrite() leaves for `count` entries, and each entry's data, written straight into the area.
  void install(const ffp::Entry* entries, const uint8_t count) {
    uint8_t* const bytes = area.raw();
    std::memcpy(bytes, "CPFC", 4);
    bytes[4] = count;
    bytes[5] = bytes[6] = bytes[7] = 0;
    std::memcpy(bytes + 8, entries, count * sizeof(ffp::Entry));
    for (uint8_t e = 0; e < count; ++e) {
      for (uint32_t i = 0; i < entries[e].dataSize; ++i) bytes[entries[e].dataOffset + i] = dataByte(i);
    }
  }

  static ffp::Entry entry(const char* family, const uint8_t points, const uint32_t offset, const uint32_t size) {
    ffp::Entry e{};
    std::strncpy(e.familyName, family, sizeof(e.familyName) - 1);
    e.pointSize = points;
    e.dataOffset = offset;
    e.dataSize = size;
    return e;
  }

  SimNorFlash area{kAreaBytes};
};

}  // namespace

TEST_F(FlashFontPartitionAreaTest, TheFontAreaEndsBelowTheHyphenationRegionAndTheLanguageSlot) {
  EXPECT_EQ(kFontArea, ffp::fontUsableSize());
  EXPECT_EQ(kFontArea, ffp::hyphRegionOffset());
}

TEST_F(FlashFontPartitionAreaTest, WithoutAnAreaThereIsNoCache) {
  sim_area::use(nullptr);
  EXPECT_EQ(0u, ffp::fontUsableSize());
  EXPECT_FALSE(ffp::hasValidIndex());
  EXPECT_FALSE(ffp::hasEntry("Bitter", 14));
  EXPECT_FALSE(ffp::beginWrite("Bitter"));
  const uint8_t* data = nullptr;
  size_t size = 0;
  EXPECT_FALSE(ffp::mmap("Bitter", 14, &data, &size));
}

TEST_F(FlashFontPartitionAreaTest, AnEntryMapsInPlaceAndIsReleasedByItsPointer) {
  const ffp::Entry entries[] = {entry("Bitter", 12, 4096, 6000), entry("Bitter", 14, 12288, 9000)};
  install(entries, 2);
  EXPECT_TRUE(ffp::hasValidIndex());
  EXPECT_TRUE(ffp::hasEntry("Bitter", 14));
  EXPECT_FALSE(ffp::hasEntry("Bitter", 16));
  const uint8_t both[] = {12, 14};
  const uint8_t more[] = {12, 14, 16};
  EXPECT_TRUE(ffp::hasFamilyComplete("Bitter", both, 2));
  EXPECT_FALSE(ffp::hasFamilyComplete("Bitter", more, 3));

  const uint8_t* data = nullptr;
  size_t size = 0;
  ASSERT_TRUE(ffp::mmap("Bitter", 14, &data, &size));
  EXPECT_EQ(area.raw() + 12288, data) << "the entry's own bytes, read in place";
  EXPECT_EQ(9000u, size);
  EXPECT_EQ(dataByte(8999), data[8999]);
  EXPECT_TRUE(ffp::isMapped());
  EXPECT_EQ(1u, area.mappings());
  ffp::unmap();
  EXPECT_FALSE(ffp::isMapped());
  EXPECT_EQ(0u, area.mappings());
}

TEST_F(FlashFontPartitionAreaTest, AWriteSessionErasesTheFontAreaAndNothingAboveIt) {
  const ffp::Entry entries[] = {entry("Bitter", 14, 4096, 9000)};
  install(entries, 1);
  std::memset(area.raw() + kFontArea, 0x5A, kAreaBytes - kFontArea);  // the hyphenation region and the slot

  ASSERT_TRUE(ffp::beginWrite("Literata"));
  EXPECT_TRUE(ffp::isWriting());
  for (uint32_t i = 0; i < kFontArea; ++i) ASSERT_EQ(0xFF, area.raw()[i]) << "at " << i;
  for (uint32_t i = kFontArea; i < kAreaBytes; ++i) ASSERT_EQ(0x5A, area.raw()[i]) << "at " << i;
  EXPECT_FALSE(ffp::finaliseWrite()) << "nothing was appended";
  EXPECT_FALSE(ffp::isWriting());
  EXPECT_FALSE(ffp::hasValidIndex());
}

TEST_F(FlashFontPartitionAreaTest, AnIndexReachingPastTheFontAreaIsNoIndex) {
  const ffp::Entry entries[] = {entry("Bitter", 14, kFontArea - 4096, 8192)};
  install(entries, 1);
  EXPECT_FALSE(ffp::hasValidIndex());
  EXPECT_FALSE(ffp::hasEntry("Bitter", 14));
}

TEST_F(FlashFontPartitionAreaTest, AGlyphStoreMappingOfTheFontAreaKeepsTheCacheOff) {
  const ffp::Entry entries[] = {entry("Bitter", 14, 65536, 4096)};
  install(entries, 1);
  FlashAreaRegion store;
  ASSERT_TRUE(store.begin());
  EXPECT_EQ(kFontArea, store.size());
  const uint8_t* const block = store.map(0, 4096);
  ASSERT_EQ(area.raw(), block);

  const uint8_t* data = nullptr;
  size_t size = 0;
  EXPECT_FALSE(ffp::mmap("Bitter", 14, &data, &size));
  EXPECT_FALSE(ffp::beginWrite("Bitter"));
  store.unmap(block);
  EXPECT_TRUE(ffp::mmap("Bitter", 14, &data, &size));
}

TEST_F(FlashFontPartitionAreaTest, CachedBytesCountTheHeaderTheIndexAndEveryEntrysData) {
  // What SystemStatus reports as the cache's use.
  EXPECT_EQ(0u, ffp::cachedBytes()) << "an erased area holds no cache";
  const ffp::Entry entries[] = {entry("Bitter", 12, 4096, 6000), entry("Bitter", 14, 12288, 9000)};
  install(entries, 2);
  EXPECT_EQ(8u + 2u * ffp::ENTRY_SIZE + 6000u + 9000u, ffp::cachedBytes());
  sim_area::use(nullptr);
  EXPECT_EQ(0u, ffp::cachedBytes());
}
