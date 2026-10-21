// Host tests: SecureHttpClient decides verification, the fallback and the session memo of
// unverifiable servers (crosspoint::UnverifiableHosts) at each https connect of a request, for the
// server that connect reaches (crosspoint::planHop()). Over the scripted network of
// RangeDownloadTest (stubs/WiFiClient.h); the stub SecureClient defined there fails the verify of a
// server in FakeNet::unverifiable() and records every handshake in FakeNet::handshakes().

#include <SecureHttpClient.h>
#include <WiFiClient.h>
#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace {

struct TlsHop : ::testing::Test {
  crosspoint::UnverifiableHosts memo;

  void SetUp() override { FakeNet::reset(); }

  // One request, as HttpDownloader makes it: a fresh client with the curated roots (any non-null
  // text here), the fallback as the plan says (planTls() allows it only for a request without
  // credentials) and the session's memo. Through redirects to each of locations, then a 200.
  int get(const std::string& url, std::initializer_list<const char*> locations, const bool fallback = true,
          const bool credentials = false) {
    FakeNet::replies().clear();
    for (const char* location : locations) FakeNet::replies().push_back(fakeRedirectTo(location, 301));
    FakeNet::replies().push_back(fakeOk());
    crosspoint::SecureHttpClient http;
    http.setTimeout(5000);
    http.setCACert("roots");
    http.setAllowInsecureFallback(fallback);
    http.setUnverifiableHosts(&memo);
    if (credentials) http.setBasicAuth("reader", "secret");
    return http.get(url, [](const uint8_t*, size_t) { return true; });
  }

  static std::vector<std::string> handshakes() { return FakeNet::handshakes(); }
};

using Handshakes = std::vector<std::string>;

TEST_F(TlsHop, AFallbackIsRememberedUnderTheServerItReached) {
  FakeNet::unverifiable() = {"opds.example:443"};
  EXPECT_EQ(get("https://opds.example/feed", {}), 200);
  EXPECT_TRUE(memo.contains("opds.example:443"));
  // The next request skips the verify that would fail; the port spelled out is the same server.
  EXPECT_EQ(get("https://opds.example:443/feed?page=2", {}), 200);
  EXPECT_EQ(handshakes(), (Handshakes{"opds.example:443 fallback", "opds.example:443 unverified"}));
}

// Gutenberg's search template is http://m.gutenberg.org/..., which redirects to https. The memo used
// to be checked against the URL the request started at (port 80) and filled from the last hop
// (port 443), so it never matched; now both are the https hop's server.
TEST_F(TlsHop, AnHttpStartIsDecidedAndRememberedAtItsHttpsHop) {
  FakeNet::unverifiable() = {"m.example:443"};
  EXPECT_EQ(get("http://m.example/search?q=kafka", {"https://m.example/search?q=kafka"}), 200);
  EXPECT_TRUE(memo.contains("m.example:443"));
  EXPECT_FALSE(memo.contains("m.example:80"));
  EXPECT_EQ(get("http://m.example/search?q=kafka", {"https://m.example/search?q=kafka"}), 200);
  EXPECT_EQ(handshakes(), (Handshakes{"m.example:443 fallback", "m.example:443 unverified"}));
}

// A catalog without a login whose downloads redirect to another server (a CDN): each server is
// checked, and remembered, for itself.
TEST_F(TlsHop, ARedirectToAnotherServerIsDecidedForThatServer) {
  FakeNet::unverifiable() = {"cdn.example:443"};
  EXPECT_EQ(get("https://opds.example/book/1", {"https://cdn.example/book1.epub"}), 200);
  EXPECT_FALSE(memo.contains("opds.example:443"));
  EXPECT_TRUE(memo.contains("cdn.example:443"));
  EXPECT_EQ(get("https://opds.example/book/1", {"https://cdn.example/book1.epub"}), 200);
  EXPECT_EQ(handshakes(), (Handshakes{"opds.example:443 verified", "cdn.example:443 fallback",
                                      "opds.example:443 verified", "cdn.example:443 unverified"}));
}

// The first server being remembered no longer takes the whole request unverified.
TEST_F(TlsHop, ARememberedServerDoesNotSpreadToTheNextHop) {
  FakeNet::unverifiable() = {"opds.example:443"};
  memo.add("opds.example:443");
  EXPECT_EQ(get("https://opds.example/book/1", {"https://cdn.example/book1.epub"}), 200);
  EXPECT_EQ(handshakes(), (Handshakes{"opds.example:443 unverified", "cdn.example:443 verified"}));
}

// A catalog with a login: planTls() gives its requests no fallback, so no hop of one falls back or
// takes the memo's shortcut, not even a redirect to another server, which the credentials do not
// follow; nothing is remembered either.
TEST_F(TlsHop, ARequestWithoutTheFallbackNeverFallsBackOnAnyHop) {
  FakeNet::unverifiable() = {"opds.example:443", "cdn.example:443"};
  memo.add("cdn.example:443");
  EXPECT_LT(get("https://opds.example/feed", {}, /*fallback=*/false, /*credentials=*/true), 0);
  FakeNet::unverifiable() = {"cdn.example:443"};
  EXPECT_LT(get("https://opds.example/book/1", {"https://cdn.example/book1.epub"}, false, true), 0);
  EXPECT_EQ(handshakes(),
            (Handshakes{"opds.example:443 refused", "opds.example:443 verified", "cdn.example:443 refused"}));
  EXPECT_FALSE(memo.contains("opds.example:443"));
}

// Even for a caller that allowed the fallback: a hop that sends the credentials never takes it, and
// is not short-cut by the memo; a hop of the same request that does not send them may.
TEST_F(TlsHop, AHopWithCredentialsNeverFallsBack) {
  FakeNet::unverifiable() = {"opds.example:443"};
  memo.add("opds.example:443");
  EXPECT_LT(get("https://opds.example/feed", {}, /*fallback=*/true, /*credentials=*/true), 0);
  FakeNet::unverifiable() = {"cdn.example:443"};
  EXPECT_EQ(get("https://opds.example/book/1", {"https://cdn.example/book1.epub"}, true, true), 200);
  EXPECT_EQ(handshakes(),
            (Handshakes{"opds.example:443 refused", "opds.example:443 verified", "cdn.example:443 fallback"}));
}

}  // namespace
