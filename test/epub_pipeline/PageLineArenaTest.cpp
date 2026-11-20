// A laid-out line's PageLine goes where its TextBlock goes (memory-and-parsing §9c): into the page's block of the
// build's line arena while that keeps what the page's room asks free after it (TextBlock::ArenaRoom, the rule 9b set
// for the TextBlock: the image walk's reserve), else onto the heap as before. PageElement's destroying operator delete
// frees only an element the heap made, so std::unique_ptr<PageElement> stays the type everywhere. This binary fills a
// rewound block (BUILD_ARENA_TRACE), so a PageLine destroyed after its page's block was given back aborts.
#include <Memory.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "BuildArena.h"
#include "Epub/Page.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "GfxRenderer.h"

namespace {

constexpr size_t kLentBytes = 52272;    // the X3's secondary framebuffer, lent to background builds
constexpr size_t kReserve = 34 * 1024;  // what the parser keeps free: EpubImageManifest::kWalkArenaReserve

// A line as the drop cap makes one, object and bytes on the heap.
std::unique_ptr<TextBlock> heapLine() {
  auto block =
      makeUniqueNoThrow<TextBlock>(std::vector<std::string>{"alpha", "beta"}, std::vector<int16_t>{0, 40},
                                   std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR, EpdFontFamily::REGULAR},
                                   BlockStyle{}, std::vector<uint8_t>{});
  EXPECT_TRUE(block && block->valid());
  return block;
}

bool inRegion(const void* object, const std::vector<uint8_t>& region) {
  const auto* const at = static_cast<const uint8_t*>(object);
  return at >= region.data() && at < region.data() + region.size();
}

struct Laid {
  size_t pages = 0;
  size_t lines = 0;
  size_t linesInRegion = 0;  // PageLines whose object lies in the lent region
};

// `body` laid out by the chapter parser with `region` lent as its build arena, as a background build lays a chapter
// out. Each page is looked at as the parser hands it off, before it is destroyed and its block rewound.
Laid layOutLent(const std::string& body, std::vector<uint8_t>& region) {
  Laid laid;
  const uint8_t* const begin = region.data();
  const uint8_t* const end = begin + region.size();
  BuildArena arena(region.data(), region.size());
  GfxRenderer renderer;
  // The parser's SAX state is a plain allocation in the region (setBuildArena), given back with this block.
  BuildArena::Block parse = arena.reserveBlock();
  {
    ChapterHtmlSlimParser parser(
        /*epub=*/nullptr, renderer, /*fontId=*/1, /*lineCompression=*/1.0f, /*extraParagraphSpacing=*/false,
        /*paragraphAlignment=*/0, /*viewportWidth=*/480, /*viewportHeight=*/400, /*hyphenationEnabled=*/false,
        /*fontSizeNormalization=*/false, /*bionicReadingEnabled=*/false,
        [&laid, begin, end](std::unique_ptr<Page> page) {
          ++laid.pages;
          for (const auto& el : page->elements) {
            if (el->getTag() != TAG_PageLine) continue;
            ++laid.lines;
            const auto* const at = reinterpret_cast<const uint8_t*>(el.get());
            if (at >= begin && at < end) ++laid.linesInRegion;
          }
          return true;
        },
        /*embeddedStyle=*/false, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/2);
    parser.setBuildArena(&arena);
    const std::string document =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>T"
        "</title></head><body>" +
        body + "</body></html>\n";
    EXPECT_TRUE(parser.setup(document.size()));
    EXPECT_EQ(document.size(), parser.write(reinterpret_cast<const uint8_t*>(document.data()), document.size()));
    EXPECT_TRUE(parser.finalize());
  }
  EXPECT_TRUE(arena.release(parse));
  EXPECT_EQ(0u, arena.releaseFailures()) << "every block of the region was released in order";
  return laid;
}

std::string paragraphs(const int count) {
  std::string out;
  for (int i = 0; i < count; ++i) {
    out += "<p>Paragraph " + std::to_string(i) + " has words enough to wrap over two or three lines of the page.</p>";
  }
  return out;
}

}  // namespace

// A build with no image manifest keeps nothing free for the walk: every line's PageLine fits in its page's block.
TEST(PageLineArena, ALentBuildLaysEachLineOutInItsPagesBlock) {
  std::vector<uint8_t> region(kLentBytes);
  const Laid laid = layOutLent(paragraphs(60), region);
  ASSERT_GT(laid.pages, 2u) << "the chapter must span pages, each its own block";
  ASSERT_GT(laid.lines, 60u);
  EXPECT_EQ(laid.lines, laid.linesInRegion) << "a PageLine was made on the heap with room for it in the page block";
}

TEST(PageLineArena, ALineGoesInTheArenaWhileItKeepsTheReserve) {
  std::vector<uint8_t> region(kReserve + 256);
  BuildArena arena(region.data(), region.size());
  TextBlock::ArenaRoom room{kReserve};
  auto block = heapLine();
  const size_t made = heapAllocations();
  const auto line = PageLine::make(std::move(block), 3, 7, &arena, &room);
  ASSERT_TRUE(line);
  EXPECT_TRUE(line->objectInArena());
  EXPECT_TRUE(inRegion(line.get(), region));
  EXPECT_EQ(made, heapAllocations()) << "no heap block for the line";
  EXPECT_GE(room.displaced, sizeof(PageLine));
  EXPECT_EQ(arena.used(), room.displaced) << "the object and its padding are what the page's objects took";
  EXPECT_EQ(3, line->xPos);
  EXPECT_EQ(7, line->yPos);
  EXPECT_EQ(std::string("beta"), line->getBlock()->wordText(1));
}

TEST(PageLineArena, ALineGoesToTheHeapWhenTheArenaWouldFallBelowTheReserve) {
  std::vector<uint8_t> region(kReserve + sizeof(PageLine) / 2);
  BuildArena arena(region.data(), region.size());
  TextBlock::ArenaRoom room{kReserve, /*displaced=*/16};
  auto block = heapLine();
  const size_t made = heapAllocations();
  const auto line = PageLine::make(std::move(block), 0, 0, &arena, &room);
  ASSERT_TRUE(line);
  EXPECT_FALSE(line->objectInArena());
  EXPECT_FALSE(inRegion(line.get(), region));
  EXPECT_EQ(made + 1, heapAllocations());
  EXPECT_EQ(0u, arena.used());
  EXPECT_EQ(16u, room.displaced) << "a heap line takes nothing from the arena";
}

TEST(PageLineArena, TheReserveCountsWhatTheArenaAlreadyHolds) {
  std::vector<uint8_t> region(kReserve + 4096);
  BuildArena arena(region.data(), region.size());
  ASSERT_NE(nullptr, arena.alloc(4096 - sizeof(PageLine) / 2, 1));  // the page's lines so far
  TextBlock::ArenaRoom room{kReserve};
  const auto line = PageLine::make(heapLine(), 0, 0, &arena, &room);
  ASSERT_TRUE(line);
  EXPECT_FALSE(line->objectInArena());
}

TEST(PageLineArena, WithoutAnArenaOrARoomALineIsOnTheHeap) {
  std::vector<uint8_t> region(kReserve + 4096);
  BuildArena arena(region.data(), region.size());
  TextBlock::ArenaRoom room{0};
  const auto noArena = PageLine::make(heapLine(), 0, 0, nullptr, &room);
  ASSERT_TRUE(noArena);
  EXPECT_FALSE(noArena->objectInArena());
  const auto noRoom = PageLine::make(heapLine(), 0, 0, &arena, nullptr);
  ASSERT_TRUE(noRoom);
  EXPECT_FALSE(noRoom->objectInArena());
  EXPECT_EQ(0u, arena.used());
}

// Room neither in the arena nor on the heap: no line, as before 9c (addLineToPage's "Dropping line"), and its TextBlock
// goes with the refusal.
TEST(PageLineArena, ALineTheHeapRefusesIsNoneAndItsTextBlockIsFreed) {
  std::vector<uint8_t> region(kReserve);
  BuildArena arena(region.data(), region.size());
  TextBlock::ArenaRoom room{kReserve};
  const size_t live = heapLiveBytes();
  std::unique_ptr<PageLine> line;
  {
    auto block = heapLine();
    const AllocationRefusal refused(sizeof(PageLine));
    line = PageLine::make(std::move(block), 0, 0, &arena, &room);
  }
  EXPECT_FALSE(line);
  EXPECT_EQ(live, heapLiveBytes());
}

// The destroying operator delete on PageElement, through each pointer type an element is held by: an arena line is
// destroyed (its heap TextBlock freed with it) and not given to the heap; a heap element is destroyed and freed.
TEST(PageLineArena, DeletingAnElementFreesOnlyWhatTheHeapMade) {
  std::vector<uint8_t> region(4096);
  BuildArena arena(region.data(), region.size());
  TextBlock::ArenaRoom room{0};
  const size_t live = heapLiveBytes();

  std::unique_ptr<PageElement> asThePageHoldsIt = PageLine::make(heapLine(), 0, 0, &arena, &room);
  ASSERT_TRUE(asThePageHoldsIt);
  ASSERT_TRUE(static_cast<const PageLine&>(*asThePageHoldsIt).objectInArena());
  EXPECT_GT(heapLiveBytes(), live) << "its TextBlock, on the heap";
  asThePageHoldsIt.reset();
  EXPECT_EQ(live, heapLiveBytes()) << "the destructor freed the TextBlock; the line itself was not given to the heap";

  auto asMade = PageLine::make(heapLine(), 0, 0, &arena, &room);
  ASSERT_TRUE(asMade);
  ASSERT_TRUE(asMade->objectInArena());
  asMade.reset();
  EXPECT_EQ(live, heapLiveBytes());

  std::unique_ptr<PageElement> heapLineElement = PageLine::make(heapLine(), 0, 0, nullptr, nullptr);
  ASSERT_TRUE(heapLineElement);
  heapLineElement.reset();
  EXPECT_EQ(live, heapLiveBytes()) << "the line and its TextBlock freed";

  std::unique_ptr<PageElement> rule = makeUniqueNoThrow<PageHR>(0, 0, 100);
  ASSERT_TRUE(rule);
  EXPECT_GE(heapLiveBytes() - live, sizeof(PageHR));
  rule.reset();
  EXPECT_EQ(live, heapLiveBytes()) << "any other element: freed";
}

// A line destroyed after the block it was built in was rewound is a use after rewind: it aborts, rather than reading
// the fill as a vtable pointer.
TEST(PageLineArenaDeathTest, ALineDestroyedAfterItsBlockWasRewoundAborts) {
  EXPECT_DEATH(
      {
        std::vector<uint8_t> region(4096);
        BuildArena arena(region.data(), region.size());
        BuildArena::Block page = arena.reserveBlock();
        TextBlock::ArenaRoom room{0};
        auto line = PageLine::make(heapLine(), 0, 0, &arena, &room);
        if (!line || !line->objectInArena() || !arena.release(page)) std::abort();
        line.reset();
      },
      "PageElement destroyed after the arena block it was built in was rewound");
}
