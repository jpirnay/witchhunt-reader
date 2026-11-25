#pragma once

#include <cstdint>
#include <ctime>

/// Whether the wall clock may be held against a server's certificate dates, when a network session asks NTP first, and
/// what a loan's due date is held against. Pure, so the rules are host-tested (test/clock_authority); HalClock feeds
/// them its state.
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

/// Whether a stamp taken at `stampedAt` is known to be less than `maxAge` old: the clock is authoritative now and the
/// age is in [0, maxAge). A software clock measures the age against itself, and after a cold boot it comes back from
/// NVS at the epoch saved before power-off, so a stamp hours old reads as minutes old. A stamp from a clock that was
/// behind (an NVS restore is a lower bound) only makes the age look larger. Unknown (0) and future stamps are not
/// fresh.
constexpr bool knownFresh(const Source source, const time_t now, const time_t stampedAt, const time_t maxAge) {
  if (!datesAuthoritative(source, now) || stampedAt <= 0) return false;
  const time_t age = now - stampedAt;
  return age >= 0 && age < maxAge;
}

// ---- The loan clock (HalClock::loanNow()) ----
// A loan's due date is held against the latest time the device has seen, its floor (NVS), so a clock set back cannot
// reopen an ended loan. The floor rises only from a clock that came from NTP or the hardware RTC, or was saved from one
// (a sleep estimate, an NVS restore): a hotspot client's browser sets the clock from any machine on the network, and a
// floor raised to its wrong year expired every loan until NVS was erased. A client's time also reaches the RTC and
// every epoch saved after it, so `clientTimeSinceNtp` (a client set the clock since the last NTP sync) keeps those out
// too. An NTP sync sets the floor to its own time, lowering one a wrong clock left in the future.

constexpr int64_t kLoanFloorMin = 1735689600;  // 2025-01-01: below it the clock was never set
constexpr int64_t kLoanFloorMax = 4102444800;  // 2100-01-01: past it a clock is a fault, not a date

constexpr bool loanTimePlausible(const int64_t t) { return t >= kLoanFloorMin && t <= kLoanFloorMax; }

/// Whether a clock from `source` may raise the loan floor.
constexpr bool raisesLoanFloor(const Source source, const bool clientTimeSinceNtp) {
  switch (source) {
    case Source::Ntp:
      return true;
    case Source::HardwareRtc:
    case Source::SleepEstimate:
    case Source::NvsRestore:
    case Source::NvsStaleRestore:
      return !clientTimeSinceNtp;
    case Source::Client:
    case Source::Unset:
      return false;
  }
  return false;
}

/// The floor after the clock read `now` from `source`: `now` when that source may raise the floor and `now` is a
/// plausible time past it.
constexpr int64_t raisedLoanFloor(const int64_t floor, const Source source, const int64_t now,
                                  const bool clientTimeSinceNtp) {
  return raisesLoanFloor(source, clientTimeSinceNtp) && loanTimePlausible(now) && now > floor ? now : floor;
}

/// The floor after an NTP sync to `ntpNow`: that time, above or below the floor; unchanged when it is implausible.
constexpr int64_t loanFloorAfterNtp(const int64_t floor, const int64_t ntpNow) {
  return loanTimePlausible(ntpNow) ? ntpNow : floor;
}

/// The time a loan is judged at: the clock, never earlier than the floor. A clock that is wrong now judges by itself
/// while it runs and leaves nothing behind. 0 when neither is a plausible time: the loan's date cannot be judged.
constexpr int64_t loanTime(const int64_t floor, const int64_t now) {
  const int64_t clock = loanTimePlausible(now) ? now : 0;
  const int64_t kept = loanTimePlausible(floor) ? floor : 0;
  return clock > kept ? clock : kept;
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
