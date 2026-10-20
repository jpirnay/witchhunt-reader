// Which https requests verify the server, and which may continue unverified when its certificate
// cannot be verified (lib/SecureNet/include/TlsPolicy.h). HttpDownloader resolves every request
// through planTls(); SecureClient::connect() retries through retryUnverified().
#include <TlsPolicy.h>
#include <gtest/gtest.h>

using crosspoint::planTls;
using crosspoint::TlsPlan;
using crosspoint::TlsPolicy;
using crosspoint::VerifiedConnectFailure;

namespace {
constexpr bool kSkip = true;
constexpr bool kCredentials = true;
constexpr bool kUnverifiable = true;

bool same(const TlsPlan& a, const TlsPlan& b) {
  return a.verify == b.verify && a.fallbackOnVerifyError == b.fallbackOnVerifyError;
}
constexpr TlsPlan kVerifyOnly{true, false};
constexpr TlsPlan kVerifyThenFallBack{true, true};
constexpr TlsPlan kUnverifiedOnly{false, false};
}  // namespace

// Firmware, and the release check that names it: verified whatever else holds.
TEST(TlsPolicy, StrictIsNeverDowngraded) {
  for (const bool skip : {false, true}) {
    for (const bool credentials : {false, true}) {
      for (const bool unverifiable : {false, true}) {
        EXPECT_TRUE(same(planTls(TlsPolicy::Strict, skip, credentials, unverifiable), kVerifyOnly))
            << skip << credentials << unverifiable;
      }
    }
  }
}

TEST(TlsPolicy, VerifiedHasNoFallbackAndHonoursTheSkipSetting) {
  for (const bool credentials : {false, true}) {
    for (const bool unverifiable : {false, true}) {
      EXPECT_TRUE(same(planTls(TlsPolicy::Verified, false, credentials, unverifiable), kVerifyOnly));
      EXPECT_TRUE(same(planTls(TlsPolicy::Verified, kSkip, credentials, unverifiable), kUnverifiedOnly));
    }
  }
}

TEST(TlsPolicy, UnverifiedNeverVerifies) {
  EXPECT_TRUE(same(planTls(TlsPolicy::Unverified, false, false, false), kUnverifiedOnly));
  EXPECT_TRUE(same(planTls(TlsPolicy::Unverified, kSkip, kCredentials, kUnverifiable), kUnverifiedOnly));
}

// An OPDS catalog without a login: verified first, unverified only if the certificate fails.
TEST(TlsPolicy, AnAnonymousRequestMayFallBack) {
  EXPECT_TRUE(same(planTls(TlsPolicy::VerifiedThenUnverified, false, false, false), kVerifyThenFallBack));
}

// The point of the policy's name: a password never goes over an unverified connection on its say-so.
TEST(TlsPolicy, ARequestWithCredentialsStaysVerified) {
  EXPECT_TRUE(same(planTls(TlsPolicy::VerifiedThenUnverified, false, kCredentials, false), kVerifyOnly));
  EXPECT_TRUE(same(planTls(TlsPolicy::VerifiedThenUnverified, false, kCredentials, kUnverifiable), kVerifyOnly))
      << "a host remembered as unverifiable is no reason to send a password unverified";
}

// The user's own choice still reaches the policy, for a request with credentials too.
TEST(TlsPolicy, TheSkipSettingStillWorks) {
  EXPECT_TRUE(same(planTls(TlsPolicy::VerifiedThenUnverified, kSkip, false, false), kUnverifiedOnly));
  EXPECT_TRUE(same(planTls(TlsPolicy::VerifiedThenUnverified, kSkip, kCredentials, false), kUnverifiedOnly));
}

// A host that already took the fallback this session skips the ~3 s verify it would fail again.
TEST(TlsPolicy, AHostThatFailedBeforeGoesStraightToTheFallback) {
  EXPECT_TRUE(same(planTls(TlsPolicy::VerifiedThenUnverified, false, false, kUnverifiable), kUnverifiedOnly));
}

// The fallback answers a certificate that cannot be verified, never a timeout, DNS or other error.
TEST(TlsPolicy, OnlyACertificateFailureIsRetriedUnverified) {
  EXPECT_TRUE(crosspoint::retryUnverified(kVerifyThenFallBack, VerifiedConnectFailure::Certificate));
  EXPECT_FALSE(crosspoint::retryUnverified(kVerifyThenFallBack, VerifiedConnectFailure::Other));
  EXPECT_FALSE(crosspoint::retryUnverified(kVerifyOnly, VerifiedConnectFailure::Certificate));
  const TlsPlan anonymous = planTls(TlsPolicy::VerifiedThenUnverified, false, false, false);
  const TlsPlan withLogin = planTls(TlsPolicy::VerifiedThenUnverified, false, kCredentials, false);
  EXPECT_TRUE(crosspoint::retryUnverified(anonymous, VerifiedConnectFailure::Certificate));
  EXPECT_FALSE(crosspoint::retryUnverified(withLogin, VerifiedConnectFailure::Certificate));
  EXPECT_FALSE(crosspoint::retryUnverified(planTls(TlsPolicy::Strict, false, false, false),
                                           VerifiedConnectFailure::Certificate));
}

// --- UnverifiableHosts ---------------------------------------------------------------------------

TEST(UnverifiableHosts, RemembersHostsWithoutRegardToCase) {
  crosspoint::UnverifiableHosts hosts;
  EXPECT_FALSE(hosts.contains("www.gutenberg.org"));
  hosts.add("www.gutenberg.org");
  EXPECT_TRUE(hosts.contains("www.gutenberg.org"));
  EXPECT_TRUE(hosts.contains("WWW.Gutenberg.ORG"));
  EXPECT_FALSE(hosts.contains("m.gutenberg.org"));
  EXPECT_FALSE(hosts.contains(""));
  hosts.add("");
  EXPECT_FALSE(hosts.contains(""));
  hosts.clear();
  EXPECT_FALSE(hosts.contains("www.gutenberg.org"));
}

TEST(UnverifiableHosts, ForgetsTheOldestWhenFull) {
  crosspoint::UnverifiableHosts hosts;
  const char* names[] = {"a.example", "b.example", "c.example", "d.example", "e.example"};
  for (const char* name : names) hosts.add(name);
  EXPECT_FALSE(hosts.contains("a.example")) << "replaced by the fifth";
  for (int i = 1; i < 5; i++) EXPECT_TRUE(hosts.contains(names[i])) << names[i];
  hosts.add("c.example");  // already there: nothing moves
  hosts.add("f.example");
  EXPECT_FALSE(hosts.contains("b.example"));
  EXPECT_TRUE(hosts.contains("c.example"));
  EXPECT_TRUE(hosts.contains("f.example"));
}

// --- tlsPeer -------------------------------------------------------------------------------------

// The fallback line, the "earlier this session" line and the memo all name a server this way.
TEST(TlsPeer, IsTheHostAndAlwaysThePort) {
  EXPECT_EQ(crosspoint::tlsPeer("www.gutenberg.org", 443), "www.gutenberg.org:443");
  EXPECT_EQ(crosspoint::tlsPeer("192.168.0.113", 8443), "192.168.0.113:8443");
  EXPECT_EQ(crosspoint::tlsPeer("opds.example", 80), "opds.example:80");
}
