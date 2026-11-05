#include <gtest/gtest.h>

#include "MultipartRoute.h"

namespace {

// Each test walks the handler through the calls WebServer::_parseRequest() and _handleRequest() make.
class MultipartRouteTest : public ::testing::Test {
 protected:
  void sendFile(const int pieces) {
    ASSERT_TRUE(route.canHandle(server, HTTP_POST, "/upload"));
    ASSERT_TRUE(route.canUpload(server, "/upload"));
    for (int i = 0; i < pieces; i++) route.upload(server, "/upload", upload);
  }

  WebServer server;
  HTTPUpload upload;
  HTTPRaw raw;
  int parts = 0;
  int finished = 0;
  MultipartRoute route{"/upload", [this] { finished++; }, [this] { parts++; }};
};

TEST_F(MultipartRouteTest, AnsweredOnlyForAPostToItsOwnPath) {
  EXPECT_TRUE(route.canHandle(server, HTTP_POST, "/upload"));
  EXPECT_FALSE(route.canHandle(server, HTTP_GET, "/upload"));
  EXPECT_FALSE(route.canHandle(server, HTTP_POST, "/upload/more"));
}

TEST_F(MultipartRouteTest, FileReachesBothFunctions) {
  sendFile(3);
  EXPECT_TRUE(route.handle(server, HTTP_POST, "/upload"));
  EXPECT_EQ(parts, 3);
  EXPECT_EQ(finished, 1);
  EXPECT_EQ(server.sentCode, 0);  // `finished` sends the reply
}

// The crash: these functions read server.upload(), which is null for such a body.
TEST_F(MultipartRouteTest, BodyOfAnotherKindReachesNeitherFunction) {
  ASSERT_TRUE(route.canHandle(server, HTTP_POST, "/upload"));
  route.raw(server, "/upload", raw);
  route.raw(server, "/upload", raw);
  EXPECT_TRUE(route.handle(server, HTTP_POST, "/upload"));
  EXPECT_EQ(parts, 0);
  EXPECT_EQ(finished, 0);
  EXPECT_EQ(server.sentCode, 415);
}

// Refused by canRaw(), a body would go to the framework's plain path, which holds all of it on the heap.
TEST_F(MultipartRouteTest, BodyOfAnotherKindIsTakenToBeDropped) { EXPECT_TRUE(route.canRaw(server, "/upload")); }

TEST_F(MultipartRouteTest, FormWithNoFileIsRefused) {
  sendFile(0);
  EXPECT_TRUE(route.handle(server, HTTP_POST, "/upload"));
  EXPECT_EQ(finished, 0);
  EXPECT_EQ(server.sentCode, 415);
}

// handle() never runs for a form the client cut off, so its file must not count for the next request.
TEST_F(MultipartRouteTest, CutOffFormDoesNotStandInForTheNextRequestsFile) {
  sendFile(2);
  sendFile(0);
  EXPECT_TRUE(route.handle(server, HTTP_POST, "/upload"));
  EXPECT_EQ(finished, 0);
  EXPECT_EQ(server.sentCode, 415);
}

TEST_F(MultipartRouteTest, EachFileIsReportedOnce) {
  sendFile(1);
  route.handle(server, HTTP_POST, "/upload");
  sendFile(1);
  route.handle(server, HTTP_POST, "/upload");
  EXPECT_EQ(finished, 2);
}

}  // namespace
