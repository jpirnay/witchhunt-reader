// Host tests: responses that carry no body (RFC 9112 §6.3). A 204, a 304, a 1xx and any reply to
// HEAD end with their header block. SecureHttpClient read them as bodies: one without a
// Content-Length fell into read-until-close, which on a kept-alive connection waited out the whole
// timeout and failed with ERR_TIMEOUT. A KOSync server built on Spring (BookLore/grimmory) answers
// with exactly such a 204. Over the scripted network of RangeDownloadTest (stubs/WiFiClient.h),
// whose clock moves only by the client's own delay() calls, so a stall shows as time spent.
// After Free-Ink PR #65 (Dr. Foulad) and PR #212 (Justin Mitchell).

#include <SecureHttpClient.h>
#include <WiFiClient.h>
#include <gtest/gtest.h>

#include <string>

namespace {

constexpr uint32_t kTimeoutMs = 5000;
// Well under the timeout: an answer that took this long waited for something.
constexpr unsigned long kPrompt = 100;

FakeReply keptOpen(const std::string& bytes) { return {bytes, FakeReply::End::KeepOpen}; }

struct BodilessReply : ::testing::Test {
  crosspoint::SecureHttpClient http;
  unsigned long startMs = 0;

  void SetUp() override {
    FakeNet::reset();
    http.setTimeout(kTimeoutMs);
    startMs = fakeNow();
  }

  unsigned long elapsed() const { return fakeNow() - startMs; }
};

// Tomcat sends a 204 with no Content-Length, and keeps the connection.
TEST_F(BodilessReply, NoContentEndsWithItsHeaders) {
  FakeNet::replies().push_back(keptOpen("HTTP/1.1 204 No Content\r\nDate: Sun, 15 Nov 2026 10:00:00 GMT\r\n\r\n"));
  EXPECT_EQ(http.PUT("https://sync.example/syncs/progress", "{}"), 204);
  EXPECT_LT(elapsed(), kPrompt);
  EXPECT_TRUE(http.getBody().empty());
}

TEST_F(BodilessReply, TheConnectionIsKeptAfterNoContent) {
  FakeNet::replies().push_back(keptOpen("HTTP/1.1 204 No Content\r\n\r\n"));
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(http.PUT("https://sync.example/syncs/progress", "{}"), 204);
  EXPECT_EQ(http.GET("https://sync.example/syncs/progress/abc"), 200);
  EXPECT_EQ(http.getBody(), "ok");
  EXPECT_EQ(FakeNet::connects(), 1);
  EXPECT_LT(elapsed(), kPrompt);
}

// The streaming path: a 204 delivers nothing to the sink.
TEST_F(BodilessReply, AStreamedNoContentEndsWithItsHeaders) {
  FakeNet::replies().push_back(keptOpen("HTTP/1.1 204 No Content\r\n\r\n"));
  int chunks = 0;
  EXPECT_EQ(http.get("https://host.example/file",
                     [&chunks](const uint8_t*, size_t) {
                       ++chunks;
                       return true;
                     }),
            204);
  EXPECT_EQ(chunks, 0);
  EXPECT_LT(elapsed(), kPrompt);
}

// A 304 repeats the full response's headers, Content-Length included; there is still no body.
TEST_F(BodilessReply, NotModifiedReadsNoBodyWhateverItsLength) {
  FakeNet::replies().push_back(keptOpen("HTTP/1.1 304 Not Modified\r\nContent-Length: 1234\r\n\r\n"));
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(http.GET("https://host.example/feed"), 304);
  EXPECT_LT(elapsed(), kPrompt);
  EXPECT_EQ(http.GET("https://host.example/feed"), 200);
  EXPECT_EQ(http.getBody(), "ok");
  EXPECT_EQ(FakeNet::connects(), 1);
}

// A reply to HEAD gives the length the body would have, and no body.
TEST_F(BodilessReply, AReplyToHeadHasNoBody) {
  FakeNet::replies().push_back(keptOpen("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n"));
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(http.request("HEAD", "https://host.example/book.epub"), 200);
  EXPECT_EQ(http.lastContentLength(), 5000);
  EXPECT_LT(elapsed(), kPrompt);
  EXPECT_EQ(http.GET("https://host.example/book.epub"), 200);
  EXPECT_EQ(http.getBody(), "ok");
  EXPECT_EQ(FakeNet::connects(), 1);
}

// HEAD stays HEAD through a redirect: as a GET, it would fetch the body it did not ask for.
TEST_F(BodilessReply, HeadStaysHeadThroughARedirect) {
  FakeNet::replies().push_back(fakeRedirectTo("https://host.example/moved.epub"));
  FakeNet::replies().push_back(keptOpen("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n"));
  EXPECT_EQ(http.request("HEAD", "https://host.example/book.epub"), 200);
  ASSERT_EQ(FakeNet::requests().size(), 2u);
  EXPECT_EQ(FakeNet::requests().at(1).rfind("HEAD /moved.epub HTTP/1.1\r\n", 0), 0u) << FakeNet::requests().at(1);
  EXPECT_LT(elapsed(), kPrompt);
}

// An interim response comes before the final one: the final one is the answer, and nothing of it is
// left on the connection for the next request to read as its own.
TEST_F(BodilessReply, AnInterimResponseIsReadPast) {
  FakeNet::replies().push_back(
      keptOpen("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
               "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal"));
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(http.PUT("https://sync.example/syncs/progress", "{}"), 200);
  EXPECT_EQ(http.getBody(), "final");
  EXPECT_EQ(http.GET("https://sync.example/healthcheck"), 200);
  EXPECT_EQ(http.getBody(), "ok");
  EXPECT_EQ(FakeNet::connects(), 1);
  EXPECT_LT(elapsed(), kPrompt);
}

// Connection: close still closes: the next request opens a new connection.
TEST_F(BodilessReply, NoContentWithConnectionCloseCloses) {
  FakeNet::replies().push_back(keptOpen("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"));
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(http.PUT("https://sync.example/syncs/progress", "{}"), 204);
  EXPECT_EQ(http.GET("https://sync.example/syncs/progress/abc"), 200);
  EXPECT_EQ(FakeNet::connects(), 2);
}

}  // namespace
