// How FlashArea::get() picks the board's area, by FLASH_AREA_PSRAM_IMAGE and by what the board has: a `spiffs`
// partition (test/flash_lang_partition's in-memory one, one too small for the area, or none) and PSRAM
// (esp_heap_caps.h here, or none). The pick is made once per run, so each scenario is its own executable
// (FLASH_AREA_SCENARIO, CMakeLists.txt). The partition backend answers 32 free mapping pages (spi_flash_mmap.h there),
// the PSRAM image as many as it has pages.

#include <gtest/gtest.h>

#include "FlashArea.h"
#include "FlashFontPartition.h"
#include "FlashLangPartition.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

namespace {
constexpr uint32_t kPartitionBytes = 0x360000;
constexpr uint32_t kImagePages = 0x360000 / NorFlash::kMapPageBytes;
// The X4 Pro's factory-compatible table, as CrossPoint's web flasher has it: an 80 KB `spiffs` partition, smaller than
// the language slot (2.40's 64 KB slot fitted into it).
constexpr uint32_t kX4ProFactoryBytes = 0x14000;
// Onyx's stock BOOX Picco table: a 512 KB `spiffs` partition holding the stock firmware's data.
constexpr uint32_t kPiccoStockBytes = 0x80000;

using FlashArea::Backend;
}  // namespace

#if FLASH_AREA_SCENARIO == 0
TEST(FlashAreaSelect, WithoutTheImageInTheBuildAndWithoutAPartitionThereIsNone) {
  fake_flash::reset(0);
  EXPECT_EQ(nullptr, FlashArea::get());
  EXPECT_EQ(Backend::None, FlashArea::backend());
  EXPECT_EQ(0u, FlashArea::partitionBytes());
  EXPECT_FALSE(FlashArea::asExpected()) << "emergency mode";
}
#elif FLASH_AREA_SCENARIO == 1
TEST(FlashAreaSelect, APartitionComesFirstAndTakesNoPsram) {
  fake_flash::reset(kPartitionBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::Partition, FlashArea::backend());
  EXPECT_EQ(kPartitionBytes, FlashArea::partitionBytes());
  EXPECT_TRUE(FlashArea::asExpected());
  EXPECT_EQ(32u, area->freeMapPages()) << "the partition, through the MMU";
  EXPECT_EQ(kPartitionBytes, area->size());
  EXPECT_EQ(area, FlashArea::get()) << "picked once";
  EXPECT_EQ(0, g_psramAllocs) << "a board with the partition is as it was: no image";
  EXPECT_TRUE(FlashArea::sync());
  EXPECT_EQ(0, fake_flash::chip().erases + fake_flash::chip().writes) << "a sync costs the partition nothing";
}
#elif FLASH_AREA_SCENARIO == 2
TEST(FlashAreaSelect, WithoutAPartitionThePsramImage) {
  fake_flash::reset(0);
  EXPECT_TRUE(FlashArea::sync()) << "a sync before the choice: nothing to keep";
  EXPECT_EQ(0, g_psramAllocs) << "and it does not make the choice";
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  EXPECT_TRUE(FlashArea::asExpected()) << "the image stands in for the partition in full";
  EXPECT_EQ(kPartitionBytes, area->size()) << "the partition's size: every layout constant holds";
  EXPECT_EQ(kImagePages, area->freeMapPages()) << "the image: a mapping costs no MMU page";
  EXPECT_EQ(1, g_psramAllocs);
  uint8_t byte = 0;
  ASSERT_TRUE(area->read(kPartitionBytes - 1, &byte, 1));
  EXPECT_EQ(0xFF, byte) << "no image on the card: erased";
}
#elif FLASH_AREA_SCENARIO == 3
TEST(FlashAreaSelect, WithoutAPartitionOrPsramThereIsNone) {
  fake_flash::reset(0);
  g_hostHasPsram = false;
  EXPECT_EQ(nullptr, FlashArea::get());
  EXPECT_EQ(Backend::None, FlashArea::backend());
  EXPECT_FALSE(FlashArea::asExpected());
}
#elif FLASH_AREA_SCENARIO == 4
TEST(FlashAreaSelect, ATestBuildTakesThePsramImageOverThePartition) {
  fake_flash::reset(kPartitionBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  EXPECT_EQ(kImagePages, area->freeMapPages());
  EXPECT_EQ(kPartitionBytes, FlashArea::partitionBytes()) << "what the table holds, though not used";
  EXPECT_TRUE(FlashArea::asExpected());
}
#elif FLASH_AREA_SCENARIO == 5
TEST(FlashAreaSelect, APartitionTooSmallForTheLanguageSlotGivesWayToThePsramImage) {
  fake_flash::reset(kX4ProFactoryBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  EXPECT_EQ(kX4ProFactoryBytes, FlashArea::partitionBytes()) << "System Information names it";
  EXPECT_TRUE(FlashArea::asExpected()) << "the S3 boards' planned stand-in: no emergency";
  EXPECT_EQ(kPartitionBytes, area->size());
  EXPECT_EQ(0, fake_flash::chip().erases + fake_flash::chip().writes) << "the small partition is left alone";
}
#elif FLASH_AREA_SCENARIO == 6
TEST(FlashAreaSelect, APartitionTooSmallForTheLanguageSlotIsNoneWithoutTheImage) {
  fake_flash::reset(FlashFontPartition::LANG_RESERVED_BYTES);
  EXPECT_EQ(nullptr, FlashArea::get());
  EXPECT_EQ(Backend::None, FlashArea::backend());
  EXPECT_EQ(FlashFontPartition::LANG_RESERVED_BYTES, FlashArea::partitionBytes());
  EXPECT_FALSE(FlashArea::asExpected());
  // The language slot then says why it has nothing to hand out.
  struct Never {
    static bool produce(void*, FlashLangPartition::StringsWriter&) { return false; }
  };
  FlashLangPartition::Mapped m{};
  EXPECT_FALSE(
      FlashLangPartition::acquire(1, 2, 3, FlashLangPartition::Strings{64, &Never::produce, nullptr}, nullptr, &m));
  EXPECT_EQ(FlashLangPartition::Failure::NoArea, FlashLangPartition::lastFailure());
}
#elif FLASH_AREA_SCENARIO == 8
TEST(FlashAreaSelect, AnyPartitionLargerThanTheLanguageSlotIsTheArea) {
  // As small as the 1 MB `spiffs` partition of the stock X3 layout (as CrossPoint's web flasher lists it): used as
  // before.
  fake_flash::reset(1024 * 1024);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::Partition, FlashArea::backend());
  EXPECT_EQ(1024u * 1024u, area->size());
  EXPECT_FALSE(FlashArea::asExpected()) << "smaller than our table's: emergency mode, the notice says so";
}
#elif FLASH_AREA_SCENARIO == 9
TEST(FlashAreaSelect, ABoardWithPsramTakesTheImageWhenItsTableHasNoPartition) {
  // Issue #733: an X4 Pro that kept its stock partition table (a `littlefs` partition, no `spiffs` one).
  fake_flash::reset(0);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  EXPECT_EQ(kPartitionBytes, area->size());
  EXPECT_TRUE(FlashArea::asExpected());
}
#elif FLASH_AREA_SCENARIO == 10
TEST(FlashAreaSelect, ABoardWithPsramAndThePartitionKeepsThePartition) {
  fake_flash::reset(kPartitionBytes);
  ASSERT_NE(nullptr, FlashArea::get());
  EXPECT_EQ(Backend::Partition, FlashArea::backend());
  EXPECT_EQ(0, g_psramAllocs);
  EXPECT_TRUE(FlashArea::asExpected());
}
#elif FLASH_AREA_SCENARIO == 11
TEST(FlashAreaSelect, ABoardWithPsramLeavesAPartitionSmallerThanTheAreaAlone) {
  // A BOOX Picco on Onyx's stock table.
  fake_flash::reset(kPiccoStockBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  EXPECT_EQ(kPartitionBytes, area->size()) << "the whole area, not what fits beside the language slot";
  EXPECT_EQ(kPiccoStockBytes, FlashArea::partitionBytes()) << "System Information names it";
  EXPECT_TRUE(FlashArea::asExpected());
  ASSERT_TRUE(area->erase(0, NorFlash::kSectorBytes));
  EXPECT_EQ(0, fake_flash::chip().erases + fake_flash::chip().writes) << "the stock firmware's data stays";
}
#elif FLASH_AREA_SCENARIO == 12
TEST(FlashAreaSelect, APartitionSmallerThanTheAreaIsUsedWhenNoImageFits) {
  fake_flash::reset(kPiccoStockBytes);
  g_hostHasPsram = false;
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::Partition, FlashArea::backend());
  EXPECT_EQ(kPiccoStockBytes, area->size());
  EXPECT_FALSE(FlashArea::asExpected());
}
#elif FLASH_AREA_SCENARIO == 13
TEST(FlashAreaSelect, ATestBuildTakesThePartitionAway) {
  // FLASH_AREA_TEST_PARTITION_KB=0 on a board with our table: emergency mode, and the partition is not touched.
  fake_flash::reset(kPartitionBytes);
  EXPECT_EQ(nullptr, FlashArea::get());
  EXPECT_EQ(Backend::None, FlashArea::backend());
  EXPECT_EQ(0u, FlashArea::partitionBytes());
  EXPECT_FALSE(FlashArea::asExpected());
  EXPECT_EQ(0, fake_flash::chip().erases + fake_flash::chip().writes);
}
#elif FLASH_AREA_SCENARIO == 14
TEST(FlashAreaSelect, ATestBuildSeesAWindowOfThePartition) {
  // FLASH_AREA_TEST_PARTITION_KB=1024: the stock X3 layout's size, as the first 1 MB of our partition.
  fake_flash::reset(kPartitionBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::Partition, FlashArea::backend());
  EXPECT_EQ(1024u * 1024u, area->size());
  EXPECT_EQ(1024u * 1024u, FlashArea::partitionBytes());
  EXPECT_FALSE(FlashArea::asExpected());
  EXPECT_EQ(1, fake_flash::chip().erases) << "the font area's header sector, so no index outlives the window";
  uint8_t byte = 0;
  EXPECT_TRUE(area->read(area->size() - 1, &byte, 1));
  EXPECT_FALSE(area->read(area->size(), &byte, 1)) << "nothing past the window";
  EXPECT_FALSE(area->erase(area->size(), NorFlash::kSectorBytes));
  EXPECT_EQ(nullptr, area->map(area->size() - 1, 2));
}
#endif
