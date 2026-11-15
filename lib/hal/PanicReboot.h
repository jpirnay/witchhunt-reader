#pragma once

#include <esp_system.h>

// Whether a reset leaves a crash report (HalSystem::isRebootFromPanic). Pure, so test/panic_reboot
// runs it on the host.
//
// A watchdog reset counts only when the panic handler ran before it (`panicCaptured`: HalSystem's
// panic hooks set an RTC marker). A watchdog panic does go through the panic handler and reboots
// with the watchdog's reset hint, but IDF reports the same reasons for a watchdog that reset the
// chip with no panic handler running: ESP_RST_TASK_WDT for any Timer Group 0 watchdog reset,
// ESP_RST_WDT for any RTC watchdog reset (IDF 5.5 esp_system/port/soc/esp32s3/reset_reason.c, the
// C3's alike). Such a reset leaves nothing to report. A BOOX Picco wrote a report with no reason
// and no stack after a reset it logged as ESP_RST_TASK_WDT, over an older genuine one.
namespace PanicReboot {

inline bool isFromPanic(const esp_reset_reason_t reason, const bool panicCaptured) {
  switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP:
      return true;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return panicCaptured;
    default:
      return false;
  }
}

}  // namespace PanicReboot
