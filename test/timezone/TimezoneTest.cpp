// What each timezone setting actually does, and that nothing describing those settings has
// fallen out of step with the table.
//
// This exists because nothing did, and the same class of bug shipped twice:
//
//  - The setting value used to index a table in HalClock.cpp. That table was extended from 19
//    entries to 51 and reordered geographically, and every timezone silently became a different
//    one -- UTC selected UTC-12, CET selected UTC-11, US Eastern selected UTC-02. The firmware
//    kept displaying the right NAME while applying the wrong rules.
//  - The label list in SettingsList.h stopped at 16 while the enum, the POSIX strings and the
//    device Clock screen all reached 19. JsonSettingsIO bounds a loaded ENUM on the settings-list
//    row's option count, so Atlantic Canada, Australia Central and Alaska saved correctly and
//    then reset to the default on the next boot.
//
// Both are one list of timezones disagreeing with another, so the tests come in groups:
//
//   Consistency -- the table, the settings row and the detection map still agree. Pure C++.
//   Migration   -- every retired enum value lands on a zone applying the SAME rule it used to.
//   Offsets     -- what a POSIX TZ string actually resolves to, via setenv/tzset/localtime_r.
//                  Needs a libc that parses POSIX DST rules, which rules out the Windows CRT.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>

#include "CrossPointSettings.h"
#include "activities/settings/TimezoneOptions.h"
#include "util/TimezoneDetectMap.h"
#include "util/Timezones.h"

// The Windows CRT's _tzset understands only "tzn[+|-]hh[:mm[:ss]][dzn]" and applies hardcoded US
// DST rules; it cannot parse the ",M3.5.0/2,M10.5.0/3" half of a POSIX TZ string, and MinGW has
// no tm_gmtoff to read the answer out of. Rather than let this file fail to compile off glibc --
// which is what it did for its whole life until now, never once built on the Windows host it was
// written on -- the offset group skips loudly and the rest runs everywhere.
#if defined(_WIN32) && !defined(__CYGWIN__)
#define CP_TZ_HAS_POSIX_TZ 0
#else
#define CP_TZ_HAS_POSIX_TZ 1
#endif

// Definitions for the two things util/Timezones.cpp reaches for outside itself. The settings
// singleton is the real type, so activeIndex() is exercised as it actually runs; applying a zone
// to the system clock is the one genuinely device-shaped thing here and is recorded rather than
// performed.
CrossPointSettings CrossPointSettings::instance;

namespace {
const char* lastAppliedTz = nullptr;
}

namespace HalClock {
void applyTimezone(const char* posixTz) { lastAppliedTz = posixTz; }
}  // namespace HalClock

// SettingInfo's inline accessors reference I18N even on rows that carry no translated options,
// so the symbol has to exist. The real lib/I18n/I18n.cpp cannot come along -- it reaches into the
// flash language partition -- but the generated English blob is plain data and does link, so
// this resolves exactly as it does on device for the one language a host test could want.
I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(const StrId id) const { return _active.data + _active.offsets[static_cast<uint16_t>(id)]; }

namespace {

// ---------------------------------------------------------------- consistency

// The regression that cost users their setting: a settings row that does not offer every zone
// the table holds is a row that clamps the missing ones back to the default on load, because
// JsonSettingsIO bounds a loaded ENUM on exactly this number.
TEST(TimezoneConsistency, SettingOffersEveryZoneInTheTable) {
  const SettingInfo row = TimezoneOptions::make(StrId::STR_CLOCK);
  EXPECT_EQ(static_cast<size_t>(row.getEnumOptionCount()), timezones::count());
}

// The row's labels must be the table's, in the table's order. They are the same strings by
// construction today; this is what notices if that construction is ever "optimised" into a
// second list.
TEST(TimezoneConsistency, SettingLabelsMatchTheTableInOrder) {
  const SettingInfo row = TimezoneOptions::make(StrId::STR_CLOCK);
  for (size_t i = 0; i < timezones::count(); ++i) {
    EXPECT_EQ(row.getEnumOptionLabel(static_cast<uint8_t>(i)), std::string(timezones::table()[i].name))
        << "at index " << i;
  }
}

TEST(TimezoneConsistency, EveryEntryHasANameAndARule) {
  for (size_t i = 0; i < timezones::count(); ++i) {
    const auto& entry = timezones::table()[i];
    ASSERT_NE(entry.name, nullptr) << "index " << i;
    ASSERT_NE(entry.posixTz, nullptr) << "index " << i;
    EXPECT_NE(entry.name[0], '\0') << "index " << i;
    EXPECT_NE(entry.posixTz[0], '\0') << "index " << i;
  }
}

// Names are how the detection map and indexByName() address entries, so a duplicate would make
// one of them unreachable.
TEST(TimezoneConsistency, NoTwoEntriesShareAName) {
  std::set<std::string> seen;
  for (size_t i = 0; i < timezones::count(); ++i) {
    const std::string name = timezones::table()[i].name;
    EXPECT_TRUE(seen.insert(name).second) << "index " << i << " repeats the name \"" << name << "\"";
  }
}

// Two entries resolving to the same rules is a picker with a duplicate row in it: harmless to the
// clock, confusing to read, and a sign the table grew a copy of something it already had.
TEST(TimezoneConsistency, NoTwoEntriesShareAPosixRule) {
  std::set<std::string> seen;
  for (size_t i = 0; i < timezones::count(); ++i) {
    const std::string rule = timezones::table()[i].posixTz;
    EXPECT_TRUE(seen.insert(rule).second)
        << "index " << i << " (" << timezones::table()[i].name << ") repeats the rule \"" << rule << "\"";
  }
}

TEST(TimezoneConsistency, UtcIndexPointsAtPlainUtc) {
  ASSERT_LT(timezones::utcIndex(), timezones::count());
  EXPECT_EQ(std::string(timezones::table()[timezones::utcIndex()].name), "UTC");
  EXPECT_EQ(timezones::table()[timezones::utcIndex()].stdOffsetQ, 0);
}

TEST(TimezoneConsistency, IndexByNameFindsEveryEntryAndRejectsOthers) {
  for (size_t i = 0; i < timezones::count(); ++i) {
    EXPECT_EQ(timezones::indexByName(timezones::table()[i].name), i);
  }
  EXPECT_EQ(timezones::indexByName("Nowhere"), 255);
  EXPECT_EQ(timezones::indexByName(nullptr), 255);
}

// Detection names its destinations as strings. A renamed or deleted entry makes the lookup fail
// at runtime -- recoverable, but it means that zone can never be detected again, silently.
TEST(TimezoneConsistency, EveryDetectionDestinationExistsInTheTable) {
  for (const auto& mapping : timezone_detect::IANA_MAP) {
    EXPECT_NE(timezones::indexByName(mapping.zone), 255)
        << mapping.iana << " maps to \"" << mapping.zone << "\", which the table no longer has";
  }
  EXPECT_NE(timezones::indexByName(timezone_detect::EUROPE_FALLBACK), 255);
}

// The bug this map had before the table existed: "UTC" resolved to the UK entry, which carries
// British Summer Time, so a device that reported UTC ran an hour fast every summer. Lisbon and
// Dublin fell through the Europe/ rule to CET and were an hour off all year.
TEST(TimezoneConsistency, DetectionSendsUtcToUtcAndLisbonToLondon) {
  uint8_t index = 255;
  ASSERT_TRUE(timezone_detect::mapIanaTimezone("UTC", index));
  EXPECT_EQ(index, timezones::utcIndex());
  ASSERT_TRUE(timezone_detect::mapIanaTimezone("Etc/UTC", index));
  EXPECT_EQ(index, timezones::utcIndex());

  ASSERT_TRUE(timezone_detect::mapIanaTimezone("Europe/Lisbon", index));
  EXPECT_EQ(std::string(timezones::table()[index].name), "London / Dublin / Lisbon");
  ASSERT_TRUE(timezone_detect::mapIanaTimezone("Europe/Dublin", index));
  EXPECT_EQ(std::string(timezones::table()[index].name), "London / Dublin / Lisbon");

  // The prefix fallback still has to work for everything else under Europe/.
  ASSERT_TRUE(timezone_detect::mapIanaTimezone("Europe/Madrid", index));
  EXPECT_EQ(std::string(timezones::table()[index].name), "Berlin / Paris / Madrid / Rome");

  EXPECT_FALSE(timezone_detect::mapIanaTimezone("Mars/Olympus_Mons", index));
}

// ------------------------------------------------------------------ migration

// The 19 POSIX rules the retired TIMEZONE enum applied, by enum value. Frozen: this is what those
// persisted bytes MEANT, and the migration is only correct if it preserves it.
constexpr const char* LEGACY_RULES[] = {
    "GMT0BST,M3.5.0/1,M10.5.0/2",        // 0  TZ_UTC
    "CET-1CEST,M3.5.0/2,M10.5.0/3",      // 1  TZ_CET
    "EET-2EEST,M3.5.0/3,M10.5.0/4",      // 2  TZ_EET
    "MSK-3",                             // 3  TZ_MSK
    "UTC-4",                             // 4  TZ_UTC_PLUS4
    "UTC-5:30",                          // 5  TZ_IST
    "UTC-7",                             // 6  TZ_UTC_PLUS7
    "UTC-8",                             // 7  TZ_UTC_PLUS8
    "UTC-9",                             // 8  TZ_UTC_PLUS9
    "AEST-10AEDT,M10.1.0/2,M4.1.0/3",    // 9  TZ_AEST
    "NZST-12NZDT,M9.5.0/2,M4.1.0/3",     // 10 TZ_NZST
    "UTC+3",                             // 11 TZ_UTC_MINUS3
    "EST5EDT,M3.2.0/2,M11.1.0/2",        // 12 TZ_EST
    "CST6CDT,M3.2.0/2,M11.1.0/2",        // 13 TZ_CST
    "MST7MDT,M3.2.0/2,M11.1.0/2",        // 14 TZ_MST
    "PST8PDT,M3.2.0/2,M11.1.0/2",        // 15 TZ_PST
    "AST4ADT,M3.2.0/2,M11.1.0/2",        // 16 TZ_AST_ADT
    "ACST-9:30ACDT,M10.1.0/2,M4.1.0/3",  // 17 TZ_ACST_ACDT
    "AKST9AKDT,M3.2.0/2,M11.1.0/2",      // 18 TZ_AKST_AKDT
};
constexpr size_t LEGACY_RULE_COUNT = sizeof(LEGACY_RULES) / sizeof(LEGACY_RULES[0]);
// EveryRetiredValueMigratesToTheSameOffsets walks this list, so a retired value missing from it
// would go unchecked. (That every value has a DESTINATION is Timezones.cpp's own static_assert on
// LEGACY_TO_TABLE; this only keeps the test's side of the comparison complete.)
static_assert(LEGACY_RULE_COUNT == static_cast<size_t>(CrossPointSettings::TZ_LEGACY_COUNT),
              "LEGACY_RULES needs the rule every retired TIMEZONE value applied");

TEST(TimezoneMigration, EveryRetiredValueLandsInsideTheTable) {
  for (uint8_t legacy = 0; legacy < CrossPointSettings::TZ_LEGACY_COUNT; ++legacy) {
    EXPECT_LT(timezones::indexForLegacy(legacy), timezones::count()) << "legacy value " << static_cast<int>(legacy);
  }
  // Out of range is the one case that cannot be mapped; it must not run off the end.
  EXPECT_LT(timezones::indexForLegacy(200), timezones::count());
}

// A device that never touched the setting holds TZ_UTC, which has always applied UK rules despite
// the name. It must keep applying them rather than jumping to the new, genuinely-UTC entry.
TEST(TimezoneMigration, UntouchedDeviceKeepsUkRulesRatherThanBecomingUtc) {
  EXPECT_EQ(std::string(timezones::table()[timezones::indexForLegacy(CrossPointSettings::TZ_UTC)].name),
            "London / Dublin / Lisbon");
}

TEST(TimezoneMigration, SentinelFallsBackToLegacyAndAChosenIndexWins) {
  SETTINGS.clockTimezone = 255;  // never chosen on this firmware
  SETTINGS.timeZone = CrossPointSettings::TZ_EST;
  EXPECT_EQ(std::string(timezones::table()[timezones::activeIndex()].name), "New York / Toronto");

  SETTINGS.clockTimezone = timezones::utcIndex();  // chosen: the legacy value stops mattering
  EXPECT_EQ(timezones::activeIndex(), timezones::utcIndex());

  SETTINGS.clockTimezone = 255;
  SETTINGS.timeZone = 200;  // corrupt / from a newer firmware
  EXPECT_EQ(timezones::activeIndex(), timezones::utcIndex());
}

TEST(TimezoneMigration, ApplyToClockPushesTheActiveZonesRule) {
  SETTINGS.clockTimezone = timezones::indexByName("Tokyo / Seoul");
  lastAppliedTz = nullptr;
  timezones::applyToClock();
  ASSERT_NE(lastAppliedTz, nullptr);
  EXPECT_EQ(std::string(lastAppliedTz), "JST-9");
}

// -------------------------------------------------------------------- offsets

// Two instants well clear of every transition in the table: mid-January and mid-July.
constexpr time_t JANUARY = 1736899200;  // 2025-01-15 00:00:00 UTC
constexpr time_t JULY = 1752537600;     // 2025-07-15 00:00:00 UTC

#if CP_TZ_HAS_POSIX_TZ
// Seconds east of UTC that `tzString` resolves to at `when`.
int offsetAt(const char* tzString, time_t when) {
  setenv("TZ", tzString, 1);
  tzset();
  struct tm local{};
  localtime_r(&when, &local);
  return static_cast<int>(local.tm_gmtoff);
}

// The instant between `from` and `to` at which `tzString` changes offset, assuming exactly one
// such instant in the range. Binary search rather than a hand-computed epoch: the expectation is
// then a calendar date, which is far easier to state correctly than a Unix timestamp.
time_t findTransition(const char* tzString, time_t from, time_t to) {
  const int startOffset = offsetAt(tzString, from);
  while (to - from > 1) {
    const time_t mid = from + (to - from) / 2;
    if (offsetAt(tzString, mid) == startOffset) {
      from = mid;
    } else {
      to = mid;
    }
  }
  return to;
}

std::string utcStamp(time_t when) {
  struct tm utc{};
  gmtime_r(&when, &utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &utc);
  return buf;
}

constexpr time_t Y2025_JAN01 = 1735689600;  // 2025-01-01 00:00:00 UTC
constexpr time_t Y2025_JUN01 = 1748736000;  // 2025-06-01 00:00:00 UTC
#endif

}  // namespace

// The regression this file exists for, in its current form: an entry's stated standard offset
// disagreeing with the rule beside it. Daylight saving only ever moves a clock FORWARD, so the
// standard offset is the smaller of the two seasons for every zone in the table, northern and
// southern alike -- which makes this checkable for all 86 rows without naming any of them.
TEST(TimezoneOffsets, EveryEntrysStatedOffsetMatchesItsRule) {
#if !CP_TZ_HAS_POSIX_TZ
  GTEST_SKIP() << "needs a libc that parses POSIX DST rules; the Windows CRT does not";
#else
  for (size_t i = 0; i < timezones::count(); ++i) {
    const auto& entry = timezones::table()[i];
    const int january = offsetAt(entry.posixTz, JANUARY);
    const int july = offsetAt(entry.posixTz, JULY);
    const int standard = january < july ? january : july;
    EXPECT_EQ(standard, entry.stdOffsetQ * 15 * 60)
        << entry.name << " (" << entry.posixTz << ") resolves to " << standard << "s standard, but claims "
        << entry.stdOffsetQ << " quarter-hours";
  }
#endif
}

// The migration's whole promise: nobody's clock moves. Each retired value must land on an entry
// applying the SAME offsets it did, in both seasons -- not merely a nearby zone.
TEST(TimezoneOffsets, EveryRetiredValueMigratesToTheSameOffsets) {
#if !CP_TZ_HAS_POSIX_TZ
  GTEST_SKIP() << "needs a libc that parses POSIX DST rules; the Windows CRT does not";
#else
  for (uint8_t legacy = 0; legacy < LEGACY_RULE_COUNT; ++legacy) {
    const char* before = LEGACY_RULES[legacy];
    const auto& after = timezones::table()[timezones::indexForLegacy(legacy)];
    EXPECT_EQ(offsetAt(before, JANUARY), offsetAt(after.posixTz, JANUARY))
        << "legacy value " << static_cast<int>(legacy) << " -> " << after.name << " in January";
    EXPECT_EQ(offsetAt(before, JULY), offsetAt(after.posixTz, JULY))
        << "legacy value " << static_cast<int>(legacy) << " -> " << after.name << " in July";
  }
#endif
}

// US Eastern is the one we can check against a real device, so it gets named explicitly.
TEST(TimezoneOffsets, UsEasternIsMinusFiveAndMinusFourInSummer) {
#if !CP_TZ_HAS_POSIX_TZ
  GTEST_SKIP() << "needs a libc that parses POSIX DST rules; the Windows CRT does not";
#else
  const char* tz = timezones::table()[timezones::indexByName("New York / Toronto")].posixTz;
  EXPECT_EQ(offsetAt(tz, JANUARY), -5 * 3600);
  EXPECT_EQ(offsetAt(tz, JULY), -4 * 3600);
#endif
}

// The UK entry carries BST; the UTC entry must not. Before the table there was only the former,
// under the name "UTC (GMT/BST)", so there was no way to ask for a clock that stays on UTC.
TEST(TimezoneOffsets, PlainUtcNeverShiftsButLondonDoes) {
#if !CP_TZ_HAS_POSIX_TZ
  GTEST_SKIP() << "needs a libc that parses POSIX DST rules; the Windows CRT does not";
#else
  const char* utc = timezones::table()[timezones::utcIndex()].posixTz;
  EXPECT_EQ(offsetAt(utc, JANUARY), 0);
  EXPECT_EQ(offsetAt(utc, JULY), 0);

  const char* london = timezones::table()[timezones::indexByName("London / Dublin / Lisbon")].posixTz;
  EXPECT_EQ(offsetAt(london, JANUARY), 0);
  EXPECT_EQ(offsetAt(london, JULY), 3600);
#endif
}

// Jerusalem's rule needs a transition hour of 26 -- "the Thursday of the fourth week of March,
// plus 26 hours" is how POSIX spells "the Friday before the last Sunday of March, at 02:00".
// Both libcs this code runs on add the rule's seconds to the named day's midnight without
// clamping (glibc here; newlib 4.3.0 on the device, whose tzcalc_limits.c computes
// `change = days * SECSPERDAY + rule->s + rule->offset` into an int), so the extra hours roll
// forward correctly. If either ever starts clamping the hour to 24, this lands a day early.
TEST(TimezoneOffsets, JerusalemSpringsForwardOnTheFridayBeforeTheLastSundayOfMarch) {
#if !CP_TZ_HAS_POSIX_TZ
  GTEST_SKIP() << "needs a libc that parses POSIX DST rules; the Windows CRT does not";
#else
  const char* tz = timezones::table()[timezones::indexByName("Jerusalem")].posixTz;
  // 2025: the last Sunday of March is the 30th, so the Friday before it is the 28th, and 02:00
  // at UTC+2 is midnight UTC.
  EXPECT_EQ(utcStamp(findTransition(tz, Y2025_JAN01, Y2025_JUN01)), "2025-03-28 00:00:00");
#endif
}

// Santiago's rule needs a transition hour of 24 -- "the first Saturday of April at 24:00", i.e.
// the following midnight. Same dependency as Jerusalem above, two hours less extreme.
TEST(TimezoneOffsets, SantiagoFallsBackAtMidnightAfterTheFirstSaturdayOfApril) {
#if !CP_TZ_HAS_POSIX_TZ
  GTEST_SKIP() << "needs a libc that parses POSIX DST rules; the Windows CRT does not";
#else
  const char* tz = timezones::table()[timezones::indexByName("Santiago")].posixTz;
  // 2025: the first Saturday of April is the 5th, so the change is midnight on the 6th, local.
  // Local is still UTC-3 at that point, which puts it at 03:00 UTC.
  EXPECT_EQ(utcStamp(findTransition(tz, Y2025_JAN01, Y2025_JUN01)), "2025-04-06 03:00:00");
#endif
}
