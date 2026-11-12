#pragma once

#include <atomic>
#include <cstdint>

// The pending screenshot request behind ScreenshotUtil, kept free of the device so it can be tested on the host.
//
// One request is pending at a time, until ScreenshotUtil::service() serves it or kWaitMs pass. A host request
// (CMD:SCREENSHOT) is owed its reply, so nothing replaces it: a second host request is refused (it answers
// SCREENSHOT_ERROR:busy at once) and a card one is skipped. A card request replaces a pending card one.
//
// Everything but askForCard() belongs to the loop task, which serves the requests. Another task (the reader asks from
// its render pass) calls askForCard(): an atomic flag the loop task takes in with takeCardAsk(), so no two tasks ever
// race on the pending request itself.
class ScreenshotRequest {
 public:
  enum class Target : uint8_t { None, Card, Host };
  static constexpr unsigned long kWaitMs = 5000;

  // False, with the pending request left as it is, while a host request is pending.
  bool request(const Target target, const unsigned long nowMs) {
    if (target_ == Target::Host) return false;
    target_ = target;
    deadlineMs_ = nowMs + kWaitMs;
    return true;
  }

  Target pending() const { return target_; }
  // The display stayed busy for kWaitMs since the request: it is given up (a host one answers busy).
  bool expired(const unsigned long nowMs) const {
    return target_ != Target::None && static_cast<long>(nowMs - deadlineMs_) >= 0;
  }
  void clear() { target_ = Target::None; }

  // Any task.
  void askForCard() { cardAsked_.store(true, std::memory_order_release); }
  // Loop task: whether a card screenshot was asked for since the last call.
  bool takeCardAsk() { return cardAsked_.exchange(false, std::memory_order_acq_rel); }

 private:
  Target target_ = Target::None;
  unsigned long deadlineMs_ = 0;
  std::atomic<bool> cardAsked_{false};
};
