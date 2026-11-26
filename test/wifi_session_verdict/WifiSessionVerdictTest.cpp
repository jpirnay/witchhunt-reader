// Whether a Wi-Fi screen reboots at its exit or carries on (src/network/WifiSessionVerdict.h): the C3s always
// reboot; the PSRAM boards carry on while the internal heap still covers the next session.
#include <gtest/gtest.h>

#include "network/WifiSessionVerdict.h"

using wifi_session::Exit;
using wifi_session::verdict;

TEST(WifiSessionVerdict, MinimumsAreBackgroundSyncs) {
  EXPECT_EQ(wifi_session::kMinFreeInternalBytes, 67584u);
  EXPECT_EQ(wifi_session::kMinLargestInternalBytes, 24576u);
}

TEST(WifiSessionVerdict, TheC3sAlwaysReboot) { EXPECT_EQ(verdict(true, 300000, 200000), Exit::Reboot); }

TEST(WifiSessionVerdict, ExactlyAtTheMinimumsCarriesOn) { EXPECT_EQ(verdict(false, 67584, 24576), Exit::CarryOn); }

TEST(WifiSessionVerdict, OneByteShortOfFreeReboots) { EXPECT_EQ(verdict(false, 67583, 200000), Exit::Reboot); }

TEST(WifiSessionVerdict, OneByteShortOfTheBlockReboots) { EXPECT_EQ(verdict(false, 300000, 24575), Exit::Reboot); }

// The figures the T5S3 left after its first Wi-Fi session (2026-11-26): about 19 KB below boot, still well above.
TEST(WifiSessionVerdict, TheT5S3AfterItsFirstSessionCarriesOn) {
  EXPECT_EQ(verdict(false, 120020, 49140), Exit::CarryOn);
}
