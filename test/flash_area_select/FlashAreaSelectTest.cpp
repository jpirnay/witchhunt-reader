// How FlashArea::get() picks the board's area, by FLASH_AREA_PSRAM_IMAGE and by what the board has: a `spiffs`
// partition (test/flash_lang_partition's in-memory one, or none) and PSRAM (esp_heap_caps.h here, or none). The pick is
// made once per run, so each scenario is its own executable (FLASH_AREA_SCENARIO, CMakeLists.txt). The partition
// backend answers 32 free mapping pages (spi_flash_mmap.h there), the PSRAM image as many as it has pages.

#include <gtest/gtest.h>

#include "FlashArea.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

namespace {
constexpr uint32_t kPartitionBytes = 0x360000;
constexpr uint32_t kImagePages = 0x360000 / NorFlash::kMapPageBytes;
}  // namespace

#if FLASH_AREA_SCENARIO == 0
TEST(FlashAreaSelect, WithoutTheImageInTheBuildAndWithoutAPartitionThereIsNone) {
  fake_flash::reset(0);
  EXPECT_EQ(nullptr, FlashArea::get());
}
#elif FLASH_AREA_SCENARIO == 1
TEST(FlashAreaSelect, APartitionComesFirst) {
  fake_flash::reset(kPartitionBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(32u, area->freeMapPages()) << "the partition, through the MMU";
  EXPECT_EQ(area, FlashArea::get()) << "picked once";
}
#elif FLASH_AREA_SCENARIO == 2
TEST(FlashAreaSelect, WithoutAPartitionThePsramImage) {
  fake_flash::reset(0);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(kPartitionBytes, area->size());
  EXPECT_EQ(kImagePages, area->freeMapPages()) << "the image: a mapping costs no MMU page";
  uint8_t byte = 0;
  ASSERT_TRUE(area->read(kPartitionBytes - 1, &byte, 1));
  EXPECT_EQ(0xFF, byte) << "no image on the card: erased";
}
#elif FLASH_AREA_SCENARIO == 3
TEST(FlashAreaSelect, WithoutAPartitionOrPsramThereIsNone) {
  fake_flash::reset(0);
  g_hostHasPsram = false;
  EXPECT_EQ(nullptr, FlashArea::get());
}
#elif FLASH_AREA_SCENARIO == 4
TEST(FlashAreaSelect, ATestBuildTakesThePsramImageOverThePartition) {
  fake_flash::reset(kPartitionBytes);
  NorFlash* const area = FlashArea::get();
  ASSERT_NE(nullptr, area);
  EXPECT_EQ(kImagePages, area->freeMapPages());
}
#endif
