#pragma once
// Minimal Arduino.h stub for host/test builds.
// Provides only what the Epub/CSS library code references.

#include <cstdint>
#include <cstdlib>

#include "HardwareSerial.h"
#include "WString.h"

// ESP stub — only getFreeHeap() is referenced by CssParser
class EspClass {
 public:
  uint32_t getFreeHeap() const { return freeHeap_; }
  void setFreeHeap(uint32_t heap) { freeHeap_ = heap; }  // test-only: simulate heap pressure
  uint32_t getMinFreeHeap() const { return 150 * 1024; }
  uint32_t getMaxAllocHeap() const { return 100 * 1024; }
  void restart() {}

 private:
  uint32_t freeHeap_ = 200 * 1024;
};
inline EspClass ESP;

// millis stub.
//
// Frozen at 0 by default, which is what the pipeline goldens want: every time-budgeted loop in
// lib/ then runs to completion in a single call, so a dump never depends on how fast the host is.
//
// The side effect is that NO sliced path is reachable on the host — Section::stepSectionBuild's
// budget yields are all `millis() - sliceStart >= budgetMs`, which is `0 >= n`, never true. So a
// build always reaches Done in one call and anything that can only be observed mid-build (a
// preempted Background-B attempt, a partial cache, abortSectionBuild's keep/delete decision) had
// no way to be tested at all.
//
// host_clock::Ticking makes millis() advance by `step` per read for its lifetime, so overBudget()
// becomes true after a bounded number of checks. Scoped and RAII so a test cannot leak the
// ticking state into the goldens.
namespace host_clock {
inline unsigned long& valueRef() {
  static unsigned long value = 0;
  return value;
}
inline unsigned long& stepRef() {
  static unsigned long step = 0;
  return step;
}
}  // namespace host_clock

inline unsigned long millis() {
  unsigned long& value = host_clock::valueRef();
  const unsigned long now = value;
  value += host_clock::stepRef();  // 0 unless a Ticking scope is active
  return now;
}

namespace host_clock {
class Ticking {
 public:
  explicit Ticking(const unsigned long step = 1) {
    stepRef() = step;
    valueRef() = 0;
  }
  ~Ticking() {
    stepRef() = 0;
    valueRef() = 0;
  }
  Ticking(const Ticking&) = delete;
  Ticking& operator=(const Ticking&) = delete;
};
}  // namespace host_clock
