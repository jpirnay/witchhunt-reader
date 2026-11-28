// BleRadioPolicy: when the Bluetooth LE radio may start, when a start is rolled back, when a running radio stops.
#include <gtest/gtest.h>

#include "BleRadioPolicy.h"

namespace {

using BleRadioPolicy::afterStart;
using BleRadioPolicy::beforeStart;
using BleRadioPolicy::Heap;
using BleRadioPolicy::Keep;
using BleRadioPolicy::kIdleNoLinkMs;
using BleRadioPolicy::kS3MinimumLargest;
using BleRadioPolicy::kS3Profile;
using BleRadioPolicy::kS3StackBytes;
using BleRadioPolicy::Verdict;
using BleRadioPolicy::whileRunning;

constexpr size_t kNeeded = kS3StackBytes + kS3MinimumLargest;

TEST(BleRadioPolicy, TheMarginCoversEveryMeasuredStart) {
  // begin() at Home, internal RAM before minus after (CMD:STATE, 2026-11-28): X4 Pro, T5S3, Picco.
  for (const size_t measured : {180920u - 134312u, 126968u - 80320u, 176780u - 130212u}) {
    EXPECT_GE(kS3StackBytes, measured + 4096);
  }
}

TEST(BleRadioPolicy, AWholeHeapStarts) { EXPECT_EQ(beforeStart(Heap{kNeeded, kS3MinimumLargest}, false), Verdict::Ok); }

TEST(BleRadioPolicy, OneByteShortOfTheRadioAndItsRoomIsHeapLow) {
  EXPECT_EQ(beforeStart(Heap{kNeeded - 1, kNeeded - 1}, false), Verdict::HeapLow);
}

TEST(BleRadioPolicy, EnoughBytesWithNoWholeBlockIsHeapInPieces) {
  EXPECT_EQ(beforeStart(Heap{kNeeded + 100000, kS3MinimumLargest - 1}, false), Verdict::HeapInPieces);
}

TEST(BleRadioPolicy, UsbDriveKeepsTheRadioOffWhateverTheHeap) {
  EXPECT_EQ(beforeStart(Heap{1u << 20, 1u << 19}, true), Verdict::StorageBusy);
  EXPECT_EQ(afterStart(Heap{1u << 20, 1u << 19}, true), Verdict::StorageBusy);
}

TEST(BleRadioPolicy, TheMeasuredBoardsStartAtHome) {
  // Internal free / largest at Home with BLE linked, TinyUSB device-only build, before and after begin().
  struct Board {
    Heap before;
    Heap after;
  };
  const Board boards[] = {
      {{180920, 139252}, {134312, 94196}},  // X4 Pro
      {{126968, 81908}, {80320, 38900}},    // T5S3
      {{176780, 131060}, {130212, 90100}},  // Picco
  };
  for (const Board& b : boards) {
    EXPECT_EQ(beforeStart(b.before, false, kS3Profile), Verdict::Ok);
    EXPECT_EQ(afterStart(b.after, false, kS3Profile), Verdict::Ok);
  }
}

TEST(BleRadioPolicy, AStartThatLeavesNoWholeBlockIsRolledBack) {
  EXPECT_EQ(afterStart(Heap{60000, kS3MinimumLargest - 1}, false), Verdict::HeapInPieces);
  EXPECT_EQ(afterStart(Heap{60000, kS3MinimumLargest}, false), Verdict::Ok);
}

TEST(BleRadioPolicy, ALinkedRemoteIsNeverIdle) {
  EXPECT_EQ(whileRunning(/*linked=*/true, /*connecting=*/false, 10 * kIdleNoLinkMs, false), Keep::Keep);
}

TEST(BleRadioPolicy, AConnectInProgressIsNotIdle) {
  EXPECT_EQ(whileRunning(/*linked=*/false, /*connecting=*/true, 10 * kIdleNoLinkMs, false), Keep::Keep);
}

TEST(BleRadioPolicy, NothingLinkedStopsAtTheIdleTimeNotBefore) {
  EXPECT_EQ(whileRunning(false, false, kIdleNoLinkMs - 1, false), Keep::Keep);
  EXPECT_EQ(whileRunning(false, false, kIdleNoLinkMs, false), Keep::StopIdleNoLink);
}

TEST(BleRadioPolicy, TheIdleTimeCoversTheSdkReconnectWindow) {
  // BleKeyboardHost::armBondedReconnect() scans for 120 s after it is armed: a shorter idle stop would cut it off.
  EXPECT_GE(kIdleNoLinkMs, 120u * 1000u);
}

TEST(BleRadioPolicy, UsbDriveStopsARunningRadioEvenWithARemoteLinked) {
  EXPECT_EQ(whileRunning(/*linked=*/true, false, 0, /*storageBusy=*/true), Keep::StopStorage);
}

}  // namespace
