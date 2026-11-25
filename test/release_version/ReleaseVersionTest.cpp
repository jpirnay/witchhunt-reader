// ReleaseVersion (src/network/ReleaseVersion.h) took the place of OtaUpdater::isUpdateNewer's
// sscanf("%d.%d.%d-rc.%d.%d"). These compare the parse with that format and pin down the ordering the OTA check uses.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

#include "ReleaseVersion.h"

namespace {

ReleaseVersion sscanfVersion(const char* text) {
  ReleaseVersion v;
  v.isRc = strstr(text, "-rc.") != nullptr;
  sscanf(text, "%d.%d.%d-rc.%d.%d", &v.major, &v.minor, &v.patch, &v.rcRelease, &v.rcBuild);
  return v;
}

void expectSame(const ReleaseVersion& a, const ReleaseVersion& b, const char* text) {
  EXPECT_EQ(a.major, b.major) << text;
  EXPECT_EQ(a.minor, b.minor) << text;
  EXPECT_EQ(a.patch, b.patch) << text;
  EXPECT_EQ(a.rcRelease, b.rcRelease) << text;
  EXPECT_EQ(a.rcBuild, b.rcBuild) << text;
  EXPECT_EQ(a.isRc, b.isRc) << text;
}

bool newer(const char* latest, const char* current) {
  return isNewerRelease(parseReleaseVersion(latest), parseReleaseVersion(current));
}

}  // namespace

TEST(ReleaseVersion, ParsesAsTheSscanfFormatDid) {
  for (const char* text : {"1.31",
                           "1.34.2",
                           "1.35.0-rc.1",
                           "1.36.0-rc.2.5",
                           "2.41-dev+feat/footnote-popup",
                           "2.51",
                           "2.51-slim",
                           "2.52.0-rc.3-slim",
                           "2.41-dev+master",
                           "3",
                           "10.200.3000",
                           "",
                           "v2.51",
                           "bench",
                           "2.51.",
                           "2.51.1-rc",
                           "2.51.1-rc.",
                           "2.51.1-rc.x",
                           "2.51.1-beta.2",
                           "2..3",
                           "02.051.001"}) {
    expectSame(parseReleaseVersion(text), sscanfVersion(text), text);
  }
}

TEST(ReleaseVersion, FieldsTheTextDoesNotReachStayZero) {
  const ReleaseVersion v = parseReleaseVersion("2.41-dev+master");
  EXPECT_EQ(v.major, 2);
  EXPECT_EQ(v.minor, 41);
  EXPECT_EQ(v.patch, 0);
  EXPECT_EQ(v.rcRelease, 0);
  EXPECT_FALSE(v.isRc);
}

TEST(ReleaseVersion, OrdersByMajorMinorPatch) {
  EXPECT_TRUE(newer("2.52", "2.51"));
  EXPECT_FALSE(newer("2.51", "2.52"));
  EXPECT_TRUE(newer("3.0", "2.99"));
  EXPECT_TRUE(newer("2.51.1", "2.51"));
  EXPECT_FALSE(newer("2.51", "2.51.1"));
  EXPECT_TRUE(newer("2.52", "2.51-dev+master"));
  EXPECT_FALSE(newer("2.51", "2.51"));
}

TEST(ReleaseVersion, AStableReleaseIsNewerThanItsCandidates) {
  EXPECT_TRUE(newer("2.52.0", "2.52.0-rc.3"));
  EXPECT_FALSE(newer("2.52.0-rc.3", "2.52.0"));
  EXPECT_TRUE(newer("2.52.0-rc.1", "2.51.0"));
}

TEST(ReleaseVersion, CandidatesOrderByReleaseThenBuild) {
  EXPECT_TRUE(newer("2.52.0-rc.2", "2.52.0-rc.1"));
  EXPECT_FALSE(newer("2.52.0-rc.1", "2.52.0-rc.2"));
  EXPECT_TRUE(newer("2.52.0-rc.2.5", "2.52.0-rc.2.4"));
  EXPECT_TRUE(newer("2.52.0-rc.2.5", "2.52.0-rc.2"));
  EXPECT_FALSE(newer("2.52.0-rc.2.5", "2.52.0-rc.2.5"));
  EXPECT_FALSE(newer("2.52.0-rc.1.9", "2.52.0-rc.2"));
}
