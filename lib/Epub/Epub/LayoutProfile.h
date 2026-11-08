#pragma once
// Throwaway layout profile (branch wip/layout-profile, never merged). Built only with -DLAYOUT_PROFILE; without it
// every macro below is empty. Microseconds from esp_timer_get_time() on the device, steady_clock on the host.
#ifdef LAYOUT_PROFILE
#include <cstdint>
#include <cstring>
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_timer.h>
#else
#include <chrono>
#endif
namespace layout_prof {
inline uint64_t now() {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  return static_cast<uint64_t>(esp_timer_get_time());
#else
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
#endif
}
struct Data {
  uint64_t widthForm, widthMeasure, widthShape, breaks, linePass, place, placeMeasure, emit, cut;
  uint64_t tokens, lines, measures;
  void reset() { memset(this, 0, sizeof *this); }
};
inline Data g;
struct Scope {
  uint64_t& acc;
  uint64_t t0;
  explicit Scope(uint64_t& a) : acc(a), t0(now()) {}
  ~Scope() { acc += now() - t0; }
};
}  // namespace layout_prof
#define LAYOUT_PROF_SCOPE(field) layout_prof::Scope _lp_##field(layout_prof::g.field)
#define LAYOUT_PROF_COUNT(field) (layout_prof::g.field++)
#else
#define LAYOUT_PROF_SCOPE(field) ((void)0)
#define LAYOUT_PROF_COUNT(field) ((void)0)
#endif
