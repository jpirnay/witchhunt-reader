// A Bluetooth LE remote's keys as Page Forward / Page Back presses (src/BleRemoteKeys.h).

#include <gtest/gtest.h>

#include "BleRemoteKeys.h"

using BleRemoteKeys::Edge;
using BleRemoteKeys::Press;
using BleRemoteKeys::Router;
using BleRemoteKeys::Target;

namespace {

Edge press(const uint32_t code, const uint32_t atMs, const uint8_t keycode) {
  return Edge{code, true, false, atMs, keycode};
}
Edge release(const uint32_t code, const uint32_t atMs, const bool wasRest = false) {
  return Edge{code, false, wasRest, atMs, 0};
}

}  // namespace

// The keys CrossPoint's remotes turn pages with (lib/BlePageTurner, pageActionFor): Enter, Backspace and the consumer
// track keys too. The volume pair is the other way round there.
TEST(BleRemoteKeys, KeyboardAndConsumerKeysTurnPages) {
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x4F), Target::PageForward);  // Right Arrow
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x51), Target::PageForward);  // Down Arrow
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x4E), Target::PageForward);  // Page Down
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x2C), Target::PageForward);  // Space
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x28), Target::PageForward);  // Enter
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0xEA), Target::PageForward);  // Volume Down
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0xB5), Target::PageForward);  // Scan Next Track
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x50), Target::PageBack);     // Left Arrow
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x52), Target::PageBack);     // Up Arrow
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x4B), Target::PageBack);     // Page Up
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x2A), Target::PageBack);     // Backspace
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0xE9), Target::PageBack);     // Volume Up
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0xB6), Target::PageBack);     // Scan Previous Track
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x04), Target::None);         // 'a'
  EXPECT_EQ(BleRemoteKeys::targetForUsage(0x00), Target::None);
}

// The Free2 (Kobo/PC firmware, left/right mode) as measured on an X4 Pro: report 1, byte 2, release 56-90 ms after
// the press.
TEST(BleRemoteKeys, Free2TapIsOnePressAtItsRelease) {
  Router router;
  router.setRemote("Free2-M");
  Press out{};
  EXPECT_FALSE(router.feed(press(0x010250, 1984623, 0x50), out));
  ASSERT_TRUE(router.feed(release(0x010250, 1984713), out));
  EXPECT_EQ(out.target, Target::PageBack);
  EXPECT_FALSE(out.hold);
  EXPECT_EQ(out.atMs, 1984713u);
  EXPECT_FALSE(router.feed(press(0x01024F, 1986363, 0x4F), out));
  ASSERT_TRUE(router.feed(release(0x01024F, 1986423), out));
  EXPECT_EQ(out.target, Target::PageForward);
}

TEST(BleRemoteKeys, HoldFromTheHardwareButtonsThreshold) {
  Router router;
  Press out{};
  router.feed(press(0x01024F, 1000, 0x4F), out);
  ASSERT_TRUE(router.feed(release(0x01024F, 1000 + BleRemoteKeys::kHoldMs - 1), out));
  EXPECT_FALSE(out.hold);
  router.feed(press(0x01024F, 5000, 0x4F), out);
  ASSERT_TRUE(router.feed(release(0x01024F, 5000 + BleRemoteKeys::kHoldMs), out));
  EXPECT_TRUE(out.hold);
  router.feed(press(0x01024F, 9000, 0x4F), out);
  ASSERT_TRUE(router.feed(release(0x01024F, 10200), out));
  EXPECT_TRUE(out.hold);
  EXPECT_EQ(out.atMs, 10200u);
}

TEST(BleRemoteKeys, NothingForAStatusByteAStrayReleaseOrAnUnknownKey) {
  Router router;
  Press out{};
  router.feed(press(0x010250, 100, 0x50), out);
  EXPECT_FALSE(router.feed(release(0x010250, 160, /*wasRest=*/true), out));
  EXPECT_FALSE(router.feed(release(0x010250, 200), out));  // that press is over
  EXPECT_FALSE(router.feed(release(0x010251, 300), out));  // never pressed
  router.feed(press(0x010204, 400, 0x04), out);            // 'a'
  EXPECT_FALSE(router.feed(release(0x010204, 460), out));
}

TEST(BleRemoteKeys, ASecondPressTakesOver) {
  Router router;
  Press out{};
  router.feed(press(0x010250, 100, 0x50), out);
  router.feed(press(0x01024F, 150, 0x4F), out);
  EXPECT_FALSE(router.feed(release(0x010250, 200), out));
  ASSERT_TRUE(router.feed(release(0x01024F, 210), out));
  EXPECT_EQ(out.target, Target::PageForward);
}

// The SDK's profiles: the Free3's third button by its raw code under any report id, the BOOX Keypage's codes on
// report 3 only; anything else falls back to the decoded key. The Free3's third button is its chapter key (CrossPoint
// turns chapters on it): a hold of Page Forward on a tap, of Page Back on its own hold code, however short the press.
TEST(BleRemoteKeys, ProfilesDecideByRawCode) {
  Router router;
  Press out{};
  router.setRemote("Free3-R");
  router.feed(press(0x050102, 100, 0), out);
  ASSERT_TRUE(router.feed(release(0x050102, 150), out));
  EXPECT_EQ(out.target, Target::PageForward);
  EXPECT_TRUE(out.hold);
  router.feed(press(0x000008, 200, 0), out);
  ASSERT_TRUE(router.feed(release(0x000008, 250), out));
  EXPECT_EQ(out.target, Target::PageBack);
  EXPECT_TRUE(out.hold);
  router.feed(press(0x01024F, 300, 0x4F), out);  // its page buttons: the keyboard decode
  ASSERT_TRUE(router.feed(release(0x01024F, 360), out));
  EXPECT_EQ(out.target, Target::PageForward);
  EXPECT_FALSE(out.hold);

  router.setRemote("BOOX Keypage");
  router.feed(press(0x030002, 400, 0), out);
  ASSERT_TRUE(router.feed(release(0x030002, 450), out));
  EXPECT_EQ(out.target, Target::PageForward);
  EXPECT_FALSE(out.hold);
  router.feed(press(0x030008, 500, 0), out);
  ASSERT_TRUE(router.feed(release(0x030008, 550), out));
  EXPECT_EQ(out.target, Target::PageBack);
  EXPECT_FALSE(out.hold);
  router.feed(press(0x020002, 600, 0), out);  // another report id: not the profile's
  EXPECT_FALSE(router.feed(release(0x020002, 650), out));
}

TEST(BleRemoteKeys, ANewRemoteForgetsAPressInFlight) {
  Router router;
  Press out{};
  router.setRemote("Free2-M");
  router.feed(press(0x010250, 100, 0x50), out);
  router.setRemote(nullptr);
  EXPECT_FALSE(router.feed(release(0x010250, 160), out));
}
