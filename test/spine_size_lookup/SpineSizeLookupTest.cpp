// findSpineForSize(): the same answer the reader's linear walk gave, from a handful of reads.
#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "SpineSizeLookup.h"

namespace {

// What resolvePercent() did before: the first item whose cumulative size reaches the target.
int linearWalk(const std::vector<size_t>& cumulative, const size_t target) {
  for (size_t i = 0; i < cumulative.size(); ++i) {
    if (target <= cumulative[i]) return static_cast<int>(i);
  }
  return static_cast<int>(cumulative.size()) - 1;
}

int lookup(const std::vector<size_t>& cumulative, const size_t target, int* reads = nullptr) {
  return findSpineForSize(static_cast<int>(cumulative.size()), target, [&](const int i) {
    if (reads) ++*reads;
    return cumulative[static_cast<size_t>(i)];
  });
}

}  // namespace

TEST(SpineSizeLookup, ABookWithNoSpineHasNoAnswer) { EXPECT_EQ(lookup({}, 10), -1); }

TEST(SpineSizeLookup, AgreesWithTheLinearWalkOnEveryTarget) {
  // Empty chapters at the start, in the middle and at the end: runs of equal cumulative sizes.
  const std::vector<size_t> cumulative{0, 0, 100, 100, 100, 250, 900, 900, 1000, 1000};
  for (size_t target = 0; target <= 1100; ++target) {
    EXPECT_EQ(lookup(cumulative, target), linearWalk(cumulative, target)) << "target " << target;
  }
}

TEST(SpineSizeLookup, ATargetPastTheBookIsTheLastItem) { EXPECT_EQ(lookup({10, 20, 30}, 31), 2); }

TEST(SpineSizeLookup, ALargeBookTakesAFewReadsNotOnePerChapter) {
  std::vector<size_t> cumulative;
  for (size_t i = 1; i <= 1700; ++i) cumulative.push_back(i * 1000);
  for (const size_t target : {size_t{1}, size_t{850000}, size_t{1699999}}) {
    int reads = 0;
    EXPECT_EQ(lookup(cumulative, target, &reads), linearWalk(cumulative, target));
    EXPECT_LE(reads, 11) << "target " << target;
  }
}
