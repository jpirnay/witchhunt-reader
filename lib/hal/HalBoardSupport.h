#pragma once

#include <HalGPIO.h>

namespace HalBoardSupport {

// Runs before BoardConfig::holdPowerRails() to filter spurious expander wakes and configure early radio state.
void earlyBoot();

// Brings up per-board support layers and adopts shared I2C bus mutexes before HalGPIO::begin().
void begin();

// Logs board-specific wake diagnostics once serial logging is up.
void onSerialReady();

// Stops board-only peripherals (e.g. haptics) on sleep entry.
void prepareForDeepSleep();

// Returns false when light sleep is unsafe on this board/toolchain.
bool allowsLightSleep();

// Handles board-specific deep sleep (e.g. expander wake). Returns true if handled (does not return);
// returns false to fall through to HalPowerManager's standard deep-sleep path.
bool enterDeepSleep();

// Allows a board with an expander power key to supply its wakeup reason. Returns true if overridden.
bool overrideWakeupReason(HalGPIO::WakeupReason& out);

}  // namespace HalBoardSupport
