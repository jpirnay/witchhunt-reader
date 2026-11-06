#pragma once

// Throwaway shaper profile (branch wip/shaper-profile, never merged). Built only with -DOT_PROFILE; without it every
// macro below is empty and the shaper compiles as before. Unit: CPU cycles on the device (esp_cpu_get_cycle_count),
// nanoseconds on the host (steady_clock). Intervals are taken as uint32 differences, so each must stay under 2^32.

#ifdef OT_PROFILE

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_cpu.h>
#define OT_PROF_UNIT "cycles"
namespace ot {
namespace prof {
using Cyc = uint32_t;
}
}  // namespace ot
#else
#include <chrono>
#define OT_PROF_UNIT "ns"
namespace ot {
namespace prof {
using Cyc = uint64_t;
}
}  // namespace ot
#endif

namespace ot {
namespace prof {

inline uint32_t now() {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  return static_cast<uint32_t>(esp_cpu_get_cycle_count());
#else
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
#endif
}

constexpr unsigned MAX_TRACKED = 384;  // lookups tracked per table (by position in plan.lookups)

struct Data {
  uint64_t total, props, normalize, masks, gdef, gsub, pauseSetup, pauseInitial, pauseFinal, pauseOther, refresh,
      gsubLoop, advances, gpos, gposLoop, tail;
  uint64_t calls;
  // counters
  uint64_t coverage, glyphProps, advance, nominal, lookupFetch, lookupApplied, subtableCalls, lookupSkippedDigest,
      lookupsInPlan, glyphsOut, codepointsIn;
  uint32_t passed[2][MAX_TRACKED];
  Cyc cycles[2][MAX_TRACKED];
  uint32_t overflow;  // lookup positions >= MAX_TRACKED, not tracked

  void reset() { memset(this, 0, sizeof *this); }
  // Every scalar accumulator and counter (not `calls`) divided by n, rounded: the per-shape mean.
  void divide(const uint64_t n) {
    uint64_t* p = &total;
    uint64_t* const end = &codepointsIn + 1;
    for (; p != end; ++p) {
      if (p != &calls) *p = n ? (*p + n / 2) / n : 0;
    }
  }
};

inline Data g;

// The stage fields as "total=.. props=.. ..." (cycles or ns, summed over g.calls shapes).
inline int formatFields(char* out, const size_t n, const Data& d = g) {
  return snprintf(out, n,
                  "unit=" OT_PROF_UNIT
                  " calls=%llu total=%llu props=%llu normalize=%llu masks=%llu gdef=%llu "
                  "refresh=%llu gsub=%llu gsub_loop=%llu pause_setup=%llu pause_initial=%llu pause_final=%llu "
                  "pause_other=%llu advances=%llu gpos=%llu gpos_loop=%llu tail=%llu",
                  (unsigned long long)d.calls, (unsigned long long)d.total, (unsigned long long)d.props,
                  (unsigned long long)d.normalize, (unsigned long long)d.masks, (unsigned long long)d.gdef,
                  (unsigned long long)d.refresh, (unsigned long long)d.gsub, (unsigned long long)d.gsubLoop,
                  (unsigned long long)d.pauseSetup, (unsigned long long)d.pauseInitial,
                  (unsigned long long)d.pauseFinal, (unsigned long long)d.pauseOther, (unsigned long long)d.advances,
                  (unsigned long long)d.gpos, (unsigned long long)d.gposLoop, (unsigned long long)d.tail);
}

inline int formatCounters(char* out, const size_t n, const Data& d = g) {
  return snprintf(out, n,
                  "cp_in=%llu glyphs_out=%llu cov=%llu gprops=%llu adv=%llu nominal=%llu lookup_fetch=%llu "
                  "lookups_run=%llu lookups_digest_skipped=%llu subtable_tries=%llu overflow=%u",
                  (unsigned long long)d.codepointsIn, (unsigned long long)d.glyphsOut, (unsigned long long)d.coverage,
                  (unsigned long long)d.glyphProps, (unsigned long long)d.advance, (unsigned long long)d.nominal,
                  (unsigned long long)d.lookupFetch, (unsigned long long)d.lookupApplied,
                  (unsigned long long)d.lookupSkippedDigest, (unsigned long long)d.subtableCalls, d.overflow);
}

// "top_<name>=<position>:<passed>:<cycles>,..." the ten costliest lookups of a table, by position in plan.lookups.
inline int formatTop(char* out, const size_t n, const char* name, const int table, const Data& d = g) {
  bool taken[MAX_TRACKED] = {};
  int len = snprintf(out, n, "top_%s=", name);
  for (int k = 0; k < 10; k++) {
    int best = -1;
    for (unsigned i = 0; i < MAX_TRACKED; i++) {
      if (!taken[i] && d.cycles[table][i] && (best < 0 || d.cycles[table][i] > d.cycles[table][best])) best = (int)i;
    }
    if (best < 0) break;
    taken[best] = true;
    if (len < (int)n) {
      len += snprintf(out + len, n - len, "%s%d:%u:%llu", k ? "," : "", best, d.passed[table][best],
                      (unsigned long long)d.cycles[table][best]);
    }
  }
  return len;
}

}  // namespace prof
}  // namespace ot

#define OTP_T(v) uint32_t v = ::ot::prof::now()
#define OTP_ADD(field, v) (::ot::prof::g.field += static_cast<uint32_t>(::ot::prof::now() - (v)))
#define OTP_COUNT(field) (++::ot::prof::g.field)
#define OTP_LAP(field, v)                                     \
  do {                                                        \
    const uint32_t lap_ = ::ot::prof::now();                  \
    ::ot::prof::g.field += static_cast<uint32_t>(lap_ - (v)); \
    (v) = lap_;                                               \
  } while (0)

#else

#define OTP_T(v) \
  do {           \
  } while (0)
#define OTP_ADD(field, v) \
  do {                    \
  } while (0)
#define OTP_COUNT(field) \
  do {                   \
  } while (0)
#define OTP_LAP(field, v) \
  do {                    \
  } while (0)

#endif
