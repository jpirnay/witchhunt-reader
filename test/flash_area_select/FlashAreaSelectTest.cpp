// How FlashArea::get() picks the board's area, by FLASH_AREA_PSRAM_IMAGE and by what the board has: a `spiffs`
// partition (test/flash_lang_partition's in-memory one, one too small for the area, or none) and PSRAM
// (esp_heap_caps.h here, or none). The pick is made once per run, so each scenario is its own executable
// (FLASH_AREA_SCENARIO, CMakeLists.txt). The partition backend answers 32 free mapping pages (spi_flash_mmap.h there),
// the PSRAM image as many as it has pages.

#include <gtest/gtest.h>

#include "FlashArea.h"
#include "FlashFontPartition.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

namespace {
constexpr uint32_t kPartitionBytes = 0x360000;
constexpr uint32_t kImagePages = 0x360000 / NorFlash::kMapPageBytes;
// What CrossPoint's web repair writes on the X4 Pro: an 80 KB `spiffs` partition, smaller than the language slot.
constexpr uint32_t kX4ProRepairBytes = 0x14000;

using FlashArea::Backend;
}  // namespace

#if FLASH_AREA_SCENARIO == 0
TEST(FlashAreaSelect, WithoutTheImageInTheBuildAndWithoutAPartitionThereIsNone) {
  fake_flash::reset(0);
  EXPECT_EQ(nullptr, FlashArea::get());
  EXPECT_EQ(Backend::None, FlashArea::backend());
}
#elif FLASH_AREA_SCENARIO == 1
TEST(FlashAreaSelect, APartitionComesFirstAndTakesNoPsram) {
  fake_flash::reset(kPartitionBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::Partition, FlashArea::backend());
  EXPECT_EQ(32u, area->freeMapPages()) << "the partition, through the MMU";
  EXPECT_EQ(kPartitionBytes, area->size());
  EXPECT_EQ(area, FlashArea::get()) << "picked once";
  EXPECT_EQ(0, g_psramAllocs) << "a board with the partition is as it was: no image";
}
#elif FLASH_AREA_SCENARIO == 2
TEST(FlashAreaSelect, WithoutAPartitionThePsramImageKeptInPsramOnly) {
  fake_flash::reset(0);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  EXPECT_EQ(kPartitionBytes, area->size()) << "the partition's size: every layout constant holds";
  EXPECT_EQ(kImagePages, area->freeMapPages()) << "the image: a mapping costs no MMU page";
  EXPECT_EQ(1, g_psramAllocs);
  uint8_t byte = 0;
  ASSERT_TRUE(area->read(kPartitionBytes - 1, &byte, 1));
  EXPECT_EQ(0xFF, byte) << "erased at boot";
  const uint8_t zero = 0;
  ASSERT_TRUE(area->program(0, &zero, 1));
  EXPECT_TRUE(area->sync()) << "nothing to keep (and this build has no card file to keep it in)";
}
#elif FLASH_AREA_SCENARIO == 3
TEST(FlashAreaSelect, WithoutAPartitionOrPsramThereIsNone) {
  fake_flash::reset(0);
  g_hostHasPsram = false;
  EXPECT_EQ(nullptr, FlashArea::get());
  EXPECT_EQ(Backend::None, FlashArea::backend());
}
#elif FLASH_AREA_SCENARIO == 4
TEST(FlashAreaSelect, ATestBuildTakesThePsramImageOverThePartition) {
  fake_flash::reset(kPartitionBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  EXPECT_EQ(kImagePages, area->freeMapPages());
}
#elif FLASH_AREA_SCENARIO == 5
TEST(FlashAreaSelect, APartitionTooSmallForTheLanguageSlotGivesWayToThePsramImage) {
  fake_flash::reset(kX4ProRepairBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  EXPECT_EQ(kPartitionBytes, area->size());
  EXPECT_EQ(0, fake_flash::chip().erases + fake_flash::chip().writes) << "the small partition is left alone";
}
#elif FLASH_AREA_SCENARIO == 6
TEST(FlashAreaSelect, APartitionTooSmallForTheLanguageSlotIsNoneWithoutTheImage) {
  fake_flash::reset(FlashFontPartition::LANG_RESERVED_BYTES);
  EXPECT_EQ(nullptr, FlashArea::get());
  EXPECT_EQ(Backend::None, FlashArea::backend());
}
#elif FLASH_AREA_SCENARIO == 7
TEST(FlashAreaSelect, AnImageKeptOnTheCardStillBuildsAndStartsErasedWithoutAFile) {
  fake_flash::reset(0);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  uint8_t byte = 0;
  ASSERT_TRUE(area->read(kPartitionBytes - 1, &byte, 1));
  EXPECT_EQ(0xFF, byte) << "no image on the card: erased";
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
}
#elif FLASH_AREA_SCENARIO == 9
TEST(FlashAreaSelect, ABoardWithPsramTakesTheImageWhenItsTableHasNoPartition) {
  // Issue #733: an X4 Pro that kept its stock partition table (a `littlefs` partition, no `spiffs` one).
  fake_flash::reset(0);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(Backend::PsramImage, FlashArea::backend());
  EXPECT_EQ(kPartitionBytes, area->size());
}
#elif FLASH_AREA_SCENARIO == 10
TEST(FlashAreaSelect, ABoardWithPsramAndThePartitionKeepsThePartition) {
  fake_flash::reset(kPartitionBytes);
  ASSERT_NE(nullptr, FlashArea::get());
  EXPECT_EQ(Backend::Partition, FlashArea::backend());
  EXPECT_EQ(0, g_psramAllocs);
}
#endif
