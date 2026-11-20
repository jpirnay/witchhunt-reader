// A laid-out line's TextBlock goes where its bytes go (memory-and-parsing §9b): into the build's line arena while that
// keeps what the caller asks free after it (the parser: the image-header walk's reserve), else onto the heap as before.
// Once an object of the page is in the arena, the line bytes keep that room too (TextBlock::ArenaRoom). Its destroying
// operator delete frees only an object the heap made, so `std::unique_ptr<TextBlock>` stays the type everywhere.
#include <BuildArena.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"

namespace {

constexpr int kFontId = 1;
constexpr size_t kReserve = 34 * 1024;  // what the parser keeps free: EpubImageManifest::kWalkArenaReserve

BlockStyle plain() {
  BlockStyle bs;
  bs.textIndent = 0;
  bs.textIndentDefined = true;
  bs.alignment = CssTextAlign::Left;
  bs.textAlignDefined = true;
  return bs;
}

// A one-line paragraph laid out with `arena` as its line arena (null: none) and `room` as the page's room (null:
// none); the lines are kept alive.
std::vector<std::unique_ptr<TextBlock>> layOut(BuildArena* arena, TextBlock::ArenaRoom* room) {
  GfxRenderer renderer;
  ParsedText text(false, false, plain());
  text.setLineArena(arena, room);
  for (const char* word : {"alpha", "beta", "gamma"}) text.addWord(word, EpdFontFamily::REGULAR);
  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(
      renderer, kFontId, 400,
      [&lines](std::unique_ptr<TextBlock> tb, bool, bool) {
        lines.push_back(std::move(tb));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  return lines;
}

// A line built directly, as ParsedText builds each one.
struct OneLine {
  std::vector<std::string> words{"alpha", "beta"};
  std::vector<EpdFontFamily::Style> styles{EpdFontFamily::REGULAR, EpdFontFamily::REGULAR};
  std::vector<int16_t> xs{0, 40};
  TextBlock::WordRange range() const {
    TextBlock::WordRange r;
    r.words = &words;
    r.styles = &styles;
    r.count = words.size();
    return r;
  }
};

// The arena room OneLine's object takes, at most (its alignment's padding included).
constexpr size_t kObjectRoom = sizeof(TextBlock) + alignof(TextBlock) - 1;

}  // namespace

TEST(LineObjectArena, ALineGoesWhereItsBytesGoWhenTheArenaKeepsTheReserve) {
  BuildArena arena(kReserve + 4096);
  ASSERT_TRUE(arena.valid());
  TextBlock::ArenaRoom room{kReserve};
  const auto lines = layOut(&arena, &room);
  ASSERT_EQ(1u, lines.size());
  EXPECT_TRUE(lines[0]->objectInArena());
  EXPECT_GE(room.displaced, sizeof(TextBlock)) << "the object took room the bytes did not give back";
  EXPECT_GE(arena.used(), sizeof(TextBlock) + lines[0]->arenaBytes());
  EXPECT_EQ(std::string("beta"), lines[0]->wordText(1));
}

TEST(LineObjectArena, ALineGoesToTheHeapWhenTheArenaWouldFallBelowTheReserve) {
  // Room for the line's bytes, and for the object too, but not for the object and the reserve: the object goes to the
  // heap, and with no object of the page in the arena the bytes take the room they always took.
  BuildArena arena(kReserve + sizeof(TextBlock) / 2);
  ASSERT_TRUE(arena.valid());
  TextBlock::ArenaRoom room{kReserve};
  const auto lines = layOut(&arena, &room);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->objectInArena());
  EXPECT_EQ(0u, room.displaced);
  EXPECT_EQ(arena.used(), lines[0]->arenaBytes()) << "the bytes alone are in the arena";
}

TEST(LineObjectArena, TheReserveCountsWhatTheArenaAlreadyHolds) {
  BuildArena arena(kReserve + 4096);
  ASSERT_TRUE(arena.valid());
  ASSERT_NE(nullptr, arena.alloc(4096 - sizeof(TextBlock) / 2, 1));  // what the parse holds below the page block
  TextBlock::ArenaRoom room{kReserve};
  const auto lines = layOut(&arena, &room);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->objectInArena());
}

TEST(LineObjectArena, ALineWithoutALineArenaIsOnTheHeap) {
  TextBlock::ArenaRoom room{kReserve};
  const auto lines = layOut(nullptr, &room);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->objectInArena());
}

TEST(LineObjectArena, ALineArenaSetWithoutARoomKeepsTheObjectOnTheHeap) {
  BuildArena arena(kReserve + 4096);
  ASSERT_TRUE(arena.valid());
  const auto lines = layOut(&arena, nullptr);  // the bytes only, as every caller had it before
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->objectInArena());
  EXPECT_EQ(arena.used(), lines[0]->arenaBytes());
}

// The object fits beside the reserve, its bytes would cross it: they go to the heap, so the room the object took never
// comes out of what the reserve holds for.
TEST(LineObjectArena, OnceAnObjectOfThePageIsInTheArenaTheBytesKeepTheReserve) {
  const OneLine source;
  BuildArena arena(kReserve + kObjectRoom + 2);
  ASSERT_TRUE(arena.valid());
  TextBlock::ArenaRoom room{kReserve};
  const size_t made = heapAllocations();
  auto line = TextBlock::make(source.range(), source.xs, plain(), &arena, &room);
  ASSERT_TRUE(line);
  ASSERT_TRUE(line->valid());
  EXPECT_TRUE(line->objectInArena());
  EXPECT_EQ(made + 1, heapAllocations()) << "the bytes, one heap block";
  EXPECT_EQ(sizeof(TextBlock) - line->arenaBytes(), room.displaced) << "the bytes gave back the room they took";
  EXPECT_GE(arena.capacity() - arena.used(), kReserve);
  EXPECT_EQ(std::string("beta"), line->wordText(1));
}

// A later line of the page, its own object refused the arena: an earlier object of the page holds its bytes back too.
TEST(LineObjectArena, AnEarlierObjectOfThePageHoldsALaterLinesBytesBack) {
  const OneLine source;
  BuildArena arena(kReserve + 16);  // room for the bytes, but only by crossing the reserve
  ASSERT_TRUE(arena.valid());
  TextBlock::ArenaRoom room{kReserve, /*displaced=*/sizeof(TextBlock)};
  auto line = TextBlock::make(source.range(), source.xs, plain(), &arena, &room);
  ASSERT_TRUE(line);
  ASSERT_TRUE(line->valid());
  EXPECT_FALSE(line->objectInArena());
  EXPECT_EQ(0u, arena.used()) << "the bytes would have taken the reserve's room: the heap";

  // The same line on a page with no object in the arena: its bytes take the room, as before 9b.
  TextBlock::ArenaRoom fresh{kReserve};
  auto before9b = TextBlock::make(source.range(), source.xs, plain(), &arena, &fresh);
  ASSERT_TRUE(before9b);
  EXPECT_FALSE(before9b->objectInArena());
  EXPECT_EQ(before9b->arenaBytes(), arena.used());
}

// Once the page's bytes have given back the room its objects took, they take any room again, as before 9b: the page
// never takes more of the arena than it did then.
TEST(LineObjectArena, BytesThatHaveGivenBackWhatTheObjectsTookTakeAnyRoomAgain) {
  const OneLine source;
  BuildArena arena(kReserve + 16);  // room for the bytes, but only by crossing the reserve
  ASSERT_TRUE(arena.valid());
  TextBlock::ArenaRoom room{kReserve, /*displaced=*/8};
  auto first = TextBlock::make(source.range(), source.xs, plain(), &arena, &room);
  ASSERT_TRUE(first);
  EXPECT_EQ(0u, arena.used()) << "8 B displaced: these bytes go to the heap";
  EXPECT_EQ(0u, room.displaced);
  auto second = TextBlock::make(source.range(), source.xs, plain(), &arena, &room);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->arenaBytes(), arena.used()) << "nothing displaced: the bytes take the room";
}

// The heap refuses the block for bytes sent there to give the walk's room back (AllocationRefusal): they take the
// arena, as they did before 9b, and the line is kept; nothing is given back.
TEST(LineObjectArena, BytesTheHeapRefusesWhileGivingBackTakeTheArena) {
  const OneLine source;
  BuildArena arena(kReserve + sizeof(TextBlock) + 20);  // the object fits beside the reserve, its bytes only across it
  ASSERT_TRUE(arena.valid());
  TextBlock::ArenaRoom room{kReserve};
  std::unique_ptr<TextBlock> line;
  {
    const AllocationRefusal refused(16);  // the bytes' block; the object, in the arena, needs none
    line = TextBlock::make(source.range(), source.xs, plain(), &arena, &room);
  }
  ASSERT_TRUE(line);
  ASSERT_TRUE(line->valid()) << "the line is kept";
  EXPECT_TRUE(line->objectInArena());
  EXPECT_EQ(sizeof(TextBlock) + line->arenaBytes(), arena.used()) << "its bytes in the arena";
  EXPECT_EQ(sizeof(TextBlock), room.displaced) << "nothing given back";
  EXPECT_EQ(std::string("beta"), line->wordText(1));
}

// Room for the bytes neither on the heap nor in the arena: an invalid line, which ParsedText drops, as before 9b.
TEST(LineObjectArena, BytesNeitherTheHeapNorTheArenaHoldsMakeAnInvalidLine) {
  const OneLine source;
  BuildArena arena(sizeof(TextBlock) + 8);  // the object, and 8 B: not the line's bytes
  ASSERT_TRUE(arena.valid());
  TextBlock::ArenaRoom room{0};  // the object goes in; its bytes cannot keep even 0 B free, so they are sent away
  std::unique_ptr<TextBlock> line;
  {
    const AllocationRefusal refused(16);
    line = TextBlock::make(source.range(), source.xs, plain(), &arena, &room);
  }
  ASSERT_TRUE(line);
  EXPECT_FALSE(line->valid());
}

// The destroying operator delete: an arena object is destroyed and not freed, a heap object is destroyed and freed.
TEST(LineObjectArena, DeletingALineFreesOnlyWhatTheHeapMade) {
  const OneLine source;
  BuildArena arena(kReserve + 4096);
  ASSERT_TRUE(arena.valid());
  const BlockStyle style = plain();

  size_t live = heapLiveBytes();
  size_t made = heapAllocations();
  TextBlock::ArenaRoom room{kReserve};
  auto inArena = TextBlock::make(source.range(), source.xs, style, &arena, &room);
  ASSERT_TRUE(inArena);
  ASSERT_TRUE(inArena->valid());
  EXPECT_TRUE(inArena->objectInArena());
  EXPECT_EQ(made, heapAllocations()) << "object and bytes both in the arena: no heap block";
  inArena.reset();
  EXPECT_EQ(live, heapLiveBytes()) << "nothing given back to the heap";

  // The object on the heap (the reserve is the whole arena), its bytes in the arena: one block, freed on delete.
  made = heapAllocations();
  TextBlock::ArenaRoom whole{arena.capacity()};
  auto onHeap = TextBlock::make(source.range(), source.xs, style, &arena, &whole);
  ASSERT_TRUE(onHeap);
  EXPECT_FALSE(onHeap->objectInArena());
  EXPECT_EQ(made + 1, heapAllocations());
  live = heapLiveBytes();
  onHeap.reset();
  EXPECT_LT(heapLiveBytes(), live) << "the heap object is freed";
  EXPECT_GE(live - heapLiveBytes(), sizeof(TextBlock));
}

// The object in the arena, its bytes on the heap (here the arena filled up behind the object; the reserve sends them
// there the same way): the destructor frees the bytes, the delete leaves the object.
TEST(LineObjectArena, AnArenaObjectWithHeapBytesFreesTheBytesOnly) {
  const OneLine source;
  BuildArena arena(sizeof(TextBlock) + alignof(TextBlock));
  ASSERT_TRUE(arena.valid());
  const size_t live = heapLiveBytes();
  TextBlock::ArenaRoom room{0};
  auto line = TextBlock::make(source.range(), source.xs, plain(), &arena, &room);
  ASSERT_TRUE(line);
  ASSERT_TRUE(line->valid());
  EXPECT_TRUE(line->objectInArena());
  EXPECT_GT(heapLiveBytes(), live) << "the bytes did not fit after the object";
  line.reset();
  EXPECT_EQ(live, heapLiveBytes());
}
