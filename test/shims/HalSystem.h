#pragma once

#include <string>

// Host-test shim for lib/hal/HalSystem.h.
//
// The real header is fine on its own, but its implementation pulls
// esp_task_wdt.h, esp_debug_helpers.h and the ESP-IDF panic internals, none of
// which exist on the host. The decode and layout code under test reaches it for
// exactly one thing -- feedWatchdog() -- and on a host there is no watchdog to
// feed, so this is a no-op.
//
// The panic helpers are declared but deliberately not defined: nothing under
// test calls them, and leaving them undefined means a future caller fails at
// link time here rather than silently testing a stub that lies.

namespace HalSystem {

inline void feedWatchdog() {}

void begin();
void checkPanic();
void clearPanic();
std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();

}  // namespace HalSystem
