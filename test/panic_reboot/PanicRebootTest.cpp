// PanicReboot (lib/hal/PanicReboot.h): which reset leaves a crash report. A BOOX Picco wrote one with
// no reason and no stack after a reset it logged as resetReason=6 (ESP_RST_TASK_WDT), with nothing
// from the panic handler behind it.

#include <gtest/gtest.h>

#include "hal/PanicReboot.h"

namespace {

constexpr esp_reset_reason_t kWatchdogResets[] = {ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT};

TEST(PanicReboot, APanicCountsWithOrWithoutTheMarker) {
  for (const auto reason : {ESP_RST_PANIC, ESP_RST_CPU_LOCKUP}) {
    EXPECT_TRUE(PanicReboot::isFromPanic(reason, true)) << reason;
    EXPECT_TRUE(PanicReboot::isFromPanic(reason, false)) << reason;
  }
}

TEST(PanicReboot, AWatchdogPanicCounts) {
  for (const auto reason : kWatchdogResets) {
    EXPECT_TRUE(PanicReboot::isFromPanic(reason, true)) << reason;
  }
}

TEST(PanicReboot, AWatchdogResetWithNoPanicBehindItDoesNotCount) {
  for (const auto reason : kWatchdogResets) {
    EXPECT_FALSE(PanicReboot::isFromPanic(reason, false)) << reason;
  }
}

// After the flasher's RTC watchdog restart the X4 Pro and the Picco logged ESP_RST_POWERON; an RTS
// reset reads as ESP_RST_USB. Neither counts, even with the marker set.
TEST(PanicReboot, NoOtherResetCountsEvenWithTheMarker) {
  for (const auto reason :
       {ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO,
        ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE, ESP_RST_PWR_GLITCH}) {
    EXPECT_FALSE(PanicReboot::isFromPanic(reason, true)) << reason;
  }
}

}  // namespace
