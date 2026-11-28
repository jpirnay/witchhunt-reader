#pragma once

#include <HalCapabilities.h>

#include <cstddef>
#include <cstdint>

// The one owner of the Bluetooth LE radio: the only code that starts, polls and stops the SDK's BleKeyboardHost
// (BleHid). Loop task only, no task of its own; the rules it follows are BleRadioPolicy.h.
//
// It is up while the user has the remote switched on (SETTINGS.bleRemote) and a remote saved, or the Bluetooth Remote
// screen is open (BleRadioPolicy::decide). It waits while Wi-Fi is up or USB Drive has the card, stops before deep
// sleep, and stops after BleRadioPolicy::kIdleNoLinkMs with no remote linked, until a book opens, a key is pressed in
// one, or the screen opens.
//
// While the radio is up the CPU stays at full speed and the chip out of light sleep (HalPowerManager::setRadioActive):
// the controller needs the 80 MHz APB clock, as WiFi does.
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
  Unwanted,  // the user switched the remote off
  WifiOn,    // Wi-Fi came up; the radio starts again once it is gone
};

// What the loop task knows on a pass (main.cpp builds it).
struct Scene {
  bool wanted = false;            // the user switched the remote on (SETTINGS.bleRemote)
  bool exclusiveStorage = false;  // USB Drive holds the card (ActivityManager::requiresExclusiveStorageLoop())
  bool wifiOn = false;            // Wi-Fi is up
  bool inReader = false;          // a book is open (ActivityManager::isReaderActivity())
  bool localKey = false;          // one of the device's own keys went up this pass
};

// A remote: found by a search, saved (bonded), or linked. Valid until the next call into BleRadio.
struct Peer {
  const char* addr = "";
  const char* name = "";
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
  bool idleStopped = false;  // stopped for idleness, until a book opens, a key is pressed in one, or the screen opens
  bool wifiPaused = false;   // wanted, and off while Wi-Fi is up
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

// The last start was refused or rolled back for want of memory.
constexpr bool refusedForMemory(const Status& s) {
  return s.lastStart == StartResult::HeapLow || s.lastStart == StartResult::HeapInPieces ||
         s.lastStart == StartResult::RolledBack;
}

#if FREEINK_CAP_BLE_HID_HOST
// The serial test API's start: the radio stays up whatever the setting says (until stop(), the idle time, USB Drive
// or sleep). Everything else asks through the setting, which service() follows.
StartResult start();
// Stops the radio, waiting at most about a second for the SDK to leave a connect or pairing wait; a stop that took
// longer is finished by service(). A radio that is off is left alone. Ends a serial start.
void stop(StopReason reason);
// Every loop pass: starts a wanted radio (BleRadioPolicy::decide), stops one nobody wants, polls the SDK.
void service(const Scene& scene);
Status status();

// The Bluetooth Remote screen (src/activities/settings/BleRemoteActivity). Loop task only.
// While it is open the radio does not stop for idleness, starts with no remote saved yet, and a stop for idleness ends.
void setScreenOpen(bool open);
// A search for remotes, `ms` long; false while the radio is off.
bool scan(uint32_t ms);
void stopScan();
bool scanning();
// What the search found: remotes that advertise the HID service first, then the other named devices; saved ones out.
uint8_t foundCount();
Peer found(uint8_t index);
// Pairs with a found remote (Just Works); it becomes a saved one once it links.
bool pair(const char* addr);
// The remotes saved (bonded) on this device, whether the radio is on or off.
uint8_t savedRemoteCount();
Peer savedRemote(uint8_t index);
// Forgets a saved remote; false while the radio is off (the SDK deletes the bond through the stack).
bool forget(const char* addr);
Peer linked();
// The last connect failure (the SDK's words) or passkey to type, kept until the next search or pairing.
bool lastFailure(char* out, size_t outLen);
bool passkey(uint32_t& out);
#else
inline StartResult start() { return StartResult::Unsupported; }
inline void stop(StopReason) {}
inline void service(const Scene&) {}
inline Status status() {
  Status out;
  out.supported = HalCapabilities::hasBleRadio();
  return out;
}
inline void setScreenOpen(bool) {}
inline bool scan(uint32_t) { return false; }
inline void stopScan() {}
inline bool scanning() { return false; }
inline uint8_t foundCount() { return 0; }
inline Peer found(uint8_t) { return {}; }
inline bool pair(const char*) { return false; }
inline uint8_t savedRemoteCount() { return 0; }
inline Peer savedRemote(uint8_t) { return {}; }
inline bool forget(const char*) { return false; }
inline Peer linked() { return {}; }
inline bool lastFailure(char*, size_t) { return false; }
inline bool passkey(uint32_t&) { return false; }
#endif

}  // namespace BleRadio
