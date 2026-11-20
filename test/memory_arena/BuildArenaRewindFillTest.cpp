// The host's rewind fill (BUILD_ARENA_TRACE): a released block, and a reset arena, read the fill pattern, so an
// object used after the block it lives in was given back fails a test instead of reading whatever was left there.
// This file is its own binary: BuildArena is header-only, and a binary with the define in some files and not in
// others would link one of the two versions for all of them.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "BuildArena.h"

namespace {

TEST(BuildArenaRewindFill, AReleasedBlockReadsTheFill) {
  alignas(std::max_align_t) uint8_t buf[256];
  BuildArena arena(buf, sizeof(buf));
  auto* const kept = static_cast<uint8_t*>(arena.alloc(16, 1));
  ASSERT_NE(kept, nullptr);
  memset(kept, 0x11, 16);
  BuildArena::Block block = arena.reserveBlock();
  auto* const gone = static_cast<uint8_t*>(arena.alloc(40, 1));
  ASSERT_NE(gone, nullptr);
  memset(gone, 0x22, 40);
  ASSERT_TRUE(arena.release(block));
  EXPECT_TRUE(buildArenaLooksRewound(gone, 40));
  for (int i = 0; i < 16; ++i) EXPECT_EQ(kept[i], 0x11) << "below the block: untouched";
}

TEST(BuildArenaRewindFill, AResetArenaReadsTheFillWhereItWasUsed) {
  alignas(std::max_align_t) uint8_t buf[128];
  memset(buf, 0x33, sizeof(buf));
  BuildArena arena(buf, sizeof(buf));
  ASSERT_NE(arena.alloc(48, 1), nullptr);
  arena.reset();
  EXPECT_TRUE(buildArenaLooksRewound(buf, 48));
  for (size_t i = 48; i < sizeof(buf); ++i) EXPECT_EQ(buf[i], 0x33) << "never handed out: untouched";
}

// What lives above the cursor is not the arena's to fill: the word-shaping cache keeps its sets in the region's top
// bytes, which the arena counts as free.
TEST(BuildArenaRewindFill, NothingAboveTheCursorIsFilled) {
  alignas(std::max_align_t) uint8_t buf[256];
  BuildArena arena(buf, sizeof(buf));
  uint8_t* const top = arena.tail(64);
  ASSERT_NE(top, nullptr);
  memset(top, 0x44, 64);
  BuildArena::Block block = arena.reserveBlock();
  ASSERT_NE(arena.alloc(100, 1), nullptr);
  ASSERT_TRUE(arena.release(block));
  for (int i = 0; i < 64; ++i) EXPECT_EQ(top[i], 0x44);
}

TEST(BuildArenaRewindFill, LooksRewoundOnlyWhenEveryByteIsTheFill) {
  uint8_t bytes[8];
  memset(bytes, kBuildArenaRewoundFill, sizeof(bytes));
  EXPECT_TRUE(buildArenaLooksRewound(bytes, sizeof(bytes)));
  bytes[5] = 0;
  EXPECT_FALSE(buildArenaLooksRewound(bytes, sizeof(bytes)));
  EXPECT_FALSE(buildArenaLooksRewound(bytes, 0)) << "nothing to read is not a rewound object";
}

}  // namespace
