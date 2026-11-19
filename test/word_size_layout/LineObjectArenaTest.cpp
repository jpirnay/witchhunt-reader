// A laid-out line's TextBlock goes where its bytes go (memory-and-parsing §9b): into the build's line arena while that
// keeps what the caller asks free after it (the parser: the image-header walk's reserve), else onto the heap as before.
// Its destroying operator delete frees only an object the heap made, so `std::unique_ptr<TextBlock>` stays the type
// everywhere.
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

// A one-line paragraph laid out with `arena` as its line arena (null: none), keeping `keepFree` free after each line's
// object; the lines are kept alive.
std::vector<std::unique_ptr<TextBlock>> layOut(BuildArena* arena, const size_t keepFree) {
  GfxRenderer renderer;
  ParsedText text(false, false, plain());
  text.setLineArena(arena, keepFree);
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

}  // namespace

TEST(LineObjectArena, ALineGoesWhereItsBytesGoWhenTheArenaKeepsTheReserve) {
  BuildArena arena(kReserve + 4096);
  ASSERT_TRUE(arena.valid());
  const auto lines = layOut(&arena, kReserve);
  ASSERT_EQ(1u, lines.size());
  EXPECT_TRUE(lines[0]->objectInArena());
  EXPECT_GE(arena.used(), sizeof(TextBlock) + lines[0]->arenaBytes());
  EXPECT_EQ(std::string("beta"), lines[0]->wordText(1));
}

TEST(LineObjectArena, ALineGoesToTheHeapWhenTheArenaWouldFallBelowTheReserve) {
  // Room for the line's bytes, and for the object too, but not for the object and the reserve: the bytes stay in the
  // arena, as before, and the object goes to the heap.
  BuildArena arena(kReserve + sizeof(TextBlock) / 2);
  ASSERT_TRUE(arena.valid());
  const auto lines = layOut(&arena, kReserve);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->objectInArena());
  EXPECT_EQ(arena.used(), lines[0]->arenaBytes()) << "the bytes alone are in the arena";
}

TEST(LineObjectArena, TheReserveCountsWhatTheArenaAlreadyHolds) {
  BuildArena arena(kReserve + 4096);
  ASSERT_TRUE(arena.valid());
  ASSERT_NE(nullptr, arena.alloc(4096 - sizeof(TextBlock) / 2, 1));  // what the parse holds below the page block
  const auto lines = layOut(&arena, kReserve);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->objectInArena());
}

TEST(LineObjectArena, ALineWithoutALineArenaIsOnTheHeap) {
  const auto lines = layOut(nullptr, kReserve);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->objectInArena());
}

TEST(LineObjectArena, ALineArenaSetWithoutAReserveKeepsTheObjectOnTheHeap) {
  BuildArena arena(kReserve + 4096);
  ASSERT_TRUE(arena.valid());
  GfxRenderer renderer;
  ParsedText text(false, false, plain());
  text.setLineArena(&arena);  // the bytes only, as every caller had it before
  text.addWord("alpha", EpdFontFamily::REGULAR);
  std::vector<std::unique_ptr<TextBlock>> lines;
  text.layoutAndExtractLines(
      renderer, kFontId, 400,
      [&lines](std::unique_ptr<TextBlock> tb, bool, bool) {
        lines.push_back(std::move(tb));
        return ParsedText::LineProcessResult::Accepted;
      },
      /*includeLastLine=*/true);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->objectInArena());
  EXPECT_EQ(arena.used(), lines[0]->arenaBytes());
}

// The destroying operator delete: an arena object is destroyed and not freed, a heap object is destroyed and freed.
TEST(LineObjectArena, DeletingALineFreesOnlyWhatTheHeapMade) {
  const OneLine source;
  BuildArena arena(kReserve + 4096);
  ASSERT_TRUE(arena.valid());
  const BlockStyle style = plain();

  size_t live = heapLiveBytes();
  size_t made = heapAllocations();
  auto inArena = TextBlock::make(source.range(), source.xs, style, &arena, kReserve);
  ASSERT_TRUE(inArena);
  ASSERT_TRUE(inArena->valid());
  EXPECT_TRUE(inArena->objectInArena());
  EXPECT_EQ(made, heapAllocations()) << "object and bytes both in the arena: no heap block";
  inArena.reset();
  EXPECT_EQ(live, heapLiveBytes()) << "nothing given back to the heap";

  // The object on the heap (the reserve is the whole arena), its bytes in the arena: one block, freed on delete.
  made = heapAllocations();
  auto onHeap = TextBlock::make(source.range(), source.xs, style, &arena, arena.capacity());
  ASSERT_TRUE(onHeap);
  EXPECT_FALSE(onHeap->objectInArena());
  EXPECT_EQ(made + 1, heapAllocations());
  live = heapLiveBytes();
  onHeap.reset();
  EXPECT_LT(heapLiveBytes(), live) << "the heap object is freed";
  EXPECT_GE(live - heapLiveBytes(), sizeof(TextBlock));
}

// The object in the arena, its bytes on the heap (the arena filled up behind the object): the destructor frees the
// bytes, the delete leaves the object.
TEST(LineObjectArena, AnArenaObjectWithHeapBytesFreesTheBytesOnly) {
  const OneLine source;
  BuildArena arena(sizeof(TextBlock) + alignof(TextBlock));
  ASSERT_TRUE(arena.valid());
  const size_t live = heapLiveBytes();
  auto line = TextBlock::make(source.range(), source.xs, plain(), &arena, 0);
  ASSERT_TRUE(line);
  ASSERT_TRUE(line->valid());
  EXPECT_TRUE(line->objectInArena());
  EXPECT_GT(heapLiveBytes(), live) << "the bytes did not fit after the object";
  line.reset();
  EXPECT_EQ(live, heapLiveBytes());
}
