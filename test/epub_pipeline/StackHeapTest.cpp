// The chapter parser's per-element stacks (inline styles, lists, container widths, background pictures, held bottom
// spacing, block insets, anchors awaiting a line) are std::vectors that grow with the document's nesting, and under
// -fno-exceptions a refused growth aborts the firmware, which reboots into the same chapter. They grow through
// ChapterHtmlSlimParser::pushOrStop, which asks the heap first and stops the build with its pages kept.
// STACK_FAULT_INJECTION refuses the n-th push; this walks n over test/epubs/make_test_nested_stacks.py's book (every
// stack 20 levels deep).
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const std::string kFixture = std::string(CORPUS_DIR) + "/test_nested_stacks.epub";

// Restores "never refuse" however the test ends.
struct StackPushesLeft {
  explicit StackPushesLeft(const size_t n) {
    stackPushesLeft() = n;
    stackRefusals().clear();
    stackPushCount() = 0;
    stackPushNames().clear();
  }
  ~StackPushesLeft() { stackPushesLeft() = SIZE_MAX; }
  StackPushesLeft(const StackPushesLeft&) = delete;
  StackPushesLeft& operator=(const StackPushesLeft&) = delete;
};

struct Built {
  bool ok = false;
  bool truncated = false;
  uint16_t pages = 0;
  size_t pushes = 0;                   // pushes the build made, refused or not
  std::vector<std::string> pushNames;  // the stack each push went to, in order
  std::string refusals;                // the `what` of each refused push
  std::vector<std::string> words;
};

struct StackHeap : testing::Test {
  fs::path work;
  int builds = 0;

  void SetUp() override {
    work = fs::temp_directory_path() / ("stack_heap_" + std::to_string(static_cast<long>(getpid())) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  Built build(const size_t pushesAllowed) {
    const fs::path cache = work / ("b" + std::to_string(builds++));
    fs::create_directories(cache);
    auto epub = std::make_shared<Epub>(kFixture, cache.string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    epub->loadImageManifest();
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    Built out;
    {
      const StackPushesLeft guard(pushesAllowed);
      Section::BuildParams p;
      p.viewportWidth = 480;
      p.viewportHeight = 300;
      p.lineCompression = 1.0f;
      p.embeddedStyle = true;
      out.ok = section.createSectionFile(p, {}, /*skipEviction=*/true);
      out.pushes = stackPushCount();
      for (const char* n : stackPushNames()) out.pushNames.emplace_back(n);
      out.refusals = stackRefusals();
    }
    if (!out.ok) return out;
    out.truncated = section.isTruncatedCache();
    out.pages = section.pageCount;
    for (uint16_t pg = 0; pg < section.pageCount; ++pg) {
      section.currentPage = pg;
      const auto page = section.loadPageFromSectionFile();
      EXPECT_NE(nullptr, page) << "page " << pg;
      if (!page) break;
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock* block = static_cast<const PageLine&>(*el).getBlock();
        if (!block) continue;
        for (uint16_t w = 0; w < block->wordCount(); ++w) out.words.emplace_back(block->wordText(w));
      }
    }
    return out;
  }
};

}  // namespace

// A build nothing refuses: every push counted, the chapter whole. The fixture must reach every stack for the walk below
// to mean anything.
TEST_F(StackHeap, TheFixtureReachesEveryStack) {
  const Built whole = build(SIZE_MAX);
  ASSERT_TRUE(whole.ok);
  EXPECT_FALSE(whole.truncated);
  EXPECT_GE(whole.pages, 3) << "the chapter must span pages so a refusal late in it has pages to keep";
  EXPECT_GT(whole.pushes, 100u) << "twenty levels of styled spans and lists make a long run of pushes";
  EXPECT_TRUE(whole.refusals.empty());
}

// Refuse the n-th push for the first push, the first push after each change of stack, and every twentieth: it ends,
// never aborts, keeps the pages before the refusal (a prefix of the whole chapter's words) and says it is truncated,
// which the reader rebuilds from. Every stack takes a refusal on the way.
TEST_F(StackHeap, ARefusedPushStopsTheBuildAndKeepsThePagesBeforeIt) {
  const Built whole = build(SIZE_MAX);
  ASSERT_TRUE(whole.ok);
  ASSERT_GT(whole.pushes, 0u);
  std::string seen;
  size_t kept = 0;
  size_t builds = 0;
  for (size_t allowed = 0; allowed < whole.pushes; ++allowed) {
    // Every stack is refused on its first push and wherever a run of another stack ends; the rest are sampled.
    const bool stackChanges = allowed == 0 || whole.pushNames[allowed] != whole.pushNames[allowed - 1];
    if (!stackChanges && allowed % 20 != 0) continue;
    ++builds;
    SCOPED_TRACE("pushes allowed: " + std::to_string(allowed));
    const Built cut = build(allowed);
    ASSERT_FALSE(cut.refusals.empty()) << "the build made " << whole.pushes << " pushes and refused none of them";
    seen += cut.refusals;
    if (!cut.ok) continue;  // the refusal came before the first page was done: nothing to keep
    ++kept;
    EXPECT_TRUE(cut.truncated) << "the chapter stopped early and must say so";
    EXPECT_LT(cut.words.size(), whole.words.size());
    for (size_t i = 0; i < cut.words.size(); ++i) ASSERT_EQ(cut.words[i], whole.words[i]) << "word " << i;
  }
  EXPECT_GT(kept, builds / 2) << "most refusals leave pages to keep";
  for (const char* stack : {"style", "list", "width", "background", "bottom spacing", "inset", "anchor"}) {
    EXPECT_NE(std::string::npos, seen.find(stack)) << "no refusal reached the " << stack << " stack";
  }
}
