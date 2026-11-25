// When TLS checks a certificate's dates against the clock, and when a network session asks NTP first
// (lib/hal/ClockAuthority.h, fed by HalClock). Issue #766: an X4 with no RTC, its clock restored from NVS days
// behind, read a server certificate renewed the day before as not yet valid, and KOReader sync failed closed.
#include <ClockAuthority.h>
#include <gtest/gtest.h>

using clock_authority::datesAuthoritative;
using clock_authority::knownFresh;
using clock_authority::ntpDueBeforeSync;
using clock_authority::plausibleForTls;
using clock_authority::softwareClockNtpDue;
using clock_authority::Source;

namespace {
constexpr time_t kNov2026 = 1794700000;  // 2026-11-14, inside the roots' window
constexpr time_t k1970 = 0;
constexpr time_t k2040 = 2208988800;  // past the window's end
constexpr time_t kInterval = 15 * 60;
constexpr bool kRecentFailure = true;

constexpr Source kAll[] = {Source::Unset,       Source::NvsRestore, Source::NvsStaleRestore, Source::SleepEstimate,
                           Source::HardwareRtc, Source::Ntp,        Source::Client};
}  // namespace

TEST(ClockAuthority, PlausibleWindowIsTheRootsOne) {
  EXPECT_FALSE(plausibleForTls(k1970));
  EXPECT_FALSE(plausibleForTls(clock_authority::kMinPlausibleEpoch - 1));
  EXPECT_TRUE(plausibleForTls(clock_authority::kMinPlausibleEpoch));
  EXPECT_TRUE(plausibleForTls(kNov2026));
  EXPECT_FALSE(plausibleForTls(clock_authority::kMaxPlausibleEpoch));
  EXPECT_FALSE(plausibleForTls(k2040));
}

// The truth table: only NTP in this boot and a hardware RTC are held against certificate dates.
TEST(ClockAuthority, DatesAreCheckedOnlyAgainstNtpOrAHardwareRtc) {
  EXPECT_TRUE(datesAuthoritative(Source::Ntp, kNov2026));
  EXPECT_TRUE(datesAuthoritative(Source::HardwareRtc, kNov2026));

  EXPECT_FALSE(datesAuthoritative(Source::NvsRestore, kNov2026));       // #766: plausible, days behind
  EXPECT_FALSE(datesAuthoritative(Source::NvsStaleRestore, kNov2026));  // a lower bound only
  EXPECT_FALSE(datesAuthoritative(Source::SleepEstimate, kNov2026));    // the LP timer's drift
  EXPECT_FALSE(datesAuthoritative(Source::Client, kNov2026));           // a browser's clock
  EXPECT_FALSE(datesAuthoritative(Source::Unset, k1970));
}

// An authoritative source with an implausible value (an RTC that kept a wild date) still has its dates waived: the
// roots would not even load against it.
TEST(ClockAuthority, AnImplausibleClockIsNeverAuthoritative) {
  for (const Source source : kAll) {
    EXPECT_FALSE(datesAuthoritative(source, k1970));
    EXPECT_FALSE(datesAuthoritative(source, k2040));
  }
}

TEST(ClockAuthority, ASoftwareClockAsksNtpUnlessItJustFailed) {
  for (const Source source : kAll) {
    const bool authoritative = datesAuthoritative(source, kNov2026);
    EXPECT_EQ(softwareClockNtpDue(source, kNov2026, !kRecentFailure), !authoritative);
    EXPECT_FALSE(softwareClockNtpDue(source, kNov2026, kRecentFailure));
  }
}

// #766's path: an NVS restore that reads "synced 5 minutes ago" against itself is synced anyway.
TEST(ClockAuthority, SyncAsksNtpForASoftwareClockWhateverItsAge) {
  const time_t justSynced = kNov2026 - 5 * 60;
  EXPECT_TRUE(ntpDueBeforeSync(Source::NvsRestore, kNov2026, justSynced, kInterval, !kRecentFailure));
  EXPECT_TRUE(ntpDueBeforeSync(Source::SleepEstimate, kNov2026, justSynced, kInterval, !kRecentFailure));
  EXPECT_TRUE(ntpDueBeforeSync(Source::NvsStaleRestore, kNov2026, justSynced, kInterval, !kRecentFailure));
  EXPECT_TRUE(ntpDueBeforeSync(Source::Unset, k1970, 0, kInterval, !kRecentFailure));
  // Not again within the backoff of a failed attempt: the dates are waived meanwhile.
  EXPECT_FALSE(ntpDueBeforeSync(Source::NvsRestore, kNov2026, justSynced, kInterval, kRecentFailure));
  EXPECT_FALSE(ntpDueBeforeSync(Source::Unset, k1970, 0, kInterval, kRecentFailure));
}

// An authoritative clock keeps the freshness rule it always had.
TEST(ClockAuthority, AnAuthoritativeClockKeepsTheFreshnessRule) {
  for (const Source source : {Source::Ntp, Source::HardwareRtc}) {
    EXPECT_FALSE(ntpDueBeforeSync(source, kNov2026, kNov2026 - 5 * 60, kInterval, !kRecentFailure));
    EXPECT_TRUE(ntpDueBeforeSync(source, kNov2026, kNov2026 - kInterval, kInterval, !kRecentFailure));
    EXPECT_TRUE(ntpDueBeforeSync(source, kNov2026, 0, kInterval, !kRecentFailure));                // never synced
    EXPECT_TRUE(ntpDueBeforeSync(source, kNov2026, kNov2026 + 3600, kInterval, !kRecentFailure));  // in the future
    // A recent failure does not hold back the freshness rule, as before.
    EXPECT_TRUE(ntpDueBeforeSync(source, kNov2026, kNov2026 - kInterval, kInterval, kRecentFailure));
  }
}

// The weather panel's rule (issue #212): a cache is fresh only when an authoritative clock says so.
TEST(ClockAuthority, AStampIsFreshOnlyByAnAuthoritativeClock) {
  constexpr time_t kTtl = 30 * 60;
  const time_t tenMinutesAgo = kNov2026 - 10 * 60;
  for (const Source source : kAll) {
    // An X4 cold boot: the NVS epoch reads "10 minutes after the fetch" whatever the real gap was.
    EXPECT_EQ(knownFresh(source, kNov2026, tenMinutesAgo, kTtl), datesAuthoritative(source, kNov2026));
  }
  for (const Source source : {Source::Ntp, Source::HardwareRtc}) {
    EXPECT_TRUE(knownFresh(source, kNov2026, kNov2026, kTtl));             // just fetched
    EXPECT_TRUE(knownFresh(source, kNov2026, kNov2026 - kTtl + 1, kTtl));  // one second inside
    EXPECT_FALSE(knownFresh(source, kNov2026, kNov2026 - kTtl, kTtl));     // at the TTL
    EXPECT_FALSE(knownFresh(source, kNov2026, kNov2026 + 60, kTtl));       // stamped in the future
    EXPECT_FALSE(knownFresh(source, kNov2026, 0, kTtl));                   // never stamped
    EXPECT_FALSE(knownFresh(source, k2040, k2040 - 60, kTtl));             // an implausible RTC value
  }
}

TEST(ClockAuthority, EverySourceHasALogName) {
  for (const Source source : kAll) {
    EXPECT_STRNE(clock_authority::describe(source), "unknown");
  }
}

// ---- The loan floor (HalClock::loanNow()) ----

namespace {
using clock_authority::loanFloorAfterNtp;
using clock_authority::loanTime;
using clock_authority::raisedLoanFloor;
using clock_authority::raisesLoanFloor;

constexpr int64_t kFloor = kNov2026 - 86400;       // the latest time seen: a day ago
constexpr int64_t k2099 = 4070908800;              // 2099-01-01, a year a browser set by mistake
constexpr int64_t kBefore2025 = 1700000000;        // 2023: below the floor's window, a clock never set
constexpr int64_t kPast2100 = 4102444800 + 86400;  // past the window: a fault, not a date
constexpr bool kClientTime = true;
}  // namespace

// Only a clock from NTP or the hardware RTC, or one that descends from either, raises the floor: never a hotspot
// client's browser time, nor anything saved or written to the RTC since one was applied.
TEST(LoanFloor, OnlyTrustedClocksRaiseIt) {
  EXPECT_TRUE(raisesLoanFloor(Source::Ntp, !kClientTime));
  EXPECT_TRUE(raisesLoanFloor(Source::Ntp, kClientTime));  // NTP is the time again
  for (const Source source :
       {Source::HardwareRtc, Source::SleepEstimate, Source::NvsRestore, Source::NvsStaleRestore}) {
    EXPECT_TRUE(raisesLoanFloor(source, !kClientTime));
    EXPECT_FALSE(raisesLoanFloor(source, kClientTime));
  }
  EXPECT_FALSE(raisesLoanFloor(Source::Client, !kClientTime));
  EXPECT_FALSE(raisesLoanFloor(Source::Client, kClientTime));
  EXPECT_FALSE(raisesLoanFloor(Source::Unset, !kClientTime));
}

TEST(LoanFloor, RisesOnlyForward) {
  EXPECT_EQ(raisedLoanFloor(kFloor, Source::Ntp, kNov2026, !kClientTime), kNov2026);
  EXPECT_EQ(raisedLoanFloor(kFloor, Source::HardwareRtc, kNov2026, !kClientTime), kNov2026);
  // A clock behind the floor (an NVS restore after power-off) leaves it.
  EXPECT_EQ(raisedLoanFloor(kFloor, Source::NvsRestore, kFloor - 3600, !kClientTime), kFloor);
  // Implausible clocks leave it: never set, or a fault.
  EXPECT_EQ(raisedLoanFloor(kFloor, Source::HardwareRtc, kBefore2025, !kClientTime), kFloor);
  EXPECT_EQ(raisedLoanFloor(kFloor, Source::HardwareRtc, kPast2100, !kClientTime), kFloor);
  EXPECT_EQ(raisedLoanFloor(0, Source::Ntp, kNov2026, !kClientTime), kNov2026);  // the first time seen
}

// The incident: a browser's wrong year in hotspot mode, then a sleep, expired every loan until NVS was erased.
TEST(LoanFloor, AClientsWrongYearDoesNotStick) {
  // The client's time itself, and the clocks that descend from it after the sleep.
  EXPECT_EQ(raisedLoanFloor(kFloor, Source::Client, k2099, !kClientTime), kFloor);
  EXPECT_EQ(raisedLoanFloor(kFloor, Source::SleepEstimate, k2099, kClientTime), kFloor);
  EXPECT_EQ(raisedLoanFloor(kFloor, Source::HardwareRtc, k2099, kClientTime), kFloor);
  // While that clock runs, a loan is judged by it (the clock is never earlier than the floor), and once it is put
  // right the floor is where it was.
  EXPECT_EQ(loanTime(kFloor, k2099), k2099);
  EXPECT_EQ(loanTime(kFloor, kNov2026), kNov2026);
}

// A floor left in the future by an older build (or a wrong RTC) is put right by the next NTP sync.
TEST(LoanFloor, NtpSetsTheFloorEitherWay) {
  EXPECT_EQ(loanFloorAfterNtp(k2099, kNov2026), kNov2026);   // lowered
  EXPECT_EQ(loanFloorAfterNtp(kFloor, kNov2026), kNov2026);  // raised
  EXPECT_EQ(loanFloorAfterNtp(0, kNov2026), kNov2026);
  EXPECT_EQ(loanFloorAfterNtp(kFloor, kBefore2025), kFloor);  // an implausible answer changes nothing
  EXPECT_EQ(loanFloorAfterNtp(kFloor, kPast2100), kFloor);
}

// A clock set back cannot reopen an ended loan: the floor holds. No plausible time at all: 0, the loan's date cannot be
// judged.
TEST(LoanFloor, LoanTimeIsTheClockNeverEarlierThanTheFloor) {
  EXPECT_EQ(loanTime(kFloor, kFloor - 30 * 86400), kFloor);
  EXPECT_EQ(loanTime(kFloor, kNov2026), kNov2026);
  EXPECT_EQ(loanTime(0, kNov2026), kNov2026);
  EXPECT_EQ(loanTime(kFloor, kBefore2025), kFloor);
  EXPECT_EQ(loanTime(kFloor, kPast2100), kFloor);
  EXPECT_EQ(loanTime(0, kBefore2025), 0);
  EXPECT_EQ(loanTime(kBefore2025, 0), 0);  // a stored floor outside the window is no time either
}
