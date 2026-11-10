// EspNorFlash and FlashFontPartition share the `spiffs` partition, and the .cpfont cache stays off it while the TTF
// glyph store holds a mapping of the font area. The hyphenation store's EspNorFlash maps a book's trie from its own
// region above the font area, and that mapping is no reason to refuse the cache. Seen on the X3: after a German book,
// picking another .cpfont family logged "beginWrite: the TTF glyph store is open on this partition" and loaded the
// family from the SD card. These tests run both over the in-memory partition of test/flash_lang_partition.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "EspNorFlash.h"
#include "FlashFontPartition.h"
#include "esp_partition.h"

namespace {

// 512 KB of font area, then the 384 KB hyphenation region and the 128 KB language slot.
constexpr uint32_t kPartitionBytes = 1024 * 1024;
constexpr uint32_t kEntryOffset = 64 * 1024;  // a mapping page: the fake maps whole pages only
constexpr uint32_t kEntryBytes = 4096;

// The index finaliseWrite() leaves for one .cpfont of `family` at `points`, written straight into the chip.
void installCpfont(const char* family, const uint8_t points) {
  uint8_t* const bytes = fake_flash::chip().bytes.data();
  std::memcpy(bytes, "CPFC", 4);
  bytes[4] = 1;
  bytes[5] = bytes[6] = bytes[7] = 0;
  FlashFontPartition::Entry entry{};
  std::strncpy(entry.familyName, family, sizeof(entry.familyName) - 1);
  entry.pointSize = points;
  entry.dataOffset = kEntryOffset;
  entry.dataSize = kEntryBytes;
  std::memcpy(bytes + 8, &entry, sizeof entry);
}

bool cpfontMaps(const char* family, const uint8_t points) {
  const uint8_t* data = nullptr;
  size_t size = 0;
  if (!FlashFontPartition::mmap(family, points, &data, &size)) return false;
  FlashFontPartition::unmap();
  return data != nullptr && size == kEntryBytes;
}

// A write session that erases the font area and ends without an entry, as a failed family switch leaves it.
bool cpfontWriteStarts(const char* family) {
  if (!FlashFontPartition::beginWrite(family)) return false;
  EXPECT_FALSE(FlashFontPartition::finaliseWrite());  // no file appended: the session just ends
  return true;
}

class EspNorFlashTest : public ::testing::Test {
 protected:
  void SetUp() override { fake_flash::reset(kPartitionBytes); }
  void TearDown() override {
    FlashFontPartition::unmap();
    EXPECT_EQ(0u, EspNorFlash::liveFontAreaMappings()) << "every mapping a test made is released";
  }
};

}  // namespace

TEST_F(EspNorFlashTest, AMappedTrieLeavesTheCpfontCacheFree) {
  EspNorFlash hyphenation;
  ASSERT_NE(0u, FlashFontPartition::hyphRegionOffset());
  ASSERT_TRUE(hyphenation.beginRegion(static_cast<uint32_t>(FlashFontPartition::hyphRegionOffset()),
                                      FlashFontPartition::HYPH_RESERVED_BYTES));
  const uint8_t* const trie = hyphenation.map(0, 3 * 64 * 1024);
  ASSERT_NE(nullptr, trie);
  EXPECT_EQ(0u, EspNorFlash::liveFontAreaMappings());

  // The family switch of the X3: the cache is written and mapped while the open book's trie stays mapped.
  EXPECT_TRUE(cpfontWriteStarts("Readerly"));
  installCpfont("Readerly", 14);
  EXPECT_TRUE(cpfontMaps("Readerly", 14));
  hyphenation.unmap(trie);
}

TEST_F(EspNorFlashTest, AGlyphStoreMappingKeepsTheCpfontCacheOff) {
  installCpfont("Readerly", 14);
  EspNorFlash store;
  ASSERT_TRUE(store.begin());
  const uint8_t* const block = store.map(0, 4096);
  ASSERT_NE(nullptr, block);
  EXPECT_EQ(1u, EspNorFlash::liveFontAreaMappings());
  EXPECT_FALSE(cpfontMaps("Readerly", 14));
  EXPECT_FALSE(cpfontWriteStarts("Readerly"));
  store.unmap(block);
  EXPECT_EQ(0u, EspNorFlash::liveFontAreaMappings());
  EXPECT_TRUE(cpfontMaps("Readerly", 14));
}

TEST_F(EspNorFlashTest, AMappingCountsForTheAreaItWasMadeIn) {
  // An instance begun again over the other area while it holds a mapping: the mapping is released as it was counted.
  EspNorFlash flash;
  ASSERT_TRUE(flash.begin());
  const uint8_t* const fontArea = flash.map(0, 4096);
  ASSERT_NE(nullptr, fontArea);
  ASSERT_TRUE(flash.beginRegion(static_cast<uint32_t>(FlashFontPartition::hyphRegionOffset()),
                                FlashFontPartition::HYPH_RESERVED_BYTES));
  const uint8_t* const region = flash.map(0, 4096);
  ASSERT_NE(nullptr, region);
  EXPECT_EQ(1u, EspNorFlash::liveFontAreaMappings());
  flash.unmap(fontArea);
  EXPECT_EQ(0u, EspNorFlash::liveFontAreaMappings());
  flash.unmap(region);
  EXPECT_EQ(0u, EspNorFlash::liveFontAreaMappings());
}

TEST_F(EspNorFlashTest, DestroyingAStoreReleasesItsCount) {
  {
    EspNorFlash store;
    ASSERT_TRUE(store.begin());
    ASSERT_NE(nullptr, store.map(0, 4096));
    ASSERT_NE(nullptr, store.map(64 * 1024, 4096));
    EXPECT_EQ(2u, EspNorFlash::liveFontAreaMappings());
  }
  EXPECT_EQ(0u, EspNorFlash::liveFontAreaMappings());
  EXPECT_TRUE(cpfontWriteStarts("Readerly"));
}
