#pragma once

#include <cstddef>
#include <cstdint>

// Ported from the serial test API written for the CrossPoint fork (crosspoint-reader branch
// feat/serial-test-api, c41fae970), where every CMD:BTN press goes this way. Here only the held form
// does: CMD:BTN:<name>:<ms> (SerialTestApi.cpp).
//
// Button presses queued by the serial CMD:BTN, played out one at a time as buttons held down. Kept
// free of the device so the timing can be tested on the host.
//
// The caller samples once per main-loop pass: step() gets the buttons the input layer has committed
// as down and returns the buttons to hold until the next sample (HalGPIO::holdSyntheticButtons()). A
// press starts once its key reads up, is held until the input layer has committed it, then for its
// hold time, then released; the next press starts once that release is committed. So each press goes
// through the same debounce and hold timing as a real key, with a press edge of its own, however long
// the loop passes take.
class ButtonPressQueue {
 public:
  static constexpr uint16_t MAX_HOLD_MS = 10000;
  // A press or release the input layer has not committed after this long, and this many samples, is
  // given up; so is a press whose key has not read up by then. The samples keep one stalled loop
  // pass (a blocking command) from dropping it.
  static constexpr uint16_t COMMIT_TIMEOUT_MS = 1000;
  static constexpr uint8_t COMMIT_TIMEOUT_SAMPLES = 4;
  static constexpr size_t CAPACITY = 4;

  // Queues a press of button index `button` (HalGPIO::BTN_*). False when the queue is full.
  bool push(const uint8_t button, const uint16_t holdMs) {
    if (count == CAPACITY || button >= 8) return false;
    queue[(head + count) % CAPACITY] = {button, holdMs};
    ++count;
    return true;
  }

  size_t freeSlots() const { return CAPACITY - count; }

  // Nothing queued, held, or waiting for its release to be committed.
  bool idle() const { return phase == Phase::Idle && count == 0; }

  // Presses given up because the input layer never committed them, or their key never read up.
  uint32_t droppedPresses() const { return dropped; }

  uint8_t step(const uint32_t nowMs, const uint8_t committed) {
    if (phase == Phase::Idle) {
      if (count == 0) return 0;
      current = queue[head];
      head = static_cast<uint8_t>((head + 1) % CAPACITY);
      --count;
      enter(Phase::Starting, nowMs);
    }
    const auto bit = static_cast<uint8_t>(1u << current.button);
    switch (phase) {
      case Phase::Idle:
        return 0;  // handled above
      case Phase::Starting:
        // The key must read up first, or the press would take over the one already down and have no
        // edge of its own: a finger may be on it, or the last press's release never went through.
        if (!(committed & bit)) {
          enter(Phase::Pressing, nowMs);
          return bit;
        }
        if (commitTimedOut(nowMs)) {
          ++dropped;
          enter(Phase::Idle, nowMs);
        }
        return 0;
      case Phase::Pressing:
        if (committed & bit) {
          enter(Phase::Holding, nowMs);  // the hold counts from the committed press
          return bit;
        }
        if (commitTimedOut(nowMs)) {
          ++dropped;
          enter(Phase::Releasing, nowMs);
          return 0;
        }
        return bit;
      case Phase::Holding:
        if (nowMs - phaseSinceMs >= current.holdMs) {
          enter(Phase::Releasing, nowMs);
          return 0;
        }
        return bit;
      case Phase::Releasing:
        if (!(committed & bit) || commitTimedOut(nowMs)) {
          enter(Phase::Idle, nowMs);
        }
        return 0;
    }
    return 0;
  }

 private:
  enum class Phase : uint8_t { Idle, Starting, Pressing, Holding, Releasing };
  struct Press {
    uint8_t button;
    uint16_t holdMs;
  };

  void enter(const Phase next, const uint32_t nowMs) {
    phase = next;
    phaseSinceMs = nowMs;
    phaseSamples = 0;
  }

  bool commitTimedOut(const uint32_t nowMs) {
    if (phaseSamples < COMMIT_TIMEOUT_SAMPLES) ++phaseSamples;
    return phaseSamples >= COMMIT_TIMEOUT_SAMPLES && nowMs - phaseSinceMs >= COMMIT_TIMEOUT_MS;
  }

  Press queue[CAPACITY] = {};
  uint8_t head = 0;
  uint8_t count = 0;
  Phase phase = Phase::Idle;
  Press current = {};
  uint32_t phaseSinceMs = 0;
  uint8_t phaseSamples = 0;
  uint32_t dropped = 0;
};
