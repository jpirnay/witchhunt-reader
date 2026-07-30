// ContentBinCompiler (fresh reader's incremental background compiler) must produce a content.bin
// byte-identical to the whole-book Section::compileBookToContentBin, regardless of how finely it is
// sliced. It also must RESUME: a compile interrupted partway (some spines committed) and reopened
// continues to a complete, identical file. Content-only mode writes NO section-cache files.

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Section.h"
#include "Epub/content/ContentBinCompiler.h"

namespace fs = std::filesystem;

namespace {

std::string corpus(const char* book) { return std::string(CORPUS_DIR) + "/" + book; }

std::string freshDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / ("content_bin_compiler_" + std::to_string(_getpid())) / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

Section::BuildParams params() {
  Section::BuildParams bp;
  bp.fontId = 1;  // any registered/default host font id; content.bin is settings-independent for blocks
  bp.viewportWidth = 460;
  bp.viewportHeight = 760;
  bp.embeddedStyle = true;
  return bp;
}

// A multi-spine book so the spine cursor + resume are exercised.
constexpr const char* kBook = "test_spine_toc_edges.epub";

}  // namespace

// The incremental compiler, sliced as finely as possible (1 ms budget → yields mid-spine many times),
// produces the same content.bin bytes as the one-shot whole-book compile.
TEST(ContentBinCompiler, SlicedMatchesWholeBook) {
  GfxRenderer renderer;

  // (A) Whole-book reference.
  const std::string dirA = freshDir("whole");
  auto epubA = std::make_shared<Epub>(corpus(kBook), dirA);
  ASSERT_TRUE(epubA->load(true, false));
  ASSERT_TRUE(Section::compileBookToContentBin(epubA, renderer, params()));
  const auto whole = readFile(epubA->getCachePath() + "/content.bin");
  ASSERT_FALSE(whole.empty());

  // (B) Incremental, 1 ms slices, to completion.
  const std::string dirB = freshDir("incr");
  auto epubB = std::make_shared<Epub>(corpus(kBook), dirB);
  ASSERT_TRUE(epubB->load(true, false));
  compiled::ContentBinCompiler comp(epubB, renderer, params());
  int guard = 0;
  compiled::ContentBinCompiler::Step s = compiled::ContentBinCompiler::Step::More;
  while (s == compiled::ContentBinCompiler::Step::More && ++guard < 200000) {
    s = comp.step(/*budgetMs=*/1);
  }
  ASSERT_EQ(s, compiled::ContentBinCompiler::Step::Done);
  const auto incr = readFile(epubB->getCachePath() + "/content.bin");

  EXPECT_EQ(whole, incr) << "incremental content.bin differs from the whole-book compile";

  // Content-only: NO section-cache page files should have been produced by the incremental compiler.
  for (const auto& e : fs::recursive_directory_iterator(epubB->getCachePath())) {
    const std::string name = e.path().filename().string();
    EXPECT_EQ(name.rfind("section_", 0), std::string::npos) << "unexpected section file: " << e.path().string();
  }
}

// Resume: run the compiler until at least one spine commits, DESTROY it (simulating a crash/exit),
// then a fresh compiler over the same cache continues to a complete file identical to the whole-book.
TEST(ContentBinCompiler, ResumeCompletesIdentically) {
  GfxRenderer renderer;

  const std::string dirRef = freshDir("resume_ref");
  auto epubRef = std::make_shared<Epub>(corpus(kBook), dirRef);
  ASSERT_TRUE(epubRef->load(true, false));
  ASSERT_TRUE(Section::compileBookToContentBin(epubRef, renderer, params()));
  const auto whole = readFile(epubRef->getCachePath() + "/content.bin");

  const std::string dir = freshDir("resume");
  auto epub = std::make_shared<Epub>(corpus(kBook), dir);
  ASSERT_TRUE(epub->load(true, false));

  // Phase 1: step until the frontier advances at least one spine, then drop the compiler.
  {
    compiled::ContentBinCompiler comp(epub, renderer, params());
    int guard = 0;
    while (comp.committedSpines() < 1 && !comp.done() && ++guard < 200000) comp.step(/*budgetMs=*/0);
    ASSERT_GE(comp.committedSpines(), 1u);
  }  // comp destructs — content.bin has a committed prefix

  // Phase 2: a fresh compiler resumes and completes.
  {
    compiled::ContentBinCompiler comp(epub, renderer, params());
    int guard = 0;
    auto s = compiled::ContentBinCompiler::Step::More;
    while (s == compiled::ContentBinCompiler::Step::More && ++guard < 200000) s = comp.step(/*budgetMs=*/0);
    ASSERT_EQ(s, compiled::ContentBinCompiler::Step::Done);
  }

  const auto resumed = readFile(epub->getCachePath() + "/content.bin");
  EXPECT_EQ(whole, resumed) << "resumed content.bin differs from the whole-book compile";
}
