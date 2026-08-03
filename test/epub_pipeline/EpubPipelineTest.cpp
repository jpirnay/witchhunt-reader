// Phase-0 equivalence harness tests (docs/compiled-book-pipeline-plan.md):
//  1. Determinism — two cold runs over the same book produce byte-identical dumps.
//  2. Warm-path equivalence — a run served from the section cache dumps
//     identically to the cold run that built it.
//  3. Golden equivalence — the dump matches the committed golden for every
//     synthetic corpus book. Regenerate intentionally changed goldens with:
//     UPDATE_GOLDENS=1 ctest -R EpubPipeline
//
// Every case runs twice: once with font-size normalization OFF (the tight ±3%
// float-rounding dead zone) and once ON (the ±10% band that snaps publisher
// near-body <span font-size:0.92em> wrappers back to native size). The two
// settings produce genuinely different layout, so each has its own golden:
// <book>.golden.txt for OFF and <book>.norm.golden.txt for ON.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "epub_pipeline_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

// One corpus book under one font-size-normalization setting.
struct Case {
  std::string epub;
  bool fontSizeNormalization = false;
};

// Without this gtest dumps the struct as raw bytes in failure messages.
void PrintTo(const Case& c, std::ostream* os) {
  *os << fs::path(c.epub).filename().string() << " [fontSizeNormalization=" << (c.fontSizeNormalization ? "on" : "off")
      << "]";
}

std::string stem(const std::string& path) { return fs::path(path).stem().string(); }

// Distinguishes the two variants' cache dirs and golden files. OFF keeps the
// historical unsuffixed golden name so its committed layout stays reviewable
// across the normalization change.
std::string variantSuffix(const Case& c) { return c.fontSizeNormalization ? "_norm" : ""; }

std::string runOnce(const Case& c, const std::string& cacheDir) {
  pipeline_harness::Profile profile;
  profile.fontSizeNormalization = c.fontSizeNormalization;
  std::ostringstream dump;
  const bool ok = pipeline_harness::runAndDump(c.epub, cacheDir, profile, dump);
  EXPECT_TRUE(ok) << "pipeline failed for " << c.epub
                  << " (fontSizeNormalization=" << (c.fontSizeNormalization ? "on" : "off") << ")\n"
                  << dump.str();
  return dump.str();
}

// Unique per (book, variant) so the two variants never share a cache dir.
std::string caseCacheDir(const Case& c, const std::string& tag) {
  return freshCacheDir(stem(c.epub) + variantSuffix(c) + "_" + tag);
}

class EpubPipelineTest : public testing::TestWithParam<Case> {};

TEST_P(EpubPipelineTest, ColdRunsAreDeterministic) {
  const Case c = GetParam();
  const std::string dump1 = runOnce(c, caseCacheDir(c, "a"));
  const std::string dump2 = runOnce(c, caseCacheDir(c, "b"));
  EXPECT_EQ(dump1, dump2) << "two cold builds of " << c.epub << " diverged";
}

TEST_P(EpubPipelineTest, WarmRunMatchesColdRun) {
  const Case c = GetParam();
  const std::string cacheDir = caseCacheDir(c, "warm");
  const std::string cold = runOnce(c, cacheDir);
  const std::string warm = runOnce(c, cacheDir);  // same cacheDir: cache-hit path
  EXPECT_EQ(cold, warm) << "cache-served layout of " << c.epub << " differs from the build that wrote it";
}

TEST_P(EpubPipelineTest, MatchesGolden) {
  const Case c = GetParam();
  const std::string dump = runOnce(c, caseCacheDir(c, "golden"));
  const fs::path goldenPath = fs::path(GOLDEN_DIR) / (stem(c.epub) + variantSuffix(c) + ".golden.txt");

  if (std::getenv("UPDATE_GOLDENS")) {
    std::ofstream(goldenPath) << dump;
    GTEST_SKIP() << "golden regenerated: " << goldenPath;
  }
  std::ifstream in(goldenPath);
  ASSERT_TRUE(in) << "missing golden " << goldenPath << " — run with UPDATE_GOLDENS=1 to create it";
  std::stringstream golden;
  golden << in.rdbuf();
  EXPECT_EQ(golden.str(), dump) << "layout drift vs golden for " << c.epub
                                << " (fontSizeNormalization=" << (c.fontSizeNormalization ? "on" : "off")
                                << ") — if intentional, regenerate with UPDATE_GOLDENS=1 and explain in the commit";
}

// Every corpus book crossed with both font-size-normalization settings.
std::vector<Case> corpusCases() {
  std::vector<std::string> paths;
  for (const auto& entry : fs::directory_iterator(CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") paths.push_back(entry.path().string());
  }
  std::sort(paths.begin(), paths.end());

  std::vector<Case> cases;
  for (const auto& path : paths) {
    cases.push_back(Case{path, /*fontSizeNormalization=*/false});
    cases.push_back(Case{path, /*fontSizeNormalization=*/true});
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(SyntheticCorpus, EpubPipelineTest, testing::ValuesIn(corpusCases()),
                         [](const testing::TestParamInfo<Case>& info) {
                           std::string name = stem(info.param.epub) + variantSuffix(info.param);
                           for (char& c : name)
                             if (!isalnum(static_cast<unsigned char>(c))) c = '_';
                           return name;
                         });

}  // namespace
