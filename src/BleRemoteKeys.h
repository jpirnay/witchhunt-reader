#pragma once

// A Bluetooth LE remote's keys as the reader's own buttons. src/BleRadio.cpp feeds the SDK's raw button edges
// (freeink::RawButtonEvent) through a Router and injects each press it returns as a press of the physical button the
// user's mapping gives Page Forward or Page Back (MappedInputManager::rawIndex), so remapping, press types, button
// actions, the reading statistics and the sleep timer all treat it as that button. Pure, for the host tests
// (test/ble_remote_keys/).

#include <BleRemoteProfiles.h>  // the SDK's raw codes of remotes it knows by name

#include <cstdint>

namespace BleRemoteKeys {

enum class Target : uint8_t { None, PageForward, PageBack };

// Held this long or longer, a press is a hold: ButtonEventManager::LONG_PRESS_MS (BleRadio.cpp pins the two together).
// A remote that sends its release a fixed time after the press (the Free3 does, 60-100 ms) only ever taps.
inline constexpr uint32_t kHoldMs = 1000;

// A key the SDK's decoder read (its HID usage, or the low byte of a consumer usage). Page turners in a keyboard mode
// send arrows, Page Up/Down or Space; shutter remotes send the consumer volume keys.
constexpr Target targetForUsage(const uint8_t usage) {
  switch (usage) {
    case 0x4F:  // Right Arrow
    case 0x51:  // Down Arrow
    case 0x4E:  // Page Down
    case 0x2C:  // Space
    case 0xEA:  // Volume Down (consumer)
      return Target::PageForward;
    case 0x50:  // Left Arrow
    case 0x52:  // Up Arrow
    case 0x4B:  // Page Up
    case 0xE9:  // Volume Up (consumer)
      return Target::PageBack;
    default:
      return Target::None;
  }
}

constexpr Target targetForRemoteKey(const freeink::RemoteKey key) {
  switch (key) {
    case freeink::RemoteKey::Right:
    case freeink::RemoteKey::PageDown:
      return Target::PageForward;
    case freeink::RemoteKey::Left:
    case freeink::RemoteKey::PageUp:
      return Target::PageBack;
    case freeink::RemoteKey::Count:
      break;
  }
  return Target::None;
}

// A remote the SDK has a profile for (freeink::remoteButtonProfileIndex(), by its name): its raw codes decide. A
// binding marked anyReport matches the value and byte index under any report id.
inline Target targetForProfile(const int profile, const uint32_t code) {
  if (profile < 0) return Target::None;
  const freeink::RemoteButtonProfile& remote = freeink::kRemoteButtonProfiles[profile];
  for (uint8_t i = 0; i < remote.count; ++i) {
    const freeink::RemoteButtonBinding& binding = remote.buttons[i];
    const uint32_t mask = binding.anyReport ? 0x00FFFFu : 0xFFFFFFu;
    if ((code & mask) == (binding.code & mask)) return targetForRemoteKey(binding.key);
  }
  return Target::None;
}

// One raw edge, the fields of freeink::RawButtonEvent the router reads.
struct Edge {
  uint32_t code;
  bool pressed;
  bool wasRest;     // a release whose "press" was the remote idling on a non-zero status byte
  uint32_t atMs;    // when the frame arrived
  uint8_t keycode;  // on a press: the key the decoder read from the same frame, 0 when none
};

struct Press {
  Target target;
  bool hold;
  uint32_t atMs;  // the release, which dates the injected press
};

// Pairs a press with its release and classifies it then, by how long it was held. One button at a time: a press while
// another is held takes over, and the earlier one's release is ignored.
class Router {
 public:
  // The remote now linked (its advertised name picks the SDK profile), or nullptr; forgets a press in flight.
  void setRemote(const char* name) {
    profile_ = freeink::remoteButtonProfileIndex(name);
    held_ = Held{};
  }

  // True when `out` holds a press to inject.
  bool feed(const Edge& edge, Press& out) {
    if (edge.pressed) {
      Target target = targetForProfile(profile_, edge.code);
      if (target == Target::None) target = targetForUsage(edge.keycode);
      held_ = Held{edge.code, target, edge.atMs, true};
      return false;
    }
    if (!held_.active || edge.code != held_.code) return false;
    held_.active = false;
    if (edge.wasRest || held_.target == Target::None) return false;
    out = Press{held_.target, edge.atMs - held_.pressAtMs >= kHoldMs, edge.atMs};
    return true;
  }

 private:
  struct Held {
    uint32_t code = 0;
    Target target = Target::None;
    uint32_t pressAtMs = 0;
    bool active = false;
  };
  Held held_;
  int profile_ = -1;
};

}  // namespace BleRemoteKeys
