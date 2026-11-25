// ToggleNoticeGate::step: when the message a toggle action asked for ("Dark Mode: On") goes on the panel, waits, or is
// given up.
#include <gtest/gtest.h>

#include "util/ToggleNoticeGate.h"

namespace {

using ToggleNoticeGate::Asked;
using ToggleNoticeGate::Now;
using ToggleNoticeGate::Step;
using ToggleNoticeGate::step;

Asked asked(const bool afterFrame) {
  Asked a;
  a.atMs = 1000;
  a.activityChanges = 7;
  a.renderPasses = 20;
  a.afterFrame = afterFrame;
  return a;
}

Now now(const unsigned long ms, const uint32_t renderPasses = 20, const bool busy = false,
        const uint32_t activityChanges = 7) {
  Now n;
  n.ms = ms;
  n.activityChanges = activityChanges;
  n.renderPasses = renderPasses;
  n.busy = busy;
  return n;
}

// Touch Lock, the reading light: nothing on the screen changes, so the message goes up as soon as the screen is idle.
TEST(ToggleNoticeGate, PaintsOnAnIdleScreen) { EXPECT_EQ(step(asked(false), now(1000)), Step::Paint); }

TEST(ToggleNoticeGate, WaitsWhileTheScreenIsBusy) {
  EXPECT_EQ(step(asked(false), now(1200, 20, /*busy=*/true)), Step::Wait);
  EXPECT_EQ(step(asked(false), now(1400, 20, /*busy=*/false)), Step::Paint);
}

// Dark Mode, a page laid out again: the frame the switch asked for would draw over a message painted before it.
TEST(ToggleNoticeGate, WaitsForTheFrameTheSwitchAskedFor) {
  EXPECT_EQ(step(asked(true), now(1100, /*renderPasses=*/20)), Step::Wait);
  EXPECT_EQ(step(asked(true), now(1300, 21, /*busy=*/true)), Step::Wait);
  EXPECT_EQ(step(asked(true), now(1500, 21, /*busy=*/false)), Step::Paint);
}

// The message was about the screen it was asked on.
TEST(ToggleNoticeGate, IsGivenUpOnAnotherScreen) {
  EXPECT_EQ(step(asked(false), now(1100, 20, false, /*activityChanges=*/8)), Step::Drop);
}

TEST(ToggleNoticeGate, IsGivenUpAfterWaitingTooLong) {
  const unsigned long late = 1000 + ToggleNoticeGate::kMaxWaitMs + 1;
  EXPECT_EQ(step(asked(true), now(late, 20)), Step::Drop);
  EXPECT_EQ(step(asked(false), now(late, 20, /*busy=*/true)), Step::Drop);
}

// millis() runs over after 49 days: the wait is measured across it.
TEST(ToggleNoticeGate, MeasuresTheWaitAcrossTheClockWrapping) {
  Asked a = asked(false);
  a.atMs = static_cast<unsigned long>(-500);
  EXPECT_EQ(step(a, now(100)), Step::Paint);
}

}  // namespace
