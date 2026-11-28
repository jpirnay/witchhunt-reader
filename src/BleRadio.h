#pragma once

#include <HalCapabilities.h>

#include <cstddef>
#include <cstdint>

// The one owner of the Bluetooth LE radio: the only code that starts, polls and stops the SDK's BleKeyboardHost
// (BleHid). Loop task only, no task of its own; the rules it follows are BleRadioPolicy.h.
//
// While the radio is up the CPU stays at full speed and the chip out of light sleep (HalPowerManager::setRadioActive):
// the controller needs the 80 MHz APB clock, as WiFi does. The radio stops on its own before deep sleep, when USB
// Drive has the card, and after BleRadioPolicy::kIdleNoLinkMs with no remote linked.
//
// Builds without a BLE host linked (HalCapabilities::hasBleRadio() false: the C3 boards, the S3 release envs) get the
// inline stubs below, so callers need no #if and those builds link no Bluetooth code; they do not compile
// src/BleRadio.cpp at all ([base] build_src_filter in platformio.ini says why).
namespace BleRadio {

enum class StartResult : uint8_t {
  None,  // no start asked since boot
  Started,
  AlreadyOn,
  Unsupported,   // this build links no BLE host
  StorageBusy,   // USB Drive has the card
  HeapLow,       // BleRadioPolicy::beforeStart
  HeapInPieces,  // BleRadioPolicy::beforeStart
  RolledBack,    // came up, then BleRadioPolicy::afterStart took it down again
  BeginFailed,   // BleHid.begin() refused
  Stopping,      // an earlier stop has not finished
};

enum class StopReason : uint8_t {
  None,  // never stopped since boot
  Requested,
  IdleNoLink,
  Sleep,
  ExclusiveStorage,
  RolledBack,
};

struct Status {
  bool supported = false;
  bool on = false;        // begin() succeeded and no stop has finished since
  bool stopping = false;  // a stop that end() has not finished yet
  bool linked = false;
  bool connecting = false;
  uint8_t bonds = 0;
  uint32_t starts = 0;    // radios that stayed up
  uint32_t refusals = 0;  // starts refused or rolled back
  StartResult lastStart = StartResult::None;
  StopReason lastStop = StopReason::None;
  // Internal RAM around the last start that reached begin(): before it and right after it.
  size_t freeBefore = 0;
  size_t largestBefore = 0;
  size_t freeAfter = 0;
  size_t largestAfter = 0;
  uint32_t lastBeginMs = 0;  // how long the last begin() took
  uint32_t lastEndMs = 0;    // how long the last finished stop took
};

const char* name(StartResult result);
const char* name(StopReason reason);

#if FREEINK_CAP_BLE_HID_HOST
StartResult start();
// Stops the radio, waiting at most about a second for the SDK to leave a connect or pairing wait; a stop that took
// longer is finished by service(). A radio that is off is left alone.
void stop(StopReason reason);
// Every loop pass. `exclusiveStorage`: USB Drive holds the card (ActivityManager::requiresExclusiveStorageLoop()).
void service(bool exclusiveStorage);
Status status();
#else
inline StartResult start() { return StartResult::Unsupported; }
inline void stop(StopReason) {}
inline void service(bool) {}
inline Status status() {
  Status out;
  out.supported = HalCapabilities::hasBleRadio();
  return out;
}
#endif

}  // namespace BleRadio
