// Host tests: what SecureHttpClient writes on the request line for a URL that is not ready for one.
// Plugins hand URLs to /api/relay and /api/fetch, and OPDS feeds hand over hrefs, unescaped. Over the
// scripted network of RangeDownloadTest (stubs/WiFiClient.h).

#include <SecureHttpClient.h>
#include <WiFiClient.h>
#include <gtest/gtest.h>

#include <string>

namespace {

struct UrlOnTheWire : ::testing::Test {
  crosspoint::SecureHttpClient http;

  void SetUp() override {
    FakeNet::reset();
    http.setTimeout(5000);
  }

  // The request line GET `url` sends, without its line end.
  std::string requestLineFor(const std::string& url) {
    FakeNet::replies().push_back(fakeOk());
    EXPECT_EQ(http.get(url, [](const uint8_t*, size_t) { return true; }), 200) << url;
    const std::string& request = FakeNet::requests().at(0);
    return request.substr(0, request.find("\r\n"));
  }
};

TEST_F(UrlOnTheWire, ASpaceIsEscaped) { EXPECT_EQ(requestLineFor("http://host/a b.txt"), "GET /a%20b.txt HTTP/1.1"); }

TEST_F(UrlOnTheWire, UtfEightIsEscapedByteByByte) {
  EXPECT_EQ(requestLineFor("http://host/Caf\xC3\xA9.txt"), "GET /Caf%C3%A9.txt HTTP/1.1");
}

// The one that matters most: unescaped, these end the request line and start headers of the URL's choosing.
TEST_F(UrlOnTheWire, ALineBreakCannotStartAHeader) {
  const std::string line = requestLineFor("http://host/x\r\nX-Injected: 1\r\nY: z");
  EXPECT_EQ(line, "GET /x%0D%0AX-Injected:%201%0D%0AY:%20z HTTP/1.1");
  EXPECT_EQ(FakeNet::requests().at(0).find("\r\nX-Injected"), std::string::npos);
}

TEST_F(UrlOnTheWire, AnEscapedUrlIsNotEscapedTwice) {
  EXPECT_EQ(requestLineFor("http://host/a%20b%2Fc.txt?q=%C3%A9"), "GET /a%20b%2Fc.txt?q=%C3%A9 HTTP/1.1");
}

TEST_F(UrlOnTheWire, AStrayPercentIsEscaped) {
  EXPECT_EQ(requestLineFor("http://host/100%.txt"), "GET /100%25.txt HTTP/1.1");
  FakeNet::reset();
  EXPECT_EQ(requestLineFor("http://host/a%2"), "GET /a%252 HTTP/1.1");
}

TEST_F(UrlOnTheWire, AnOrdinaryUrlIsLeftAlone) {
  EXPECT_EQ(requestLineFor("http://host/opds/feed.xml?page=2&q=a+b#top"),
            "GET /opds/feed.xml?page=2&q=a+b#top HTTP/1.1");
}

TEST_F(UrlOnTheWire, ALineBreakInTheHostIsRefused) {
  FakeNet::replies().push_back(fakeOk());
  EXPECT_LT(http.get("http://host\r\nX-Injected: 1\r\n/x", [](const uint8_t*, size_t) { return true; }), 0);
  EXPECT_EQ(FakeNet::connects(), 0);
}

}  // namespace
