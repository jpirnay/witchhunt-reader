// When the emergency-mode notice is due: once per layout, never on the layout the firmware was made for.

#include <gtest/gtest.h>

#include "FlashLayoutNotice.h"

namespace {
using FlashLayoutNotice::dueFor;
using FlashLayoutNotice::layoutMark;

constexpr uint32_t kNone = 0;
constexpr uint32_t kStockX3 = 1024 * 1024;  // the X3's original table: a 1 MB `spiffs` partition
constexpr uint32_t kNeverShown = 0;
}  // namespace

TEST(FlashLayoutNotice, NeverOnTheExpectedLayout) {
  EXPECT_FALSE(dueFor(true, 0x360000, kNeverShown));
  EXPECT_FALSE(dueFor(true, kNone, kNeverShown)) << "a PSRAM image stands in: no notice";
}

TEST(FlashLayoutNotice, OnceInEmergencyMode) {
  EXPECT_TRUE(dueFor(false, kNone, kNeverShown));
  EXPECT_FALSE(dueFor(false, kNone, layoutMark(kNone)));
  EXPECT_TRUE(dueFor(false, kStockX3, kNeverShown));
  EXPECT_FALSE(dueFor(false, kStockX3, layoutMark(kStockX3)));
}

TEST(FlashLayoutNotice, AgainForAnotherLayout) {
  EXPECT_TRUE(dueFor(false, kStockX3, layoutMark(kNone)));
  EXPECT_TRUE(dueFor(false, kNone, layoutMark(kStockX3)));
}

TEST(FlashLayoutNotice, NoLayoutMarksAsNeverShown) {
  EXPECT_NE(kNeverShown, layoutMark(kNone)) << "0 in state.json must keep meaning never shown";
  EXPECT_NE(layoutMark(kNone), layoutMark(80 * 1024));
}
