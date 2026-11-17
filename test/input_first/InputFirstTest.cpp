// InputFirst: the reader's background work starts no unit while a press is on its way to becoming a turn.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <cstdint>

#include "activities/reader/InputFirst.h"

namespace {

using InputFirst::mayRunSlice;
using InputFirst::Press;
using InputFirst::Work;

Press press(const bool down, const bool gestureOpen, const bool turnWaiting) {
  Press p;
  p.down = down;
  p.gestureOpen = gestureOpen;
  p.turnWaiting = turnWaiting;
  return p;
}

TEST(InputFirst, LookAheadRunsOnlyWithNoPressInFlight) {
  for (int bits = 0; bits < 8; ++bits) {
    const Press p = press(bits & 1, bits & 2, bits & 4);
    EXPECT_EQ(mayRunSlice(Work::LookAhead, p), bits == 0)
        << "down=" << p.down << " gestureOpen=" << p.gestureOpen << " turnWaiting=" << p.turnWaiting;
  }
}

// The page a press asks for may be one the chapter's build has not laid out yet: the build goes on through the press.
TEST(InputFirst, TheChapterTheReaderWaitsOnStopsOnlyForATurnThatWaitsForTheLock) {
  for (int bits = 0; bits < 8; ++bits) {
    const Press p = press(bits & 1, bits & 2, bits & 4);
    EXPECT_EQ(mayRunSlice(Work::ChapterForPage, p), !p.turnWaiting)
        << "down=" << p.down << " gestureOpen=" << p.gestureOpen << " turnWaiting=" << p.turnWaiting;
  }
}

// Once the page asked for is drawn, the build lays out the pages after it: a press holds it as it holds the look-ahead.
TEST(InputFirst, TheChapterBehindItsPageRunsOnlyWithNoPressInFlight) {
  for (int bits = 0; bits < 8; ++bits) {
    const Press p = press(bits & 1, bits & 2, bits & 4);
    EXPECT_EQ(mayRunSlice(Work::ChapterBehindPage, p), bits == 0)
        << "down=" << p.down << " gestureOpen=" << p.gestureOpen << " turnWaiting=" << p.turnWaiting;
  }
}

TEST(InputFirst, AStepOfTheChapterIsForThePageUntilThatPageIsDrawn) {
  EXPECT_EQ(InputFirst::chapterWork(false), Work::ChapterForPage);
  EXPECT_EQ(InputFirst::chapterWork(true), Work::ChapterBehindPage);
}

// A build in slices of 40 ms, as Section::stepSectionBuild() runs one: chunks of work until the budget is used up.
class SlicedBuild {
 public:
  explicit SlicedBuild(const int chunks) : chunksLeft_(chunks) {}

  bool done() const { return chunksLeft_ == 0; }
  int slices() const { return slices_; }

  void step(const uint32_t budgetMs) {
    const unsigned long start = millis();
    ++slices_;
    while (chunksLeft_ > 0) {
      --chunksLeft_;
      if (millis() - start >= budgetMs) return;
    }
  }

 private:
  int chunksLeft_;
  int slices_ = 0;
};

constexpr uint32_t kBudgetMs = 40;

// One pass of the reader's loop that found no input event: the background work's turn, under the gate.
void loopPass(SlicedBuild& build, const Work work, const Press& p) {
  if (!build.done() && mayRunSlice(work, p)) build.step(kBudgetMs);
}

TEST(InputFirst, ALookAheadBuildMakesNoStepWhileAPressIsOpen) {
  const host_clock::Ticking tick(1);  // or one slice finishes the build and nothing is sliced
  SlicedBuild build(400);
  for (int pass = 0; pass < 20; ++pass) loopPass(build, Work::LookAhead, press(true, false, false));  // button down
  for (int pass = 0; pass < 20; ++pass) loopPass(build, Work::LookAhead, press(false, true, false));  // released
  for (int pass = 0; pass < 20; ++pass) loopPass(build, Work::LookAhead, press(false, false, true));  // its turn waits
  EXPECT_EQ(build.slices(), 0);
  EXPECT_FALSE(build.done());
}

TEST(InputFirst, ALookAheadBuildRunsInSlicesOnceThePressIsOver) {
  const host_clock::Ticking tick(1);
  SlicedBuild build(400);
  for (int pass = 0; pass < 20; ++pass) loopPass(build, Work::LookAhead, press(true, true, false));
  ASSERT_EQ(build.slices(), 0);
  for (int pass = 0; pass < 200 && !build.done(); ++pass) loopPass(build, Work::LookAhead, Press{});
  EXPECT_TRUE(build.done());
  EXPECT_GT(build.slices(), 1);  // sliced: the clock ticks, so a slice ends at its budget
}

TEST(InputFirst, TheChapterKeepsBuildingThroughAPressWhileItsPageIsNotDrawn) {
  const host_clock::Ticking tick(1);
  SlicedBuild build(400);
  for (int pass = 0; pass < 200 && !build.done(); ++pass)
    loopPass(build, InputFirst::chapterWork(false), press(true, true, false));
  EXPECT_TRUE(build.done());
  EXPECT_GT(build.slices(), 1);
}

// A press on a page that is up: the passes it takes to become a turn carry no slice, and the build goes on after it.
TEST(InputFirst, TheChapterMakesNoStepThroughAPressOnceItsPageIsDrawn) {
  const host_clock::Ticking tick(1);
  SlicedBuild build(400);
  const Work work = InputFirst::chapterWork(true);
  loopPass(build, work, Press{});  // the page is up, nothing pressed: the build runs
  ASSERT_EQ(build.slices(), 1);
  for (int pass = 0; pass < 20; ++pass) loopPass(build, work, press(true, false, false));  // an edge not taken in yet
  for (int pass = 0; pass < 20; ++pass) loopPass(build, work, press(false, true, false));  // held, then released
  for (int pass = 0; pass < 20; ++pass) loopPass(build, work, press(false, false, true));  // its turn waits
  EXPECT_EQ(build.slices(), 1);
  EXPECT_FALSE(build.done());
  // The turn asked for a page the build has not drawn: the reader waits on the build again, whatever the keys do.
  for (int pass = 0; pass < 200 && !build.done(); ++pass)
    loopPass(build, InputFirst::chapterWork(false), press(true, true, false));
  EXPECT_TRUE(build.done());
  EXPECT_GT(build.slices(), 2);
}

}  // namespace
