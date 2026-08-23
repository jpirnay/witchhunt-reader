#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/LanguageHyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"

#ifndef HYPHENATION_RESOURCES_DIR
#error "HYPHENATION_RESOURCES_DIR must be defined by the build system"
#endif

namespace {

struct DanteCase {
  std::string word;
  std::vector<size_t> legal;
  std::vector<size_t> preferred;
  std::vector<size_t> undesirable;
};

std::vector<size_t> parsePositions(const std::string& text) {
  std::vector<size_t> result;
  if (text.empty()) {
    return result;
  }

  std::istringstream stream(text);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (!token.empty()) {
      result.push_back(static_cast<size_t>(std::stoul(token)));
    }
  }
  return result;
}

std::vector<DanteCase> loadDanteCases() {
  const std::string path = std::string(HYPHENATION_RESOURCES_DIR) + "/german_dante_eval.txt";
  std::ifstream file(path);
  std::vector<DanteCase> result;

  if (!file.is_open()) {
    return result;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream stream(line);
    std::string word;
    std::string legal;
    std::string preferred;
    std::string undesirable;

    if (!std::getline(stream, word, '|') || !std::getline(stream, legal, '|') ||
        !std::getline(stream, preferred, '|') || !std::getline(stream, undesirable)) {
      continue;
    }

    result.push_back({word, parsePositions(legal), parsePositions(preferred), parsePositions(undesirable)});
  }

  return result;
}

bool contains(const std::vector<size_t>& values, const size_t value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<size_t> hyphenate(const LanguageHyphenator& hyphenator, const std::string& word) {
  auto cps = collectCodepoints(word);
  trimSurroundingPunctuationAndFootnote(cps);
  return hyphenator.breakIndexes(cps);
}

}  // namespace

TEST(GermanHybridEval, HeldOutDantePrecisionAndRecall) {
  const auto* hyphenator = getLanguageHyphenatorForPrimaryTag("de");
  ASSERT_NE(hyphenator, nullptr);

  const auto cases = loadDanteCases();
  ASSERT_FALSE(cases.empty()) << "german_dante_eval.txt missing; run scripts/update_hyphenation.sh first";

  size_t truePositives = 0;
  size_t falsePositives = 0;
  size_t falseNegatives = 0;
  size_t preferredFound = 0;
  size_t preferredTotal = 0;
  size_t undesirableUsed = 0;
  size_t exactWords = 0;

  for (const auto& testCase : cases) {
    const auto actual = hyphenate(*hyphenator, testCase.word);

    for (const size_t position : actual) {
      if (contains(testCase.legal, position)) {
        ++truePositives;
      } else {
        ++falsePositives;
      }
      if (contains(testCase.undesirable, position)) {
        ++undesirableUsed;
      }
    }

    for (const size_t position : testCase.legal) {
      if (!contains(actual, position)) {
        ++falseNegatives;
      }
    }

    for (const size_t position : testCase.preferred) {
      ++preferredTotal;
      if (contains(actual, position)) {
        ++preferredFound;
      }
    }

    std::vector<size_t> sortedActual = actual;
    std::vector<size_t> sortedLegal = testCase.legal;
    std::sort(sortedActual.begin(), sortedActual.end());
    std::sort(sortedLegal.begin(), sortedLegal.end());
    if (sortedActual == sortedLegal) {
      ++exactWords;
    }
  }

  const double precision = (truePositives + falsePositives) == 0
                               ? 1.0
                               : static_cast<double>(truePositives) / (truePositives + falsePositives);
  const double recall = (truePositives + falseNegatives) == 0
                            ? 1.0
                            : static_cast<double>(truePositives) / (truePositives + falseNegatives);
  const double preferredRecall = preferredTotal == 0 ? 1.0 : static_cast<double>(preferredFound) / preferredTotal;
  const double exactRate = static_cast<double>(exactWords) / cases.size();

  ::testing::Test::RecordProperty("precision_percent", std::to_string(precision * 100.0));
  ::testing::Test::RecordProperty("recall_percent", std::to_string(recall * 100.0));
  ::testing::Test::RecordProperty("preferred_recall_percent", std::to_string(preferredRecall * 100.0));
  ::testing::Test::RecordProperty("exact_word_percent", std::to_string(exactRate * 100.0));
  ::testing::Test::RecordProperty("false_positives", std::to_string(falsePositives));
  ::testing::Test::RecordProperty("undesirable_used", std::to_string(undesirableUsed));
  ::testing::Test::RecordProperty("test_cases", std::to_string(cases.size()));

  std::cout << "German hybrid DANTE: precision=" << precision * 100.0 << "% recall=" << recall * 100.0
            << "% preferred-recall=" << preferredRecall * 100.0 << "% exact=" << exactRate * 100.0
            << "% false-positive-breaks=" << falsePositives << " undesirable-used=" << undesirableUsed << '\n';

  // Bootstrap gates. The conservative hybrid is intentionally optimized for
  // precision, not for reproducing every optional DANTE/Pyphen break.
  // After the first full regeneration, tighten these to ~0.5 pp below the
  // measured branch baseline.
  EXPECT_GE(precision, 0.990) << "German hybrid generated too many illegal breaks";
  EXPECT_GE(recall, 0.550) << "German hybrid became too conservative to be useful";
}
