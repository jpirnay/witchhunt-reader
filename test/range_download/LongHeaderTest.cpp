// Host tests: a response header line longer than SecureHttpClient keeps (MAX_LINE, 4096 B). GitHub's
// release downloads answer with a 302 whose Content-Security-Policy line is 4174 B (measured
// 2026-11-11), after the Location and before the Content-Length; the client gave up on that line, so
// every firmware update and web-bundle install from a release failed with ERR_TIMEOUT. Over the
// scripted network of RangeDownloadTest (stubs/WiFiClient.h).

#include <RangeDownload.h>
#include <SecureHttpClient.h>
#include <WiFiClient.h>
#include <gtest/gtest.h>

#include <string>

namespace {

// A header line of exactly `length` bytes, its CRLF not counted.
std::string headerLine(const std::string& name, size_t length) {
  std::string line = name + ": ";
  line.append(length - line.size(), 'x');
  return line + "\r\n";
}

// GitHub's release-download redirect, header for header in the order it sends them.
FakeReply githubStyleRedirectTo(const std::string& location, size_t policyLength = 4174) {
  return {
      "HTTP/1.1 302 Found\r\nDate: Wed, 11 Nov 2026 13:28:26 GMT\r\nContent-Type: text/html; charset=utf-8\r\n"
      "Location: " +
          location + "\r\nCache-Control: no-cache\r\n" + headerLine("Content-Security-Policy", policyLength) +
          "Server: github.com\r\nContent-Length: 0\r\n\r\n",
      FakeReply::End::KeepOpen};
}

struct LongHeader : ::testing::Test {
  crosspoint::SecureHttpClient http;
  std::string body;

  void SetUp() override {
    FakeNet::reset();
    http.setTimeout(5000);
  }

  int get(const std::string& url) {
    return http.get(url, [this](const uint8_t* data, size_t len) {
      body.append(reinterpret_cast<const char*>(data), len);
      return true;
    });
  }
};

TEST_F(LongHeader, GithubsReleaseRedirectIsFollowed) {
  FakeNet::replies().push_back(githubStyleRedirectTo("https://release-assets.example/asset?sig=abc"));
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(get("https://github.example/owner/repo/releases/download/2.50/firmware-x4pro.bin"), 200);
  ASSERT_EQ(FakeNet::requests().size(), 2u);
  EXPECT_EQ(FakeNet::requests().at(1).rfind("GET /asset?sig=abc HTTP/1.1\r\n", 0), 0u) << FakeNet::requests().at(1);
  EXPECT_EQ(body, "ok");
}

// However long the header: the X4 Pro's log showed hdr=11008B, two attempts at the 302 above.
TEST_F(LongHeader, ARedirectWithElevenKilobytesOfHeaderIsFollowed) {
  FakeNet::replies().push_back(githubStyleRedirectTo("https://release-assets.example/asset", 11008));
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(get("https://github.example/owner/repo/releases/download/2.50/firmware-x4pro.bin"), 200);
  EXPECT_EQ(body, "ok");
}

// The way the update installs, over downloadToSink(): a restartable sink, on the S3's heap (one
// streamed response) and on the C3's (Range chunks, sent to the redirect target after the first).
size_t g_largestBlock = 0;
size_t largestBlock() { return g_largestBlock; }

int installThroughRedirect(crosspoint::SecureHttpClient& http, const std::string& image, std::string& written) {
  FakeNet::replies().push_back(githubStyleRedirectTo("https://release-assets.example/asset?sig=abc"));
  FakeNet::resource() = image;
  FakeNet::serving() = true;
  crosspoint::DownloadSink sink;
  sink.write = [&written](const uint8_t* data, size_t len) {
    written.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  sink.rewind = [&written] {
    written.clear();
    return true;
  };
  crosspoint::ChunkSession session;
  return crosspoint::downloadToSink(http, "https://github.example/owner/repo/releases/download/2.50/firmware.bin", sink,
                                    session, &largestBlock);
}

TEST_F(LongHeader, AnUpdateStreamsThroughGithubsRedirect) {
  g_largestBlock = 7864308;  // the X4 Pro's largest block when its update failed
  const std::string image(50000, 'i');
  std::string written;
  EXPECT_EQ(installThroughRedirect(http, image, written), 200);
  EXPECT_EQ(written, image);
  // The scripted server answers whatever host is asked, so the image must have come from the target.
  ASSERT_EQ(FakeNet::requests().size(), 2u);
  EXPECT_EQ(FakeNet::requests().at(1).rfind("GET /asset?sig=abc HTTP/1.1\r\n", 0), 0u) << FakeNet::requests().at(1);
}

TEST_F(LongHeader, AnUpdateChunksThroughGithubsRedirect) {
  g_largestBlock = 16372;  // what the X3 showed after a handshake
  const std::string image(50000, 'i');
  std::string written;
  EXPECT_EQ(installThroughRedirect(http, image, written), 200);
  EXPECT_EQ(written, image);
  EXPECT_GT(FakeNet::requests().size(), 2u);  // chunked: several Range requests after the redirect
  for (size_t i = 1; i < FakeNet::requests().size(); ++i) {
    EXPECT_EQ(FakeNet::requests().at(i).rfind("GET /asset?sig=abc HTTP/1.1\r\n", 0), 0u) << "request " << i;
  }
}

// The headers after the long one still count: here the Content-Length that frames the body.
TEST_F(LongHeader, HeadersAfterTheLongOneAreRead) {
  FakeNet::replies().push_back(
      {"HTTP/1.1 200 OK\r\n" + headerLine("X-Padding", 9000) + "Content-Length: 5\r\n\r\nhello trailing",
       FakeReply::End::KeepOpen});
  EXPECT_EQ(get("https://host.example/file"), 200);
  EXPECT_EQ(body, "hello");
}

// A Location cut at MAX_LINE would send the request somewhere else: the response is refused instead.
TEST_F(LongHeader, AnOverlongLocationIsNotFollowed) {
  const std::string location = "https://host.example/" + std::string(5000, 'a');
  FakeNet::replies().push_back(fakeRedirectTo(location));
  FakeNet::replies().push_back(fakeOk());
  EXPECT_LT(get("https://host.example/start"), 0);
  EXPECT_EQ(FakeNet::requests().size(), 1u);
}

// Likewise the headers that frame the body: a cut value cannot be trusted.
TEST_F(LongHeader, AnOverlongFramingHeaderIsRefused) {
  const std::string length = "Content-Length: " + std::string(5000, ' ') + "2\r\n";
  FakeNet::replies().push_back({"HTTP/1.1 200 OK\r\n" + length + "\r\nok", FakeReply::End::KeepOpen});
  EXPECT_LT(get("https://host.example/file"), 0);
  EXPECT_TRUE(body.empty());
}

// An overlong status line is no HTTP response.
TEST_F(LongHeader, AnOverlongStatusLineIsRefused) {
  FakeNet::replies().push_back(
      {"HTTP/1.1 200 " + std::string(5000, 'O') + "\r\nContent-Length: 2\r\n\r\nok", FakeReply::End::KeepOpen});
  EXPECT_LT(get("https://host.example/file"), 0);
}

// A chunk-size line is a few hex digits; one past MAX_LINE is garbage and ends the body as truncated.
TEST_F(LongHeader, AnOverlongChunkSizeLineIsTruncation) {
  FakeNet::replies().push_back({"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n" +
                                    std::string(5000, '0') + "2\r\nok\r\n0\r\n\r\n",
                                FakeReply::End::KeepOpen});
  EXPECT_EQ(get("https://host.example/file"), crosspoint::SecureHttpClient::ERR_TRUNCATED);
}

}  // namespace
