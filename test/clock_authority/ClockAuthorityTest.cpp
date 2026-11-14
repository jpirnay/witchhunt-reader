// When TLS checks a certificate's dates against the clock, and when a network session asks NTP first
// (lib/hal/ClockAuthority.h, fed by HalClock). Issue #766: an X4 with no RTC, its clock restored from NVS days
// behind, read a server certificate renewed the day before as not yet valid, and KOReader sync failed closed.
#include <ClockAuthority.h>
#include <gtest/gtest.h>

using clock_authority::datesAuthoritative;
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

TEST(ClockAuthority, EverySourceHasALogName) {
  for (const Source source : kAll) {
    EXPECT_STRNE(clock_authority::describe(source), "unknown");
  }
}
