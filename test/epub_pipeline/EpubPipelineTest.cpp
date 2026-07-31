// Phase-0 equivalence harness tests (docs/compiled-book-pipeline-plan.md):
//  1. Determinism — two cold runs over the same book produce byte-identical dumps.
//  2. Warm-path equivalence — a run served from the section cache dumps
//     identically to the cold run that built it.
//  3. Golden equivalence — the dump matches the committed golden for every
//     synthetic corpus book. Regenerate intentionally changed goldens with:
//     UPDATE_GOLDENS=1 ctest -R EpubPipeline
#include <gtest/gtest.h>
#include <process.h>  // _getpid — per-process temp isolation under parallel ctest

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

// Per-PROCESS temp root: gtest_discover_tests runs each test in its own process, ctest runs them in
// parallel, and they build overlapping corpus books. Scoping every cache dir under the PID keeps
// parallel processes from ever touching the same path (freshCacheDir's remove_all would otherwise
// race a sibling process's build).
std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / ("epub_pipeline_test_" + std::to_string(_getpid())) / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

std::string runOnce(const std::string& epubPath, const std::string& cacheDir) {
  std::ostringstream dump;
  const bool ok = pipeline_harness::runAndDump(epubPath, cacheDir, pipeline_harness::Profile{}, dump);
  EXPECT_TRUE(ok) << "pipeline failed for " << epubPath << "\n" << dump.str();
  return dump.str();
}

std::string stem(const std::string& path) { return fs::path(path).stem().string(); }

class EpubPipelineTest : public testing::TestWithParam<std::string> {};

TEST_P(EpubPipelineTest, ColdRunsAreDeterministic) {
  const std::string epub = GetParam();
  const std::string dump1 = runOnce(epub, freshCacheDir(stem(epub) + "_a"));
  const std::string dump2 = runOnce(epub, freshCacheDir(stem(epub) + "_b"));
  EXPECT_EQ(dump1, dump2) << "two cold builds of " << epub << " diverged";
}

TEST_P(EpubPipelineTest, WarmRunMatchesColdRun) {
  const std::string epub = GetParam();
  const std::string cacheDir = freshCacheDir(stem(epub) + "_warm");
  const std::string cold = runOnce(epub, cacheDir);
  const std::string warm = runOnce(epub, cacheDir);  // same cacheDir: cache-hit path
  EXPECT_EQ(cold, warm) << "cache-served layout of " << epub << " differs from the build that wrote it";
}

TEST_P(EpubPipelineTest, MatchesGolden) {
  const std::string epub = GetParam();
  const std::string dump = runOnce(epub, freshCacheDir(stem(epub) + "_golden"));
  const fs::path goldenPath = fs::path(GOLDEN_DIR) / (stem(epub) + ".golden.txt");

  if (std::getenv("UPDATE_GOLDENS")) {
    std::ofstream(goldenPath) << dump;
    GTEST_SKIP() << "golden regenerated: " << goldenPath;
  }
  std::ifstream in(goldenPath);
  ASSERT_TRUE(in) << "missing golden " << goldenPath << " — run with UPDATE_GOLDENS=1 to create it";
  std::stringstream golden;
  golden << in.rdbuf();
  EXPECT_EQ(golden.str(), dump) << "layout drift vs golden for " << epub
                                << " — if intentional, regenerate with UPDATE_GOLDENS=1 and explain in the commit";
}

std::vector<std::string> corpusEpubs() {
  std::vector<std::string> paths;
  for (const auto& entry : fs::directory_iterator(CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") paths.push_back(entry.path().string());
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

INSTANTIATE_TEST_SUITE_P(SyntheticCorpus, EpubPipelineTest, testing::ValuesIn(corpusEpubs()),
                         [](const testing::TestParamInfo<std::string>& info) {
                           std::string name = stem(info.param);
                           for (char& c : name)
                             if (!isalnum(static_cast<unsigned char>(c))) c = '_';
                           return name;
                         });

}  // namespace
