// Host tests: a name that does not resolve fails at once, with no connection attempted. On the device
// a failed lookup took 9 ms, but connecting anyway then waited out 3 s on 0.0.0.0, once per TLS
// attempt. Over the scripted network of RangeDownloadTest (stubs/WiFiClient.h).

#include <SecureHttpClient.h>
#include <WiFiClient.h>
#include <gtest/gtest.h>

namespace {

struct DeadHost : ::testing::Test {
  crosspoint::SecureHttpClient http;

  void SetUp() override {
    FakeNet::reset();
    http.setTimeout(5000);
  }

  int get(const char* url) {
    return http.get(url, [](const uint8_t*, size_t) { return true; });
  }
};

TEST_F(DeadHost, FailsWithoutAConnectAttempt) {
  FakeNet::unresolvable().insert("dead.invalid");
  EXPECT_LT(get("http://dead.invalid/x"), 0);
  EXPECT_EQ(FakeNet::connects(), 0);
}

TEST_F(DeadHost, AHostThatResolvesIsNotAffected) {
  FakeNet::unresolvable().insert("dead.invalid");
  FakeNet::replies().push_back(fakeOk());
  EXPECT_EQ(get("http://alive.example/x"), 200);
  EXPECT_EQ(FakeNet::connects(), 1);
}

}  // namespace
