#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();

// Feed the task watchdog -- but only if the calling task is actually subscribed
// to it.
//
// esp_task_wdt_reset() does not fail quietly. From a task that was never
// esp_task_wdt_add()ed it returns ESP_ERR_NOT_FOUND and emits an ESP_LOGE
// ("task not found") on EVERY call. None of this firmware's worker tasks are
// subscribed, so a decode or layout loop that feeds the watchdog every few rows
// turns into thousands of error lines on the console.
//
// That is not merely noise. Device-measured on the X4 Pro opening a book: a
// ~4.8 second unbroken wall of those lines, during which the page render took
// 9.6 s and one loop pass 5.7 s. Each line is a synchronous write to a USB-CDC
// console that has to be drained by the host.
//
// So ask first. The lookup walks the watchdog's subscribed-task list, which is
// two or three entries, and is far cheaper than the log line it replaces. It is
// deliberately NOT cached in a static: this is called from several tasks and
// the answer is per-task.
//
// The SDK reached the same conclusion independently for its SD streaming path
// (SDCardManager::readFileToStream), which hoists the same query out of its
// loop. Same idiom, one place.
void feedWatchdog();
}  // namespace HalSystem
