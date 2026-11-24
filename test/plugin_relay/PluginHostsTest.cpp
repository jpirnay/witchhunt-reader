// The hosts a plugin may reach (src/network/PluginHosts.h): what the relay, fetch-to-SD and the device events judge
// a URL by. The judged host is read with crosspoint::splitUrl(), as SecureHttpClient reads it to connect
// (test/range_download/UrlTargetTest.cpp).
#include <gtest/gtest.h>

#include <string>

#include "network/PluginHosts.h"

using PluginHosts::hostMatches;
using PluginHosts::Reach;
using PluginHosts::urlHost;

TEST(PluginHostsTest, TheJudgedHostIsTheConnectedOneInLowerCase) {
  EXPECT_EQ(urlHost("https://covers.openlibrary.org/b/id/1-L.jpg"), "covers.openlibrary.org");
  EXPECT_EQ(urlHost("http://192.168.0.10:8080/opds"), "192.168.0.10");
  EXPECT_EQ(urlHost("https://good.example?next=x"), "good.example");
  // Upper case: the Plugins page stores an approval in lower case, so a host judged as written never matched it and
  // https://Example.org/ could not be approved at all.
  EXPECT_EQ(urlHost("https://Example.ORG/Path"), "example.org");
  EXPECT_EQ(urlHost("HTTPS://EXAMPLE.org"), "example.org");
}

// "https://evil.com:443@good.com/x": the old judge dropped everything up to the '@' and allowed good.com; the client
// connected to evil.com:443. Neither reads such a URL now.
TEST(PluginHostsTest, AUrlWithUserInfoHasNoHost) {
  for (const char* url : {"https://evil.example:443@good.example/x", "https://good.example@evil.example/",
                          "https://user:pw@good.example/"}) {
    EXPECT_EQ(urlHost(url), "") << url;
  }
  EXPECT_EQ(urlHost("ftp://good.example/"), "");
  EXPECT_EQ(urlHost("https://evil.example\\.good.example/"), "");
  EXPECT_EQ(urlHost("http://[::1]:80/"), "");
}

TEST(PluginHostsTest, AnEntryMatchesExactlyOrAsAParentDomain) {
  EXPECT_TRUE(hostMatches("openlibrary.org", "openlibrary.org"));
  EXPECT_TRUE(hostMatches("covers.openlibrary.org", ".openlibrary.org"));
  EXPECT_FALSE(hostMatches("openlibrary.org", ".openlibrary.org"));  // a dot entry is subdomains only
  EXPECT_FALSE(hostMatches("evilopenlibrary.org", ".openlibrary.org"));
  EXPECT_FALSE(hostMatches("openlibrary.org.evil.example", "openlibrary.org"));
  EXPECT_FALSE(hostMatches("openlibrary.org", ""));
  EXPECT_FALSE(hostMatches("", "openlibrary.org"));
  // A manifest may spell its entries in any case.
  EXPECT_TRUE(hostMatches("covers.openlibrary.org", ".OpenLibrary.org"));
  EXPECT_TRUE(hostMatches("books.google.com", "Books.Google.com"));
}

TEST(PluginHostsTest, AReachAllowsItsManifestHostsAndTheApprovedOnes) {
  Reach reach;
  reach.allow("openlibrary.org");
  reach.allow(".gr-assets.com");
  reach.approve("Sync.Example");  // stored in lower case by the Plugins page; either way it matches
  EXPECT_TRUE(reach.allows("https://openlibrary.org/search.json?q=x"));
  EXPECT_TRUE(reach.allows("https://i.gr-assets.com/images/1.jpg"));
  EXPECT_TRUE(reach.allows("https://sync.example/syncs/progress"));
  EXPECT_TRUE(reach.allows("https://SYNC.example:8443/syncs/progress"));
  EXPECT_TRUE(reach.allows("https://OpenLibrary.ORG/"));
  EXPECT_FALSE(reach.allows("https://evil.example/"));
  // An approval is exact: it does not reach subdomains.
  EXPECT_FALSE(reach.allows("https://a.sync.example/"));
  EXPECT_FALSE(reach.allows("not a url"));
}

TEST(PluginHostsTest, UserInfoCannotSmuggleAnAllowedHostPastTheJudge) {
  Reach reach;
  reach.allow("good.example");
  reach.approve("good.example");
  EXPECT_FALSE(reach.allows("https://evil.example:443@good.example/x"));
  EXPECT_FALSE(reach.allows("https://evil.example@good.example/x"));
  EXPECT_FALSE(reach.allows("https://good.example@evil.example/x"));
  EXPECT_TRUE(reach.allows("https://good.example/x?u=evil.example@good.example"));
}

TEST(PluginHostsTest, AnEmptyReachAllowsNothing) {
  const Reach reach{};
  EXPECT_FALSE(reach.allows("https://openlibrary.org/"));
  Reach blank;
  blank.allow("");
  blank.approve("");
  EXPECT_FALSE(blank.allows("https://openlibrary.org/"));
}
