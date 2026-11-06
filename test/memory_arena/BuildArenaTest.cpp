#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "BuildArena.h"

namespace {

TEST(BuildArena, AllocatesAlignedAndBumps) {
  BuildArena arena(256);
  ASSERT_TRUE(arena.valid());

  auto* a = static_cast<uint8_t*>(arena.alloc(3, 1));
  auto* b = static_cast<uint8_t*>(arena.alloc(4, 4));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(b) % 4, 0u);
  EXPECT_GE(b, a + 3);
  EXPECT_EQ(arena.used(), static_cast<size_t>((b - a) + 4));
}

TEST(BuildArena, DefaultAlignmentIsMaxAlign) {
  BuildArena arena(256);
  arena.alloc(1, 1);
  void* p = arena.alloc(8);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t), 0u);
}

TEST(BuildArena, RefusesWhenFullAndRecordsShortfall) {
  BuildArena arena(64);
  ASSERT_NE(arena.alloc(48, 1), nullptr);
  EXPECT_EQ(arena.alloc(32, 1), nullptr);
  EXPECT_EQ(arena.failedAllocSize(), 32u);
  // A refused alloc leaves the cursor untouched: a smaller one still fits.
  EXPECT_NE(arena.alloc(16, 1), nullptr);
  EXPECT_EQ(arena.used(), 64u);
}

TEST(BuildArena, OverflowingAlignmentPaddingIsRefused) {
  BuildArena arena(64);
  ASSERT_NE(arena.alloc(63, 1), nullptr);
  // 1 byte left, but 8-byte alignment would pad past the end — must refuse,
  // not wrap.
  EXPECT_EQ(arena.alloc(1, 8), nullptr);
}

TEST(BuildArena, InvalidAlignmentIsRefused) {
  BuildArena arena(64);
  EXPECT_EQ(arena.alloc(1, 0), nullptr);
  EXPECT_EQ(arena.alloc(1, 3), nullptr);
  EXPECT_EQ(arena.used(), 0u);
}

TEST(BuildArena, BlockReleaseReclaimsNested) {
  BuildArena arena(128);
  arena.alloc(16, 1);
  auto outer = arena.reserveBlock();
  arena.alloc(32, 1);
  auto inner = arena.reserveBlock();
  arena.alloc(32, 1);

  EXPECT_TRUE(arena.release(inner));
  EXPECT_EQ(arena.used(), 48u);
  EXPECT_TRUE(arena.release(outer));
  EXPECT_EQ(arena.used(), 16u);
  // Reclaimed space is reusable.
  EXPECT_NE(arena.alloc(100, 1), nullptr);
}

TEST(BuildArena, OutOfOrderReleaseIsRejected) {
  BuildArena arena(128);
  auto outer = arena.reserveBlock();
  arena.alloc(32, 1);
  auto inner = arena.reserveBlock();
  arena.alloc(16, 1);

  EXPECT_FALSE(arena.release(outer));
  EXPECT_EQ(arena.releaseFailures(), 1u);
  EXPECT_EQ(arena.used(), 48u);
  EXPECT_TRUE(arena.release(inner));
  EXPECT_EQ(arena.used(), 32u);
  EXPECT_TRUE(arena.release(outer));
  EXPECT_EQ(arena.used(), 0u);
  EXPECT_FALSE(arena.release(outer));
  EXPECT_EQ(arena.releaseFailures(), 2u);
}

TEST(BuildArena, BlockCannotReleaseAnotherArena) {
  BuildArena first(64);
  BuildArena second(64);
  auto firstBlock = first.reserveBlock();
  auto secondBlock = second.reserveBlock();
  ASSERT_NE(first.alloc(16, 1), nullptr);
  ASSERT_NE(second.alloc(16, 1), nullptr);

  EXPECT_FALSE(second.release(firstBlock));
  EXPECT_EQ(second.used(), 16u);
  EXPECT_EQ(second.releaseFailures(), 1u);
  EXPECT_TRUE(second.release(secondBlock));
  EXPECT_TRUE(first.release(firstBlock));
}

TEST(BuildArena, ArrayAllocationRejectsSizeOverflow) {
  BuildArena arena(64);
  EXPECT_EQ(arena.allocArray<uint32_t>(SIZE_MAX / sizeof(uint32_t) + 1), nullptr);
  EXPECT_EQ(arena.used(), 0u);
  EXPECT_EQ(arena.failedAllocSize(), SIZE_MAX);
}

TEST(BuildArena, CommittedBlockRemainsAllocated) {
  BuildArena arena(128);
  auto permanent = arena.reserveBlock();
  arena.alloc(32, 1);
  EXPECT_TRUE(arena.commit(permanent));

  auto scratch = arena.reserveBlock();
  arena.alloc(16, 1);
  EXPECT_TRUE(arena.release(scratch));
  EXPECT_EQ(arena.used(), 32u);
  EXPECT_FALSE(arena.release(permanent));
}

TEST(BuildArena, HighWaterTracksPeakAcrossReleaseAndReset) {
  BuildArena arena(128);
  auto block = arena.reserveBlock();
  arena.alloc(100, 1);
  EXPECT_TRUE(arena.release(block));
  arena.alloc(10, 1);
  EXPECT_EQ(arena.highWater(), 100u);
  arena.reset();
  EXPECT_EQ(arena.used(), 0u);
  EXPECT_EQ(arena.highWater(), 100u);  // survives reset for post-build diagnostics
}

TEST(BuildArena, ResetInvalidatesLiveBlocks) {
  BuildArena arena(128);
  auto stale = arena.reserveBlock();
  arena.alloc(32, 1);
  arena.reset();
  arena.alloc(16, 1);

  EXPECT_FALSE(arena.release(stale));
  EXPECT_EQ(arena.used(), 16u);
}

TEST(BuildArena, ExternalBufferIsUsedInPlace) {
  alignas(std::max_align_t) uint8_t buf[64];
  BuildArena arena(buf, sizeof(buf));
  ASSERT_TRUE(arena.valid());
  auto* p = static_cast<uint8_t*>(arena.alloc(8, 1));
  EXPECT_EQ(p, buf);
}

TEST(BuildArena, AlignsAgainstUnalignedExternalBuffer) {
  uint8_t storage[65];
  BuildArena arena(storage + 1, 64);
  auto* p = static_cast<uint8_t*>(arena.alloc(8, 8));
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);
  EXPECT_GE(p, storage + 1);
}

TEST(BuildArena, FreeTailStartsAboveTheCursorAndRunsToTheEnd) {
  BuildArena arena(256);
  ASSERT_NE(arena.alloc(40, 1), nullptr);
  size_t bytes = 0;
  uint8_t* const tail = arena.freeTail(&bytes);
  ASSERT_NE(tail, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(tail) % alignof(std::max_align_t), 0u);
  EXPECT_LE(bytes, 256u - 40u);
  EXPECT_GT(bytes, 256u - 40u - alignof(std::max_align_t));
}

TEST(BuildArena, ASecondArenaOnTheFreeTailLeavesTheOwnerAsItWas) {
  BuildArena owner(256);
  auto* const kept = static_cast<uint8_t*>(owner.alloc(32, 1));
  ASSERT_NE(kept, nullptr);
  memset(kept, 0x5A, 32);
  BuildArena::Block block = owner.reserveBlock();
  ASSERT_NE(owner.alloc(16, 1), nullptr);
  const size_t used = owner.used();
  const size_t highWater = owner.highWater();
  {
    size_t bytes = 0;
    uint8_t* const tail = owner.freeTail(&bytes);
    BuildArena lodger(tail, bytes);
    auto* const p = static_cast<uint8_t*>(lodger.alloc(bytes, 1));
    ASSERT_NE(p, nullptr);
    memset(p, 0xFF, bytes);
    lodger.reset();  // what the memory ladder does between steps: rewinds the lodger alone
  }
  EXPECT_EQ(owner.used(), used);
  EXPECT_EQ(owner.highWater(), highWater);
  EXPECT_TRUE(owner.release(block));  // the build's scopes are as it left them
  for (int i = 0; i < 32; ++i) EXPECT_EQ(kept[i], 0x5A);
}

TEST(BuildArena, NoFreeTailOnAFullOrInvalidArena) {
  BuildArena full(64);
  ASSERT_NE(full.alloc(64, 1), nullptr);
  size_t bytes = 1;
  EXPECT_EQ(full.freeTail(&bytes), nullptr);
  EXPECT_EQ(bytes, 0u);
  BuildArena none(nullptr, 64);
  bytes = 1;
  EXPECT_EQ(none.freeTail(&bytes), nullptr);
  EXPECT_EQ(bytes, 0u);
}

TEST(BuildArena, NullExternalBufferIsInvalidAndRefuses) {
  BuildArena arena(nullptr, 64);
  EXPECT_FALSE(arena.valid());
  EXPECT_EQ(arena.alloc(1, 1), nullptr);
  EXPECT_EQ(arena.failedAllocSize(), 1u);
}

}  // namespace
