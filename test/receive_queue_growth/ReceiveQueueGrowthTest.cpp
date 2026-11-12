// When the USB serial receive queue grows (util/ReceiveQueueGrowth.h). Growing it deletes it, so commands a host sent
// back to back must not be waiting in it then: on the X3 the second and third of three commands sent in one write,
// as the first input after a restart, were lost (the queue grew at the first one).

#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <vector>

#include "util/ReceiveQueueGrowth.h"

namespace {

// The main loop, as main.cpp runs it: each pass takes one line out of the queue, then asks whether the queue may
// grow now; growing it replaces the queue, losing whatever waited there.
struct Wire {
  std::deque<char> queue;
  ReceiveQueueGrowth growth;
  std::vector<std::string> taken;
  int grownOnPass = -1;

  void hostWrites(const std::string& bytes) { queue.insert(queue.end(), bytes.begin(), bytes.end()); }

  void pass(const int number) {
    std::string line;
    while (!queue.empty()) {
      const char c = queue.front();
      queue.pop_front();
      if (c == '\n') {
        taken.push_back(line);
        break;
      }
      line += c;
    }
    if (growth.growNow(queue.size())) {
      queue.clear();  // HWCDC deletes the old queue
      grownOnPass = number;
    }
  }
};

}  // namespace

TEST(ReceiveQueueGrowth, CommandsSentBackToBackAreAllTakenBeforeTheQueueGrows) {
  Wire wire;
  wire.hostWrites("CMD:BTN:LEFT\nCMD:IDLE:5000\nCMD:STATE\n");
  for (int i = 0; i < 5; ++i) wire.pass(i);
  EXPECT_EQ(wire.taken, (std::vector<std::string>{"CMD:BTN:LEFT", "CMD:IDLE:5000", "CMD:STATE"}));
  EXPECT_EQ(wire.grownOnPass, 2) << "at the last of them, with nothing behind it";
  EXPECT_TRUE(wire.growth.grown());
}

TEST(ReceiveQueueGrowth, ALoneCommandGrowsTheQueueAtOnce) {
  Wire wire;
  wire.hostWrites("CMD:FS:PUT:/test/a.bin:5:3610a686\n");
  wire.pass(0);
  EXPECT_EQ(wire.grownOnPass, 0) << "before the upload's pieces come";
}

TEST(ReceiveQueueGrowth, NotWhileBytesWaitAndOnlyOnce) {
  ReceiveQueueGrowth growth;
  EXPECT_FALSE(growth.growNow(12));
  EXPECT_FALSE(growth.grown());
  EXPECT_TRUE(growth.growNow(0));
  EXPECT_FALSE(growth.growNow(0)) << "grown for good: the queue is never replaced again";
  EXPECT_TRUE(growth.grown());
}
