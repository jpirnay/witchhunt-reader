// The pending screenshot request (util/ScreenshotRequest.h): every CMD:SCREENSHOT gets one reply, so a pending host
// request is never replaced; a second one is refused (the command answers SCREENSHOT_ERROR:busy at once). A card
// screenshot asked for from another task reaches the loop task through one atomic flag.

#include <gtest/gtest.h>

#include <thread>

#include "util/ScreenshotRequest.h"

namespace {
using Target = ScreenshotRequest::Target;
}

TEST(ScreenshotRequest, ASecondHostRequestIsRefusedAndTheFirstStays) {
  ScreenshotRequest requests;
  ASSERT_TRUE(requests.request(Target::Host, 1000));
  EXPECT_FALSE(requests.request(Target::Host, 1010)) << "the second CMD:SCREENSHOT answers busy";
  EXPECT_EQ(requests.pending(), Target::Host);
  EXPECT_FALSE(requests.expired(5999)) << "the first keeps its own deadline";
  EXPECT_TRUE(requests.expired(6000));
}

TEST(ScreenshotRequest, ACardRequestDoesNotReplaceAPendingHostOne) {
  ScreenshotRequest requests;
  ASSERT_TRUE(requests.request(Target::Host, 1000));
  EXPECT_FALSE(requests.request(Target::Card, 1010));
  EXPECT_EQ(requests.pending(), Target::Host);
}

TEST(ScreenshotRequest, AHostRequestTakesThePlaceOfACardOne) {
  ScreenshotRequest requests;
  ASSERT_TRUE(requests.request(Target::Card, 1000));
  EXPECT_TRUE(requests.request(Target::Host, 1010));
  EXPECT_EQ(requests.pending(), Target::Host);
  EXPECT_FALSE(requests.expired(6000)) << "the deadline counts from the host request";
}

TEST(ScreenshotRequest, ACardRequestReplacesACardOne) {
  ScreenshotRequest requests;
  ASSERT_TRUE(requests.request(Target::Card, 1000));
  EXPECT_TRUE(requests.request(Target::Card, 2000));
  EXPECT_FALSE(requests.expired(6500));
}

TEST(ScreenshotRequest, ServedOrGivenUpItTakesTheNextRequest) {
  ScreenshotRequest requests;
  EXPECT_FALSE(requests.expired(1000)) << "nothing pending";
  ASSERT_TRUE(requests.request(Target::Host, 1000));
  requests.clear();
  EXPECT_EQ(requests.pending(), Target::None);
  EXPECT_TRUE(requests.request(Target::Host, 2000));
}

TEST(ScreenshotRequest, ACardAskedFromAnotherTaskIsTakenInOnce) {
  ScreenshotRequest requests;
  EXPECT_FALSE(requests.takeCardAsk());
  std::thread renderTask([&requests] { requests.askForCard(); });
  renderTask.join();
  EXPECT_TRUE(requests.takeCardAsk());
  EXPECT_FALSE(requests.takeCardAsk());
  EXPECT_EQ(requests.pending(), Target::None) << "the loop task turns it into a request itself";
}
