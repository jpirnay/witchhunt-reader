#pragma once

#include <cstdint>
#include <ctime>

/// Whether the wall clock may be held against a server's certificate dates, and when a network session asks NTP
/// first. Pure, so the rules are host-tested (test/clock_authority); HalClock feeds them its state.
namespace clock_authority {

/// Where the wall clock's value came from: the last of HalClock's restore(), syncNtp() and applyClientTime().
enum class Source : uint8_t {
  Unset,            // nothing set it: 1970
  NvsRestore,       // a cold boot, on the epoch saved before the last sleep or restart: the time off is lost
  NvsStaleRestore,  // the same, more than 72 h after the last NTP sync: a lower bound only, not shown
  SleepEstimate,    // a wake or restart, on the saved epoch plus the LP timer's count since: off by its drift
  HardwareRtc,      // read from the board's RTC chip
  Ntp,              // set by SNTP in this boot
  Client,           // set by a hotspot client's browser
};

// The curated roots' validity window: a clock outside it cannot load them (HalClock::isPlausibleForTls()).
constexpr time_t kMinPlausibleEpoch = 1735689600;  // 2025-01-01 00:00:00 UTC
constexpr time_t kMaxPlausibleEpoch = 2114380800;  // 2037-01-01 00:00:00 UTC, below the roots' latest notAfter (2038)

constexpr bool plausibleForTls(const time_t now) { return now >= kMinPlausibleEpoch && now < kMaxPlausibleEpoch; }

/// Whether a TLS connect may check a certificate's notBefore and notAfter against `now`: the clock came from NTP in
/// this boot or from a hardware RTC, and is plausible. Any other clock can be days behind and still look plausible,
/// and a server's renewed certificate then reads as not yet valid: issue #766, an X4 whose clock came back from NVS,
/// against a certificate renewed the day before. Such a connect waives the dates and nothing else.
constexpr bool datesAuthoritative(const Source source, const time_t now) {
  return (source == Source::Ntp || source == Source::HardwareRtc) && plausibleForTls(now);
}

/// How long after a failed NTP sync a software clock is not sent to NTP again: `recentFailure` below.
constexpr uint32_t kRetryAfterFailureMs = 10UL * 60UL * 1000UL;

/// Whether a network session asks NTP before its first TLS connect because the clock is not authoritative: not
/// again while an attempt failed recently (`recentFailure`), so a network without NTP costs the timeout once, not
/// at every sync. TLS does not wait for it (the dates are waived); the clock itself is what it is for.
constexpr bool softwareClockNtpDue(const Source source, const time_t now, const bool recentFailure) {
  return !datesAuthoritative(source, now) && !recentFailure;
}

/// KOReaderSyncActivity's rule: softwareClockNtpDue(); on an authoritative clock, a last sync that is unknown, in
/// the future, or at least `minInterval` old.
constexpr bool ntpDueBeforeSync(const Source source, const time_t now, const time_t lastSync, const time_t minInterval,
                                const bool recentFailure) {
  if (!datesAuthoritative(source, now)) return !recentFailure;
  if (lastSync <= 0) return true;
  const time_t age = now - lastSync;
  return age < 0 || age >= minInterval;
}

/// For the log: "restored from NVS".
constexpr const char* describe(const Source source) {
  switch (source) {
    case Source::Unset:
      return "unset";
    case Source::NvsRestore:
      return "restored from NVS";
    case Source::NvsStaleRestore:
      return "restored from a stale NVS epoch";
    case Source::SleepEstimate:
      return "estimated over a sleep or restart";
    case Source::HardwareRtc:
      return "read from the hardware RTC";
    case Source::Ntp:
      return "set by NTP";
    case Source::Client:
      return "set by a hotspot client";
  }
  return "unknown";
}

}  // namespace clock_authority
