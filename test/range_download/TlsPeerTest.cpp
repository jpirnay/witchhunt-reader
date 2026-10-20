// Host tests: SecureHttpClient::peerOf(), the host:port that HttpDownloader's session memo of
// unverifiable servers and its "earlier this session" log line take from a request's URL. It parses
// the URL as the client does for the connection, so it names the server as SecureClient's fallback
// line does (crosspoint::tlsPeer()). Built here because this target already builds SecureHttpClient.

#include <SecureHttpClient.h>
#include <gtest/gtest.h>

using crosspoint::SecureHttpClient;

// A redirect's Location comes back with the port spelled out (resolveRedirect()), so a URL with and
// without its default port must name one server, or the memo would miss it.
TEST(TlsPeer, ADefaultPortIsSpelledOut) {
  EXPECT_EQ(SecureHttpClient::peerOf("https://www.gutenberg.org/ebooks.opds/"), "www.gutenberg.org:443");
  EXPECT_EQ(SecureHttpClient::peerOf("https://www.gutenberg.org:443/ebooks/search.opds/?query=kafka"),
            "www.gutenberg.org:443");
  EXPECT_EQ(SecureHttpClient::peerOf("http://m.gutenberg.org/ebooks/search.opds/?query=kafka"), "m.gutenberg.org:80");
  EXPECT_EQ(SecureHttpClient::peerOf("https://opds.example"), "opds.example:443");
}

TEST(TlsPeer, AnExplicitPortIsKept) {
  EXPECT_EQ(SecureHttpClient::peerOf("https://192.168.0.113:8443/opds"), "192.168.0.113:8443");
}

TEST(TlsPeer, AnUnparseableUrlNamesNoServer) {
  EXPECT_EQ(SecureHttpClient::peerOf("opds.example/feed"), "");
  EXPECT_EQ(SecureHttpClient::peerOf("ftp://opds.example/feed"), "");
}
