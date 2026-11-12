// CMD:BTN:<name>:<ms>'s held presses (util/ButtonPressQueue.h) played against a model of the SDK's
// InputManager debounce: a raw change commits once it has been stable for more than 5 ms at an
// update. Each pass of the model loop runs one input update with the buttons held so far, OR'd with
// any real key down (the board-button hook ORs them the same way), then one step().
// Ported with the queue from the CrossPoint port's serial test API (feat/serial-test-api).

#include <gtest/gtest.h>

#include "util/ButtonPressQueue.h"
#include "util/IdleGate.h"

namespace {

constexpr uint8_t BACK = 0;
constexpr uint8_t CONFIRM = 1;
constexpr uint8_t DOWN = 5;
constexpr uint16_t SHORT_HOLD_MS = 60;
constexpr uint16_t LONG_HOLD_MS = 1100;

struct InputModel {
  uint8_t lastState = 0;
  uint8_t currentState = 0;
  uint32_t lastDebounceTime = 0;
  uint8_t pressedEvents = 0;
  uint8_t releasedEvents = 0;
  uint32_t pressStart = 0;
  uint32_t pressFinish = 0;
  bool ignoresHook = false;  // a board whose input path never reads the hook

  void update(const uint32_t now, uint8_t hook, const uint8_t realKeys = 0) {
    if (ignoresHook) hook = 0;
    const uint8_t raw = hook | realKeys;
    pressedEvents = 0;
    releasedEvents = 0;
    if (raw != lastState) {
      lastDebounceTime = now;
      lastState = raw;
    }
    if (now - lastDebounceTime > 5 && raw != currentState) {
      pressedEvents = raw & ~currentState;
      releasedEvents = currentState & ~raw;
      if (pressedEvents && currentState == 0) pressStart = now;
      if (releasedEvents && raw == 0) pressFinish = now;
      currentState = raw;
    }
  }
};

struct Loop {
  ButtonPressQueue queue;
  InputModel input;
  uint32_t now = 1000;
  uint32_t passMs = 10;
  uint8_t held = 0;
  uint8_t realKeys = 0;  // keys a finger holds down meanwhile
  int presses[8] = {};
  int releases[8] = {};
  uint32_t lastHeldMs = 0;

  void pass() {
    now += passMs;
    input.update(now, held, realKeys);
    for (int b = 0; b < 8; ++b) {
      if (input.pressedEvents & (1u << b)) presses[b]++;
      if (input.releasedEvents & (1u << b)) {
        releases[b]++;
        lastHeldMs = input.pressFinish - input.pressStart;
      }
    }
    held = queue.step(now, input.currentState);
  }

  // Passes until the input layer has committed a press of `button`; false if it never did. A press
  // commits within a few passes, so the bound only keeps a broken queue from hanging the test.
  bool runUntilPressed(const uint8_t button, const int maxPasses = 200) {
    for (int i = 0; i < maxPasses && presses[button] == 0; ++i) pass();
    return presses[button] > 0;
  }

  // Passes until the queue is done; false if it never was.
  bool runUntilIdle(const int maxPasses = 2000) {
    for (int i = 0; i < maxPasses; ++i) {
      pass();
      if (queue.idle()) return true;
    }
    return false;
  }
};

}  // namespace

TEST(ButtonPressQueue, AShortPressIsOneCommittedPressAndRelease) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  EXPECT_FALSE(loop.queue.idle());
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_EQ(loop.releases[CONFIRM], 1);
  EXPECT_GE(loop.lastHeldMs, SHORT_HOLD_MS);
  EXPECT_LT(loop.lastHeldMs, 200u) << "well below the shortest long press (350 ms)";
  EXPECT_EQ(loop.held, 0);
}

TEST(ButtonPressQueue, ALongPressIsHeldPastTheOneSecondThreshold) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(BACK, LONG_HOLD_MS));
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[BACK], 1);
  EXPECT_GT(loop.lastHeldMs, 1000u);
  EXPECT_LT(loop.lastHeldMs, 1500u) << "short of the 1.5 s holds";
}

// The point of the held form: for the whole hold the input layer reports the button down, pass after
// pass, as it does for a finger on the key.
TEST(ButtonPressQueue, TheButtonReadsDownForTheWholeHold) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(DOWN, 2000));
  ASSERT_TRUE(loop.runUntilPressed(DOWN)) << "the press was never committed";
  const uint32_t pressedAt = loop.now;
  while (loop.now - pressedAt < 1990) {
    loop.pass();
    ASSERT_TRUE(loop.input.currentState & (1u << DOWN)) << (loop.now - pressedAt) << " ms into the hold";
  }
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.releases[DOWN], 1);
  EXPECT_GE(loop.lastHeldMs, 2000u);
  EXPECT_LT(loop.lastHeldMs, 2050u);
}

// Idle passes run up to 50 ms apart; a press must still be committed, never lost between samples.
TEST(ButtonPressQueue, SlowPassesStillCommitThePress) {
  Loop loop;
  loop.passMs = 50;
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_EQ(loop.releases[CONFIRM], 1);
}

// Fast passes: the commit needs more than 5 ms, whatever the number of samples.
TEST(ButtonPressQueue, FastPassesWaitForTheDebounce) {
  Loop loop;
  loop.passMs = 1;
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_GE(loop.lastHeldMs, SHORT_HOLD_MS);
}

// Two presses of one button, each released and committed before the next.
TEST(ButtonPressQueue, TwoPressesOfOneButtonStayTwoPresses) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  const uint32_t start = loop.now;
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 2);
  EXPECT_EQ(loop.releases[CONFIRM], 2);
  EXPECT_LT(loop.now - start, 300u) << "inside a double-press window";
}

TEST(ButtonPressQueue, PressesPlayInOrder) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(BACK, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilPressed(BACK)) << "the press was never committed";
  EXPECT_EQ(loop.presses[CONFIRM], 0);
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
}

TEST(ButtonPressQueue, AFullQueueRefuses) {
  ButtonPressQueue queue;
  for (size_t i = 0; i < ButtonPressQueue::CAPACITY; ++i) EXPECT_TRUE(queue.push(BACK, 60));
  EXPECT_EQ(queue.freeSlots(), 0u);
  EXPECT_FALSE(queue.push(BACK, 60));
  EXPECT_FALSE(ButtonPressQueue().push(8, 60)) << "no such button bit";
}

// A board whose input path never sees the hook: the press is given up, not held forever.
TEST(ButtonPressQueue, AnUncommittedPressIsGivenUp) {
  Loop loop;
  loop.input.ignoresHook = true;
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.queue.push(BACK, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.queue.droppedPresses(), 2u);
  EXPECT_EQ(loop.held, 0);
}

// What the host sees for that press: BTN_OK, then an IDLE. SerialTestApi::serviceIdle() samples the
// queue so (effectHandled = idle(), droppedPresses = droppedPresses()); the IDLE must fail, not answer
// IDLE:<ms> as if the press had gone through.
TEST(ButtonPressQueue, AnUncommittedPressFailsTheIdleThatFollowsIt) {
  Loop loop;
  loop.input.ignoresHook = true;
  IdleGate gate;
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  gate.noteEffect();  // BTN_OK
  ASSERT_TRUE(gate.begin(loop.now, 10000));
  IdleGate::Verdict verdict = IdleGate::Verdict::Waiting;
  for (int i = 0; i < 2000 && verdict == IdleGate::Verdict::Waiting; ++i) {
    loop.pass();
    IdleGate::Sample sample;
    sample.nowMs = loop.now;
    sample.effectHandled = loop.queue.idle();
    sample.droppedPresses = loop.queue.droppedPresses();
    verdict = gate.step(sample);
  }
  EXPECT_EQ(loop.queue.droppedPresses(), 1u);
  EXPECT_EQ(verdict, IdleGate::Verdict::PressDropped);
}

// One loop pass blocked for seconds (a command waiting for the render lock) right after the press
// started: the input layer commits it on the next update, so it must not be given up meanwhile.
TEST(ButtonPressQueue, AStalledPassDoesNotDropThePress) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  loop.pass();  // the queue starts holding the button
  // The next pass: one input update sees the button (its debounce starts), then the pass stalls
  // before step() runs.
  loop.now += 10;
  loop.input.update(loop.now, loop.held);
  loop.now += 3000;
  loop.held = loop.queue.step(loop.now, loop.input.currentState);
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_EQ(loop.queue.droppedPresses(), 0u);
}

TEST(ButtonPressQueue, NotIdleUntilTheReleaseIsCommitted) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilPressed(CONFIRM)) << "the press was never committed";
  for (int i = 0; i < 200 && loop.held != 0; ++i) loop.pass();
  ASSERT_EQ(loop.held, 0) << "the queue never released the press";
  // Released by the queue, but the input layer still reports the button down.
  EXPECT_TRUE(loop.input.currentState & (1u << CONFIRM));
  EXPECT_FALSE(loop.queue.idle());
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.input.currentState, 0);
}

// A finger on another key during a held press: its press and release go through on their own, and
// the held press is not cut short.
TEST(ButtonPressQueue, ARealKeyPressedDuringAHoldKeepsItsOwnEdges) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(DOWN, 1000));
  ASSERT_TRUE(loop.runUntilPressed(DOWN)) << "the press was never committed";
  for (int i = 0; i < 20; ++i) loop.pass();
  loop.realKeys = 1u << BACK;
  for (int i = 0; i < 10; ++i) loop.pass();
  loop.realKeys = 0;
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[BACK], 1);
  EXPECT_EQ(loop.releases[BACK], 1);
  EXPECT_EQ(loop.presses[DOWN], 1);
  EXPECT_EQ(loop.releases[DOWN], 1);
  EXPECT_EQ(loop.queue.droppedPresses(), 0u);
}

// A finger on the same key: the two are one press, released when both have let go, and the queue
// does not hang waiting for a release it cannot cause.
TEST(ButtonPressQueue, ARealKeyHeldPastTheHoldEndsThePressAtItsRelease) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(DOWN, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilPressed(DOWN)) << "the press was never committed";
  loop.realKeys = 1u << DOWN;
  for (int i = 0; i < 300; ++i) loop.pass();  // 3 s, long past the hold and the commit timeout
  EXPECT_TRUE(loop.queue.idle()) << "given up waiting for the release";
  EXPECT_EQ(loop.releases[DOWN], 0) << "the finger still holds it";
  loop.realKeys = 0;
  for (int i = 0; i < 5; ++i) loop.pass();
  EXPECT_EQ(loop.presses[DOWN], 1);
  EXPECT_EQ(loop.releases[DOWN], 1);
}

// The key still reads down when the next press of it is due (a finger on it, or a release the input
// layer never took in): that press waits for the release, so it gets a press edge of its own rather
// than taking over the one already down.
TEST(ButtonPressQueue, APressOfAKeyStillDownWaitsForItsRelease) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(DOWN, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.queue.push(DOWN, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilPressed(DOWN)) << "the press was never committed";
  loop.realKeys = 1u << DOWN;
  for (int i = 0; i < 130; ++i) loop.pass();  // past the first press's release timeout
  EXPECT_EQ(loop.presses[DOWN], 1);
  EXPECT_FALSE(loop.queue.idle()) << "the second press waits for the key to come up";
  loop.realKeys = 0;
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[DOWN], 2) << "the second press has its own edge";
  EXPECT_EQ(loop.releases[DOWN], 2);
  EXPECT_EQ(loop.queue.droppedPresses(), 0u);
}

// The key never comes up within the commit timeout: the press is given up and counted, as one the
// input layer never took in (STATE's btn_dropped, and the next IDLE fails).
TEST(ButtonPressQueue, APressOfAKeyThatStaysDownIsGivenUp) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(DOWN, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.queue.push(DOWN, SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilPressed(DOWN)) << "the press was never committed";
  loop.realKeys = 1u << DOWN;
  for (int i = 0; i < 400; ++i) loop.pass();  // 4 s
  EXPECT_TRUE(loop.queue.idle());
  EXPECT_EQ(loop.presses[DOWN], 1) << "no second press edge";
  EXPECT_EQ(loop.queue.droppedPresses(), 1u);
  EXPECT_EQ(loop.held, 0);
  loop.realKeys = 0;
  for (int i = 0; i < 5; ++i) loop.pass();
  EXPECT_EQ(loop.releases[DOWN], 1);
}

// Another key held down when the queue starts a press does not hold that press up.
TEST(ButtonPressQueue, AnotherKeyDownDoesNotDelayAPress) {
  Loop loop;
  loop.realKeys = 1u << BACK;
  for (int i = 0; i < 5; ++i) loop.pass();
  ASSERT_TRUE(loop.queue.push(CONFIRM, SHORT_HOLD_MS));
  loop.pass();
  EXPECT_EQ(loop.held, 1u << CONFIRM) << "held from the first step";
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_EQ(loop.queue.droppedPresses(), 0u);
}
