#include "BleRadio.h"

#if FREEINK_CAP_BLE_HID_HOST
#include <Arduino.h>
#include <BleKeyboardHost.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include "BleRadioPolicy.h"
#include "BleRemoteKeys.h"
#include "ButtonEventManager.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#endif

#include <cstdio>
#include <cstring>

extern MappedInputManager mappedInputManager;  // main.cpp

static_assert(BleRemoteKeys::kHoldMs == ButtonEventManager::LONG_PRESS_MS,
              "a remote's hold must be the hardware buttons' long press");

namespace BleRadio {

const char* name(const StartResult result) {
  switch (result) {
    case StartResult::None:
      return "None";
    case StartResult::Started:
      return "Started";
    case StartResult::AlreadyOn:
      return "AlreadyOn";
    case StartResult::Unsupported:
      return "Unsupported";
    case StartResult::StorageBusy:
      return "StorageBusy";
    case StartResult::HeapLow:
      return "HeapLow";
    case StartResult::HeapInPieces:
      return "HeapInPieces";
    case StartResult::RolledBack:
      return "RolledBack";
    case StartResult::BeginFailed:
      return "BeginFailed";
    case StartResult::Stopping:
      return "Stopping";
  }
  return "?";
}

const char* name(const StopReason reason) {
  switch (reason) {
    case StopReason::None:
      return "None";
    case StopReason::Requested:
      return "Requested";
    case StopReason::IdleNoLink:
      return "IdleNoLink";
    case StopReason::Sleep:
      return "Sleep";
    case StopReason::ExclusiveStorage:
      return "ExclusiveStorage";
    case StopReason::RolledBack:
      return "RolledBack";
    case StopReason::Unwanted:
      return "Unwanted";
    case StopReason::WifiOn:
      return "WifiOn";
  }
  return "?";
}

#if FREEINK_CAP_BLE_HID_HOST
namespace {

// The name a remote sees for this reader while it pairs.
constexpr const char* kHostName = "Witch Reader";
// How long stop() waits for the SDK to leave a connect, pairing or discovery wait (BleKeyboardHost::end() caps the
// wait at 2 s). A stop that takes longer is finished by service().
constexpr uint32_t kStopWaitMs = 1000;

// All of it is the loop task's: start(), stop(), service() and status() are called from there alone.
Status state;
bool exclusiveStorage = false;  // the last service() pass's answer
uint32_t idleSinceMs = 0;       // the start, or the last pass a remote was linked
StopReason pendingStop = StopReason::None;
uint32_t stopStartedMs = 0;
BleRemoteKeys::Router router;  // routeInput()
bool routerLinked = false;     // the link state the router was last told about

bool serialStart = false;  // start(): up whatever the setting says, until stop(Requested)
bool screenOpen = false;   // setScreenOpen()
bool idleLatched = false;  // stopped for idleness, and nothing has woken it since
bool refused = false;      // a start refused since the last one that stayed up
uint32_t refusedAtMs = 0;
bool wasInReader = false;  // the last pass's Scene, for its edges
bool wasWanted = false;
// The SDK's bond table holds this many remotes; -1 until it is read (with the radio off that reads NVS, so once).
int savedCount = -1;
// A search cancels the SDK's reconnect plan for the saved remotes: it is armed again when the search ends.
bool rearmAfterScan = false;
// What the SDK reported once (its take* calls consume it), kept for the screen until the next search or pairing.
char failure[48] = {};
bool hasFailure = false;
uint32_t pendingPasskey = 0;
bool hasPasskey = false;

BleRadioPolicy::Heap internalHeap() {
  constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return {heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps)};
}

StartResult refuse(const StartResult result) {
  state.lastStart = result;
  ++state.refusals;
  refused = true;
  refusedAtMs = millis();
  LOG_INF("BLE", "radio start refused (%s): internal %u/%u", name(result), static_cast<unsigned>(state.freeBefore),
          static_cast<unsigned>(state.largestBefore));
  return result;
}

// One end() attempt; waitMs 0 never waits. The controller's deinit must run at the normal clock (BleKeyboardHost::end),
// which the radio hold keeps, but a waveform wait may have dropped it before the hold was set.
bool tryEnd(const uint32_t waitMs) {
  powerManager.ensureFullSpeed();
  return BleHid.end(waitMs);
}

// end() finished: the radio's memory is back, and the chip may slow down and light-sleep again.
void finishStop(const StopReason reason) {
  state.on = false;
  state.stopping = false;
  state.linked = false;
  state.connecting = false;
  state.lastStop = reason;
  state.lastEndMs = millis() - stopStartedMs;
  pendingStop = StopReason::None;
  router.setRemote(nullptr);
  routerLinked = false;
  rearmAfterScan = false;
  // Asleep until a book opens, a key is pressed in one, the screen opens or the remote is switched on again.
  if (reason == StopReason::IdleNoLink) idleLatched = true;
  powerManager.setRadioActive(false);
  const BleRadioPolicy::Heap heap = internalHeap();
  LOG_INF("BLE", "radio off (%s) after %lu ms: internal %u/%u", name(reason),
          static_cast<unsigned long>(state.lastEndMs), static_cast<unsigned>(heap.freeBytes),
          static_cast<unsigned>(heap.largestBlock));
}

void beginStop(const StopReason reason) {
  stopStartedMs = millis();
  if (tryEnd(kStopWaitMs)) {
    finishStop(reason);
    return;
  }
  state.stopping = true;
  pendingStop = reason;
  LOG_INF("BLE", "radio stop (%s) waits for the SDK", name(reason));
}

// The remote's keys as the mapped Page Forward / Page Back buttons (BleRemoteKeys.h). The key ring is drained unread:
// every press also arrives as a raw edge, which carries the timing a hold needs. A press is injected at its release.
void routeInput(const bool linked) {
  if (linked != routerLinked) router.setRemote(linked ? BleHid.connectedName() : nullptr);
  routerLinked = linked;
  freeink::KeyEvent key;
  while (BleHid.popKey(key)) {
  }
  freeink::RawButtonEvent raw;
  while (BleHid.popRawButton(raw)) {
    BleRemoteKeys::Press press{};
    if (!router.feed(BleRemoteKeys::Edge{raw.code(), raw.pressed, raw.wasRest, raw.atMs, raw.keycode}, press)) {
      continue;
    }
    // Swap Remote Keys: for a remote whose two keys turn the wrong way (the Free2 on its Kobo/PC firmware sends Left
    // Arrow from its right key).
    const bool forward = (press.target == BleRemoteKeys::Target::PageForward) != (SETTINGS.bleSwapKeys != 0);
    const MappedInputManager::Button button =
        forward ? MappedInputManager::Button::PageForward : MappedInputManager::Button::PageBack;
    mappedInputManager.injectRawPress(mappedInputManager.rawIndex(button), press.hold, press.atMs);
    LOG_DBG("BLE", "remote %s%s", forward ? "page forward" : "page back", press.hold ? " (hold)" : "");
  }
}

StartResult startRadio() {
  if (state.stopping) return refuse(StartResult::Stopping);
  if (state.on) return StartResult::AlreadyOn;
  const BleRadioPolicy::Heap before = internalHeap();
  state.freeBefore = before.freeBytes;
  state.largestBefore = before.largestBlock;
  switch (BleRadioPolicy::beforeStart(before, exclusiveStorage)) {
    case BleRadioPolicy::Verdict::Ok:
      break;
    case BleRadioPolicy::Verdict::StorageBusy:
      return refuse(StartResult::StorageBusy);
    case BleRadioPolicy::Verdict::HeapLow:
      return refuse(StartResult::HeapLow);
    case BleRadioPolicy::Verdict::HeapInPieces:
      return refuse(StartResult::HeapInPieces);
  }

  // Full speed before the controller comes up, and held from here: begin() initialises the controller.
  powerManager.ensureFullSpeed();
  powerManager.setRadioActive(true);
  const uint32_t beginAt = millis();
  const bool begun = BleHid.begin(kHostName);
  state.lastBeginMs = millis() - beginAt;
  const BleRadioPolicy::Heap after = internalHeap();
  state.freeAfter = after.freeBytes;
  state.largestAfter = after.largestBlock;
  if (!begun) {
    powerManager.setRadioActive(false);
    return refuse(StartResult::BeginFailed);
  }
  state.on = true;
  if (BleRadioPolicy::afterStart(after, exclusiveStorage) != BleRadioPolicy::Verdict::Ok) {
    beginStop(StopReason::RolledBack);
    return refuse(StartResult::RolledBack);
  }

  state.lastStart = StartResult::Started;
  ++state.starts;
  refused = false;
  idleSinceMs = millis();
  // A remote bonded before comes back by itself once it advertises: the SDK scans for it for 120 s.
  if (BleHid.pairedCount() > 0) BleHid.armBondedReconnect(freeink::PickPolicy::First, nullptr);
  LOG_INF("BLE", "radio on in %lu ms: internal %u/%u -> %u/%u, %u bonds", static_cast<unsigned long>(state.lastBeginMs),
          static_cast<unsigned>(before.freeBytes), static_cast<unsigned>(before.largestBlock),
          static_cast<unsigned>(after.freeBytes), static_cast<unsigned>(after.largestBlock),
          static_cast<unsigned>(BleHid.pairedCount()));
  return StartResult::Started;
}

uint8_t savedRemotes() {
  if (state.on) return BleHid.pairedCount();
  if (savedCount < 0) {
    freeink::PairedHidDevice first;
    BleHid.firstSavedBond(first);  // with the radio off it loads the table from NVS
    savedCount = BleHid.pairedCount();
  }
  return static_cast<uint8_t>(savedCount);
}

// The k-th remote the search found, as an index into the SDK's list: the ones that advertise HID first, then the other
// named devices; saved remotes and nameless non-HID devices left out. -1 past the last.
int foundDevice(const uint8_t k) {
  uint8_t seen = 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint8_t i = 0; i < BleHid.deviceCount(); ++i) {
      const freeink::DiscoveredDevice& d = BleHid.device(i);
      const bool wanted = pass == 0 ? d.hid : (!d.hid && d.hasName);
      if (!wanted) continue;
      bool saved = false;
      for (uint8_t b = 0; b < BleHid.pairedCount(); ++b) saved |= std::strcmp(BleHid.paired(b).addr, d.addr) == 0;
      if (saved) continue;
      if (seen++ == k) return i;
    }
  }
  return -1;
}

void noteSdkReports() {
  if (BleHid.takeConnectFailure(failure, sizeof(failure))) {
    hasFailure = true;
    LOG_INF("BLE", "connect failed: %s", failure);
  }
  uint32_t key = 0;
  if (BleHid.takePairingPasskey(key)) {
    pendingPasskey = key;
    hasPasskey = true;
  }
}

}  // namespace

StartResult start() {
  serialStart = true;
  return startRadio();
}

void stop(const StopReason reason) {
  if (reason == StopReason::Requested) serialStart = false;
  if (!state.on || state.stopping) return;  // off, or service() finishes the stop under way
  beginStop(reason);
}

void service(const Scene& scene) {
  exclusiveStorage = scene.exclusiveStorage;
  if (state.stopping) {
    if (tryEnd(0)) finishStop(pendingStop);
    return;
  }

  // What ends a stop for idleness, the moments a remote is wanted again: a book opens, a key is pressed in one, the
  // remote is switched on, the screen is open. Opening a book or switching on also retries a start refused for memory.
  const bool enteredReader = scene.inReader && !wasInReader;
  const bool switchedOn = scene.wanted && !wasWanted;
  wasInReader = scene.inReader;
  wasWanted = scene.wanted;
  if (enteredReader || switchedOn || screenOpen || (scene.inReader && scene.localKey)) idleLatched = false;
  if (enteredReader || switchedOn) refused = false;

  // With no remote saved only the screen wants the radio: there is nothing to reconnect to.
  const bool wanted = serialStart || (scene.wanted && (screenOpen || savedRemotes() > 0));
  state.wifiPaused = wanted && scene.wifiOn;
  const BleRadioPolicy::Wish wish{wanted, scene.wifiOn, exclusiveStorage, idleLatched,
                                  !refused || millis() - refusedAtMs >= BleRadioPolicy::kRetryAfterRefusalMs};
  switch (BleRadioPolicy::decide(state.on, wish)) {
    case BleRadioPolicy::Decision::Keep:
      break;
    case BleRadioPolicy::Decision::Start:
      startRadio();
      return;
    case BleRadioPolicy::Decision::StopUnwanted:
      beginStop(StopReason::Unwanted);
      return;
    case BleRadioPolicy::Decision::StopWifi:
      beginStop(StopReason::WifiOn);
      return;
  }
  if (!state.on) return;

  BleHid.poll();
  noteSdkReports();
  state.linked = BleHid.isConnected();
  routeInput(state.linked);
  state.connecting = BleHid.isConnecting();
  savedCount = BleHid.pairedCount();
  if (state.linked) hasPasskey = false;
  if (rearmAfterScan && !BleHid.isScanning()) {
    rearmAfterScan = false;
    if (!state.linked && !state.connecting && savedCount > 0) {
      BleHid.armBondedReconnect(freeink::PickPolicy::First, nullptr);
    }
  }
  const uint32_t now = millis();
  // The screen keeps a scan or a pairing from looking idle.
  if (state.linked || screenOpen) idleSinceMs = now;
  switch (BleRadioPolicy::whileRunning(state.linked, state.connecting, now - idleSinceMs, exclusiveStorage)) {
    case BleRadioPolicy::Keep::Keep:
      return;
    case BleRadioPolicy::Keep::StopStorage:
      beginStop(StopReason::ExclusiveStorage);
      return;
    case BleRadioPolicy::Keep::StopIdleNoLink:
      beginStop(StopReason::IdleNoLink);
      return;
  }
}

Status status() {
  Status out = state;
  out.supported = HalCapabilities::hasBleRadio();
  out.bonds = out.on ? BleHid.pairedCount() : 0;
  out.idleStopped = idleLatched;
  return out;
}

void setScreenOpen(const bool open) {
  screenOpen = open;
  if (open) idleLatched = false;
}

bool scan(const uint32_t ms) {
  if (!state.on || state.stopping) return false;
  hasFailure = false;
  hasPasskey = false;
  BleHid.releaseScanResults();
  BleHid.startScan(ms);
  rearmAfterScan = true;
  return true;
}

void stopScan() {
  if (state.on && BleHid.isScanning()) BleHid.stopScan();
}

bool scanning() { return state.on && BleHid.isScanning(); }

uint8_t foundCount() {
  if (!state.on) return 0;
  uint8_t count = 0;
  while (foundDevice(count) >= 0) ++count;
  return count;
}

Peer found(const uint8_t index) {
  const int i = state.on ? foundDevice(index) : -1;
  if (i < 0) return {};
  const freeink::DiscoveredDevice& d = BleHid.device(static_cast<uint8_t>(i));
  return {d.addr, d.hasName ? d.name : ""};
}

bool pair(const char* addr) {
  if (!state.on || state.stopping || addr == nullptr || addr[0] == '\0') return false;
  hasFailure = false;
  hasPasskey = false;
  rearmAfterScan = false;
  LOG_INF("BLE", "pairing %s", addr);
  return BleHid.connect(addr);
}

uint8_t savedRemoteCount() { return savedRemotes(); }

Peer savedRemote(const uint8_t index) {
  if (index >= savedRemotes()) return {};
  const freeink::PairedHidDevice& d = BleHid.paired(index);
  return {d.addr, d.name};
}

bool forget(const char* addr) {
  if (!state.on || state.stopping || addr == nullptr) return false;
  BleHid.forget(addr);
  savedCount = BleHid.pairedCount();
  LOG_INF("BLE", "forgot %s, %d saved", addr, savedCount);
  return true;
}

Peer linked() {
  if (!state.on || !BleHid.isConnected()) return {};
  return {BleHid.connectedAddr(), BleHid.connectedName()};
}

bool lastFailure(char* out, const size_t outLen) {
  if (!hasFailure || out == nullptr || outLen == 0) return false;
  snprintf(out, outLen, "%s", failure);
  return true;
}

bool passkey(uint32_t& out) {
  if (!hasPasskey) return false;
  out = pendingPasskey;
  return true;
}
#endif

}  // namespace BleRadio
