#include "BleRadio.h"

#if FREEINK_CAP_BLE_HID_HOST
#include <Arduino.h>
#include <BleKeyboardHost.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include "BleRadioPolicy.h"
#endif

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

BleRadioPolicy::Heap internalHeap() {
  constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return {heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps)};
}

StartResult refused(const StartResult result) {
  state.lastStart = result;
  ++state.refusals;
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

// Until the remote's keys are routed to the buttons, a key is logged and dropped: the SDK's rings never hold a press
// older than the page it would turn.
void drainInput() {
  freeink::KeyEvent key;
  while (BleHid.popKey(key)) {
    LOG_DBG("BLE", "key %02x (special %u) dropped", key.keycode, static_cast<unsigned>(key.special));
  }
  freeink::RawButtonEvent raw;
  while (BleHid.popRawButton(raw)) {
  }
}

}  // namespace

StartResult start() {
  if (state.stopping) return refused(StartResult::Stopping);
  if (state.on) return StartResult::AlreadyOn;
  const BleRadioPolicy::Heap before = internalHeap();
  state.freeBefore = before.freeBytes;
  state.largestBefore = before.largestBlock;
  switch (BleRadioPolicy::beforeStart(before, exclusiveStorage)) {
    case BleRadioPolicy::Verdict::Ok:
      break;
    case BleRadioPolicy::Verdict::StorageBusy:
      return refused(StartResult::StorageBusy);
    case BleRadioPolicy::Verdict::HeapLow:
      return refused(StartResult::HeapLow);
    case BleRadioPolicy::Verdict::HeapInPieces:
      return refused(StartResult::HeapInPieces);
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
    return refused(StartResult::BeginFailed);
  }
  state.on = true;
  if (BleRadioPolicy::afterStart(after, exclusiveStorage) != BleRadioPolicy::Verdict::Ok) {
    beginStop(StopReason::RolledBack);
    return refused(StartResult::RolledBack);
  }

  state.lastStart = StartResult::Started;
  ++state.starts;
  idleSinceMs = millis();
  // A remote bonded before comes back by itself once it advertises: the SDK scans for it for 120 s.
  if (BleHid.pairedCount() > 0) BleHid.armBondedReconnect(freeink::PickPolicy::First, nullptr);
  LOG_INF("BLE", "radio on in %lu ms: internal %u/%u -> %u/%u, %u bonds", static_cast<unsigned long>(state.lastBeginMs),
          static_cast<unsigned>(before.freeBytes), static_cast<unsigned>(before.largestBlock),
          static_cast<unsigned>(after.freeBytes), static_cast<unsigned>(after.largestBlock),
          static_cast<unsigned>(BleHid.pairedCount()));
  return StartResult::Started;
}

void stop(const StopReason reason) {
  if (!state.on || state.stopping) return;  // off, or service() finishes the stop under way
  beginStop(reason);
}

void service(const bool exclusive) {
  exclusiveStorage = exclusive;
  if (state.stopping) {
    if (tryEnd(0)) finishStop(pendingStop);
    return;
  }
  if (!state.on) return;
  BleHid.poll();
  drainInput();
  state.linked = BleHid.isConnected();
  state.connecting = BleHid.isConnecting();
  const uint32_t now = millis();
  if (state.linked) idleSinceMs = now;
  switch (BleRadioPolicy::whileRunning(state.linked, state.connecting, now - idleSinceMs, exclusive)) {
    case BleRadioPolicy::Keep::Keep:
      return;
    case BleRadioPolicy::Keep::StopStorage:
      stop(StopReason::ExclusiveStorage);
      return;
    case BleRadioPolicy::Keep::StopIdleNoLink:
      stop(StopReason::IdleNoLink);
      return;
  }
}

Status status() {
  Status out = state;
  out.supported = HalCapabilities::hasBleRadio();
  out.bonds = out.on ? BleHid.pairedCount() : 0;
  return out;
}
#endif

}  // namespace BleRadio
