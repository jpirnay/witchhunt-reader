// Host tests: crosspoint::splitUrl() (lib/SecureNet/include/UrlTarget.h), the one reading of where a URL sends a
// request, and SecureHttpClient connecting where it says. The plugin allowlists read URLs with the same function
// (src/network/PluginHosts.h, test/plugin_relay): the host they judge is the host the client connects to.
// Over the scripted network of RangeDownloadTest (stubs/WiFiClient.h).

#include <SecureHttpClient.h>
#include <UrlTarget.h>
#include <WiFiClient.h>
#include <gtest/gtest.h>

#include <string>

namespace {

using crosspoint::splitUrl;
using crosspoint::UrlTarget;

TEST(UrlTarget, SplitsSchemeHostPortAndRest) {
  UrlTarget t;
  ASSERT_TRUE(splitUrl("https://covers.example.org:8443/a/b?q=1#f", t));
  EXPECT_EQ(t.scheme, "https");
  EXPECT_EQ(t.host, "covers.example.org");
  EXPECT_EQ(t.port, 8443);
  EXPECT_EQ(t.rest, "/a/b?q=1#f");

  ASSERT_TRUE(splitUrl("http://192.168.0.10", t));
  EXPECT_EQ(t.host, "192.168.0.10");
  EXPECT_EQ(t.port, 80);
  EXPECT_EQ(t.rest, "");

  ASSERT_TRUE(splitUrl("HTTPS://Example.ORG/x", t));  // scheme and host keep their case; the port is https's
  EXPECT_EQ(t.scheme, "HTTPS");
  EXPECT_EQ(t.host, "Example.ORG");
  EXPECT_EQ(t.port, 443);
}

// The authority ends at the first '/', '?' or '#', as RFC 3986 has it: what follows is not the host.
TEST(UrlTarget, AQueryOrFragmentEndsTheHost) {
  UrlTarget t;
  ASSERT_TRUE(splitUrl("https://good.example?next=evil.example", t));
  EXPECT_EQ(t.host, "good.example");
  EXPECT_EQ(t.rest, "?next=evil.example");
  ASSERT_TRUE(splitUrl("https://good.example#@evil.example", t));
  EXPECT_EQ(t.host, "good.example");
  EXPECT_EQ(t.rest, "#@evil.example");
}

// The bug: the allowlist judged what follows the last '@' (good.example), the client split host from port at the last
// ':' and connected to evil.example:443. A URL with user info now names no host for anyone.
TEST(UrlTarget, UserInfoIsRefused) {
  UrlTarget t;
  for (const char* url : {"https://evil.example:443@good.example/x", "https://good.example@evil.example/",
                          "https://user:pw@good.example/", "http://@good.example/", "https://a@b@good.example/"}) {
    EXPECT_FALSE(splitUrl(url, t)) << url;
  }
}

TEST(UrlTarget, OnlyTheBytesOfAHostNameMakeAHost) {
  UrlTarget t;
  // A backslash (a browser reads it as '/'), an escape, a space, a line break, UTF-8, an IPv6 literal.
  for (const char* url : {"https://evil.example\\.good.example/", "https://evil.example%2egood.example/",
                          "https://good example/", "http://host\r\nX: 1/",
                          "https://b\xC3\xBC"
                          "cher.example/",
                          "http://[::1]/", "http://[2001:db8::1]:8080/"}) {
    EXPECT_FALSE(splitUrl(url, t)) << url;
  }
  EXPECT_TRUE(splitUrl("https://my_host-1.example/", t));
  EXPECT_EQ(t.host, "my_host-1.example");
}

TEST(UrlTarget, APortIsOneTo65535InDigits) {
  UrlTarget t;
  for (const char* url : {"https://h.example:/", "https://h.example:0/", "https://h.example:65536/",
                          "https://h.example:443x/", "https://h.example:44:3/", "https://h.example:123456/"}) {
    EXPECT_FALSE(splitUrl(url, t)) << url;
  }
  ASSERT_TRUE(splitUrl("https://h.example:65535/", t));
  EXPECT_EQ(t.port, 65535);
}

TEST(UrlTarget, OnlyHttpAndHttps) {
  UrlTarget t;
  for (const char* url : {"", "h.example/x", "https:/h.example", "https://", "https:///x", "ftp://h.example/",
                          "file:///etc/passwd", "javascript://h.example/"}) {
    EXPECT_FALSE(splitUrl(url, t)) << url;
  }
}

struct ClientUrls : ::testing::Test {
  crosspoint::SecureHttpClient http;

  void SetUp() override {
    FakeNet::reset();
    http.setTimeout(5000);
  }

  int get(const std::string& url) {
    return http.get(url, [](const uint8_t*, size_t) { return true; });
  }
};

// The client refuses it too, before any connect: nothing connects with user info, so nothing can connect elsewhere
// than an allowlist judged.
TEST_F(ClientUrls, AUrlWithUserInfoIsNeverConnected) {
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(get("http://evil.example:80@good.example/x"), crosspoint::SecureHttpClient::ERR_BAD_URL);
  EXPECT_EQ(get("http://good.example@evil.example/x"), crosspoint::SecureHttpClient::ERR_BAD_URL);
  EXPECT_EQ(FakeNet::connects(), 0);
  EXPECT_TRUE(FakeNet::requests().empty());
}

// A query straight after the host still asks for "/", from that host.
TEST_F(ClientUrls, AQueryAfterTheHostAsksForTheRoot) {
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(get("http://good.example?next=evil.example"), 200);
  const std::string& request = FakeNet::requests().at(0);
  EXPECT_EQ(request.substr(0, request.find("\r\n")), "GET /?next=evil.example HTTP/1.1");
  EXPECT_NE(request.find("\r\nHost: good.example\r\n"), std::string::npos);
}

// A redirect to a URL with user info stops there: the client cannot read it, so it cannot go anywhere.
TEST_F(ClientUrls, ARedirectWithUserInfoGoesNowhere) {
  FakeNet::replies().push_back(
      {"HTTP/1.1 302 Found\r\nLocation: http://evil.example:80@good.example/x\r\nContent-Length: 0\r\n\r\n"});
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(get("http://good.example/start"), crosspoint::SecureHttpClient::ERR_BAD_URL);
  EXPECT_EQ(FakeNet::requests().size(), 1u);
}

}  // namespace
