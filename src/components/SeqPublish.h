#pragma once

#include <atomic>
#include <cstdint>

// Publishing a small plain struct from the render task to the loop task, so a half-written one is
// detected rather than acted on.
//
// The firmware already answers this question three times -- ButtonHintStrip, ListTouchBand and
// TapTargets::Recorder each carry their own copy of the same sequence counter -- because each of
// them publishes geometry the draw produced for the input side to match against. Everything that
// crosses that boundary needs it: a torn read of a rectangle is a tap resolved against a mix of
// two frames, which is a wrong action, not a missing one.
//
// This is the counter alone, with the storage left to the caller, for two reasons. A template
// over the payload would mint a fresh copy of the code in flash per type, which this firmware
// cannot afford; and the three existing users keep their storage in shapes of their own (a
// namespace-scope static, a class member) that a container would have to be bent around. They
// can adopt this as-is when someone touches them; nothing here obliges them to.
//
// Read protocol:
//
//     uint32_t before = 0;
//     if (seq.beginRead(before)) {
//       copy the payload
//       if (seq.endRead(before)) { ...the copy is consistent... }
//     }
//
// A failed read means "nothing here", which is the safe way to be wrong: the tap falls through to
// whoever else wants it.
class SeqPublish {
 public:
  // Bracket every write. Between these the counter is odd, so no reader accepts what it sees.
  void beginWrite() { seq_.fetch_add(1, std::memory_order_acq_rel); }
  void endWrite() { seq_.fetch_add(1, std::memory_order_release); }

  // False when nothing has been published yet (counter 0) or a write is in flight (odd).
  bool beginRead(uint32_t& before) const {
    before = seq_.load(std::memory_order_acquire);
    return before != 0 && (before & 1u) == 0;
  }

  // True when no write happened while the payload was being copied.
  bool endRead(const uint32_t before) const { return seq_.load(std::memory_order_acquire) == before; }

 private:
  std::atomic<uint32_t> seq_{0};
};
