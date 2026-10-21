// Host tests: SecureHttpClient::isHttps(), the scheme test a caller asks before a request to know
// whether it will run a TLS handshake (KOSync's heap gate, a download's chunking). It is parseUrl's
// own verdict, so it cannot disagree with what request() then does.

#include <SecureHttpClient.h>
#include <gtest/gtest.h>

namespace {

using crosspoint::SecureHttpClient;

TEST(SecureHttpClientIsHttps, AnHttpsUrlIsTls) {
  EXPECT_TRUE(SecureHttpClient::isHttps("https://sync.example/users/auth"));
  EXPECT_TRUE(SecureHttpClient::isHttps("https://192.168.0.10:8443"));
}

TEST(SecureHttpClientIsHttps, TheSchemeMatchesInAnyCase) {
  EXPECT_TRUE(SecureHttpClient::isHttps("HTTPS://sync.example/"));
  EXPECT_TRUE(SecureHttpClient::isHttps("Https://sync.example/"));
}

TEST(SecureHttpClientIsHttps, PlainHttpIsNotTlsOnAnyPort) {
  EXPECT_FALSE(SecureHttpClient::isHttps("http://sync.example/users/auth"));
  EXPECT_FALSE(SecureHttpClient::isHttps("http://192.168.0.10:443/"));
}

// A URL the client would refuse (ERR_BAD_URL) never reaches a handshake either.
TEST(SecureHttpClientIsHttps, AUrlTheClientCannotParseIsNotTls) {
  EXPECT_FALSE(SecureHttpClient::isHttps(""));
  EXPECT_FALSE(SecureHttpClient::isHttps("sync.example"));
  EXPECT_FALSE(SecureHttpClient::isHttps("https:/sync.example"));
  EXPECT_FALSE(SecureHttpClient::isHttps("https://"));
  EXPECT_FALSE(SecureHttpClient::isHttps("ftp://sync.example/"));
}

}  // namespace
