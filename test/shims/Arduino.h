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

// millis stub
inline unsigned long millis() { return 0; }
