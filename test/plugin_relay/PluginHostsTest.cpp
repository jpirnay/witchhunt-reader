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

// The hosts a plugin's device events were refused for (PluginHosts::wantedWith()): what GET /api/plugins lists as
// "wants" and the Plugins page offers to Allow or Dismiss. Kept in the plugin's folder, so removing the plugin removes
// them (CrossPointWebServer::handlePluginRemove() deletes the folder).
namespace {
std::string listed(const std::string& list) {
  std::string_view hosts[PluginHosts::kMaxWanted];
  const size_t n = PluginHosts::wantedHosts(list, hosts);
  std::string out;
  for (size_t i = 0; i < n; i++) out += std::string(hosts[i]) + ";";
  return out;
}
}  // namespace

TEST(PluginHostsTest, ARefusedHostIsAskedForOnce) {
  std::string list = PluginHosts::wantedWith("", "w.example");
  EXPECT_EQ(list, "w.example\n");
  EXPECT_EQ(PluginHosts::wantedWith(list, "w.example"), list);  // no second entry, and no write
  list = PluginHosts::wantedWith(list, "cdn.example");
  EXPECT_EQ(listed(list), "w.example;cdn.example;");
}

TEST(PluginHostsTest, TheRequestsAreBoundedOldestFirst) {
  std::string list;
  for (const char* host : {"a.example", "b.example", "c.example", "d.example", "e.example", "f.example"}) {
    list = PluginHosts::wantedWith(list, host);
  }
  EXPECT_EQ(listed(list), "c.example;d.example;e.example;f.example;");
  EXPECT_EQ(PluginHosts::kMaxWanted, 4u);
}

TEST(PluginHostsTest, OnlyAHostUrlHostGivesIsRecorded) {
  for (const char* host : {"", "W.Example", "a b.example", "evil.example:443", "u@evil.example", "x/y", "a\nb"}) {
    EXPECT_EQ(PluginHosts::wantedWith("", host), "") << host;
  }
  EXPECT_EQ(PluginHosts::wantedWith("", std::string(PluginHosts::kMaxWantedHost + 1, 'a')), "");
  EXPECT_EQ(PluginHosts::wantedWith("", std::string(PluginHosts::kMaxWantedHost, 'a')),
            std::string(PluginHosts::kMaxWantedHost, 'a') + "\n");
}

TEST(PluginHostsTest, AllowClearsARequestAndDismissKeepsItFromComingBack) {
  std::string list = PluginHosts::wantedWith(PluginHosts::wantedWith("", "w.example"), "x.example");
  // Allow (an approval) answers the request.
  EXPECT_EQ(listed(PluginHosts::wantedWithout(list, "w.example", false)), "x.example;");
  // Dismiss drops it from the offer, and a later refusal does not ask again.
  list = PluginHosts::wantedWithout(list, "w.example", true);
  EXPECT_EQ(listed(list), "x.example;");
  EXPECT_EQ(PluginHosts::wantedWith(list, "w.example"), list);
  EXPECT_EQ(listed(PluginHosts::wantedWith(list, "w.example")), "x.example;");
  // An approval after all clears the dismissal too: the next refusal would ask again.
  list = PluginHosts::wantedWithout(list, "w.example", false);
  EXPECT_EQ(listed(PluginHosts::wantedWith(list, "w.example")), "x.example;w.example;");
  EXPECT_EQ(PluginHosts::wantedWithout("", "w.example", false), "");
}

// The file sits in the plugin's folder, which the plugin may write: anything that is not a well-formed entry is left
// out, and the list stays within its bound.
TEST(PluginHostsTest, AWrongListIsReadAsFarAsItIsRight) {
  const std::string written = "ok.example\nNOT OK\n-gone.example\n\n:bad\r\nlast.example";
  EXPECT_EQ(listed(written), "ok.example;last.example;");
  EXPECT_EQ(PluginHosts::wantedWith(written, "gone.example"), written);  // dismissed stays dismissed
  EXPECT_EQ(PluginHosts::wantedWith(written, "new.example"), "ok.example\n-gone.example\nlast.example\nnew.example\n");
}
