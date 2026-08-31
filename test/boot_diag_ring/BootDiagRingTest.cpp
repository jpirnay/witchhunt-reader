// Tests for the pure half of the boot/sleep diagnostics (src/BootDiagRing.h).
//
// Every defect this feature shipped was in exactly this logic, and every one of them was
// found by flashing a device and reading the screen:
//
//   * a sleep record's stage read as a verdict while it was still provisional, so a healthy
//     X4 sleep reported "never reached deep sleep" (RunScan/OutcomeOf below);
//   * a reset reason trusted to separate a power-on from a reset press, which the C3 cannot
//     express (OutcomeOf, which no longer consults it at all);
//   * a newest-first scan that broke on its own boot record, so the aborted-boot counter
//     read zero for every device that had actually been looping (RunScan).
//
// None of that needs hardware. This file depends on nothing but the header under test.

#include <gtest/gtest.h>

#include <vector>

#include "BootDiagRing.h"

namespace {

using namespace BootDiag;

Record sleepRecord(SleepStage stage, uint8_t flags) {
  Record r{};
  r.kind = KindSleep;
  r.code = static_cast<uint8_t>(stage);
  r.flags = flags;
  return r;
}

Record bootRecord() {
  Record r{};
  r.kind = KindBoot;
  return r;
}

Record abortRecord(SleepTrigger trigger, uint8_t verdict, uint16_t count) {
  Record r{};
  r.kind = KindAborted;
  r.reason = static_cast<uint8_t>(trigger);
  r.code = verdict;
  r.msA = count;
  return r;
}

// ---------------------------------------------------------------------------
// Ring layout
// ---------------------------------------------------------------------------

TEST(BootDiagRing, InitProducesAValidEmptyRing) {
  std::vector<uint8_t> image(kImageBytes);
  ringInit(image.data());

  EXPECT_TRUE(ringValid(image.data()));
  const RingHeader header = ringHeaderOf(image.data());
  EXPECT_EQ(header.count, 0);
  EXPECT_EQ(header.head, 0);
  EXPECT_EQ(ringNewestSlot(header), kCapacity) << "an empty ring has no newest slot";
}

TEST(BootDiagRing, RejectsAForeignOrStaleImage) {
  std::vector<uint8_t> image(kImageBytes, 0);
  EXPECT_FALSE(ringValid(image.data())) << "all-zero is not a ring";

  ringInit(image.data());
  RingHeader header = ringHeaderOf(image.data());
  header.version = kRingVersion - 1;
  ringSetHeader(image.data(), header);
  // The version bump is load-bearing: v1 sleep records are indistinguishable from an
  // artefact, so they must be discarded rather than rendered.
  EXPECT_FALSE(ringValid(image.data()));
}

TEST(BootDiagRing, AppendStampsSequenceAndReadsBackNewestFirst) {
  std::vector<uint8_t> image(kImageBytes);
  ringInit(image.data());

  Record a = sleepRecord(SleepStage::WakeArmed, 0);
  Record b = bootRecord();
  ringAppend(image.data(), a);
  ringAppend(image.data(), b);

  EXPECT_LT(a.seq, b.seq) << "sequence numbers must increase with append order";

  Record out[kCapacity] = {};
  ASSERT_EQ(ringLoadNewestFirst(image.data(), out, kCapacity), 2);
  EXPECT_EQ(out[0].kind, KindBoot) << "newest first";
  EXPECT_EQ(out[1].kind, KindSleep);
  EXPECT_EQ(out[0].seq, b.seq);
}

TEST(BootDiagRing, WrapsAndDropsTheOldest) {
  std::vector<uint8_t> image(kImageBytes);
  ringInit(image.data());

  // One more than the ring holds, so the first is overwritten.
  for (int i = 0; i < kCapacity + 1; i++) {
    Record r = bootRecord();
    r.msB = static_cast<uint16_t>(i);
    ringAppend(image.data(), r);
  }

  const RingHeader header = ringHeaderOf(image.data());
  EXPECT_EQ(header.count, kCapacity);
  EXPECT_EQ(header.head, 1) << "head walks forward once the ring is full";

  Record out[kCapacity] = {};
  ASSERT_EQ(ringLoadNewestFirst(image.data(), out, kCapacity), kCapacity);
  EXPECT_EQ(out[0].msB, kCapacity) << "newest is the last appended";
  EXPECT_EQ(out[kCapacity - 1].msB, 1) << "entry 0 was dropped, so the oldest kept is 1";
  for (uint8_t i = 1; i < kCapacity; i++) {
    EXPECT_LT(out[i].seq, out[i - 1].seq) << "newest-first order must be strictly descending at " << int(i);
  }
}

TEST(BootDiagRing, LoadHonoursACallerCapSmallerThanTheRing) {
  std::vector<uint8_t> image(kImageBytes);
  ringInit(image.data());
  for (int i = 0; i < 5; i++) {
    Record r = bootRecord();
    ringAppend(image.data(), r);
  }
  Record out[2] = {};
  EXPECT_EQ(ringLoadNewestFirst(image.data(), out, 2), 2);
}

// ---------------------------------------------------------------------------
// Record semantics
// ---------------------------------------------------------------------------

TEST(OutcomeOf, InProgressIsNeverAVerdict) {
  // The bug that made every healthy X4 sleep report "never reached deep sleep": the record
  // is written at PanelAsleep, and until the next boot finalises it that stage says nothing
  // about whether the device actually slept.
  const Record r = sleepRecord(SleepStage::PanelAsleep, kFlagInProgress);
  EXPECT_EQ(outcomeOf(r), SleepOutcome::Unfinished);
}

TEST(OutcomeOf, WakeArmedIsAMeasuredSuccess) {
  const Record r = sleepRecord(SleepStage::WakeArmed, kFlagStageFromRtc);
  EXPECT_EQ(outcomeOf(r), SleepOutcome::ReachedDeepSleep);
}

TEST(OutcomeOf, ReleaseTimeoutOutranksEverythingElse) {
  // The only failure verdict that survives a rail cut, so it must win even when the stage
  // was later inferred to be a completed sleep.
  const Record r = sleepRecord(SleepStage::WakeArmed, kFlagReleaseTimeout | kFlagStageInferred);
  EXPECT_EQ(outcomeOf(r), SleepOutcome::ReleaseTimedOut);
}

TEST(OutcomeOf, InferredOutcomeIsQualifiedByTheSleepPolicy) {
  // With no breadcrumb all that is known is that the rail went away. Whether that is right
  // depends on what the sleep asked for — NOT on the reset reason, which on the C3 cannot
  // separate a power-on from a reset press.
  const Record latchCut = sleepRecord(SleepStage::PanelAsleep, kFlagStageInferred);
  EXPECT_EQ(outcomeOf(latchCut), SleepOutcome::PoweredOffAsAsked);

  const Record latchKept = sleepRecord(SleepStage::PanelAsleep, kFlagStageInferred | kFlagKeepClock);
  EXPECT_EQ(outcomeOf(latchKept), SleepOutcome::PoweredOffUnasked);
}

TEST(OutcomeOf, AStageShortOfWakeArmedIsAFailure) {
  const Record r = sleepRecord(SleepStage::AwaitingRelease, 0);
  EXPECT_EQ(outcomeOf(r), SleepOutcome::DidNotSleep);
}

TEST(OutcomeOf, NonSleepRecordsAreNotSleepVerdicts) { EXPECT_EQ(outcomeOf(bootRecord()), SleepOutcome::DidNotSleep); }

// ---------------------------------------------------------------------------
// Current-run scan
// ---------------------------------------------------------------------------

TEST(RunScan, StepsOverOurOwnBootRecord) {
  // The exact shape persistBoot() leaves behind, newest first: our own boot record, then the
  // abort summary it just drained, then the sleep before it. Breaking on slot 0 made this
  // return 0 for every device that had been looping.
  const Record records[] = {
      bootRecord(),
      abortRecord(SleepTrigger::WakeGateRejected, 4, 7),
      sleepRecord(SleepStage::WakeArmed, 0),
      bootRecord(),
  };
  EXPECT_EQ(abortsInCurrentRun(records, 4), 7);
}

TEST(RunScan, SumsEveryReasonInTheRun) {
  // persistBoot() emits one record per distinct reason, so the total is a sum.
  const Record records[] = {
      bootRecord(),
      abortRecord(SleepTrigger::WakeGateRejected, 4, 3),
      abortRecord(SleepTrigger::UsbPowerBoot, 0, 2),
      bootRecord(),
  };
  EXPECT_EQ(abortsInCurrentRun(records, 4), 5);
}

TEST(RunScan, IgnoresAbortsBelongingToAnEarlierRun) {
  const Record records[] = {
      bootRecord(),
      sleepRecord(SleepStage::WakeArmed, 0),
      bootRecord(),                                       // previous completed boot — stop here
      abortRecord(SleepTrigger::WakeGateRejected, 4, 9),  // an earlier run's aborts
  };
  EXPECT_EQ(abortsInCurrentRun(records, 4), 0);
}

TEST(RunScan, ReportsNoneWhenTheRunHadNoAborts) {
  const Record records[] = {
      bootRecord(),
      sleepRecord(SleepStage::WakeArmed, 0),
      bootRecord(),
  };
  EXPECT_EQ(abortsInCurrentRun(records, 3), 0);
}

TEST(RunScan, HandlesAFreshCardWithNoPreviousBoot) {
  // First ever boot: one record, no run to compare against.
  const Record records[] = {bootRecord()};
  EXPECT_EQ(abortsInCurrentRun(records, 1), 0);
  EXPECT_EQ(abortsInCurrentRun(records, 0), 0) << "an empty list must not walk off the front";
}

TEST(RunScan, RangeExcludesBothBoundingBootRecords) {
  const Record records[] = {
      bootRecord(),
      abortRecord(SleepTrigger::WakeGateRejected, 4, 1),
      sleepRecord(SleepStage::WakeArmed, 0),
      bootRecord(),
  };
  uint8_t first = 0;
  uint8_t last = 0;
  ASSERT_TRUE(currentRunRange(records, 4, first, last));
  EXPECT_EQ(first, 1) << "starts after our own boot record";
  EXPECT_EQ(last, 3) << "stops at the previous completed boot";
}

}  // namespace
