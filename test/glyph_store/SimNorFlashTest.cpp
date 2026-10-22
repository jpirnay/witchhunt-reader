// The simulated NOR flash the glyph store's host tests run on: it has to behave like the chip.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "SimNorFlash.h"

TEST(SimNorFlash, ProgramClearsBitsOnly) {
  SimNorFlash flash(8192);
  const uint8_t first = 0xF0;
  const uint8_t second = 0x3C;  // bits 2-3 would have to go from 0 back to 1
  ASSERT_TRUE(flash.program(10, &first, 1));
  ASSERT_TRUE(flash.program(10, &second, 1));
  uint8_t cell = 0;
  ASSERT_TRUE(flash.read(10, &cell, 1));
  EXPECT_EQ(0x30, cell);
  EXPECT_EQ(1u, flash.violations());
}

TEST(SimNorFlash, EraseSetsWholeSectorsAndCountsThem) {
  SimNorFlash flash(3 * 4096);
  const uint8_t zero = 0;
  ASSERT_TRUE(flash.program(4096 + 5, &zero, 1));
  ASSERT_TRUE(flash.erase(4096, 4096));
  uint8_t cell = 0;
  ASSERT_TRUE(flash.read(4096 + 5, &cell, 1));
  EXPECT_EQ(0xFF, cell);
  EXPECT_EQ(0u, flash.sectorErases(0));
  EXPECT_EQ(1u, flash.sectorErases(1));
  EXPECT_FALSE(flash.erase(100, 4096));  // not sector-aligned
  EXPECT_FALSE(flash.erase(4096, 100));
  EXPECT_EQ(1u, flash.totalSectorErases());
}

TEST(SimNorFlash, PowerCutStopsAWriteAndEverythingAfter) {
  SimNorFlash flash(8192);
  const uint8_t zeros[8] = {};
  flash.cutAfterSteps(3);
  EXPECT_FALSE(flash.program(0, zeros, sizeof zeros));
  EXPECT_TRUE(flash.dead());
  uint8_t cells[8] = {};
  ASSERT_TRUE(flash.read(0, cells, sizeof cells));
  for (int i = 0; i < 3; ++i) EXPECT_EQ(0x00, cells[i]) << i;
  for (int i = 3; i < 8; ++i) EXPECT_EQ(0xFF, cells[i]) << i;
  EXPECT_FALSE(flash.program(100, zeros, 1));
  EXPECT_FALSE(flash.erase(4096, 4096));
  flash.powerCycle();
  EXPECT_TRUE(flash.program(100, zeros, 1));
}

TEST(SimNorFlash, PartialCutLeavesSomeBitsOfTheByteInFlight) {
  SimNorFlash flash(8192);
  flash.partialCut(true);
  flash.cutAfterSteps(0);
  const uint8_t zero = 0x00;
  EXPECT_FALSE(flash.program(7, &zero, 1));
  uint8_t cell = 0;
  ASSERT_TRUE(flash.read(7, &cell, 1));
  EXPECT_EQ(0x5A, cell);
}

TEST(SimNorFlash, PowerCutDuringEraseLeavesTheSectorHalfErased) {
  SimNorFlash flash(8192);
  const std::vector<uint8_t> zeros(4096, 0);
  ASSERT_TRUE(flash.program(0, zeros.data(), 4096));
  flash.cutAfterSteps(0);
  EXPECT_FALSE(flash.erase(0, 4096));
  uint8_t first = 0;
  uint8_t last = 0xFF;
  ASSERT_TRUE(flash.read(0, &first, 1));
  ASSERT_TRUE(flash.read(4095, &last, 1));
  EXPECT_EQ(0xFF, first);
  EXPECT_EQ(0x00, last);
}

TEST(SimNorFlash, StepsCountBytesAndSectors) {
  SimNorFlash flash(3 * 4096);
  const uint8_t zeros[10] = {};
  ASSERT_TRUE(flash.program(0, zeros, sizeof zeros));
  EXPECT_EQ(10u, flash.steps());
  ASSERT_TRUE(flash.erase(4096, 8192));
  EXPECT_EQ(12u, flash.steps());
}
