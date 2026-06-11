#pragma once

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
#include <esp_heap_caps.h>

// Temporary static-init heap-corruption bisect (companion to the boot probes in
// main.cpp). Field data: preCtors=OK setupEntry=CORRUPT — one of OUR C++ global
// constructors corrupts the heap on every boot. Each BootHeapProbe global records
// whether the heap was intact at its own construction point; a CORRUPT slot whose
// paired "pre" slot is OK convicts the global declared between them (same-TU
// construction order is top-to-bottom). Slots are logged from setup() once serial
// is up. Remove together with the other heap tripwires once the writer is found.
inline bool* bootHeapProbeSlots() {
  static bool slots[16] = {true, true, true, true, true, true, true, true,
                           true, true, true, true, true, true, true, true};
  return slots;
}

struct BootHeapProbe {
  explicit BootHeapProbe(const int slot) {
    if (slot >= 0 && slot < 16) {
      bootHeapProbeSlots()[slot] = heap_caps_check_integrity_all(/*print_errors=*/false);
    }
  }
};
#else
inline bool* bootHeapProbeSlots() {
  static bool slots[16] = {true, true, true, true, true, true, true, true,
                           true, true, true, true, true, true, true, true};
  return slots;
}

struct BootHeapProbe {
  explicit BootHeapProbe(const int) {}
};
#endif
