#pragma once
// Minimal Arduino.h stub for host/test builds.
// Provides only what the Epub/CSS library code references.

#include "HardwareSerial.h"
#include "WString.h"

#include <cstdint>
#include <cstdlib>

// ESP stub — only getFreeHeap() is referenced by CssParser
class EspClass {
 public:
  uint32_t getFreeHeap() const { return 200 * 1024; }  // 200 KB — generous stub
  uint32_t getMinFreeHeap() const { return 150 * 1024; }
  uint32_t getMaxAllocHeap() const { return 100 * 1024; }
  void restart() {}
};
inline EspClass ESP;

// millis stub
inline unsigned long millis() { return 0; }
